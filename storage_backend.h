#pragma once
#include <stdint.h>
#include <vector>

#include "block.h"
#include "types.h"


class storage_backend
{
public:
	storage_backend();
	virtual ~storage_backend();

	virtual offset_t get_size() const = 0;
	virtual block * get_data(const offset_t offset, const uint32_t size) = 0;
	virtual void put_data(const offset_t offset, const block & b) = 0;
	void put_data(const offset_t offset, const std::vector<uint8_t> & d);

	virtual void fsync() = 0;
};
