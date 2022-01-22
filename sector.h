#pragma once
#include "block.h"


class sector : public block
{
private:

public:
	sector(const uint8_t *const data);
	virtual ~sector();
};
