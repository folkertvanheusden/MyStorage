#include <atomic>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "error.h"
#include "logging.h"
#include "net.h"
#include "socket_listener_unixdomain.h"
#include "str.h"


socket_listener_unixdomain::socket_listener_unixdomain(const std::string & path) : path(path)
{
	struct sockaddr_un addr { 0 };

	if (path.size() > sizeof(addr.sun_path) - 1)
		throw "socket_listener_unixdomain: socket path to long";

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
		throw myformat("socket_listener_unixdomain: failed to create socket: %s", strerror(errno));

	if (remove(path.c_str()) == -1 && errno != ENOENT)
		throw myformat("socket_listener_unixdomain: failed to delete \"%s\": %s", path.c_str(), strerror(errno));

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path.c_str());

	// Binding newly created socket to given IP and verification
	if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1)
		throw myformat("socket_listener_unixdomain: failed to bind to %s: %s", path.c_str(), strerror(errno));

	if (listen(fd, SOMAXCONN) == -1)
		throw myformat("socket_listener_unixdomain: failed to listen on socket: %s", strerror(errno));

	dolog(ll_info, "socket_listener_unixdomain: listening on \"%s\"", path.c_str());
}

socket_listener_unixdomain::~socket_listener_unixdomain()
{
}

socket_listener_unixdomain * socket_listener_unixdomain::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string path = cfg["path"].as<std::string>();

	return new socket_listener_unixdomain(path);
}

YAML::Node socket_listener_unixdomain::emit_configuration() const
{
	YAML::Node out_cfg;
	out_cfg["path"] = path;

	YAML::Node out;
	out["type"] = "socket-listener-unixdomain";
	out["cfg"] = out_cfg;

	return out;
}

std::string socket_listener_unixdomain::get_listen_address() const
{
	return path;
}

int socket_listener_unixdomain::wait_for_client(std::atomic_bool *const stop_flag)
{
	dolog(ll_debug, "socket_listener_unixdomain::wait_for_client: starting to wait on fd %d", fd);

	int cfd = -1;

	struct pollfd fds[] = { { fd, POLLIN, 0 } };

	for(;!*stop_flag;) {
		int rc = poll(fds, 1, 250);
		if (rc == 0)
			continue;

		if (rc == -1) {
			dolog(ll_error, "socket_listener_unixdomain::wait_for_client: failed to invoke poll on fd %d", fd);
			break;
		}

		cfd = accept(fd, nullptr, nullptr);
		if (cfd != -1) {
			dolog(ll_info, "socket_listener_unixdomain: connected to \"%s\" on fd %d", get_endpoint_name(cfd).c_str(), cfd);

			break;
		}

		dolog(ll_error, "socket_listener_unixdomain::wait_for_client: failed to accept connection (%s)", strerror(errno));
	}

	return cfd;
}
