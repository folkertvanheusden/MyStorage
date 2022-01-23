#include <errno.h>
#include <netdb.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "logging.h"
#include "str.h"


std::string get_endpoint_name(int fd)
{
	char host[256] { "?" };
	char serv[256] { "?" };
	struct sockaddr_in6 addr { 0 };
	socklen_t addr_len = sizeof addr;

	if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == -1)
		dolog(ll_warning, "get_endpoint_name: failed to find name of fd %d", fd);
	else
		getnameinfo((struct sockaddr *)&addr, addr_len, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);

	return myformat("[%s]:%s", host, serv);
}

ssize_t READ(int fd, uint8_t *whereto, size_t len)
{
	ssize_t cnt=0;

	while(len > 0)
	{
		ssize_t rc = read(fd, whereto, len);

		if (rc == -1)
		{
			if (errno == EINTR || errno == EAGAIN) {
				dolog(ll_warning, "READ: %s", strerror(errno));
				continue;
			}

			return -1;
		}
		else if (rc == 0)
			break;
		else
		{
			whereto += rc;
			len -= rc;
			cnt += rc;
		}
	}

	return cnt;
}

ssize_t WRITE(int fd, const uint8_t *whereto, size_t len)
{
	ssize_t cnt=0;

	while(len > 0)
	{
		ssize_t rc = write(fd, whereto, len);

		if (rc == -1)
		{
			if (errno == EINTR || errno == EAGAIN) {
				dolog(ll_warning, "WRITE: %s", strerror(errno));
				continue;
			}

			return -1;
		}
		else if (rc == 0)
			return -1;
		else
		{
			whereto += rc;
			len -= rc;
			cnt += rc;
		}
	}

	return cnt;
}
