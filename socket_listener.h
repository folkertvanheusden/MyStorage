#pragma once
#include <atomic>


class socket_listener
{
public:
	socket_listener();
	virtual ~socket_listener();

	virtual int wait_for_client(std::atomic_bool *const stop_flag) = 0;
};
