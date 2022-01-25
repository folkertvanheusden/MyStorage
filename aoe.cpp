#include <fcntl.h>
#include <string.h>
#include <thread>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "aoe.h"
#include "aoe-generic.h"
#include "error.h"
#include "logging.h"
#include "net.h"


aoe::aoe(const std::string & dev_name, storage_backend *const storage_backend, const uint8_t my_mac[6]) : sb(storage_backend)
{
	fd = open_tun(dev_name);
	if (fd == -1)
		error_exit(false, "aoe: failed creating network device \"%s\"", dev_name.c_str());

	memcpy(this->my_mac, my_mac, 6);

	th = new std::thread(std::ref(*this));
}

aoe::~aoe()
{
}

bool aoe::announce()
{
	dolog(ll_debug, "aoe::announce: announce shelf");

	std::vector<uint8_t> out;

	/// ethernet header
	for(int i=0; i<6; i++)
		out.push_back(0xff);
	// SRC
	for(int i=0; i<6; i++)
		out.push_back(my_mac[i]);

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
	out.push_back(2);    // max number of sectors in 1 command
	out.push_back(0x10 | Ccmd_read);

	add_uint16(out, 0);  // configuration length

	if (write(fd, out.data(), out.size()) != ssize_t(out.size())) {
		dolog(ll_error, "aoe::operator: failed to tansmit Ethernet frame: %s", strerror(errno));
		return false;
	}

	return true;
}

