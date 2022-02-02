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
#include "socket_listener_ipv6.h"
#include "str.h"


socket_listener_ipv6::socket_listener_ipv6(const std::string & listen_addr, const int listen_port) : socket_listener_ipv4(listen_addr, listen_port)
{
}

socket_listener_ipv6::~socket_listener_ipv6()
{
}

bool socket_listener_ipv6::begin()
{
	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd == -1) {
		dolog(ll_error, "socket_listener_ipv6: failed to create socket: %s", strerror(errno));
		return false;
	}

	int reuse_addr = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse_addr, sizeof(reuse_addr)) == -1) {
		dolog(ll_error, "socket_listener_ipv6: failed to set \"re-use address\": %s", strerror(errno));
		return false;
	}

	struct sockaddr_in6 servaddr { 0 };
	servaddr.sin6_family = AF_INET6;
	servaddr.sin6_port = htons(listen_port);

	if (inet_pton(AF_INET6, listen_addr.c_str(), &servaddr.sin6_addr) == -1) {
		dolog(ll_error, "socket_listener_ipv6: problem interpreting \"%s\": %s", listen_addr.c_str(), strerror(errno));
		return false;
	}

	// Binding newly created socket to given IP and verification
	if (bind(fd, reinterpret_cast<sockaddr *>(&servaddr), sizeof(servaddr)) == -1) {
		dolog(ll_error, "socket_listener_ipv6: failed to bind to [%s]:%d: %s", listen_addr.c_str(), listen_port, strerror(errno));
		return false;
	}

	if (listen(fd, SOMAXCONN) == -1) {
		dolog(ll_error, "socket_listener_ipv6: failed to listen on socket: %s", strerror(errno));
		return false;
	}

	int qlen = SOMAXCONN;
	if (setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) == -1) {
		dolog(ll_error, "socket_listener_ipv6: failed to enable \"tcp fastopen\": %s", strerror(errno));
		return false;
	}

	dolog(ll_info, "socket_listener_ipv6: listening on [%s]:%d", listen_addr.c_str(), listen_port);

	return true;
}

socket_listener_ipv6 * socket_listener_ipv6::load_configuration(const YAML::Node & node)
{
	dolog(ll_info, " * socket_listener_ipv6::load_configuration");

	const YAML::Node cfg = node["cfg"];

	std::string listen_addr = cfg["listen-addr"].as<std::string>();
	int listen_port = cfg["listen-port"].as<int>();

	return new socket_listener_ipv6(listen_addr, listen_port);
}

YAML::Node socket_listener_ipv6::emit_configuration() const
{
	YAML::Node out_cfg;
	out_cfg["listen-addr"] = listen_addr;
	out_cfg["listen-port"] = listen_port;

	YAML::Node out;
	out["type"] = "socket-listener-ipv6";
	out["cfg"] = out_cfg;

	return out;
}
