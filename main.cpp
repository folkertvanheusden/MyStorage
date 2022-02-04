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
#include "server.h"
#include "server_aoe.h"
#include "server_nbd.h"
#include "snapshots.h"
#include "storage_backend.h"
#include "str.h"


std::atomic_bool stop_flag { false };

void sigh(int sig)
{
	stop_flag = true;
}

void store_configuration(const std::vector<server *> & servers, const std::vector<storage_backend *> & storage, const std::string & file)
{
	YAML::Node out;

	std::vector<YAML::Node> y_servers;
	for(auto s : servers)
		y_servers.push_back(s->emit_configuration());

	out["servers"] = y_servers;

	std::vector<YAML::Node> y_storage;
	for(auto sb : storage)
		y_storage.push_back(sb->emit_configuration());

	out["storage"] = y_storage;

	YAML::Node logging;
	logging["file"] = logfile;
	logging["loglevel-files"] = ll_to_str(log_level_file);
	logging["loglevel-screen"] = ll_to_str(log_level_screen);

	out["logging"] = logging;

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

std::map<std::string, std::vector<base *> > load_configuration(const std::string & file)
{
	dolog(ll_info, "load_configuration: loading configuration from \"%s\"", file.c_str());

	YAML::Node config = YAML::LoadFile(file);

	YAML::Node cfg_storage = config["storage"];

	std::vector<base *> snapshotters;

	std::vector<storage_backend *> storage;
	std::vector<base *> storage_rc;

	for(YAML::const_iterator it = cfg_storage.begin(); it != cfg_storage.end(); it++) {
		const YAML::Node node = it->as<YAML::Node>();

		storage_backend *sb = storage_backend::load_configuration(node, { }, { });

		storage.push_back(sb);
		storage_rc.push_back(sb);

		try {
			snapshots *s = dynamic_cast<snapshots *>(sb);

			snapshotters.push_back(s);
		}
		catch(std::bad_cast & bc) {
			// not a snapshot-object
		}
	}

	YAML::Node cfg_servers = config["servers"];

	std::vector<base *> servers;

	for(YAML::const_iterator it = cfg_servers.begin(); it != cfg_servers.end(); it++) {
		const YAML::Node node = it->as<YAML::Node>();

		servers.push_back(server::load_configuration(node, storage));
	}

	YAML::Node cfg_logging = config["logging"];
	const std::string logfile = cfg_logging["file"].as<std::string>();

	log_level_t ll_file = str_to_ll(cfg_logging["loglevel-files"].as<std::string>());
	log_level_t ll_screen = str_to_ll(cfg_logging["loglevel-screen"].as<std::string>());

	setlog(logfile.c_str(), ll_file, ll_screen);

	std::map<std::string, std::vector<base *> > out;
	out.insert({ "servers", servers });
	out.insert({ "storage", storage_rc });
	out.insert({ "snapshots", snapshotters });  // do not manually free as it is a storage object

	return out;
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

		for(auto sb : modules["storage"])
			delete sb;
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
