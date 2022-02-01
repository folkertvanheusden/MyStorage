#include <yaml-cpp/yaml.h>

#include "socket_listener.h"


class socket_listener_ipv4 : public socket_listener
{
private:
	int               fd { -1 };
	const std::string listen_addr;
	const int         listen_port { -1 };

public:
	socket_listener_ipv4(const std::string & listen_addr, const int listen_port);
	~socket_listener_ipv4();

	std::string get_listen_address() const override;

	int wait_for_client(std::atomic_bool *const stop_flag) override;

	YAML::Node emit_configuration() const override;
	static socket_listener_ipv4 * load_configuration(const YAML::Node & node);
};
