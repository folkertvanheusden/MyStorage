#include <optional>
#include <stdint.h>

#include "hash.h"
#include "logging.h"


hash::hash()
{
}

hash::~hash()
{
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
