#include <stdint.h>
#include <stdlib.h>


class hash
{
public:
	hash();
	virtual ~hash();

	virtual int get_size() const = 0;

	virtual void do_hash(const uint8_t *const in, const size_t len, uint8_t **const out) = 0;
};
