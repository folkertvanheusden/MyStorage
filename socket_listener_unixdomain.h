#include <yaml-cpp/yaml.h>

#include "socket_listener.h"


class socket_listener_unixdomain : public socket_listener
{
private:
	int               fd { -1 };
	const std::string path;

public:
	socket_listener_unixdomain(const std::string & path);
	~socket_listener_unixdomain();

	std::string get_listen_address() const override;

	int wait_for_client(std::atomic_bool *const stop_flag) override;

	YAML::Node emit_configuration() const override;
	static socket_listener_unixdomain * load_configuration(const YAML::Node & node);
};
