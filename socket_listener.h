#pragma once
#include <atomic>

#include "base.h"


class socket_listener : public base
{
public:
	socket_listener();
	virtual ~socket_listener();

	virtual std::string get_listen_address() const = 0;

	virtual int wait_for_client(std::atomic_bool *const stop_flag) = 0;

	virtual YAML::Node emit_configuration() const = 0;
	static socket_listener * load_configuration(const YAML::Node & node);
};
