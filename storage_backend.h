#pragma once
#include <stdint.h>

#include "sector.h"


class storage_backend
{
public:
	storage_backend();
	virtual ~storage_backend();

	virtual uint64_t get_n_sectors() const = 0;
	virtual sector get_sector(const uint64_t s_nr) = 0;
	virtual void put_sector(const uint64_t s_nr, const sector & s) = 0;

	virtual void fsync() = 0;
};
