#pragma once
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "base.h"
#include "storage_backend.h"


class server : public base
{
public:
	server(const std::string & id);
	virtual ~server();

	static server * load_configuration(const YAML::Node & node, const std::vector<storage_backend *> & storage);
};

storage_backend * find_storage(const std::vector<storage_backend *> & storage, const std::string & sb_name);
