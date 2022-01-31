#include <errno.h>
#include <fcntl.h>
#include <string>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>

#include "error.h"
#include "io.h"
#include "logging.h"
#include "nbd-common.h"
#include "net.h"
#include "socket_client.h"
#include "storage_backend_nbd.h"
#include "str.h"
#include "yaml-helpers.h"


storage_backend_nbd::storage_backend_nbd(const std::string & id, socket_client *const sc, const std::string & export_name, int block_size, const std::vector<mirror *> & mirrors) : storage_backend(id, block_size, mirrors), sc(sc), export_name(export_name)
{
	seq_nr = time(nullptr);

	reconnect();
}

storage_backend_nbd::~storage_backend_nbd()
{
}

storage_backend_nbd * storage_backend_nbd::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string id = yaml_get_string(cfg, "id", "name of this backend");

	std::vector<mirror *> mirrors;
	YAML::Node y_mirrors = cfg["mirrors"];
	for(YAML::const_iterator it = y_mirrors.begin(); it != y_mirrors.end(); it++)
		mirrors.push_back(mirror::load_configuration(it->as<YAML::Node>()));

	std::string export_name = yaml_get_string(cfg, "export-name", "name of the exported storage");

	socket_client *sc = socket_client::load_configuration(cfg["target"]);

	int block_size = yaml_get_int(cfg, "block-size", "block size when transmitting blocks");

	return new storage_backend_nbd(id, sc, export_name, block_size, mirrors);
}

YAML::Node storage_backend_nbd::emit_configuration() const
{
	std::vector<YAML::Node> out_mirrors;
	for(auto m : mirrors)
		out_mirrors.push_back(m->emit_configuration());

	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["target"] = sc->emit_configuration();
	out_cfg["export-name"] = export_name;
	out_cfg["mirrors"] = out_mirrors;
	out_cfg["block-size"] = block_size;

	YAML::Node out;
	out["type"] = "storage-backend-nbd";
	out["cfg"] = out_cfg;

	return out;
}

typedef struct __attribute__((packed)) {
	char     magic_name[8];
	char     magic_opt [8];
	uint16_t flags;
} handshake_server_t;

typedef struct __attribute__((packed)) {
	char     magic_opt[8];
	uint32_t option;
	uint32_t data_len;
	uint8_t  data[0];
} client_option_t;

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint16_t command_flags;
	uint16_t type;
	uint64_t handle;
	uint64_t offset;
	uint32_t length;
	uint8_t  data[0];
} client_command_t;

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t error;
	uint64_t handle;
	uint8_t  data[0];
} server_command_reply_t;

bool storage_backend_nbd::reconnect()
{
	enum sbn_state_t { SBN_connect, SBN_init, SBN_options, SBN_options_recv, SBN_go };
	constexpr const char *const sbn_state_str[] = { "connect", "init", "options", "options_recv", "go" };

	sbn_state_t state = SBN_connect;

	for(;!stop_flag;) {
		dolog(ll_info, "storage_backend_nbd::reconnect(%s): state: \"%s\"", export_name.c_str(), sbn_state_str[state]);

		if (state == SBN_connect) {
			fd = sc->connect();

			if (fd != -1)
				state = SBN_init;
		}
		else if (state == SBN_init) {
			handshake_server_t hs { 0 };

			if (READ(fd, reinterpret_cast<uint8_t *>(&hs), sizeof(hs)) != sizeof(hs)) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): negotiation failed while receiving server message: %s", export_name.c_str(), strerror(errno));
				return false;
			}

			if (memcmp(hs.magic_name, "NBDMAGIC", 8) != 0) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): server send invalid handshake", export_name.c_str());
				return false;
			}

			if (memcmp(hs.magic_opt, "IHAVEOPT", 8) != 0) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): server send invalid magic name", export_name.c_str());
				return false;
			}

			uint32_t client_flags = htonl(NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES);

			if (WRITE(fd, reinterpret_cast<uint8_t *>(&client_flags), sizeof(client_flags)) != sizeof(client_flags)) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): negotiation failed while transmitting client flags: %s", export_name.c_str(), strerror(errno));
				return false;
			}

			state = SBN_options;
		}
		else if (state == SBN_options) {
			size_t command_size = sizeof(client_option_t) + export_name.size();
			client_option_t *co = reinterpret_cast<client_option_t *>(calloc(1, command_size));
			if (!co) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): failed allocating NBD_OPT_EXPORT_NAME request: %s", export_name.c_str(), strerror(errno));
				return false;
			}

			memcpy(co->magic_opt, "IHAVEOPT", 8);
			co->option = htonl(NBD_OPT_EXPORT_NAME);
			co->data_len = htonl(export_name.size());
			memcpy(co->data, export_name.c_str(), export_name.size());

			if (WRITE(fd, reinterpret_cast<uint8_t *>(co), command_size) != ssize_t(command_size)) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): negotiation failed while transmitting option request: %s", export_name.c_str(), strerror(errno));
				return false;
			}

			free(co);

			state = SBN_options_recv;
		}
		else if (state == SBN_options_recv) {
			auto size = receive_uint64(fd);
			if (size.has_value() == false) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): \"size\" receiving error", export_name.c_str(), strerror(errno));
				return false;
			}

			auto flags = receive_uint16(fd);
			if (flags.has_value() == false) {
				dolog(ll_info, "storage_backend_nbd::reconnect(%s): \"flags\" receiving error", export_name.c_str(), strerror(errno));
				return false;
			}

			this->size = size.value();
			dolog(ll_info, "storage_backend_nbd::reconnect(%s): size is %ld bytes", export_name.c_str(), this->size);

			state = SBN_go;
		}
		else if (state == SBN_go) {
			dolog(ll_info, "storage_backend_nbd::reconnect(%s): connection set-up", export_name.c_str(), strerror(errno));
			return true;
		}
		else {
			dolog(ll_info, "storage_backend_nbd::reconnect(%s): unknown internal state %d", export_name.c_str(), state, strerror(errno));
			return false;
		}
	}

	return false;
}

