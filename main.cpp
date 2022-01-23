#include <stdio.h>

#include "logging.h"
#include "nbd.h"
#include "socket_listener_ipv4.h"
#include "storage_backend_file.h"


int main(int argc, char *argv[])
{
	setlog("mystorage.log", ll_debug, ll_debug);

	socket_listener *sl = new socket_listener_ipv4("0.0.0.0", 10809);

	storage_backend *sb = new storage_backend_file("primary", "/home/folkert/temp/mystorage.dat");
	std::vector<storage_backend *> storage_backends { sb };

	nbd *nbd_ = new nbd(sl, storage_backends);

	getchar();

	delete nbd_;
	delete sb;
	delete sl;

	return 0;
}
