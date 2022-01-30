#include "logging.h"
#include "socket_listener.h"
#include "socket_listener_ipv4.h"
#include "socket_listener_unixdomain.h"
#include "str.h"


socket_listener::socket_listener() : base("socket_listener")
{
}

socket_listener::~socket_listener()
{
}

socket_listener * socket_listener::load_configuration(const YAML::Node & node)
{
        const std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "socket-listener-ipv4")
		return socket_listener_ipv4::load_configuration(node);

	if (type == "socket-listener-unixdomain")
		return socket_listener_unixdomain::load_configuration(node);

	dolog(ll_error, "socket_listener::load_configuration: socket listener type \"%s\" is not known", type.c_str());

	return nullptr;
}
