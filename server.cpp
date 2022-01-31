#include <yaml-cpp/yaml.h>

#include "aoe.h"
#include "nbd.h"
#include "server.h"
#include "str.h"


server::server(const std::string & id) : base(id)
{
}

server::~server()
{
}

server * server::load_configuration(const YAML::Node & node, const std::vector<storage_backend *> & storage)
{
	std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "nbd")
		return nbd::load_configuration(node, storage);

	if (type == "aoe")
		return aoe::load_configuration(node, storage);

	throw myformat("load_configuration: server type \"%s\" is unknown", type.c_str());
}

storage_backend * find_storage(const std::vector<storage_backend *> & storage, const std::string & sb_name)
{
	for(auto s : storage) {
		if (s->get_id() == sb_name)
			return s;
	}

	return nullptr;
}
