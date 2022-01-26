#include <stdlib.h>
#include <string.h>
#include <vector>

#include "block.h"
#include "str.h"


block::block(const uint8_t *const data, const size_t len) : data(data), len(len)
{
}

// TODO: get rid of this constructor variant (slow & ugly)
block::block(const std::vector<uint8_t> & data) : data(reinterpret_cast<uint8_t *>(malloc(data.size()))), len(data.size())
{
	if (!this->data)
		throw myformat("block: cannot allocate %zu bytes of memory", data.size());

	memcpy(const_cast<uint8_t *>(this->data), data.data(), data.size());
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
