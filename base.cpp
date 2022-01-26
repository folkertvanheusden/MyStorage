#include "base.h"
#include "logging.h"
#include "str.h"


base::base(const std::string & id) : id(id)
{
}

base::~base()
{
	for(auto o : obj_used_by)
		dolog(ll_warning, "base::~base: object %s still in use by %s", get_id().c_str(), o->get_id().c_str());
}

void base::stop()
{
	stop_flag = true;
}

std::string base::get_id() const
{
	return id;
}

const std::set<const base *> & base::obj_in_use_by() const
{
	return obj_used_by;
}

void base::acquire(const base *const b)
{
	if (obj_used_by.find(b) != obj_used_by.end())
		throw myformat("base::acquire: object \"%s\" is already in use by \"%s\"", get_id().c_str(), b->get_id().c_str());

	obj_used_by.insert(b);
}

void base::release(const base *const b)
{
	if (obj_used_by.find(b) == obj_used_by.end())
		throw myformat("base::release: object \"%s\" not in use by \"%s\"", get_id().c_str(), b->get_id().c_str());

	obj_used_by.erase(b);
}
