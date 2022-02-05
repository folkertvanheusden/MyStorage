#include <map>
#include <string>
#include <string.h>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "base.h"
#include "logging.h"
#include "server.h"
#include "snapshots.h"
#include "storage_backend.h"
#include "str.h"


std::string yaml_get_string(const YAML::Node & node, const std::string & key, const std::string & description)
{
	try {
		return node[key].as<std::string>();
	}
	catch(YAML::InvalidNode & yin) {
		throw myformat("yaml_get_string: item \"%s\" (%s) is missing in YAML file", key.c_str(), description.c_str());
	}
}

int yaml_get_int(const YAML::Node & node, const std::string & key, const std::string & description)
{
	try {
		return node[key].as<int>();
	}
	catch(YAML::InvalidNode & yin) {
		throw myformat("yaml_get_int: item \"%s\" (%s) is missing in YAML file", key.c_str(), description.c_str());
	}
}

uint64_t yaml_get_uint64_t(const YAML::Node & node, const std::string & key, const std::string & description, const bool units)
{
	try {
		if (units) {
			std::string value = str_tolower(node[key].as<std::string>());

			char unit = value.at(value.size() - 1);
			uint64_t mul = 1;

			if (unit == 'g')
				mul = 1024 * 1024 * 1024;
			else if (unit == 'm')
				mul = 1024 * 1024;
			else if (unit == 'k')
				mul = 1024;
			else if (unit >= 'a' && unit <= 'z')
				throw myformat("yaml_get_int: unit \"%c\" is not known/supported", unit);

			return uint64_t(atoll(value.c_str())) * mul;
		}

		return node[key].as<uint64_t>();
	}
	catch(YAML::InvalidNode & yin) {
		throw myformat("yaml_get_uint64_t: item \"%s\" (%s) is missing in YAML file", key.c_str(), description.c_str());
	}
}

const YAML::Node yaml_get_yaml_node(const YAML::Node & node, const std::string & key, const std::string & description)
{
	try {
		return node[key];
	}
	catch(YAML::InvalidNode & yin) {
		throw myformat("yaml_get_yaml_node: item \"%s\" (%s) is missing in YAML file", key.c_str(), description.c_str());
	}
}

bool yaml_get_bool(const YAML::Node & node, const std::string & key, const std::string & description)
{
	try {
		return node[key].as<bool>();
	}
	catch(YAML::InvalidNode & yin) {
		throw myformat("yaml_get_bool: item \"%s\" (%s) is missing in YAML file", key.c_str(), description.c_str());
	}
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
