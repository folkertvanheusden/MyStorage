#include <unistd.h>

#include "logging.h"
#include "socket_client.h"
#include "socket_client_ipv4.h"
#include "socket_client_ipv6.h"
#include "str.h"


socket_client::socket_client() : fd(-1)
{
}

socket_client::~socket_client()
{
	if (fd != -1)
		close(fd);
}

int socket_client::get_fd()
{
	return fd;
}

socket_client * socket_client::load_configuration(const YAML::Node & node)
{
	dolog(ll_info, " * socket_client::load_configuration");

        const std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "socket-client-ipv4")
		return socket_client_ipv4::load_configuration(node);

	if (type == "socket-client-ipv6")
		return socket_client_ipv6::load_configuration(node);

	dolog(ll_error, "socket_client::load_configuration: socket client type \"%s\" is not known", type.c_str());

	return nullptr;
}
