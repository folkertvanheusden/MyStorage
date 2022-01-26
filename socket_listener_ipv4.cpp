#include <atomic>
#include <errno.h>
#include <poll.h>
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
#include "str.h"


socket_listener_ipv4::socket_listener_ipv4(const char *const listen_addr, const int listen_port) : listen_addr(listen_addr), listen_port(listen_port)
{
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1)
		throw myformat("socket_listener_ipv4: failed to create socket: %s", strerror(errno));

	int reuse_addr = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse_addr, sizeof(reuse_addr)) == -1)
		throw myformat("socket_listener_ipv4: failed to set \"re-use address\": %s", strerror(errno));

	struct sockaddr_in servaddr { 0 };
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(listen_port);

	if (inet_aton(listen_addr, &servaddr.sin_addr) == -1)
		throw myformat("socket_listener_ipv4: problem interpreting \"%s\": %s", listen_addr, strerror(errno));

	// Binding newly created socket to given IP and verification
	if (bind(fd, reinterpret_cast<sockaddr *>(&servaddr), sizeof(servaddr)) == -1)
		throw myformat("socket_listener_ipv4: failed to bind to [%s]:%d: %s", listen_addr, listen_port, strerror(errno));

	if (listen(fd, SOMAXCONN) == -1)
		throw myformat("socket_listener_ipv4: failed to listen on socket: %s", strerror(errno));

	int qlen = SOMAXCONN;
	if (setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) == -1)
		throw myformat("socket_listener_ipv4: failed to enable \"tcp fastopen\": %s", strerror(errno));

	dolog(ll_info, "socket_listener_ipv4: listening on [%s]:%d", listen_addr, listen_port);
}

socket_listener_ipv4::~socket_listener_ipv4()
{
}

YAML::Node socket_listener_ipv4::emit_configuration() const
{
	YAML::Node out_cfg;
	out_cfg["listen-addr"] = listen_addr;
	out_cfg["listen-port"] = listen_port;

	YAML::Node out;
	out["type"] = "socket-listener-ipv4";
	out["cfg"] = out_cfg;

	return out;
}

std::string socket_listener_ipv4::get_listen_address() const
{
	return myformat("[%s]:%d", listen_addr.c_str(), listen_port);
}

int socket_listener_ipv4::wait_for_client(std::atomic_bool *const stop_flag)
{
	int cfd = -1;

	struct pollfd fds[] = { { 1, POLLIN, 0 } };

	for(;!*stop_flag;) {
		int rc = poll(fds, 1, 250);
		if (rc == 0)
			continue;

		if (rc == -1) {
			dolog(ll_error, "socket_listener_ipv4::wait_for_client: failed to invoke poll on fd %d", cfd);
			break;
		}

		cfd = accept(fd, nullptr, nullptr);
		if (cfd != -1) {
			dolog(ll_info, "socket_listener_ipv4: connected to \"%s\" on fd %d", get_endpoint_name(cfd).c_str(), cfd);

			int nodelay = 1;
			if (setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(int)) == -1) {
				dolog(ll_error, "socket_listener_ipv4::wait_for_client: failed to disable Naggle on fd %d", cfd);
				close(cfd);
				continue;
			}

			break;
		}

		dolog(ll_error, "socket_listener_ipv4::wait_for_client: failed to accept connection (%s)", strerror(errno));
	}

	return cfd;
}
