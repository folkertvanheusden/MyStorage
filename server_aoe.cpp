#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "aoe-common.h"
#include "error.h"
#include "logging.h"
#include "net.h"
#include "server.h"
#include "server_aoe.h"
#include "str.h"
#include "yaml-helpers.h"


aoe::aoe(storage_backend *const sb, const uint16_t major, const uint8_t minor, const std::vector<aoe_path_t> & paths) :
	server(myformat("%d.%d", major, minor)),
	sb(sb), major(major), minor(minor),
	paths(paths)
{
	sb->acquire(this);

	dolog(ll_info, "aoe(%s): %zu network paths configured", id.c_str(), this->paths.size());

	for(auto & path : this->paths) {
		if (open_tun(path.dev_name, &path.fd, &path.mtu_size) == false)
			throw myformat("aoe(%s): failed creating network device \"%s\"", id.c_str(), path.dev_name.c_str());

		dolog(ll_debug, "aoe(%s): local MAC address: %s", id.c_str(), mac_to_str(path.my_mac).c_str());

		path.th = new std::thread([this, &path] { worker_thread(path); });
		if (!path.th)
			throw myformat("aoe(%s): failed starting thread for network device \"%s\"", id.c_str(), path.dev_name.c_str());

		dolog(ll_info, "aoe(%s): started for \"%s\"", id.c_str(), path.dev_name.c_str());
	}
}

aoe::~aoe()
{
	stop();

	for(auto & path : paths) {
		if (path.fd != -1)
			close(path.fd);

		path.th->join();
		delete path.th;
	}

	sb->release(this);
}

YAML::Node aoe::emit_configuration() const
{
	std::vector<YAML::Node> paths_out;

	for(const auto & path : paths) {
		YAML::Node path_cfg;

		path_cfg["dev-name"]    = path.dev_name;
		path_cfg["my-mac"]      = mac_to_str(path.my_mac);
		path_cfg["allowed-mac"] = mac_to_str(path.allowed_mac);
		path_cfg["mtu-size"]    = path.mtu_size;
	}

	YAML::Node out_cfg;
	out_cfg["paths"] = paths_out;

	out_cfg["storage-backend"] = sb->get_id();
	out_cfg["major"] = major;
	out_cfg["minor"] = minor;

	YAML::Node out;
	out["type"] = "AoE";
	out["cfg"] = out_cfg;

	return out;
}

aoe * aoe::load_configuration(const YAML::Node & node, const std::vector<storage_backend *> & storage)
{
	dolog(ll_info, " * aoe::load_configuration");

	const YAML::Node cfg = yaml_get_yaml_node(node, "cfg", "aoe configuration");

	// global AoE settings
	uint16_t major = cfg["major"].as<int>();
	uint8_t minor = cfg["minor"].as<int>();

	std::string sb_name = cfg["storage-backend"].as<std::string>();
	storage_backend *sb = find_storage(storage, sb_name);
	if (!sb) {
		dolog(ll_error, "aoe::load_configuration: storage \"%s\" not known", sb_name.c_str());
		return nullptr;
	}

	// network paths
	std::vector<aoe_path_t> paths;

	const YAML::Node paths_cfg = yaml_get_yaml_node(cfg, "paths", "network paths");
        for(YAML::const_iterator it = paths_cfg.begin(); it != paths_cfg.end(); it++) {
		YAML::Node path_cfg = it->as<YAML::Node>();

		aoe_path_t ap;

		std::string str_my_mac = yaml_get_string(path_cfg, "my-mac", "local MAC address");
		std::string str_allowed_mac = yaml_get_string(path_cfg, "allowed-mac", "MAC address of host that is allowed to use this export, broadcast MAC for everyone");

		if (!str_to_mac(str_my_mac, ap.my_mac)) {
			dolog(ll_error, "aoe::load_configuration: cannot parse MAC-address \"%s\"", str_my_mac.c_str());
			return nullptr;
		}

		if (!str_to_mac(str_allowed_mac, ap.allowed_mac)) {
			dolog(ll_error, "aoe::load_configuration: cannot parse MAC-address \"%s\"", str_allowed_mac.c_str());
			return nullptr;
		}

		ap.dev_name = yaml_get_string(path_cfg, "dev-name", "network device name");
		ap.mtu_size = yaml_get_int(path_cfg, "mtu-size", "mtu size of this network device");

		paths.push_back(ap);

		dolog(ll_info, "aoe::load_configuration: new AoE server on device \"%s\" with storage \"%s\"", ap.dev_name.c_str(), sb_name.c_str());
	}

	return new aoe(sb, major, minor, paths);
}

