#include <algorithm>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

#include "error.h"
#include "io.h"
#include "logging.h"
#include "nbd-common.h"
#include "net.h"
#include "server.h"
#include "server_nbd.h"
#include "socket_listener.h"
#include "storage_backend.h"
#include "str.h"
#include "yaml-helpers.h"


const char *const nbd_cmd_names[] = { "read", "write", "disc", "flush", "trim", "cache", "zero", "status", "resize" };

typedef enum { nbd_st_init, nbd_st_client_flags, nbd_st_options, nbd_st_transmission, nbd_st_terminate } nbd_state_t;
constexpr const char *const nbd_st_strings[] { "init", "client flags", "options", "transmission", "terminate" };

nbd::nbd(const std::string & id, const std::vector<socket_listener *> & socket_listeners, const std::vector<storage_backend *> & storage_backends) :
	server(id),
	storage_backends(storage_backends),
	socket_listeners(socket_listeners)
{
	if (storage_backends.empty())
		throw "nbd: backends list is empty";

	for(auto sb : storage_backends) {
		if (maximum_transaction_size == -1)
			maximum_transaction_size = sb->get_maximum_transaction_size();
		else
			maximum_transaction_size = std::min(maximum_transaction_size, sb->get_maximum_transaction_size());
	}

	if (maximum_transaction_size == -1)
		throw myformat("nbd(%s): no storage configured", id.c_str());

	dolog(ll_info, "nbd(%s): maximum transaction size: %d bytes", id.c_str(), maximum_transaction_size);

	for(auto sb : storage_backends)
		sb->acquire(this);

	for(auto sl : socket_listeners) {
		if (!sl->begin())
			throw myformat("nbd(%s): cannot setup socket-listener for \"%s\"", id.c_str(), sl->get_listen_address().c_str());

		sl->acquire(this);

		std::thread *th = new std::thread([this, sl] { worker_thread(sl); });
		if (!th)
			throw myformat("nbd(%s): cannot start worker-thread for \"%s\"", id.c_str(), sl->get_listen_address().c_str());

		worker_threads.push_back(th);
	}

	dolog(ll_info, "nbd(%s): started", id.c_str());
}

nbd::~nbd()
{
	stop();

	for(auto t : threads) {
		t.first->join();

		delete t.first;
		delete t.second;
	}

	for(auto t : worker_threads) {
		t->join();
		delete t;
	}

	for(auto sl : socket_listeners)
		sl->release(this);

	for(auto sb : storage_backends)
		sb->release(this);

	for(auto sl : socket_listeners)
		delete sl;
}

nbd * nbd::load_configuration(const YAML::Node & node, const std::vector<storage_backend *> & storage)
{
	dolog(ll_info, " * nbd::load_configuration");

	const YAML::Node cfg = node["cfg"];

	std::string id = yaml_get_string(cfg, "id", "module id");

	// storage backends
	std::vector<storage_backend *> sbs;
        YAML::Node y_sbs = cfg["storage-backends"];
        for(YAML::const_iterator it = y_sbs.begin(); it != y_sbs.end(); it++) {
		std::string sb_name = it->as<std::string>();

		storage_backend *sb = find_storage(storage, sb_name);
		if (!sb) {
			dolog(ll_error, "nbd::load_configuration: storage \"%s\" not known", sb_name.c_str());
			return nullptr;
		}

		dolog(ll_info, "nbd::load_configuration: loaded configuration \"%s\" of size %ld bytes", sb->get_id().c_str(), sb->get_size());

                sbs.push_back(sb);
	}

	// socket listeners
	std::vector<socket_listener *> socket_listeners;
        YAML::Node y_socket_listeners = cfg["socket-listeners"];
        for(YAML::const_iterator it = y_socket_listeners.begin(); it != y_socket_listeners.end(); it++) {
		YAML::Node sl_node = it->as<YAML::Node>();

		socket_listener *sl = socket_listener::load_configuration(sl_node);

		if (!sl) {
			dolog(ll_error, "nbd::load_configuration: failed loading socket listener");
			return nullptr;
		}

		socket_listeners.push_back(sl);
	}

	dolog(ll_info, "nbd::load_configuration: NBD server started, listening on %zu socket(s) for %zu storage(s)", socket_listeners.size(), sbs.size());

	return new nbd(id, socket_listeners, sbs);
}

