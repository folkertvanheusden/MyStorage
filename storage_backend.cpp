#include "storage_backend.h"


storage_backend::storage_backend(const std::string & id) : id(id)
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

std::string storage_backend::get_identifier() const
{
	return id;
}
