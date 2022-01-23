#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "error.h"
#include "logging.h"
#include "net.h"
#include "socket_listener_ipv4.h"


socket_listener_ipv4::socket_listener_ipv4(const char *const listen_addr, const int listen_port)
{
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1)
		error_exit(true, "socket_listener_ipv4: failed to create socket");

	int reuse_addr = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse_addr, sizeof(reuse_addr)) == -1)
		error_exit(true, "socket_listener_ipv4: failed to set \"re-use address\"");

	struct sockaddr_in servaddr { 0 };
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(listen_port);

	if (inet_aton(listen_addr, &servaddr.sin_addr) == -1)
		error_exit(true, "socket_listener_ipv4: problem interpreting \"%s\"", listen_addr);

	// Binding newly created socket to given IP and verification
	if (bind(fd, reinterpret_cast<sockaddr *>(&servaddr), sizeof(servaddr)) == -1)
		error_exit(true, "socket_listener_ipv4: failed to bind to [%s]:%d", listen_addr, listen_port);

	if (listen(fd, SOMAXCONN) == -1)
		error_exit(true, "socket_listener_ipv4: failed to listen on socket");

	int qlen = SOMAXCONN;
	if (setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) == -1)
		error_exit(true, "socket_listener_ipv4: failed to enable \"tcp fastopen\"");
}

socket_listener_ipv4::~socket_listener_ipv4()
{
	close(fd);
}

int socket_listener_ipv4::wait_for_client()
{
	int cfd = -1;

	for(;;) {
		cfd = accept(fd, nullptr, nullptr);
		if (cfd != -1) {
			dolog(ll_info, "socket_listener_ipv4: connected to \"%s\" on fd %d", get_endpoint_name(cfd).c_str(), cfd);
			break;
		}

		dolog(ll_error, "socket_listener_ipv4: failed to accept connection (%s)", strerror(errno));
	}

	return cfd;
}
