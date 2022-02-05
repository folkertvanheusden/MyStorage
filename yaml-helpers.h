#include <map>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "base.h"
#include "server.h"
#include "storage_backend.h"


std::string yaml_get_string(const YAML::Node & node, const std::string & key, const std::string & description);
int yaml_get_int(const YAML::Node & node, const std::string & key, const std::string & description);
uint64_t yaml_get_uint64_t(const YAML::Node & node, const std::string & key, const std::string & description, const bool units);
const YAML::Node yaml_get_yaml_node(const YAML::Node & node, const std::string & key, const std::string & description);
bool yaml_get_bool(const YAML::Node & node, const std::string & key, const std::string & description);

void store_configuration(const std::vector<server *> & servers, const std::vector<storage_backend *> & storage, const std::string & file);
std::map<std::string, std::vector<base *> > load_configuration(const std::string & file);
