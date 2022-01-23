#include <stdint.h>
#include <stdlib.h>


class compresser
{
public:
	compresser();
	virtual ~compresser();

	virtual bool compress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) = 0;
	virtual bool decompress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) = 0;
};
