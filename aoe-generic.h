#pragma once
#include <string>


#define AoE_EtherType 0x88a2

#define CommandATA	0
#define CommandInfo	1
#define CommandMacMask	2
#define CommandResRel	3

#define Ccmd_read	0
#define Ccmd_test	1
#define Ccmd_test_prefix	2
#define Ccmd_set_config	3
#define Ccmd_force_set_config	4

#define E_BadCmd	1
#define E_BadArg	2
#define E_DevUnavailable	3
#define E_ConfigErr	4
#define E_BadVersion	5

#define FlagR		8  // reply
#define FlagE		4  // error

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

bool open_tun(const std::string & dev_name, int *const fd, int *const mtu_size);
