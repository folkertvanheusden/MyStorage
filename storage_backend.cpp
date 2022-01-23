#include "storage_backend.h"


storage_backend::storage_backend()
{
}

storage_backend::~storage_backend()
{
}

void storage_backend::put_data(const offset_t offset, const std::vector<uint8_t> & d)
{
	block b(d.data(), d.size());

	put_data(offset, b);
}
