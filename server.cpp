#include <yaml-cpp/yaml.h>

#include "logging.h"
#include "server.h"
#include "server_aoe.h"
#include "server_nbd.h"
#include "str.h"


server::server(const std::string & id) : base(id)
{
}

server::~server()
{
}

server * server::load_configuration(const YAML::Node & node, const std::vector<storage_backend *> & storage)
{
	dolog(ll_info, " * server::load_configuration");

	std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "nbd")
		return nbd::load_configuration(node, storage);

	if (type == "aoe")
		return aoe::load_configuration(node, storage);

	throw myformat("load_configuration: server type \"%s\" is unknown", type.c_str());
}

storage_backend * find_storage(const std::vector<storage_backend *> & storage, const std::string & sb_name)
{
	auto it = std::find_if(storage.begin(), storage.end(), [sb_name] (const auto & sb) { return sb->get_id() == sb_name; });

	if (it == storage.end())
		return nullptr;

	return *it;
}
