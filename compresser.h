#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <yaml-cpp/yaml.h>


class compresser
{
public:
	compresser();
	virtual ~compresser();

	virtual std::string get_type() const = 0;

	virtual bool compress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) = 0;
	virtual bool decompress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) = 0;

	virtual YAML::Node emit_configuration() const = 0;
	static compresser * load_configuration(const YAML::Node & node);
};
