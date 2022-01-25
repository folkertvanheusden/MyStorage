#include <string>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>

#include "aoe-generic.h"
#include "error.h"
#include "logging.h"
#include "mirror.h"
#include "storage_backend_aoe.h"
#include "str.h"


storage_backend_aoe::storage_backend_aoe(const std::string & id, const std::vector<mirror *> & mirrors, const std::string & dev_name, const uint8_t my_mac[6], const uint16_t major, const uint8_t minor) : storage_backend(id, mirrors), dev_name(dev_name), major(major), minor(minor)
{
	memcpy(this->my_mac, my_mac, 6);

	if (!connect())
		dolog(ll_warning, "storage_backend_aoe(%s): failed to connect to AoE target %d:%d (via interface \"%s\")", id.c_str(), major, minor, dev_name.c_str());
}

storage_backend_aoe::~storage_backend_aoe()
{
	if (connection.fd != -1)
		close(connection.fd);
}

typedef struct __attribute__((packed))
{
	uint8_t   dst[6];
	uint8_t   src[6];
	uint16_t  type;
	uint8_t   flags;
	uint8_t   error;
	uint16_t  major;
	uint8_t   minor;
	uint8_t   command;
	uint32_t  tag;
} aoe_ethernet_header_t;

typedef struct __attribute__((packed))
{
	aoe_ethernet_header_t aeh;

	uint16_t  n_buffers;
	uint16_t  firmware_version;
	uint8_t   n_sectors;
	uint8_t   ver_cmd;
	uint16_t  len;
	uint8_t   data[1024];
} aoe_configuration_t;

typedef struct __attribute__((packed))
{
	aoe_ethernet_header_t aeh;

	uint8_t aflags;
	uint8_t error;
	uint8_t n_sectors;
	uint8_t command;
	uint8_t lba[6];
	uint16_t reserved;
	uint16_t data[0];
} aoe_ata_t;

typedef enum { ACS_discover, ACS_discover_sent, ACS_identify, ACS_identify_sent } aoe_connect_state_t;

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

