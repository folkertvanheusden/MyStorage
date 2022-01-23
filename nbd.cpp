#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

#include "error.h"
#include "io.h"
#include "logging.h"
#include "nbd.h"
#include "net.h"
#include "socket_listener.h"
#include "storage_backend.h"


#define NBD_CMD_READ		0
#define NBD_CMD_WRITE		1
#define NBD_FLAG_C_NO_ZEROES    (1 << 1)
#define NBD_INFO_EXPORT		0
#define NBD_OPT_EXPORT_NAME	1
#define NBD_OPT_GO		7
#define NBD_REP_ACK             1
#define NBD_REP_ERR_UNSUP	(1 | NBD_REP_FLAG_ERROR)
#define NBD_REP_FLAG_ERROR      (1 << 31)
#define NBD_REP_INFO		3

typedef enum { nbd_st_init, nbd_st_client_flags, nbd_st_options, nbd_st_transmission, nbd_st_terminate } nbd_state_t;

void add_uint64(std::vector<uint8_t> & target, const uint64_t v)
{
	target.push_back(v >> 56);
	target.push_back(v >> 48);
	target.push_back(v >> 40);
	target.push_back(v >> 32);
	target.push_back(v >> 24);
	target.push_back(v >> 16);
	target.push_back(v >>  8);
	target.push_back(v);
}

void add_uint32(std::vector<uint8_t> & target, const uint32_t v)
{
	target.push_back(v >> 24);
	target.push_back(v >> 16);
	target.push_back(v >>  8);
	target.push_back(v);
}

void add_uint16(std::vector<uint8_t> & target, const uint16_t v)
{
	target.push_back(v >>  8);
	target.push_back(v);
}

void add_uint8(std::vector<uint8_t> & target, const uint8_t v)
{
	target.push_back(v);
}

