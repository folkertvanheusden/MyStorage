#pragma once
#include <yaml-cpp/yaml.h>


class socket_client
{
protected:
	int fd;

public:
	socket_client();
	virtual ~socket_client();

	virtual int connect() = 0;

	int get_fd();

	virtual YAML::Node emit_configuration() const = 0;
	static socket_client * load_configuration(const YAML::Node & node);
};
