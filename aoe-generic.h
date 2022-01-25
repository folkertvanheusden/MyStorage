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

bool open_tun(const std::string & dev_name, int *const fd, int *const mtu_size);
