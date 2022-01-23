#include <stdint.h>

#include "compresser.h"


class compresser_zlib : public compresser
{
private:
	const int compression_level;

public:
	compresser_zlib(const int compression_level);
	virtual ~compresser_zlib();

	virtual bool compress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) = 0;
	virtual bool decompress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) = 0;
};
