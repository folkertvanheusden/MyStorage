#include <yaml-cpp/yaml.h>

#include "socket_listener_ipv4.h"


class socket_listener_ipv6 : public socket_listener_ipv4
{
public:
	socket_listener_ipv6(const std::string & listen_addr, const int listen_port);
	~socket_listener_ipv6();

	bool begin() override;

	YAML::Node emit_configuration() const override;
	static socket_listener_ipv6 * load_configuration(const YAML::Node & node);
};
