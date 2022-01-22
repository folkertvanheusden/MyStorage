#include <string>
#include <sys/socket.h>

#include "logging.h"
#include "str.h"

std::string get_endpoint_name(int fd)
{
	char host[256] { "?" };
	char serv[256] { "?" };
	struct sockaddr_in6 addr { 0 };
	socklen_t addr_len = sizeof addr;

	if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == -1) {
		dolog(ll_warning, "get_endpoint_name: failed to find name of fd %d", fd);
	else
		getnameinfo((struct sockaddr *)&addr, addr_len, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);

	return myformat("[%s]:%s", host, serv);
}
