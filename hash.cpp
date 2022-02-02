#include <assert.h>
#include <optional>
#include <stdint.h>
#include <crypto++/crc.h>

#include "hash.h"
#include "hash_sha384.h"
#include "logging.h"
#include "str.h"
#include "yaml-helpers.h"


hash::hash()
{
}

hash::~hash()
{
}

hash * hash::load_configuration(const YAML::Node & node)
{
	dolog(ll_info, " * hash::load_configuration");

	const std::string type = str_tolower(yaml_get_string(node, "type", "type of hash function"));

	if (type == "hash-sha384")
		return hash_sha384::load_configuration(node);

	dolog(ll_error, "hash::load_configuration: hash function \"%s\" is not known", type.c_str());

	return nullptr;
}

std::optional<std::string> hash::do_hash(const uint8_t *const in, const size_t len)
{
	uint8_t *new_hash = nullptr;
	do_hash(in, len, &new_hash);

	if (!new_hash) {
		dolog(ll_error, "hash::do_hash: failed hashing");
		return { };
	}

	int h_size = get_size();

	std::string out;
	out.resize(h_size * 2);

	for(int i=0; i<h_size; i++) {
		uint8_t nh = new_hash[i] >> 4;
		out.at(i * 2 + 0) = nh >= 10 ? 'a' + nh - 10 : '0' + nh;

		uint8_t nl = new_hash[i] & 15;
		out.at(i * 2 + 1) = nl >= 10 ? 'a' + nl - 10 : '0' + nl;
	}

	free(new_hash);

	return out;
}

uint32_t calc_crc(const uint8_t *const in, const size_t len)
{
	CryptoPP::CRC32 crc;

	crc.Update(in, len);

	assert(crc.DigestSize() == 4);

	return (crc.GetCrcByte(0) << 24) | (crc.GetCrcByte(1) << 16) | (crc.GetCrcByte(2) << 8) | crc.GetCrcByte(3);
}
