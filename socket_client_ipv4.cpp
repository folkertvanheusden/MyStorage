#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "error.h"
#include "logging.h"
#include "socket_client_ipv4.h"
#include "str.h"


socket_client_ipv4::socket_client_ipv4(const std::string & hostname, const int tport) : hostname(hostname), port(tport)
{
}

socket_client_ipv4::~socket_client_ipv4()
{
}

int socket_client_ipv4::connect()
{
	std::string portstr = myformat("%d", port);
	int fd = -1;

        struct addrinfo hints{ 0 };
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = 0;
        hints.ai_protocol = 0;

        struct addrinfo* result = nullptr;
        int rc = getaddrinfo(hostname.c_str(), portstr.c_str(), &hints, &result);
        if (rc)
                error_exit(false, "socket_client_ipv4: cannot resolve host name \"%s\": %s", hostname.c_str(), gai_strerror(rc));

	for (struct addrinfo *rp = result; rp != nullptr; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd == -1)
			continue;

		int old_flags = fcntl(fd, F_GETFL, 0);
		if (old_flags == -1)
			error_exit(true, "socket_client_ipv4: fcntl F_GETFL failed");

		if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
			error_exit(true, "socket_client_ipv4: fcntl F_SETFL(O_NONBLOCK) failed");

		/* wait for connection */
		/* connect to peer */
		if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
			/* connection made, return */
			if (fcntl(fd, F_SETFL, old_flags) == -1)
				error_exit(true, "socket_client_ipv4: fcntl F_SETFL(old_flags) failed");

			freeaddrinfo(result);
			return fd;
		}

		for(;;) {
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(fd, &wfds);

			struct timeval tv { 0, 100 * 1000 };

			/* wait for connection */
			rc = select(fd + 1, nullptr, &wfds, nullptr, &tv);
			if (rc == 0)	// time out
			{
				// timeout is handled implicitly in this loop
			}
			else if (rc == -1)	// error
			{
				if (errno == EINTR || errno == EAGAIN || errno == EINPROGRESS)
					continue;

				dolog(ll_info, "socket_client_ipv4: select failed during connect to \"%s\": %s", hostname.c_str(), strerror(errno));
				break;
			}
			else if (FD_ISSET(fd, &wfds))
			{
				int optval=0;
				socklen_t optvallen = sizeof(optval);

				/* see if the connect succeeded or failed */
				if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optvallen) == -1)
					dolog(ll_info, "socket_client_ipv4: getsockopt (SO_ERROR) failed: %s", strerror(errno));
				else if (optval == 0) { /* no error? */
					if (fcntl(fd, F_SETFL, old_flags) == -1)
						error_exit(true, "socket_client_ipv4: fcntl F_SETFL(old_flags) failed");

					freeaddrinfo(result);
					return fd;
				}
			}
		}

		close(fd);
		fd = -1;
	}

	freeaddrinfo(result);

	return fd;
}
