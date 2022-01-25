#include <stdlib.h>
#include <string.h>

#include "block.h"


block::block(const uint8_t *const data, const size_t len) : data(data), len(len)
{
}

block::~block()
{
	free(const_cast<uint8_t *>(data));
}

size_t block::get_size() const
{
	return len;
}

const uint8_t * block::get_data() const
{
	return data;
}