bool storage_backend_aoe::connect() const
{
	if (connection.fd != -1) {
		close(connection.fd);
		connection.fd = -1;
	}

	connection.fd = open_tun(dev_name);
	if (connection.fd == -1) {
		dolog(ll_warning, "storage_backend_aoe(%s): failed to setup interface \"%s\"", id.c_str(), dev_name.c_str());
		return false;
	}

	aoe_connect_state_t state = ACS_discover;
	time_t state_since = time(nullptr);

	uint32_t tag = rand();

	char recv_buffer[65536] { 0 };  // maximum Ethernet frame size

	for(;;) {
		dolog(ll_debug, "storage_backend_aoe(%s): connect state %d", id.c_str(), state);

		if (state == ACS_discover) {
			aoe_ethernet_header_t aeh { 0 };

			memset(aeh.dst, 0xff, sizeof aeh.dst);
			memcpy(aeh.src, my_mac, sizeof aeh.src);
			aeh.type = htons(AoE_EtherType);

			aeh.flags   = 0;
			aeh.error   = 0;
			aeh.major   = 0xffff;
			aeh.minor   = 0xff;
			aeh.command = CommandInfo;
			aeh.tag     = tag;

			if (write(connection.fd, &aeh, sizeof aeh) != sizeof(aeh)) {
				dolog(ll_warning, "storage_backend_aoe(%s): failed to transmit discover packet", id.c_str());
				sleep(1);
			}
			else {
				state = ACS_discover_sent;
				state_since = time(nullptr);
			}
		}
		else if (state == ACS_discover_sent) {
			int rc = read(connection.fd, recv_buffer, sizeof recv_buffer);
			if (rc == -1)
				dolog(ll_error, "storage_backend_aoe(%s): problem receiving (%s)", id.c_str(), strerror(errno));
			else {
				const aoe_configuration_t *ac = reinterpret_cast<const aoe_configuration_t *>(recv_buffer);

				if (time(nullptr) - state_since >= 1) {
					state = ACS_discover;
					state_since = time(nullptr);
				}

				if (ac->aeh.type != htons(AoE_EtherType)) {
					dolog(ll_debug, "storage_backend_aoe(%s): ignoring Ethernet frame of type %04x", id.c_str(), ntohs(ac->aeh.type));
					continue;
				}

				dolog(ll_debug, "storage_backend_aoe(%s): packet from %02x:%02x:%02x:%02x:%02x:%02x", 
						id.c_str(),
						ac->aeh.src[0], ac->aeh.src[1], ac->aeh.src[2], ac->aeh.src[3], ac->aeh.src[4], ac->aeh.src[5]);

				dolog(ll_debug, "storage_backend_aoe(%s): buffers: %d, sectors: %d, firmware: %04x, ver/cmd: %02x, major: %d, minor: %d", id.c_str(), ntohs(ac->n_buffers), ac->n_sectors, ntohs(ac->firmware_version), ac->ver_cmd, ntohs(ac->aeh.major), ac->aeh.minor);

				if (ntohs(ac->aeh.major) != major || ac->aeh.minor != minor) {
					dolog(ll_debug, "storage_backend_aoe(%s): ignoring %d:%d", id.c_str(), ac->aeh.major, ac->aeh.minor);
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
				dolog(ll_warning, "storage_backend_aoe(%s): failed to transmit identify packet", id.c_str());
				sleep(1);
			}
			else {
				state = ACS_identify_sent;
				state_since = time(nullptr);
			}
		}
		else if (state == ACS_identify_sent) {
			int rc = read(connection.fd, recv_buffer, sizeof recv_buffer);
			if (rc == -1)
				dolog(ll_error, "storage_backend_aoe(%s): problem receiving (%s)", id.c_str(), strerror(errno));
			else {
				const aoe_ata_t *aa = reinterpret_cast<const aoe_ata_t *>(recv_buffer);

				if (time(nullptr) - state_since >= 1) {
					state = ACS_identify;
					state_since = time(nullptr);
				}

				if (aa->aeh.type != htons(AoE_EtherType)) {
					dolog(ll_debug, "storage_backend_aoe(%s): ignoring Ethernet frame of type %04x", id.c_str(), ntohs(aa->aeh.type));
					continue;
				}

				if ((aa->aeh.flags & 4) || aa->aeh.error || aa->error) {
					dolog(ll_debug, "storage_backend_aoe(%s): error message from storage: %02x / %02x", id.c_str(), aa->aeh.error, aa->error);
					break;
				}

				uint16_t parameters[256] { 0 };
				memcpy(parameters, aa->data, std::min(int(256 * sizeof(uint16_t)), rc - 36));

				std::string serial_number = get_idi_string(&parameters[10], 20 / 2);
				std::string firmware_rev  = get_idi_string(&parameters[23], 8 / 2);
				std::string model         = get_idi_string(&parameters[27], 40 / 2);

				dolog(ll_debug, "storage_backend_aoe(%s): device model: \"%s\", firmware revision: \"%s\", serial number: \"%s\"", id.c_str(), model.c_str(), firmware_rev.c_str(), serial_number.c_str());


				if (parameters[49] & (1 << 9)) {  // LBA supported
					connection.size = get_idi_value(&parameters[100], 6 / 2) * 512;
				}
				else {
					connection.size = parameters[1] * parameters[3] * parameters[4] * parameters[5];
				}

				dolog(ll_debug, "storage_backend_aoe(%s): size: %llu bytes", id.c_str(), connection.size);
			}
		}
		else {
			error_exit(false, "storage_backend_aoe(%s): connect state %d invalid", id.c_str(), state);
		}
	}

	// TODO

	return true;
}

offset_t storage_backend_aoe::get_size() const
{
	if (connection.fd == -1)
		connect();

	if (connection.fd != -1)
		return size;

	dolog(ll_warning, "storage_backend_aoe::get_size(%s): not connected to AoE target", id.c_str());

	return 0;
}

void storage_backend_aoe::get_data(const offset_t offset, const uint32_t size, block **const b, int *const err)
{
}

void storage_backend_aoe::put_data(const offset_t offset, const block & b, int *const err)
{
}

bool storage_backend_aoe::fsync()
{
}

bool storage_backend_aoe::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
}