YAML::Node nbd::emit_configuration() const
{
	std::vector<std::string> out_storage_backends;
	for(auto sb : storage_backends)
		out_storage_backends.push_back(sb->get_id());

	YAML::Node out_cfg;
	out_cfg["storage-backends"] = out_storage_backends;

	std::vector<YAML::Node> socket_listeners_out;
	for(auto sl : socket_listeners)
		socket_listeners_out.push_back(sl->emit_configuration());
	out_cfg["socket-listeners"] = socket_listeners_out;

	YAML::Node out;
	out["type"] = "nbd";
	out["cfg"] = out_cfg;

	return out;
}

bool nbd::send_option_reply(const int fd, const uint32_t opt, const uint32_t reply_type, const std::vector<uint8_t> & data)
{
	std::vector<uint8_t> msg;
	add_uint64(msg, 0x3e889045565a9);
	add_uint32(msg, opt);
	add_uint32(msg, reply_type);
	add_uint32(msg, data.size());

	if (WRITE(fd, msg.data(), msg.size()) != ssize_t(msg.size()))
		return false;

	if (WRITE(fd, data.data(), data.size()) != ssize_t(data.size()))
		return false;

	return true;
}

bool nbd::send_cmd_reply(const int fd, const uint32_t err, const uint64_t handle, const std::vector<uint8_t> & data)
{
	std::vector<uint8_t> reply;

	add_uint32(reply, 0x67446698);  // magic
	add_uint32(reply, err);  // error
	add_uint64(reply, handle);

	if (WRITE(fd, reply.data(), reply.size()) != ssize_t(reply.size())) {
		dolog(ll_info, "nbd::send_cmd_reply: failed transmitting header");
		return false;
	}

	if (data.empty() == false) {
		if (WRITE(fd, data.data(), data.size()) != ssize_t(data.size())) {
			dolog(ll_info, "nbd::send_cmd_reply: failed transmitting data");
			return false;
		}
	}

	return true;
}

std::string uint_vector_to_string(const std::vector<uint8_t> & in)
{
	return std::string(reinterpret_cast<const char *>(in.data()), in.size());
}

std::optional<size_t> nbd::find_storage_backend_by_id(const std::string & id)
{
	dolog(ll_info, "nbd::find_storage_backend_by_id: selecting storage \"%s\"", id.c_str());

	if (id.empty()) {
		dolog(ll_info, "nbd::find_storage_backend_by_id: returning default");

		return 0;  // default storage
	}

	for(size_t idx=0; idx<storage_backends.size(); idx++) {
		if (storage_backends.at(idx)->get_id() == id)
			return idx;
	}

	dolog(ll_warning, "nbd::find_storage_backend_by_id: storage \"%s\" not found", id.c_str());

	return { };
}

