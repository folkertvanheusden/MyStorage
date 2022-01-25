#include "socket_client_ipv6.h"


socket_client_ipv6::socket_client_ipv6(const std::string & target, const int tport) : socket_client_ipv4(target, tport)
{
}

socket_client_ipv6::~socket_client_ipv6()
{
}
