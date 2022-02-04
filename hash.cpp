#include <assert.h>
#include <optional>
#include <stdint.h>
#include <arpa/inet.h>
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

// from https://en.wikipedia.org/wiki/MurmurHash
// with a minor change to make it endiannes-safe
static inline uint32_t murmur_32_scramble(uint32_t k)
{
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    return k;
}

uint32_t murmur3_32(const uint8_t* key, size_t len, uint32_t seed)
{
	uint32_t h = seed;
	uint32_t k = 0;

	/* Read in groups of 4. */
	for (size_t i = len >> 2; i; i--) {
		// // Here is a source of differing results across endiannesses.
		// // A swap here has no effects on hash properties though.
		// memcpy(&k, key, sizeof(uint32_t));
		// htonl introduced by fvh
		k = htonl(*reinterpret_cast<const uint32_t *>(key));

		key += sizeof(uint32_t);
		h ^= murmur_32_scramble(k);
		h = (h << 13) | (h >> 19);
		h = h * 5 + 0xe6546b64;
	}

	/* Read the rest. */
	k = 0;
	for (size_t i = len & 3; i; i--) {
		k <<= 8;
		k |= key[i - 1];
	}

	// A swap is *not* necessary here because the preceding loop already
	// places the low bytes in the low places according to whatever endianness
	// we use. Swaps only apply when the memory is copied in a chunk.
	h ^= murmur_32_scramble(k);
	/* Finalize. */
	h ^= len;
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;

	return h;
}
