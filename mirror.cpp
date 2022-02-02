#include <yaml-cpp/yaml.h>

#include "logging.h"
#include "mirror.h"
#include "mirror_storage_backend.h"
#include "str.h"


mirror::mirror(const std::string & id) : base(id)
{
}

mirror::~mirror()
{
}

mirror * mirror::load_configuration(const YAML::Node & node)
{
	dolog(ll_info, " * mirror::load_configuration");

        const std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "mirror-storage-backend")
		return mirror_storage_backend::load_configuration(node);

	dolog(ll_error, "mirror::load_configuration: mirror type \"%s\" is not known", type.c_str());

	return nullptr;
}
