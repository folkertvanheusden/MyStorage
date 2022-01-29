#include <atomic>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "aoe.h"
#include "logging.h"
#include "nbd.h"
#include "str.h"


std::atomic_bool stop_flag { false };

void sigh(int sig)
{
	stop_flag = true;
}

void store_configuration(const std::vector<base *> & servers, const std::string & file)
{
	YAML::Node out;
	out["type"] = "MyStorage";

	std::vector<YAML::Node> y_servers;
	for(auto s : servers)
		y_servers.push_back(s->emit_configuration());

	out["cfg"] = y_servers;

	YAML::Emitter output;
	output << out;

	FILE *fh = fopen(file.c_str(), "w");
	if (fh) {
		if (size_t(fprintf(fh, "%s", output.c_str())) != output.size())
			dolog(ll_error, "store_configuration: cannot write to file \"%s\": %s", file.c_str(), strerror(errno));

		fclose(fh);
	}
	else {
		dolog(ll_error, "store_configuration: cannot create file \"%s\": %s", file.c_str(), strerror(errno));
	}
}

std::vector<base *> load_configuration(const std::string & file)
{
	dolog(ll_info, "load_configuration: loading configuration from \"%s\"", file.c_str());

	YAML::Node config = YAML::LoadFile(file);

	YAML::Node cfg = config["cfg"];

	std::vector<base *> servers;

	for(YAML::const_iterator it = cfg.begin(); it != cfg.end(); it++) {
		const YAML::Node node = it->as<YAML::Node>();

		std::string type = str_tolower(node["type"].as<std::string>());

		if (type == "nbd")
			servers.push_back(nbd::load_configuration(node));
		else if (type == "aoe")
			servers.push_back(aoe::load_configuration(node));
		else
			dolog(ll_error, "load_configuration: server type \"%s\" is unknown", type.c_str(), strerror(errno));
	}

	return servers;
}

int main(int argc, char *argv[])
{
	std::string yaml_file = "mystorage.yaml";

	int c = -1;
	while((c = getopt(argc, argv, "c:")) != -1) {
		if (c == 'c')
			yaml_file = optarg;
	}

	setlog("mystorage.log", ll_info, ll_info);  // TODO add to configuration file
	dolog(ll_info, "MyStorage starting");

	signal(SIGTERM, sigh);
	signal(SIGINT, sigh);

	auto servers = load_configuration(yaml_file);

	dolog(ll_info, "MyStorage running");

	for(;!stop_flag;)
		pause();

	dolog(ll_info, "MyStorage terminating");

	for(auto s : servers)
		delete s;

	fflush(nullptr);
	sync();

	dolog(ll_info, "MyStorage stopped");

	closelog();

	return 0;
}
