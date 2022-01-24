#include "hash.h"


class hash_sha384 : public hash
{
public:
	hash_sha384();
	~hash_sha384();

	int get_size() const override;

	void do_hash(const uint8_t *const in, const size_t len, uint8_t **const out) override;
};