bool aoe::announce(const aoe_path_t & ap)
{
	dolog(ll_debug, "aoe::announce(%s): announce shelf", id.c_str());

	std::vector<uint8_t> out;

	/// ethernet header
	// DST
	for(int i=0; i<6; i++)
		out.push_back(0xff);
	// SRC
	for(int i=0; i<6; i++)
		out.push_back(ap.my_mac[i]);

	// Ethernet type
	add_uint16(out, AoE_EtherType);

	// AoE header
	out.push_back(0x10 | FlagR);  // version | flags
	out.push_back(0);  // error

	add_uint16(out, major);  // major
	add_uint8(out, minor);  // minor
	out.push_back(CommandInfo);  // command(config)
	for(int i=0; i<4; i++)  // tag
		out.push_back(0x00);

	// Configuration announcement payload
	add_uint16(out, 16);  // buffer count
	add_uint16(out, firmware_version);  // firmware version
	out.push_back(std::min(255, (ap.mtu_size - 36) / 512));    // max number of sectors in 1 command
	out.push_back(0x10 | Ccmd_read);

	add_uint16(out, 0);  // configuration length

	if (write(ap.fd, out.data(), out.size()) != ssize_t(out.size())) {
		dolog(ll_error, "aoe::operator(%s): failed to tansmit Ethernet frame: %s (announce)", id.c_str(), strerror(errno));
		return false;
	}

	return true;
}

