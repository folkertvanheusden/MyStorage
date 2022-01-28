#pragma once
#include <optional>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <yaml-cpp/yaml.h>


class hash
{
public:
	hash();
	virtual ~hash();

	virtual int get_size() const = 0;

	// return binary
	virtual void do_hash(const uint8_t *const in, const size_t len, uint8_t **const out) = 0;

	// return hex string
	std::optional<std::string> do_hash(const uint8_t *const in, const size_t len);

	virtual YAML::Node emit_configuration() const = 0;
	static hash * load_configuration(const YAML::Node & node);
};

// acoording to the libcrypt++ documentation, this uses polynomial 0xEDB88320
uint32_t calc_crc(const uint8_t *const in, const size_t len);