offset_t storage_backend_nbd::get_size() const
{
	return size;
}

bool storage_backend_nbd::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	seq_nr++;

	dolog(ll_debug, "storage_backend_nbd::get_block(%s): requesting block %ld, handle: %x", export_name.c_str(), block_nr, seq_nr);

	bool do_reconnect = false;

	for(;!stop_flag;) {
		if (do_reconnect) {
			do_reconnect = false;
			reconnect();
		}

		client_command_t cc { 0 };
		cc.magic  = htonl(0x25609513);
		cc.type   = htons(NBD_CMD_READ);
		cc.handle = seq_nr;  // htonl not required(!)
		cc.offset = HTONLL(block_nr * block_size);
		cc.length = htonl(block_size);

		if (WRITE(fd, reinterpret_cast<const uint8_t *>(&cc), sizeof cc) != sizeof(cc)) {
			dolog(ll_info, "storage_backend_nbd::get_block(%s): problem transmitting NBD_CMD_READ", export_name.c_str());
			do_reconnect = true;
			continue;
		}

		size_t command_size = sizeof(server_command_reply_t) + block_size;
		server_command_reply_t *scr = reinterpret_cast<server_command_reply_t *>(calloc(1, command_size));
		if (!scr) {
			dolog(ll_info, "storage_backend_nbd::get_block(%s): problem allocating %zu bytes of memory", export_name.c_str(), command_size);
			return false;
		}

		if (READ(fd, reinterpret_cast<uint8_t *>(scr), command_size) != ssize_t(command_size)) {
			dolog(ll_info, "storage_backend_nbd::get_block(%s): problem receiving NBD_CMD_READ reply", export_name.c_str());
			free(scr);
			do_reconnect = true;
			continue;
		}

		if (ntohl(scr->magic) != 0x67446698) {
			free(scr);
			dolog(ll_info, "storage_backend_nbd::get_block(%s): magic (%lx) mismatch", export_name.c_str(), scr->magic);
			do_reconnect = true;
			continue;
		}

		if (scr->handle != cc.handle) {
			free(scr);
			dolog(ll_info, "storage_backend_nbd::get_block(%s): handle (%lx) mismatch (expected %lx)", export_name.c_str(), scr->handle, cc.handle);
			do_reconnect = true;
			continue;
		}

		if (scr->error != 0) {
			dolog(ll_info, "storage_backend_nbd::get_block(%s): NBD server indicated error %d", export_name.c_str(), ntohl(scr->error));
			free(scr);
			return false;
		}

		*data = reinterpret_cast<uint8_t *>(malloc(block_size));
		if (!data) {
			free(scr);
			dolog(ll_info, "storage_backend_nbd::get_block(%s): problem allocating %zu bytes of memory for result", export_name.c_str(), command_size);
			return false;
		}

		memcpy(*data, scr->data, block_size);

		free(scr);

		return true;
	}

	return false;
}

