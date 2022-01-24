#pragma once
#include <string>

#include "block.h"
#include "types.h"


class mirror
{
private:
	const std::string id;

public:
	mirror(const std::string & id);
	virtual ~mirror();

	std::string get_id();

	virtual bool put_block(const offset_t o, const block & b) = 0;

	// used for async mirrors
	virtual bool sync() = 0;

	virtual bool trim_zero(const offset_t offset, const uint32_t len, const bool trim) = 0;
};
