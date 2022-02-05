#include <assert.h>
#include <poll.h>
#include <string>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>

#include "aoe-common.h"
#include "error.h"
#include "logging.h"
#include "mirror.h"
#include "net.h"
#include "storage_backend_aoe.h"
#include "str.h"
#include "yaml-helpers.h"


storage_backend_aoe::storage_backend_aoe(const std::string & id, const std::vector<mirror *> & mirrors, const std::string & dev_name, const uint8_t my_mac[6], const uint16_t major, const uint8_t minor, const int mtu_size, const int block_size) :
	storage_backend(id, block_size, mirrors),
	dev_name(dev_name),
	major(major), minor(minor)
{
	memcpy(this->my_mac, my_mac, 6);

	connection.mtu_size = mtu_size;

	if (!verify_mirror_sizes())
		throw myformat("storage_backend_aoe(%s): mirrors sanity check failed", id.c_str());

	if (!connect())
		dolog(ll_warning, "storage_backend_aoe(%s): failed to connect to AoE target %d:%d (via interface \"%s\")", id.c_str(), major, minor, dev_name.c_str());
}

storage_backend_aoe::~storage_backend_aoe()
{
	if (connection.fd != -1)
		close(connection.fd);
}

YAML::Node storage_backend_aoe::emit_configuration() const
{
	std::vector<YAML::Node> out_mirrors;
	for(auto m : mirrors)
		out_mirrors.push_back(m->emit_configuration());

	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["mirrors"] = out_mirrors;
	out_cfg["dev-name"] = dev_name;
	out_cfg["major"] = major;
	out_cfg["minor"] = minor;
	out_cfg["mtu-size"] = connection.mtu_size;
	out_cfg["my-mac"] = myformat("%02x:%02x:%02x:%02x:%02x:%02x", my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);;
	out_cfg["block-size"] = block_size;

	YAML::Node out;
	out["type"] = "storage-backend-aoe";
	out["cfg"] = out_cfg;

	return out;
}

storage_backend_aoe * storage_backend_aoe::load_configuration(const YAML::Node & node, const std::optional<uint64_t> size, std::optional<int> block_size)
{
	dolog(ll_info, " * socket_backend_aoe::load_configuration");

	if (size.has_value())
		dolog(ll_debug, "storage_backend_aoe::load_configuration: cannot override size setting");

	const YAML::Node cfg = node["cfg"];

	std::string name = cfg["id"].as<std::string>();

	std::vector<mirror *> mirrors;
	YAML::Node y_mirrors = cfg["mirrors"];
	for(YAML::const_iterator it = y_mirrors.begin(); it != y_mirrors.end(); it++)
		mirrors.push_back(mirror::load_configuration(it->as<YAML::Node>()));

	std::string dev_name = cfg["dev-name"].as<std::string>();
	uint16_t major = cfg["major"].as<uint16_t>();
	uint8_t minor = cfg["minor"].as<uint8_t>();
	int mtu_size = cfg["mtu-size"].as<int>();
	int final_block_size = block_size.has_value() ? block_size.value() : yaml_get_int(cfg, "block-size", "block size");

	std::string mac = cfg["my-mac"].as<std::string>();

	uint8_t my_mac[6] = { 0 };
	if (!str_to_mac(mac, my_mac)) {
		dolog(ll_error, "storage_backend_aoe::load_configuration: cannot parse MAC-address \"%s\"", mac.c_str());
		return nullptr;
	}

	return new storage_backend_aoe(name, mirrors, dev_name, my_mac, major, minor, mtu_size, final_block_size);
}

typedef enum { ACS_discover, ACS_discover_sent, ACS_identify, ACS_identify_sent, ACS_running, ACS_end } aoe_connect_state_t;
constexpr const char *const ACSstrings[] = { "discover", "discover sent", "identify", "idenfity sent", "running", "end" };

static std::string get_idi_string(const uint16_t *const p, const size_t len)
{
	std::string out;

	for(size_t i=0; i<len; i++) {
		uint16_t c2 = ntohs(p[i]);

		if ((c2 & 255) == 0)
			break;

		out += char(c2);

		if ((c2 >> 8) == 0)
			break;

		out += char(c2 >> 8);
	}

	return out;
}

