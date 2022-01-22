#include <stdlib.h>
#include <string.h>

#include "block.h"


block::block(const uint8_t *const data, const size_t len) : data(static_cast<uint8_t *>(malloc(len))), len(len)
{
	memcpy(this->data, data, len);
}

block::~block()
{
	free(data);
}

size_t block::get_size() const
{
	return len;
}

void block::get_data(uint8_t *const target)
{
	memcpy(target, data, len);
}
