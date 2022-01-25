#include <algorithm>
#include <fcntl.h>
#include <string>
#include <string.h>
//#include <linux/if.h>  // included in headerfile
//#include <linux/if_arp.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "aoe-generic.h"
#include "error.h"
#include "logging.h"


void set_ifr_name(struct ifreq *ifr, const std::string & dev_name)
{
	size_t copy_name_n = std::min(size_t(IFNAMSIZ), dev_name.size());

	memcpy(ifr->ifr_name, dev_name.c_str(), copy_name_n);

	ifr->ifr_name[IFNAMSIZ - 1] = 0x00;
}

int open_tun(const std::string & dev_name)
{
	int fd = open("/dev/net/tun", O_RDWR);
	if (fd == -1)
		error_exit(true, "aoe: cannot open /dev/net/tun");

	if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
		error_exit(true, "aoe: settinf FD_CLOEXEC on fd failed");

	struct ifreq ifr_tap { 0 };
	ifr_tap.ifr_flags = IFF_TAP | IFF_NO_PI;
	set_ifr_name(&ifr_tap, dev_name);

	if (ioctl(fd, TUNSETIFF, &ifr_tap) == -1)
		error_exit(true, "aoe: ioctl TUNSETIFF failed");

	return fd;
}
