#include "compresser.h"
#include "compresser_lzo.h"
#include "compresser_zlib.h"
#include "logging.h"
#include "str.h"


compresser::compresser()
{
}

compresser::~compresser()
{
}

compresser * compresser::load_configuration(const YAML::Node & node)
{
        const std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "compresser-lzo")
		return compresser_lzo::load_configuration(node);

	if (type == "compresser-zlib")
		return compresser_zlib::load_configuration(node);

	dolog(ll_error, "compresser::load_configuration: compresser type \"%s\" is not known", type.c_str());

	return nullptr;
}