static uint64_t get_idi_value(const uint16_t *const p, const int len)
{
	const uint8_t *const p8 = reinterpret_cast<const uint8_t *>(p);

	uint64_t out = 0;
	for(int i=len * 2 - 1; i >= 0; i--) {
		out <<= 8;
		out |= uint64_t(p8[i]);
	}

	return out;
}

bool storage_backend_aoe::can_do_multiple_blocks() const
{
	return false;
}

bool storage_backend_aoe::connect() const
{
	if (connection.fd != -1) {
		close(connection.fd);
		connection.fd = -1;
	}

	if (open_tun(dev_name, &connection.fd, &connection.mtu_size) == false) {
		dolog(ll_warning, "storage_backend_aoe::connect(%s): failed to setup interface \"%s\"", id.c_str(), dev_name.c_str());
		return false;
	}

	aoe_connect_state_t state = ACS_discover;
	time_t state_since = time(nullptr);

	uint32_t tag = rand();

	uint8_t recv_buffer[65536] { 0 };  // maximum Ethernet frame size

	for(;state != ACS_end;) {
		dolog(ll_debug, "storage_backend_aoe::connect(%s): connect state \"%s\" (%d)", id.c_str(), ACSstrings[state], state);

		if (state == ACS_discover) {
			aoe_configuration_t ac { 0 };

			memset(ac.aeh.dst, 0xff, sizeof ac.aeh.dst);
			memcpy(ac.aeh.src, my_mac, sizeof ac.aeh.src);
			ac.aeh.type = htons(AoE_EtherType);

			ac.aeh.flags   = 0x10 | 0;
			ac.aeh.error   = 0;
			ac.aeh.major   = 0xffff;
			ac.aeh.minor   = 0xff;
			ac.aeh.command = CommandInfo;
			ac.aeh.tag     = 0;

			ac.ver_cmd     = Ccmd_read;
			ac.len         = 1024;
			memset(ac.data, 0xed, ac.len);

			if (write(connection.fd, &ac, sizeof ac) != sizeof(ac)) {
				dolog(ll_warning, "storage_backend_aoe::connect(%s): failed to transmit discover packet", id.c_str());
				sleep(1);
			}
			else {
				state = ACS_discover_sent;
				state_since = time(nullptr);
			}
		}
		else if (state == ACS_discover_sent) {
			int rc = 0;
			bool read_error = false, timeout = false;
			wait_for_packet(recv_buffer, sizeof recv_buffer, &read_error, &timeout, &rc);

			if (read_error) {
				dolog(ll_error, "storage_backend_aoe::connect(%s): problem receiving (%s)", id.c_str(), strerror(errno));
				break;
			}

			if (!timeout) {
				const aoe_configuration_t *ac = reinterpret_cast<const aoe_configuration_t *>(recv_buffer);

				if (time(nullptr) - state_since >= 1) {
					state = ACS_discover;
					state_since = time(nullptr);
				}

				if (ac->aeh.type != htons(AoE_EtherType)) {
					dolog(ll_debug, "storage_backend_aoe::connect(%s): ignoring Ethernet frame of type %04x", id.c_str(), ntohs(ac->aeh.type));
					continue;
				}

				dolog(ll_debug, "storage_backend_aoe::connect(%s): packet from %02x:%02x:%02x:%02x:%02x:%02x", 
						id.c_str(),
						ac->aeh.src[0], ac->aeh.src[1], ac->aeh.src[2], ac->aeh.src[3], ac->aeh.src[4], ac->aeh.src[5]);

				dolog(ll_debug, "storage_backend_aoe::connect(%s): buffers: %d, sectors: %d, firmware: %04x, ver/cmd: %02x, major: %d, minor: %d", id.c_str(), ntohs(ac->n_buffers), ac->n_sectors, ntohs(ac->firmware_version), ac->ver_cmd, ntohs(ac->aeh.major), ac->aeh.minor);

				if (ntohs(ac->aeh.major) != major || ac->aeh.minor != minor) {
					dolog(ll_debug, "storage_backend_aoe::connect(%s): ignoring %d:%d", id.c_str(), ac->aeh.major, ac->aeh.minor);
					continue;
				}

				memcpy(connection.tgt_mac, ac->aeh.src, 6);

				state = ACS_identify;
				state_since = time(nullptr);
			}
		}
		else if (state == ACS_identify) {
			aoe_ata_t aa { 0 };

			memset(aa.aeh.dst, 0xff, sizeof aa.aeh.dst);

			dolog(ll_debug, "storage_backend_aoe::connect(%s): request Identification from %d.%d",
					id.c_str(),
					major, minor);

			memcpy(aa.aeh.src, my_mac, sizeof aa.aeh.src);
			aa.aeh.type = htons(AoE_EtherType);

			aa.aeh.flags   = 0;
			aa.aeh.error   = 0;
			aa.aeh.major   = htons(major);
			aa.aeh.minor   = minor;
			aa.aeh.command = CommandATA;
			aa.aeh.tag     = tag;

			aa.aflags    = 64;  // LBA48 extended command
			aa.command   = 0xec;  // identify
			aa.n_sectors = 1;

			if (write(connection.fd, &aa, sizeof aa) != sizeof(aa)) {
				dolog(ll_warning, "storage_backend_aoe::connect(%s): failed to transmit identify packet", id.c_str());
				sleep(1);
			}
			else {
				state = ACS_identify_sent;
				state_since = time(nullptr);
			}
		}
		else if (state == ACS_identify_sent) {
			int rc = 0;
			bool read_error = false, timeout = false;
			wait_for_packet(recv_buffer, sizeof recv_buffer, &read_error, &timeout, &rc);

			if (read_error) {
				dolog(ll_error, "storage_backend_aoe::connect(%s): problem receiving (%s)", id.c_str(), strerror(errno));
				break;
			}

			if (!timeout) {
				const aoe_ata_t *aa = reinterpret_cast<const aoe_ata_t *>(recv_buffer);

				if (time(nullptr) - state_since >= 1) {
					state = ACS_identify;
					state_since = time(nullptr);
				}

				if (aa->aeh.type != htons(AoE_EtherType)) {
					dolog(ll_debug, "storage_backend_aoe::connect(%s): ignoring Ethernet frame of type %04x", id.c_str(), ntohs(aa->aeh.type));
					continue;
				}

				if ((aa->aeh.flags & 4) || aa->aeh.error || aa->error) {
					dolog(ll_debug, "storage_backend_aoe::connect(%s): error message from storage: %02x / %02x", id.c_str(), aa->aeh.error, aa->error);
					break;
				}

				uint16_t parameters[256] { 0 };
				memcpy(parameters, aa->data, std::min(int(256 * sizeof(uint16_t)), rc - 36));

				std::string serial_number = get_idi_string(&parameters[10], 20 / 2);
				std::string firmware_rev  = get_idi_string(&parameters[23], 8 / 2);
				std::string model         = get_idi_string(&parameters[27], 40 / 2);

				dolog(ll_debug, "storage_backend_aoe::connect(%s): device model: \"%s\", firmware revision: \"%s\", serial number: \"%s\"", id.c_str(), model.c_str(), firmware_rev.c_str(), serial_number.c_str());


				if (parameters[49] & (1 << 9)) {  // LBA supported
					connection.size = get_idi_value(&parameters[100], 6 / 2) * 512;
				}
				else {
					connection.size = offset_t(parameters[1]) * offset_t(parameters[3]) * offset_t(parameters[4]) * offset_t(parameters[5]);
				}

				dolog(ll_debug, "storage_backend_aoe::connect(%s): size: %llu bytes", id.c_str(), connection.size);

				state = ACS_running;
				state_since = time(nullptr);
			}
		}
		else if (state == ACS_running) {
			break;
		}
		else {
			error_exit(false, "storage_backend_aoe::connect(%s): connect state %d invalid", id.c_str(), state);
		}
	}

	return state == ACS_running;
}

