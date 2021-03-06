#pragma once
#include "socket_client_ipv4.h"


class socket_client_ipv6 : public socket_client_ipv4
{
public:
	socket_client_ipv6(const std::string & target, const int tport);
	virtual ~socket_client_ipv6();

	static socket_client_ipv6 * load_configuration(const YAML::Node & node);
};
