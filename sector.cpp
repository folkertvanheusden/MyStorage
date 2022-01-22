#include "sector.h"


sector::sector(const uint8_t *const data) : block(data, 4096)
{
}

sector::~sector()
{
}
