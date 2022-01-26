#include <crypto++/sha3.h>

#include "hash_sha384.h"
#include "logging.h"


hash_sha384::hash_sha384()
{
}

hash_sha384::~hash_sha384()
{
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
		CryptoPP::SHA3_384().CalculateDigest(reinterpret_cast<CryptoPP::byte *>(*out), in, len);
	else
		dolog(ll_error, "hash_sha384::do_hash: cannot allocate %d bytes of memory", size);
}
