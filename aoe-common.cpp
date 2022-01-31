#include <algorithm>
#include <fcntl.h>
#include <string>
#include <string.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "aoe-common.h"
#include "error.h"
#include "logging.h"
#include "str.h"


static void set_ifr_name(struct ifreq *ifr, const std::string & dev_name)
{
	size_t copy_name_n = std::min(size_t(IFNAMSIZ), dev_name.size());

	memcpy(ifr->ifr_name, dev_name.c_str(), copy_name_n);

	ifr->ifr_name[IFNAMSIZ - 1] = 0x00;
}

bool open_tun(const std::string & dev_name, int *const fd, int *const mtu_size)
{
	*fd = open("/dev/net/tun", O_RDWR);
	if (*fd == -1)
		error_exit(true, myformat("aoe(%s): cannot open /dev/net/tun", dev_name.c_str()).c_str());

	if (fcntl(*fd, F_SETFD, FD_CLOEXEC) == -1)
		error_exit(true, myformat("aoe(%s): settinf FD_CLOEXEC on fd failed", dev_name.c_str()).c_str());

	struct ifreq ifr_tap { 0 };
	ifr_tap.ifr_flags = IFF_TAP | IFF_NO_PI;
	set_ifr_name(&ifr_tap, dev_name);

	if (ioctl(*fd, TUNSETIFF, &ifr_tap) == -1)
		error_exit(true, myformat("aoe(%s): ioctl TUNSETIFF failed", dev_name.c_str()).c_str());

	int temp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (temp_fd == -1)
		error_exit(true, myformat("aoe(%s): cannot create socket", dev_name.c_str()).c_str());

	if (ioctl(temp_fd, SIOCGIFFLAGS, &ifr_tap) == -1)
		error_exit(true, myformat("aoe(%s): ioctl SIOCGIFFLAGS failed", dev_name.c_str()).c_str());

	ifr_tap.ifr_flags = IFF_UP | IFF_RUNNING | IFF_MULTICAST | IFF_BROADCAST;

	if (ioctl(temp_fd, SIOCSIFFLAGS, &ifr_tap) == -1)
		error_exit(true, myformat("aoe(%s): ioctl SIOCSIFFLAGS failed", dev_name.c_str()).c_str());

	if (*mtu_size != 0) {
		struct ifreq ifr_tap2 { 0 };
		set_ifr_name(&ifr_tap2, dev_name);
		ifr_tap2.ifr_addr.sa_family = AF_INET;

		ifr_tap2.ifr_mtu = *mtu_size;

		if (ioctl(temp_fd, SIOCSIFMTU, &ifr_tap2) == -1)
			error_exit(true, myformat("aoe(%s): ioctl SIOCSIFMTU failed", dev_name.c_str()).c_str());
	}
	else {
		struct ifreq ifr_tap2 { 0 };
		set_ifr_name(&ifr_tap2, dev_name);
		ifr_tap2.ifr_addr.sa_family = AF_INET;

		if (ioctl(temp_fd, SIOCGIFMTU, &ifr_tap2) == -1)
			error_exit(true, myformat("aoe(%s): ioctl SIOCGIFMTU failed", dev_name.c_str()).c_str());

		*mtu_size = ifr_tap2.ifr_mtu;
	}

	dolog(ll_info, "aoe(%s): MTU size: %d bytes", dev_name.c_str(), *mtu_size);

	return true;
}