std::optional<uint64_t> receive_uint64(const int fd)
{
	uint8_t buffer[8] { 0 };
	if (READ(fd, buffer, sizeof buffer) != sizeof buffer)
		return { };
	
	return (uint64_t(buffer[0]) << 56) | (uint64_t(buffer[1]) << 48) | (uint64_t(buffer[2]) << 40) | (uint64_t(buffer[3]) << 32) | (uint64_t(buffer[4]) << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
}

std::optional<uint32_t> receive_uint32(const int fd)
{
	uint8_t buffer[4] { 0 };
	if (READ(fd, buffer, sizeof buffer) != sizeof buffer)
		return { };
	
	return (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
}

std::optional<uint16_t> receive_uint16(const int fd)
{
	uint8_t buffer[2] { 0 };
	if (READ(fd, buffer, sizeof buffer) != sizeof buffer)
		return { };
	
	return (buffer[0] << 8) | buffer[1];
}

std::optional<std::vector<uint8_t> > receive_n_uint8(const int fd, const size_t n)
{
	std::vector<uint8_t> out;
	out.resize(n);

	if (READ(fd, out.data(), n) != ssize_t(n))
		return { };

	return out;
}

nbd::nbd(socket_listener *const sl, const std::vector<storage_backend *> & storage_backends) : sl(sl), storage_backends(storage_backends)
{
	if (storage_backends.empty())
		error_exit(true, "nbd::nbd: backends list is empty");

	th = new std::thread(std::ref(*this));
}

nbd::~nbd()
{
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

std::string uint_vector_to_string(const std::vector<uint8_t> & in)
{
	return std::string(reinterpret_cast<const char *>(in.data()), in.size());
}

std::optional<size_t> nbd::find_sb(const std::string & id)
{
	if (id.empty())
		return 0;  // default storage

	for(size_t idx=0; idx<storage_backends.size(); idx++) {
		if (storage_backends.at(idx)->get_identifier() == id)
			return idx;
	}

	return { };
}

void nbd::handle_client(const int fd)
{
	nbd_state_t state = nbd_st_init;

	size_t current_sb = 0;
	bool use_0x00_padding = true;

	for(;state != nbd_st_terminate;) {
		dolog(ll_debug, "nbd::handle_client: state: %d", state);

		if (state == nbd_st_init) {
			std::vector<uint8_t> msg;
			add_uint64(msg, 0x4e42444d41474943);  // 'NBDMAGIC'
			add_uint64(msg, 0x49484156454F5054);  // 'IHAVEOPT'
			add_uint16(msg, 0x0000);              // handshake flags;
			if (WRITE(fd, msg.data(), msg.size()) != ssize_t(msg.size()))
				break;

			state = nbd_st_client_flags;
		}
		else if (state == nbd_st_client_flags) {
			auto client_flags = receive_uint32(fd);
			if (!client_flags.has_value())
				break;

			if (client_flags.value() & NBD_FLAG_C_NO_ZEROES)
				use_0x00_padding = false;

			state = nbd_st_options;
		}
		else if (state == nbd_st_options) {
			auto option_magic = receive_uint64(fd);
			if (!option_magic.has_value() || option_magic.value() != 0x49484156454F5054)
				break;

			auto option = receive_uint32(fd);
			if (!option.has_value())
				break;

			auto data_len = receive_uint32(fd);
			if (!data_len.has_value())
				break;

			std::optional<std::vector<uint8_t> > option_data;
			if (data_len) {
				option_data = receive_n_uint8(fd, data_len.value());

				if (!option_data.has_value())
					break;
			}

			switch(option.value())  {
				case NBD_OPT_EXPORT_NAME:
					{
						auto sel_sb = find_sb(uint_vector_to_string(option_data.value()));

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
						std::vector<uint8_t> msg;
						add_uint16(msg, NBD_INFO_EXPORT);
						add_uint64(msg, storage_backends.at(current_sb)->get_size());
						add_uint16(msg, 0);

						if (send_option_reply(fd, option.value(), NBD_REP_INFO, msg) == false) {
							dolog(ll_info, "nbd::handle_client: failed transmitting NBD_REP_INFO");
							state = nbd_st_terminate;
							break;
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
					break;
			}
		}
		else if (state == nbd_st_transmission) {
			auto magic = receive_uint32(fd);
			if (!magic.has_value() || magic.value() != 0x25609513)
				break;

			auto flags = receive_uint16(fd);
			if (!flags.has_value())
				break;

			auto type = receive_uint16(fd);
			if (!type.has_value())
				break;

			auto handle = receive_uint64(fd);
			if (!handle.has_value())
				break;

			auto offset = receive_uint64(fd);
			if (!offset.has_value())
				break;

			auto length = receive_uint32(fd);
			if (!length.has_value() || length.value() == 0)
				break;

			std::optional<std::vector<uint8_t> > data;
			if (type == NBD_CMD_WRITE) {
				data = receive_n_uint8(fd, length.value());

				if (!data.has_value())
					break;
			}

			printf("flags: %x, type: %d, offset: %lu, length: %u\n", flags.value(), type.value(), offset.value(), length.value());

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
						break;
					}

					if (err == 0) {
						if (WRITE(fd, b->get_data(), b->get_size()) != ssize_t(b->get_size())) {
							dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_READ data");
							state = nbd_st_terminate;
							break;
						}
					}

					delete b;
					break;

				case NBD_CMD_WRITE:
					storage_backends.at(current_sb)->put_data(offset.value(), data.value(), &err);

					add_uint32(reply, 0x67446698);  // magic
					add_uint32(reply, err);  // error
					add_uint64(reply, handle.value());

					if (WRITE(fd, reply.data(), reply.size()) != ssize_t(reply.size())) {
						dolog(ll_info, "nbd::handle_client: failed transmitting NBD_CMD_WRITE header");
						state = nbd_st_terminate;
						break;
					}
					break;

				default:
					dolog(ll_info, "nbd::handle_client: unknown option %d", type.value());
					state = nbd_st_terminate;
					break;
			}
		}
	}

	close(fd);

	dolog(ll_info, "nbd::handle_client: connection closed");
}

void nbd::operator()()
{
	for(;;) {
		int cfd = sl->wait_for_client();

		std::thread *th = new std::thread([this, cfd] { this->handle_client(cfd); });
		th->detach();
	}
}