bool storage_backend_nbd::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	seq_nr++;

	dolog(ll_debug, "storage_backend_nbd::put_block(%s): writing block %ld, handle: %x", export_name.c_str(), block_nr, seq_nr);

	bool do_reconnect = false;

	for(;!stop_flag;) {
		if (do_reconnect) {
			do_reconnect = false;
			reconnect();
		}

		size_t command_size = sizeof(client_command_t) + block_size;
		client_command_t *cc = reinterpret_cast<client_command_t *>(calloc(1, command_size));
		if (!cc) {
			dolog(ll_info, "storage_backend_nbd::put_block(%s): problem allocating %zu bytes of memory", export_name.c_str(), command_size);
			return false;
		}

		cc->magic  = htonl(0x25609513);
		cc->type   = htons(NBD_CMD_WRITE);
		cc->handle = seq_nr;  // htonl not required(!)
		cc->offset = HTONLL(block_nr * block_size);
		cc->length = htonl(block_size);
		memcpy(cc->data, data, block_size);

		if (WRITE(fd, reinterpret_cast<const uint8_t *>(cc), command_size) != ssize_t(command_size)) {
			free(cc);
			dolog(ll_info, "storage_backend_nbd::put_block(%s): problem transmitting NBD_CMD_WRITE", export_name.c_str());
			do_reconnect = true;
			continue;
		}

		server_command_reply_t scr { 0 };

		if (READ(fd, reinterpret_cast<uint8_t *>(&scr), sizeof scr) != sizeof(scr)) {
			dolog(ll_info, "storage_backend_nbd::put_block(%s): problem receiving NBD_CMD_WRITE reply", export_name.c_str());
			free(cc);
			do_reconnect = true;
			continue;
		}

		if (scr.handle != cc->handle) {
			dolog(ll_info, "storage_backend_nbd::put_block(%s): handle (%lx) mismatch (expected %lx)", export_name.c_str(), scr.handle, cc->handle);
			free(cc);
			do_reconnect = true;
			continue;
		}

		free(cc);

		if (scr.error != 0) {
			dolog(ll_info, "storage_backend_nbd::put_block(%s): NBD server indicated error %d", export_name.c_str(), ntohl(scr.error));
			return false;
		}

		return true;
	}

	return false;
}

bool storage_backend_nbd::fsync()
{
	seq_nr++;

	dolog(ll_debug, "storage_backend_nbd::put_block(%s): fsyn, handle: %x", export_name.c_str(), seq_nr);
	bool do_reconnect = false;
	
	for(;!stop_flag;) {
		if (do_reconnect) {
			do_reconnect = false;
			reconnect();
		}

		client_command_t cc { 0 };
		cc.magic  = htonl(0x25609513);
		cc.type   = htons(NBD_CMD_FLUSH);
		cc.handle = seq_nr;  // htonl not required(!)
		cc.offset = 0;
		cc.length = 0;

		if (WRITE(fd, reinterpret_cast<uint8_t *>(&cc), sizeof(cc)) != sizeof(cc)) {
			dolog(ll_info, "storage_backend_nbd::fsync(%s): problem transmitting NBD_CMD_FLUSH", export_name.c_str());
			do_reconnect = true;
			continue;
		}

		server_command_reply_t scr { 0 };

		if (READ(fd, reinterpret_cast<uint8_t *>(&scr), sizeof scr) != sizeof(scr)) {
			dolog(ll_info, "storage_backend_nbd::fsync(%s): problem receiving NBD_CMD_FLUSH reply", export_name.c_str());
			do_reconnect = true;
			continue;
		}

		if (scr.handle != cc.handle) {
			dolog(ll_info, "storage_backend_nbd::fsync(%s): handle (%x) mismatch (expected %x)", export_name.c_str(), scr.handle, cc.handle);
			do_reconnect = true;
			continue;
		}

		if (scr.error != 0) {
			dolog(ll_info, "storage_backend_nbd::fsync(%s): NBD server indicated error %d", export_name.c_str(), ntohl(scr.error));
			return false;
		}

		return true;
	}

	return false;
}

bool storage_backend_nbd::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	seq_nr++;

	dolog(ll_debug, "storage_backend_nbd::put_block(%s): %s offset %ld, len %d, handle: %x", export_name.c_str(), trim ? "trim" : "zero", offset, len, seq_nr);

	bool do_reconnect = false;

	for(;!stop_flag;) {
		if (do_reconnect) {
			do_reconnect = false;
			reconnect();
		}

		client_command_t cc { 0 };
		cc.magic  = htonl(0x25609513);
		cc.type   = htons(trim ? NBD_CMD_TRIM : NBD_CMD_WRITE_ZEROES);
		cc.handle = seq_nr;  // htonl not required(!)
		cc.offset = 0;
		cc.length = 0;

		if (WRITE(fd, reinterpret_cast<uint8_t *>(&cc), sizeof(cc)) != sizeof(cc)) {
			dolog(ll_info, "storage_backend_nbd::trim_zero(%s): problem transmitting NBD_CMD_FLUSH", export_name.c_str());
			do_reconnect = true;
			continue;
		}

		server_command_reply_t scr { 0 };

		if (READ(fd, reinterpret_cast<uint8_t *>(&scr), sizeof scr) != sizeof(scr)) {
			dolog(ll_info, "storage_backend_nbd::trim_zero(%s): problem receiving NBD_CMD_FLUSH reply", export_name.c_str());
			do_reconnect = true;
			continue;
		}

		if (scr.handle != cc.handle) {
			dolog(ll_info, "storage_backend_nbd::trim_zero(%s): handle (%x) mismatch (expected %x)", export_name.c_str(), scr.handle, cc.handle);
			do_reconnect = true;
			continue;
		}

		if (scr.error != 0) {
			dolog(ll_info, "storage_backend_nbd::trim_zero(%s): NBD server indicated error %d", export_name.c_str(), ntohl(scr.error));
			return false;
		}

		return true;
	}

	return false;
}