void aoe::operator()()
{
	std::thread announcer([this] {
				for(;;) {
					sleep(2);

					announce();
				}
			});
	announcer.detach();

	for(;;) {
		// enough for jumbo frames
		uint8_t frame[65536];

		int size = read(fd, (char *)frame, sizeof frame);
		if (size == -1) {
			dolog(ll_error, "aoe::operator: failed to retrieve frame from virtual Ethernet interface");
			break;
		}

		if (frame[12] != 0x88 || frame[13] != 0xa2) {  // verify ethertype
			dolog(ll_debug, "aoe::operator: ignoring frame with ethertype %02x%02x", frame[12], frame[13]);
			continue;
		}

		// check mac address
		constexpr uint8_t bc_mac[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		if (memcmp(&frame[0], my_mac, 6) != 0 && memcmp(&frame[0], bc_mac, 6) != 0) {
			dolog(ll_debug, "aoe::operator: ignoring frame not for us %02x%02x%02x%02x%02x%02x", frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
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
			out.at(i + 6) = my_mac[i];

		out.at(16) = major >> 8;
		out.at(17) = major;
		out.at(18) = minor;

		out.at(14) = (out.at(14) & 0xf0) | FlagR;

		dolog(ll_debug, "aoe::operator: command %d", command);

		if (command == CommandInfo) {  // query configuration information
			const uint8_t sub_command = out.at(29) & 0x0f;
			const uint16_t sc_data_len = (out.at(30) << 8) | out.at(31);

			dolog(ll_debug, "aoe::operator: CommandInfo, sub-command %d", sub_command);

			out.at(24) = 0;  // buffer count
			out.at(25) = 16;

			out.at(26) = firmware_version >> 8;
			out.at(27) = firmware_version & 255;

			out.at(28) = 0;  // sector count

			out.at(29) = 0x10 | sub_command;  // version & sub command

			bool respond = true;

			if (sub_command == Ccmd_read) {
				out.at(30) = 0; // sizeof(configuration) >> 8;
				out.at(31) = 0; // sizeof(configuration) & 255;

				out.resize(32);

//				for(size_t idx=0; idx<sizeof configuration; idx++)
//					out.push_back(configuration[idx]);
			}
			else if (sub_command == Ccmd_test) {
				if (sc_data_len != sizeof(configuration) || memcmp(configuration, out.data() + 32, sc_data_len) != 0)
					respond = false;
			}
			else if (sub_command == Ccmd_test_prefix) {
				if (sc_data_len > sizeof(configuration) || memcmp(configuration, out.data() + 32, sc_data_len) != 0)
					respond = false;
			}
			else if (sub_command == Ccmd_set_config || sub_command == Ccmd_force_set_config) {
				if (sc_data_len != sizeof(configuration)) {
					out.at(14) |= FlagE;
					out.at(15) = E_ConfigErr;
				}
				else {
					memcpy(configuration, out.data() + 32, sc_data_len);
				}
			}
			else {
				dolog(ll_warning, "aoe::operator: sub-command %d not understood", sub_command);
			}

			if (respond) {
				dolog(ll_debug, "aoe::operator: send response to %d (%zu bytes)", sub_command, out.size());

				if (write(fd, out.data(), out.size()) != ssize_t(out.size())) {
					dolog(ll_error, "aoe::operator: failed to tansmit Ethernet frame: %s", strerror(errno));
					// TODO terminate session?
				}
			}
		}
		else if (command == CommandATA) {
			uint64_t lba = uint64_t(out[28]) | (uint64_t(out[29]) << 8) | (uint64_t(out[30]) << 16) | (uint64_t(out[31]) << 24) | (uint64_t(out[32]) << 32) | (uint64_t(out[33]) << 40);

			out.at(24) = 0;  // flags

			dolog(ll_debug, "aoe::operator: CommandATA, lba: %ld, sector count: %d, cmd: %02x", lba, out[26], out[27]);

			if (out[27] == 0xec) {  // identify drive
				dolog(ll_debug, "aoe::operator: CommandATA: IdentifyDrive");

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
				response[83] = (1 << 10 /* LBA48 */);
				response[84] = 0x4000;  // from vblade
				response[86] = (1 << 10 /* LBA48 */);
				response[87] = 0x4000;  // from vblade
				response[93] = 0x400b;  // from vblade

				uint64_t sectors = sb->get_size() / 512;
				dolog(ll_debug, "aoe::operator: CommandATA, IdentifyDrive: backend is %d sectors", sectors);
				response[57] = sectors >> 16;  // current capacity in sectors
				response[58] = sectors & 65535;

				response[54] = sectors / (255 * 63);
				response[55] = 255;
				response[56] = 63;

				// LBA28
				response[60] = response[57] & 0x0fff;  // user addressable sectors (LBA)
				response[61] = response[58];  // user addressable sectors (LBA)

				// LBA48
				response[69] = 0;  // if bit 3 is 0, then this is 48 bit, else 32
				response[100] = sectors >> 48;
				response[101] = sectors >> 32;
				response[102] = sectors >> 16;
				response[103] = sectors;

				out.resize(36);
				for(int i=0; i<256; i++)
					add_uint16(out, response[i]);

				if (write(fd, out.data(), out.size()) != ssize_t(out.size())) {
					dolog(ll_error, "aoe::operator: failed to transmit Ethernet frame: %s", strerror(errno));
					// TODO terminate session?
				}
			}
			else if (out[27] == 0x20 || out[27] == 0x24) {  // read sectors, max 28bit/48bit
				dolog(ll_debug, "aoe::operator: CommandATA: ReadSector(s)");

				lba &= out[27] == 0x20 ? 0x0fffffff : 0x0000ffffffffffffll;

				int err = 0;
				block *b = nullptr;
			 	sb->get_data(lba * 512, out[26] * 512, &b, &err);

				if (err) {
					dolog(ll_error, "aoe::operator: failed to retrieve data from storage backend: %s", strerror(err));
					// TODO send error back
				}
				else {
					out[27] = 64;  // DRDY set
					out.resize(36);

					for(size_t i=0; i<b->get_size(); i++)
						out.push_back(b->get_data()[i]);

					if (write(fd, out.data(), out.size()) != ssize_t(out.size())) {
						dolog(ll_error, "aoe::operator: failed to transmit Ethernet frame: %s (%zu bytes)", strerror(errno), out.size());
						// TODO terminate session?
					}
				}
			}
			else {
				dolog(ll_warning, "aoe::operator: ata command %02x not supported", out[27]);
			}
		}
	}

	dolog(ll_error, "aoe::operator: thread terminates");
}
