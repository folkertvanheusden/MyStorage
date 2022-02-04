#include <stdlib.h>
#include <string.h>
#include <vector>

#include "block.h"
#include "str.h"


block::block(const uint8_t *const data, const size_t len, const bool do_free) : data(data), len(len), do_free(do_free)
{
}

block::block(const uint8_t *const data, const size_t len) : data(data), len(len), do_free(true)
{
}

// TODO: get rid of this constructor variant (slow & ugly)
block::block(const std::vector<uint8_t> & data) : data(reinterpret_cast<uint8_t *>(malloc(data.size()))), len(data.size()), do_free(true)
{
	if (!this->data)
		throw myformat("block: cannot allocate %zu bytes of memory", data.size());

	memcpy(const_cast<uint8_t *>(this->data), data.data(), data.size());
}

block::block(const block & other) : data(reinterpret_cast<uint8_t *>(malloc(other.get_size()))), len(other.get_size()), do_free(true)
{
	if (!this->data)
		throw myformat("block: cannot allocate %zu bytes of memory", other.get_size());

	memcpy(const_cast<uint8_t *>(this->data), other.get_data(), other.get_size());
}
							    
block::~block()
{
	if (do_free)
		free(const_cast<uint8_t *>(data));
}

bool block::empty() const
{
	return len == 0;
}

size_t block::get_size() const
{
	return len;
}

const uint8_t * block::get_data() const
{
	return data;
}

bool block::operator==(const block & other) const
{
	if (other.get_size() != get_size())
		return false;

	return memcmp(other.get_data(), get_data(), get_size()) == 0;
}