offset_t storage_backend_aoe::get_size() const
{
	if (connection.fd == -1)
		connect();

	if (connection.fd != -1)
		return connection.size;

	dolog(ll_warning, "storage_backend_aoe::get_size(%s): not connected to AoE target", id.c_str());

	return 0;
}

bool storage_backend_aoe::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	*data = reinterpret_cast<uint8_t *>(malloc(block_size));
	if (!*data) {
		dolog(ll_error, "storage_backend_aoe::get_block(%s): cannot allocate %d bytes of memory", id.c_str(), block_size);
		return false;
	}

	block_nr_t work_block_nr = block_nr;
	uint32_t work_size       = block_size;
	uint8_t  *work_buffer    = *data;

	aoe_ata_t aa { 0 };

	memset(aa.aeh.dst, 0xff, sizeof aa.aeh.dst);
	memcpy(aa.aeh.src, my_mac, sizeof aa.aeh.src);
	aa.aeh.type = htons(AoE_EtherType);

	aa.aeh.flags   = 0;
	aa.aeh.error   = 0;
	aa.aeh.major   = htons(major);
	aa.aeh.minor   = minor;
	aa.aeh.command = CommandATA;

	aa.aflags    = 64;  // LBA48 extended command
	aa.command   = 0x24;  // read sector, lba48
	aa.n_sectors = 1;

	uint8_t recv_buffer[65536] { 0 };
	const aoe_ata_t *const aa_rb = reinterpret_cast<const aoe_ata_t *>(recv_buffer);

	int err = 0;

	while(work_size > 0) {
		aa.aeh.tag = rand();

		aa.lba[0] = work_block_nr;
		aa.lba[1] = work_block_nr >> 8;
		aa.lba[2] = work_block_nr >> 16;
		aa.lba[3] = work_block_nr >> 24;
		aa.lba[4] = work_block_nr >> 32;
		aa.lba[5] = work_block_nr >> 40;

		if (do_ata_command(&aa, sizeof aa, recv_buffer, sizeof recv_buffer, &err) == false) {
			dolog(ll_error, "storage_backend_aoe::get_data(%s): device refused ATAPI command", id.c_str());
			break;
		}

		// add to buffer
		memcpy(work_buffer, aa_rb->data, 512);

		work_size -= 512;
		work_block_nr++;
		work_buffer += 512;
	}

	if (err)
		free(*data);
	else
		assert(work_buffer - *data == block_size);

	return err == 0;
}

