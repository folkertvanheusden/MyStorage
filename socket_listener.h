#pragma once


class socket_listener
{
public:
	socket_listener();
	virtual ~socket_listener();

	virtual int wait_for_client() = 0;
};