void nbd::handle_client(const int fd, std::atomic_bool *const thread_stopped)
{
	nbd_state_t state = nbd_st_init;

	size_t current_sb = 0;
	bool use_0x00_padding = true;

	for(;state != nbd_st_terminate && !stop_flag;) {
		dolog(ll_debug, "nbd::handle_client: state: \"%s\" (%d)", nbd_st_strings[state], state);

		if (state == nbd_st_init) {
			std::vector<uint8_t> msg;
			add_uint64(msg, 0x4e42444d41474943);  // 'NBDMAGIC'
			add_uint64(msg, 0x49484156454F5054);  // 'IHAVEOPT'
			add_uint16(msg, NBD_FLAG_C_NO_ZEROES | NBD_FLAG_C_FIXED_NEWSTYLE);  // handshake flags;
			if (WRITE(fd, msg.data(), msg.size()) != ssize_t(msg.size())) {
				dolog(ll_info, "nbd::handle_client: transmission failed");
				break;
			}

			state = nbd_st_client_flags;
		}
		else if (state == nbd_st_client_flags) {
			auto client_flags = receive_uint32(fd);
			if (!client_flags.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (client_flags)");
				break;
			}

			if (client_flags.value() & NBD_FLAG_C_NO_ZEROES) {
				use_0x00_padding = false;
				dolog(ll_debug, "nbd::handle_client: 0x00 padding disabled");
			}

			state = nbd_st_options;
		}
		else if (state == nbd_st_options) {
			auto option_magic = receive_uint64(fd);
			if (!option_magic.has_value() || option_magic.value() != 0x49484156454F5054) {
				dolog(ll_info, "nbd::handle_client: receive fail of magic invalid (%x)", option_magic.has_value() ? option_magic.value() : -1);
				break;
			}

			auto option = receive_uint32(fd);
			if (!option.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (option)");
				break;
			}

			auto data_len = receive_uint32(fd);
			if (!data_len.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (data_len)");
				break;
			}

			std::optional<std::vector<uint8_t> > option_data;
			if (data_len) {
				option_data = receive_n_uint8(fd, data_len.value());

				if (!option_data.has_value()) {
					dolog(ll_info, "nbd::handle_client: receive fail (option_data)");
					break;
				}

				dolog(ll_debug, "nbd::handle_client: option dump: %s", bin_to_text(option_data.value().data(), option_data.value().size()).c_str());
			}

			dolog(ll_debug, "nbd::handle_client: option %x, data_len %d", option.value(), data_len.value());

			switch(option.value())  {
				case NBD_OPT_EXPORT_NAME:
					{
						auto sel_sb = find_storage_backend_by_id(uint_vector_to_string(option_data.value()));

						if (sel_sb.has_value() == false) {
							send_option_reply(fd, option.value(), NBD_REP_ERR_UNSUP, { });
						}
						else {
							current_sb = sel_sb.value();

							if (send_option_reply(fd, option.value(), NBD_REP_ACK, { }) == false) {
								dolog(ll_info, "nbd::handle_client: failed transmitting NBD_REP_ACK");
								state = nbd_st_terminate;
								break;
							}

							state = nbd_st_transmission;
						}
					}
					break;

				case NBD_OPT_GO:
					{
						uint32_t export_name_len = (option_data.value()[0] << 24) | (option_data.value()[1] << 16) | (option_data.value()[2] << 8) | option_data.value()[3];

						std::vector<uint8_t> name;
						for(size_t idx=0; idx<export_name_len; idx++)
							name.push_back(option_data.value()[4 + idx]);

						std::string name_str = uint_vector_to_string(name);
						auto sel_sb = find_storage_backend_by_id(name_str);

						if (sel_sb.has_value() == false) {
							dolog(ll_info, "nbd::handle_client: export %s not known", name_str.c_str());
							state = nbd_st_terminate;
							break;
						}

						int info_req_offset = 4 + export_name_len;
						uint16_t n_info_req = (option_data.value()[info_req_offset + 0] << 8) | option_data.value()[info_req_offset + 1];
						dolog(ll_debug, "nbd::handle_client: number of information requests: %d", n_info_req);

						current_sb = sel_sb.value();

						// retrieve list of info requests from option data
						std::set<uint16_t> requests;
						for(int i=0; i<n_info_req; i++)
							requests.insert((option_data.value()[info_req_offset + (i + 1) * 2 + 0] << 8) | option_data.value()[info_req_offset + (i + 1) * 2 + 1]);
						// this is the minium to send
						requests.insert(NBD_INFO_EXPORT);

						// go through each request item
						for(auto info_req : requests) {
							if (info_req == NBD_INFO_EXPORT) {
								dolog(ll_debug, "nbd::handle_client: NBD_INFO_EXPORT information request");
								std::vector<uint8_t> msg_flags;
								add_uint16(msg_flags, NBD_INFO_EXPORT);
								add_uint64(msg_flags, storage_backends.at(current_sb)->get_size());
								add_uint16(msg_flags, NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_FUA | NBD_FLAG_SEND_TRIM | NBD_FLAG_CAN_MULTI_CONN | NBD_FLAG_SEND_WRITE_ZEROES | NBD_FLAG_CAN_MULTI_CONN);

								if (send_option_reply(fd, option.value(), NBD_REP_INFO, msg_flags) == false) {
									dolog(ll_info, "nbd::handle_client: failed transmitting NBD_REP_INFO/NBD_INFO_EXPORT");
									state = nbd_st_terminate;
									break;
								}
							}
							else if (info_req == NBD_INFO_BLOCK_SIZE) {
								dolog(ll_debug, "nbd::handle_client: NBD_INFO_BLOCK_SIZE information request");
								std::vector<uint8_t> msg_block_sizes;
								add_uint16(msg_block_sizes, NBD_INFO_BLOCK_SIZE);
								add_uint32(msg_block_sizes, 512);  // minium block size
								add_uint32(msg_block_sizes, 4096);  // preferred TODO: find most common from storage backends?
								add_uint32(msg_block_sizes, maximum_transaction_size);  // maximum block size

								if (send_option_reply(fd, option.value(), NBD_REP_INFO, msg_block_sizes) == false) {
									dolog(ll_info, "nbd::handle_client: failed transmitting NBD_REP_INFO/NBD_INFO_BLOCK_SIZE");
									state = nbd_st_terminate;
									break;
								}
							}
							else {
								dolog(ll_debug, "nbd::handle_client: unknown information request %d", info_req);
							}
						}

						if (send_option_reply(fd, option.value(), NBD_REP_ACK, { }) == false) {
							state = nbd_st_terminate;
							dolog(ll_info, "nbd::handle_client: failed transmitting NBD_REP_ACK");
							break;
						}

						state = nbd_st_transmission;
					}
					break;

				default:
					dolog(ll_info, "nbd::handle_client: unknown option %d", option.value());
					send_option_reply(fd, option.value(), NBD_REP_ERR_UNSUP, { });
					break;
			}
		}
		else if (state == nbd_st_transmission) {
			auto magic = receive_uint32(fd);
			if (!magic.has_value() || magic.value() != 0x25609513) {
				dolog(ll_info, "nbd::handle_client: receive fail (magic (%08x))", magic.has_value() ? magic.value() : -1);
				break;
			}

			auto flags = receive_uint16(fd);
			if (!flags.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (flags)");
				break;
			}

			auto type = receive_uint16(fd);
			if (!type.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (type)");
				break;
			}

			auto handle = receive_uint64(fd);
			if (!handle.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (handle)");
				break;
			}

			auto offset = receive_uint64(fd);
			if (!offset.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (offset)");
				break;
			}

			auto length = receive_uint32(fd);
			if (!length.has_value()) {
				dolog(ll_info, "nbd::handle_client: receive fail (length)");
				break;
			}

			std::optional<std::vector<uint8_t> > data;
			if (type == NBD_CMD_WRITE) {
				data = receive_n_uint8(fd, length.value());

				if (!data.has_value()) {
					dolog(ll_info, "nbd::handle_client: receive fail (data)");
					break;
				}
			}

			dolog(ll_debug, "nbd::handle_client: command, flags: %x, type: %s (%d), offset: %lu, length: %u", flags.value(), nbd_cmd_names[type.value()], type.value(), offset.value(), length.value());  // TODO length of nbd_cmd_names array indexing check

			std::vector<uint8_t> reply;
			block *b = nullptr;
			int err = 0;

			switch(type.value()) {
				case NBD_CMD_READ:
					storage_backends.at(current_sb)->get_data(offset.value(), length.value(), &b, &err);
					add_uint32(reply, 0x67446698);  // magic
					add_uint32(reply, err);  // error
					add_uint64(reply, handle.value());

					if (WRITE(fd, reply.data(), reply.size()) != ssize_t(reply.size())) {
						dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_READ header");
						state = nbd_st_terminate;
						delete b;
						break;
					}

					if (err == 0) {
						if (WRITE(fd, b->get_data(), b->get_size()) != ssize_t(b->get_size())) {
							dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_READ data");
							state = nbd_st_terminate;
							delete b;
							break;
						}
					}

					delete b;
					break;

				case NBD_CMD_WRITE:
					storage_backends.at(current_sb)->put_data(offset.value(), data.value(), &err);

					if (send_cmd_reply(fd, err, handle.value(), { }) == false) {
						dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_WRITE reply");
						state = nbd_st_terminate;
						break;
					}

					break;

				case NBD_CMD_FLUSH:
					{
						int err = 0;

						if (storage_backends.at(current_sb)->fsync() == false) {
							dolog(ll_info, "nbd::handle_client: fsync failed"); 
							err = EIO;
							state = nbd_st_terminate;
						}

						if (send_cmd_reply(fd, err, handle.value(), { }) == false) {
							dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_FLUSH reply");
							state = nbd_st_terminate;
							break;
						}
					}

					break;

				case NBD_CMD_DISC:
					dolog(ll_info, "nbd::handle_client: client asked to terminate");
					state = nbd_st_terminate;

					if (send_cmd_reply(fd, 0, handle.value(), { }) == false) {
						dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_DISC reply");
						state = nbd_st_terminate;
						break;
					}
	
					break;

				case NBD_CMD_TRIM:
				case NBD_CMD_WRITE_ZEROES:
					dolog(ll_debug, "nbd::handle_client: trim/write-zeros");
					storage_backends.at(current_sb)->trim_zero(offset.value(), length.value(), type.value() == NBD_CMD_TRIM, &err);

					if (send_cmd_reply(fd, err, handle.value(), { }) == false) {
						dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_TRIM or NBD_CMD_WRITE_ZEROES reply");
						state = nbd_st_terminate;
						break;
					}

					break;

				default:
					dolog(ll_info, "nbd::handle_client: unknown command %d", type.value());
					state = nbd_st_terminate;
					break;
			}

			if ((flags.value() & NBD_CMD_FLAG_FUA) && state != nbd_st_terminate) {
				if (storage_backends.at(current_sb)->fsync() == false) {
					dolog(ll_info, "nbd::handle_client: NBD_CMD_FLAG_FUA failed"); 
					err = EIO;
					state = nbd_st_terminate;
				}
			}
		}
	}

	close(fd);

	dolog(ll_info, "nbd::handle_client: connection closed");

	*thread_stopped = true;
}

void nbd::worker_thread(socket_listener *const sl)
{
	for(;!stop_flag;) {
		int cfd = sl->wait_for_client(&stop_flag);

		if (cfd == -1) {
			dolog(ll_info, "nbd::operator: failed accepting connection");
			continue;
		}

		dolog(ll_info, "nbd::operator(%s): connection made with %s", id.c_str(), get_endpoint_name(cfd).c_str());

		std::atomic_bool *flag = new std::atomic_bool(false);
		std::thread *th = new std::thread([this, cfd, flag] { this->handle_client(cfd, flag); });

		threads.push_back({ th, flag });

		for(size_t i=0; i<threads.size();) {
			if (*threads.at(i).second == true) {
				threads.at(i).first->join();

				delete threads.at(i).first;
				delete threads.at(i).second;

				threads.erase(threads.begin() + i);
			}
			else {
				i++;
			}
		}
	}

	dolog(ll_info, "nbd::operator(%s): listener thread for \"%s\" terminating", id.c_str(), sl->get_listen_address().c_str());
}