void aoe::worker_thread(aoe_path_t & ap)
{
	std::atomic_bool local_stop_flag { false };

	std::thread announcer([this, ap, &local_stop_flag] {
			for(;!local_stop_flag;) {
				announce(ap);

				for(int i=0; i<5 && !local_stop_flag; i++)
					sleep(1);
			}
		});

	struct pollfd fds[] = { { ap.fd, POLLIN, 0 } };

	for(;!stop_flag;) {
		int rc = poll(fds, 1, 250);
		if (rc == 0)
		       	continue;

		if (rc == -1) {
			dolog(ll_error, "aoe::operator(%s): poll call failed: %s", id.c_str(), strerror(errno));
			break;
		}

		// enough for jumbo frames
		uint8_t frame[65536];

		int size = read(ap.fd, (char *)frame, sizeof frame);
		if (size == -1) {
			dolog(ll_error, "aoe::operator(%s): failed to retrieve frame from virtual Ethernet interface", id.c_str());
			break;
		}

		if (frame[12] != 0x88 || frame[13] != 0xa2) {  // verify ethertype
			dolog(ll_debug, "aoe::operator(%s): ignoring frame with ethertype %02x%02x", id.c_str(), frame[12], frame[13]);
			continue;
		}

		// check destination mac address
		constexpr uint8_t bc_mac[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		if (memcmp(&frame[0], ap.my_mac, 6) != 0 && memcmp(&frame[0], bc_mac, 6) != 0) {
			dolog(ll_debug, "aoe::operator(%s): ignoring frame not for us %02x%02x%02x%02x%02x%02x", id.c_str(), mac_to_str(&frame[0]).c_str());
			continue;
		}

		// check source map
		if (memcmp(ap.allowed_mac, bc_mac, 6) != 0 && memcmp(&frame[6], ap.allowed_mac, 6) != 0) {
			dolog(ll_debug, "aoe::operator(%s): ignoring frame from a prohibited MAC address %s", id.c_str(), mac_to_str(&frame[6]).c_str());
			continue;
		}

		const uint8_t *const p = frame;
		const uint8_t command = p[19];

		std::vector<uint8_t> out(frame, &frame[size]);
		// new DST
		for(int i=0; i<6; i++)
			out.at(i) = frame[i + 6];
		// new SRC
		for(int i=0; i<6; i++)
			out.at(i + 6) = ap.my_mac[i];

		out.at(16) = major >> 8;
		out.at(17) = major;
		out.at(18) = minor;

		out.at(14) = (out.at(14) & 0xf0) | FlagR;

		dolog(ll_debug, "aoe::operator(%s): command %d", id.c_str(), command);

		if (command == CommandInfo) {  // query configuration information
			const uint8_t sub_command = out.at(29) & 0x0f;
			const uint16_t sc_data_len = (out.at(30) << 8) | out.at(31);

			dolog(ll_debug, "aoe::operator(%s): CommandInfo, sub-command %d", id.c_str(), sub_command);

			out.at(24) = 0;  // buffer count
			out.at(25) = 16;

			out.at(26) = firmware_version >> 8;
			out.at(27) = firmware_version & 255;

			out.at(28) = std::min(255, (ap.mtu_size - 36) / 512);  // max number of sectors in 1 command
			dolog(ll_debug, "aoe::operator(%s): max. nr. of sectors per command: %d", id.c_str(), out.at(28));

			out.at(29) = 0x10 | sub_command;  // version & sub command

			bool respond = true;

			if (sub_command == Ccmd_read) {
				out.at(30) = 0; // sizeof(configuration) >> 8;
				out.at(31) = 0; // sizeof(configuration) & 255;

				out.resize(32);
			}
			else if (sub_command == Ccmd_test) {
				if (sc_data_len != sizeof(ap.configuration) || memcmp(ap.configuration, out.data() + 32, sc_data_len) != 0)
					respond = false;
			}
			else if (sub_command == Ccmd_test_prefix) {
				if (sc_data_len > sizeof(ap.configuration) || memcmp(ap.configuration, out.data() + 32, sc_data_len) != 0)
					respond = false;
			}
			else if (sub_command == Ccmd_set_config || sub_command == Ccmd_force_set_config) {
				if (sc_data_len != sizeof(ap.configuration)) {
					out.at(14) |= FlagE;
					out.at(15) = E_ConfigErr;
				}
				else {
					memcpy(ap.configuration, out.data() + 32, sc_data_len);
				}
			}
			else {
				dolog(ll_warning, "aoe::operator(%s): sub-command %d not understood", id.c_str(), sub_command);
			}

			if (respond) {
				dolog(ll_debug, "aoe::operator(%s): send response to sub-command %d (%zu bytes)", id.c_str(), sub_command, out.size());

				if (write(ap.fd, out.data(), out.size()) != ssize_t(out.size())) {
					dolog(ll_error, "aoe::operator(%s): failed to tansmit Ethernet frame: %s (Info)", id.c_str(), strerror(errno));
					break;
				}
			}
		}
		else if (command == CommandATA) {
			uint64_t lba = uint64_t(out[28]) | (uint64_t(out[29]) << 8) | (uint64_t(out[30]) << 16) | (uint64_t(out[31]) << 24) | (uint64_t(out[32]) << 32) | (uint64_t(out[33]) << 40);

			out.at(24) = 0;  // flags

			dolog(ll_debug, "aoe::operator(%s): CommandATA, lba: %ld, sector count: %d, cmd: %02x", id.c_str(), lba, out[26], out[27]);

			if (out[27] == 0xec) {  // identify drive
				dolog(ll_debug, "aoe::operator(%s): CommandATA: IdentifyDrive", id.c_str());

				out[26] = 0;  // sector count

				out[27] = 64;  // DRDY set

				uint16_t response[256] { 0 };

				response[5] = 512;  // bytes per sector

				memset(reinterpret_cast<char *>(&response[27]), ' ', 20);
				memcpy(reinterpret_cast<char *>(&response[27]), "MyStorage", 9);  // model number

				response[49] = 1 << 9;  // LBA supported

				response[47] = 0x8000;  // as per spec
				response[49] = 0x0300;  // as per spec
				response[50] = 0x4000;  // capabilities
				response[83] = 1 << 10 /* LBA48 */;
				response[84] = 0x4000;  // from vblade
				response[86] = 1 << 10 /* LBA48 */;
				response[87] = 0x4000;  // from vblade
				response[93] = 0x400b;  // from vblade

				uint64_t sectors = sb->get_size() / 512;
				dolog(ll_debug, "aoe::operator(%s): CommandATA, IdentifyDrive: backend is %d sectors", id.c_str(), sectors);

				// LBA48
				response[69] = 0;  // if bit 3 is 0, then this is 48 bit, else 32

				response[100] = sectors;
				response[101] = sectors >> 16;
				response[102] = sectors >> 32;
				response[103] = sectors >> 48;

				out.resize(36);
				for(int i=0; i<256; i++)
					add_uint16(out, htons(response[i]));

				if (write(ap.fd, out.data(), out.size()) != ssize_t(out.size())) {
					dolog(ll_error, "aoe::operator(%s): failed to transmit Ethernet frame: %s (Identify)", id.c_str(), strerror(errno));
					break;
				}
			}
			else if (out[27] == 0x20 || out[27] == 0x24) {  // read sectors, max 28bit/48bit
				lba &= out[27] == 0x20 ? 0x0fffffff : 0x0000ffffffffffffll;

				dolog(ll_debug, "aoe::operator(%s): CommandATA: ReadSector(s) (%d) from LBA %llu", id.c_str(), out[26], lba);

				int err = 0;
				block *b = nullptr;
			 	sb->get_data(lba * 512, out[26] * 512, &b, &err);  // TODO range check

				if (err) {
					dolog(ll_error, "aoe::operator(%s): failed to retrieve data from storage backend: %s", id.c_str(), strerror(err));
					// TODO send error back
				}
				else {
					out[27] = 64;  // DRDY set
					out.resize(36);  // move any extra data

					out.resize(36 + b->get_size());  // make room
					memcpy(out.data() + 36, b->get_data(), b->get_size());

					if (write(ap.fd, out.data(), out.size()) != ssize_t(out.size())) {
						dolog(ll_error, "aoe::operator(%s): failed to transmit Ethernet frame: %s (%zu bytes, ReadSector)", id.c_str(), strerror(errno), out.size());
						break;
					}
				}

				delete b;
			}
			else if (out[27] == 0x30 || out[27] == 0x34) {  // write sectors, max 28bit/48bit
				lba &= out[27] == 0x30 ? 0x0fffffff : 0x0000ffffffffffffll;

				dolog(ll_debug, "aoe::operator(%s): CommandATA: WriteSector(s) to LBA %llu", id.c_str(), lba);

				block b(&out[36], out[26] * 512, false);  // TODO range check

				int err = 0;
				sb->put_data(lba * 512, b, &err);

				if (err) {
					dolog(ll_error, "aoe::operator(%s): failed to write data to storage backend: %s", id.c_str(), strerror(err));
					// TODO send error back
				}
				else {
					out[27] = 64;  // DRDY set
					out.resize(36);

					if (write(ap.fd, out.data(), out.size()) != ssize_t(out.size())) {
						dolog(ll_error, "aoe::operator(%s): failed to transmit Ethernet frame: %s (%zu bytes, WriteSector)", id.c_str(), strerror(errno), out.size());
						break;
					}
				}
			}
			else {
				dolog(ll_warning, "aoe::operator(%s): ata command %02x not supported", id.c_str(), out[27]);
			}
		}
	}

	local_stop_flag = true;
	announcer.join();

	dolog(ll_error, "aoe::operator(%s): thread terminates", id.c_str());
}
