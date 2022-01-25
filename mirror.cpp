#include "mirror.h"


mirror::mirror(const std::string & id) : id(id)
{
}

mirror::~mirror()
{
}

std::string mirror::get_id()
{
	return id;
}
