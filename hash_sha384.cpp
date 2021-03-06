#include <crypto++/sha3.h>

#include "hash_sha384.h"
#include "logging.h"


// see https://stackoverflow.com/questions/47510106/compiling-bfx-cpp-api-byte-datatype-undeclared
#if defined(CRYPTOPP_NO_GLOBAL_BYTE)
	typedef unsigned char byte;
#endif

hash_sha384::hash_sha384()
{
}

hash_sha384::~hash_sha384()
{
}

YAML::Node hash_sha384::emit_configuration() const
{
        YAML::Node out;
        out["type"] = "hash-sha384";

        return out;
}

hash_sha384 * hash_sha384::load_configuration(const YAML::Node & node)
{
	dolog(ll_info, " * hash_sha384::load_configuration");

	return new hash_sha384();
}

int hash_sha384::get_size() const
{
	return CryptoPP::SHA3_384::DIGESTSIZE;
}

void hash_sha384::do_hash(const uint8_t *const in, const size_t len, uint8_t **const out)
{
	constexpr int size = CryptoPP::SHA3_384::DIGESTSIZE;
	*out = reinterpret_cast<uint8_t *>(malloc(size));

	if (out)
		CryptoPP::SHA3_384().CalculateDigest(reinterpret_cast<byte *>(*out), in, len);
	else
		dolog(ll_error, "hash_sha384::do_hash: cannot allocate %d bytes of memory", size);
}