bool storage_backend_aoe::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	int err = 0;
	block_nr_t work_block_nr   = block_nr;
	uint32_t work_size         = block_size;
	const uint8_t *work_buffer = data;

	const size_t send_size = sizeof(aoe_ata_t) + 512;
	aoe_ata_t *aa = reinterpret_cast<aoe_ata_t *>(calloc(1, send_size));

	memset(aa->aeh.dst, 0xff, sizeof aa->aeh.dst);
	memcpy(aa->aeh.src, my_mac, sizeof aa->aeh.src);
	aa->aeh.type = htons(AoE_EtherType);

	aa->aeh.flags   = 64 | 2 | 1;  // A, W -> async / write - a call to fsync() should make certain data is on disk
	aa->aeh.error   = 0;
	aa->aeh.major   = htons(major);
	aa->aeh.minor   = minor;
	aa->aeh.command = CommandATA;

	aa->aflags    = 64;  // LBA48 extended command
	aa->command   = 0x34;  // write sector, lba48
	aa->n_sectors = 1;

	uint8_t recv_buffer[65536] { 0 };

	while(work_size > 0) {
		aa->aeh.tag = rand();

		aa->lba[0] = work_block_nr;
		aa->lba[1] = work_block_nr >> 8;
		aa->lba[2] = work_block_nr >> 16;
		aa->lba[3] = work_block_nr >> 24;
		aa->lba[4] = work_block_nr >> 32;
		aa->lba[5] = work_block_nr >> 40;

		memcpy(aa->data, work_buffer, 512);

		if (do_ata_command(aa, send_size, recv_buffer, sizeof recv_buffer, &err) == false) {
			dolog(ll_debug, "storage_backend_aoe::put_block(%s): device refused ATAPI command", id.c_str());
			break;
		}

		work_size -= 512;
		work_block_nr++;
		work_buffer += 512;
	}

	free(aa);

	return err == 0;
}

