#pragma once
#include <string>
#include <yaml-cpp/yaml.h>

#include "base.h"
#include "block.h"
#include "types.h"


class mirror : public base
{
public:
	mirror(const std::string & id);
	virtual ~mirror();

	virtual offset_t get_size() const = 0;

	virtual bool put_block(const offset_t o, const block & b) = 0;

	// used for async mirrors
	virtual bool sync() = 0;

	virtual bool trim_zero(const offset_t offset, const uint32_t len, const bool trim) = 0;

	virtual YAML::Node emit_configuration() const = 0;
	static mirror * load_configuration(const YAML::Node & node);
};
