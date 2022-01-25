#include <string>

#include "socket_client.h"


class socket_client_ipv4 : public socket_client
{
private:
	const std::string hostname;
	const int         port;

public:
	socket_client_ipv4(const std::string & target, const int tport);
	virtual ~socket_client_ipv4();

	int connect() override;
};