bool storage_backend_aoe::fsync()
{
	dolog(ll_debug, "storage_backend_aoe::fsync(%s): flush cache", id.c_str());

	aoe_ata_t aa { 0 };

	memset(aa.aeh.dst, 0xff, sizeof aa.aeh.dst);
	memcpy(aa.aeh.src, my_mac, sizeof aa.aeh.src);
	aa.aeh.type = htons(AoE_EtherType);

	aa.aeh.flags   = 64;
	aa.aeh.error   = 0;
	aa.aeh.major   = htons(major);
	aa.aeh.minor   = minor;
	aa.aeh.command = CommandATA;

	aa.aflags    = 64;  // LBA48 extended command
	aa.command   = 0xe7;  // flush cache
	aa.n_sectors = 0;

	uint8_t recv_buffer[65536] { 0 };

	int err = 0;
	if (do_ata_command(&aa, sizeof aa, recv_buffer, sizeof recv_buffer, &err) == false) {
		dolog(ll_debug, "storage_backend_aoe::fsync(%s): device refused ATAPI command", id.c_str());
		return false;
	}

	if (do_sync_mirrors() == false) {
		dolog(ll_error, "storage_backend_aoe::fsync(%s): failed to sync data to mirror(s)", id.c_str());
		return false;
	}

	return true;
}

void storage_backend_aoe::wait_for_packet(uint8_t *const recv_buffer, const int rb_size, bool *const error, bool *const timeout, int *const n_data) const
{
	struct pollfd fds[] = { { connection.fd, POLLIN, 0 } };

	*timeout = *error = false;
	*n_data = 0;

	int rc = poll(fds, 1, 500);
	if (rc == -1) {
		dolog(ll_warning, "storage_backend_aoe(%s)::wait_for_packet: poll error: %s", id.c_str(), strerror(errno));
		*error = true;
		return;
	}

	if (rc) {
		*n_data = read(connection.fd, recv_buffer, rb_size);

		if (*n_data == -1) {
			dolog(ll_warning, "storage_backend_aoe(%s)::wait_for_packet: read error: %s", id.c_str(), strerror(errno));
			*error = true;
			return;
		}
	}
	else {
		*timeout = true;
	}
}

bool storage_backend_aoe::do_ata_command(aoe_ata_t *const aa_in, const int len, uint8_t *const recv_buffer, const int rb_size, int *const err)
{
	bool send = true;

	for(;;) {
		if (send && write(connection.fd, aa_in, len) != len) {
			dolog(ll_warning, "storage_backend_aoe(%s)::do_ata_command: failed to transmit msg", id.c_str());
			*err = EIO;
			return false;
		}

		send = false;

		int n = 0;

		// wait 500ms for a reply, else: resend
		bool read_error = false, timeout = false;
		wait_for_packet(recv_buffer, rb_size, &read_error, &timeout, &n);

		if (read_error) {
			dolog(ll_debug, "storage_backend_aoe(%s)::do_ata_command: problem receiving", id.c_str());
			*err = EIO;
			return false;
		}

		if (timeout) {
			send = true;
			continue;
		}

		if (n < 36) {
			dolog(ll_debug, "storage_backend_aoe(%s)::do_ata_command: packet too small", id.c_str());
			continue;
		}

		const aoe_ata_t *const aa = reinterpret_cast<aoe_ata_t *>(recv_buffer);

		if (aa->aeh.type != htons(AoE_EtherType)) {
			dolog(ll_debug, "storage_backend_aoe(%s)::do_ata_command: ignoring Ethernet frame of type %04x", id.c_str(), ntohs(aa->aeh.type));
			continue;
		}

		if ((aa->aeh.flags & 4) || aa->aeh.error || aa->error) {
			dolog(ll_debug, "storage_backend_aoe(%s)::do_ata_command: error message from storage: %02x / %02x", id.c_str(), aa->aeh.error, aa->error);
			*err = EIO;
			return false;
		}

		if (ntohs(aa->aeh.major) != major || aa->aeh.minor != minor) {
			dolog(ll_debug, "storage_backend_aoe::do_ata_command(%s): ignoring message from %d:%d", id.c_str(), ntohs(aa->aeh.major), aa->aeh.minor);
			continue;
		}

		if (aa->aeh.tag != aa_in->aeh.tag) {
			dolog(ll_debug, "storage_backend_aoe::do_ata_command(%s): tag mismatch (expected: %x, got: %x)", id.c_str(), aa_in->aeh.tag, aa->aeh.tag);
			continue;
		}

		if ((aa->aeh.error & FlagE) || aa->error) {
			dolog(ll_warning, "storage_backend_aoe::do_ata_command(%s): server indicated error (%d|%d)", id.c_str(), aa->aeh.error, aa->error);
			*err = EIO;
			return false;
		}

		// OK!

		break;
	}

	return true;
}

