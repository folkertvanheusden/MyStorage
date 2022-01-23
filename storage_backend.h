#pragma once
#include <stdint.h>

#include "block.h"


class storage_backend
{
public:
	storage_backend();
	virtual ~storage_backend();

	virtual uint64_t get_size() const = 0;
	virtual block * get_data(const uint64_t offset, const uint32_t size) = 0;
	virtual void put_data(const uint64_t offset, const block & b) = 0;

	virtual void fsync() = 0;
};
