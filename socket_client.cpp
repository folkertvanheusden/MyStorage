#include <unistd.h>

#include "socket_client.h"


socket_client::socket_client()
{
}

socket_client::~socket_client()
{
	if (fd != -1)
		close(fd);
}

int socket_client::get_fd()
{
	return fd;
}