bool storage_backend_aoe::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

	if (offset & 511) {
		dolog(ll_warning, "storage_backend_aoe::trim_zero(%s): offset must be multiple of 512 bytes (1 sector)", id.c_str());
		*err = EIO;
		return false;
	}

	if (len & 511) {
		dolog(ll_warning, "storage_backend_aoe::trim_zero(%s): length must be multiple of 512 bytes (1 sector)", id.c_str());
		*err = EIO;
		return false;
	}

	if (trim) {
		dolog(ll_debug, "storage_backend_aoe::trim_zero(%s): trim", id.c_str());

		const size_t send_size = sizeof(aoe_ata_t) + 512;
		aoe_ata_t *aa = reinterpret_cast<aoe_ata_t *>(calloc(1, send_size));

		memset(aa->aeh.dst, 0xff, sizeof aa->aeh.dst);
		memcpy(aa->aeh.src, my_mac, sizeof aa->aeh.src);
		aa->aeh.type = htons(AoE_EtherType);

		aa->aeh.flags   = 64;
		aa->aeh.error   = 0;
		aa->aeh.major   = htons(major);
		aa->aeh.minor   = minor;
		aa->aeh.command = CommandATA;

		aa->aflags    = 64;  // LBA48 extended command
		aa->command   = 0x06;  // data set management (trim)
		aa->n_sectors = 0;

		offset_t work_offset = offset;
		uint32_t work_len = len;

		while(work_len > 0) {
			uint64_t entry = (uint64_t(1) << 48) | (work_offset / 512);

			aa->data[0] = entry;
			aa->data[1] = entry >> 8;
			aa->data[2] = entry >> 16;
			aa->data[3] = entry >> 24;
			aa->data[4] = entry >> 32;
			aa->data[5] = entry >> 40;
			aa->data[6] = entry >> 48;
			aa->data[7] = entry >> 56;

			uint8_t recv_buffer[65536] { 0 };
			aa->aeh.tag = rand();

			if (do_ata_command(aa, send_size, recv_buffer, sizeof recv_buffer, err) == false) {
				dolog(ll_debug, "storage_backend_aoe::trim_zero(%s): device refused ATAPI command", id.c_str());
				free(aa);
				return false;
			}

			work_len -= 512;
			work_offset += 512;
		}

		if (do_mirror_trim_zero(offset, len, trim) == false) {
			dolog(ll_error, "storage_backend_aoe::trim_zero(%s): failed to send to mirror(s)", id.c_str());
			free(aa);
			return false;
		}

		free(aa);

		return true;
	}
	else {
		dolog(ll_debug, "storage_backend_aoe::trim_zero(%s): write %d zeros to offset %lld", id.c_str(), len, offset);

		uint8_t *data0x00 = reinterpret_cast<uint8_t *>(calloc(1, len));

		put_data(offset, block(data0x00, len), err);

		if (do_mirror_trim_zero(offset, len, trim) == false) {
			dolog(ll_error, "storage_backend_aoe::trim_zero(%s): failed to send to mirror(s)", id.c_str());
			return false;
		}

		return *err == 0;
	}
}
