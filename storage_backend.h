#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "block.h"
#include "types.h"


class storage_backend
{
private:
	const std::string id;

public:
	storage_backend(const std::string & id);
	virtual ~storage_backend();

	std::string get_identifier() const;
	virtual offset_t get_size() const = 0;

	virtual void get_data(const offset_t offset, const uint32_t size, block **const b, int *const err) = 0;
	virtual void put_data(const offset_t offset, const block & b, int *const err) = 0;
	void put_data(const offset_t offset, const std::vector<uint8_t> & d, int *const err);

	virtual bool fsync() = 0;

	virtual bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) = 0;
};
