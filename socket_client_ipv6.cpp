#include "socket_client_ipv6.h"


socket_client_ipv6::socket_client_ipv6(const std::string & target, const int tport) : socket_client_ipv4(target, tport)
{
}

socket_client_ipv6::~socket_client_ipv6()
{
}

socket_client_ipv6 * socket_client_ipv6::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string target = cfg["target"].as<std::string>();
	int tport = cfg["tport"].as<int>();

	return new socket_client_ipv6(target, tport);
}
