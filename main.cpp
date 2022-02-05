#include <atomic>
#include <errno.h>
#include <map>
#include <optional>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "logging.h"
#include "str.h"
#include "yaml-helpers.h"


std::atomic_bool stop_flag { false };

void sigh(int sig)
{
	stop_flag = true;
}

int main(int argc, char *argv[])
{
	std::string yaml_file = "mystorage.yaml";

	int c = -1;
	while((c = getopt(argc, argv, "c:")) != -1) {
		if (c == 'c')
			yaml_file = optarg;
	}

	setlog("mystorage.log", ll_debug, ll_info);  // TODO add to configuration file
	dolog(ll_info, "MyStorage starting");

	signal(SIGTERM, sigh);
	signal(SIGINT, sigh);

	try {
		auto modules = load_configuration(yaml_file);

		dolog(ll_info, "MyStorage running");

		for(;!stop_flag;)
			pause();

		dolog(ll_info, "MyStorage terminating");

		for(auto srv : modules["servers"])
			delete srv;

		for(auto sb : modules["storage"]) {
			dynamic_cast<storage_backend *>(sb)->dump_stats("./s-");
			delete sb;
		}
	}
	catch(const std::string & err) {
		dolog(ll_error, "main: caught exception \"%s\"", err.c_str());
	}

	fflush(nullptr);
	sync();

	dolog(ll_info, "MyStorage stopped");

	closelog();

	return 0;
}
