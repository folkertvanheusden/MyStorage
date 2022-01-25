#include "logging.h"
#include "storage_backend.h"


storage_backend::storage_backend(const std::string & id, const std::vector<mirror *> & mirrors) : id(id), mirrors(mirrors)
{
}

storage_backend::~storage_backend()
{
}

bool storage_backend::verify_mirror_sizes()
{
	for(auto m : mirrors) {
		if (m->get_size() < get_size()) {
			dolog(ll_error, "storage_backend::verify_mirror_sizes(%s): mirror %s is too small", id.c_str(), m->get_id().c_str());
			return false;
		}
	}

	return true;
}

bool storage_backend::do_mirror(const offset_t offset, const block & b)
{
	bool ok = true;

	for(auto m : mirrors) {
		if (m->put_block(offset, b) == false) {
			ok = false;
			dolog(ll_error, "storage_backend::do_mirror(%s): failed writing to mirror %s", id.c_str(), m->get_id().c_str());
		}
	}

	return ok;
}

bool storage_backend::do_sync_mirrors()
{
	bool ok = true;

	for(auto m : mirrors) {
		if (m->sync() == false) {
			ok = false;
			dolog(ll_error, "storage_backend::do_sync_mirrors(%s): failed syncing mirror %s", id.c_str(), m->get_id().c_str());
		}
	}

	return ok;
}

bool storage_backend::do_trim_zero(const offset_t offset, const uint32_t size)
{
	bool ok = true;

	for(auto m : mirrors) {
		if (m->sync() == false) {
			ok = false;
			dolog(ll_error, "storage_backend::do_trim_zero(%s): failed trim/zero mirror %s", id.c_str(), m->get_id().c_str());
		}
	}

	return ok;
}

void storage_backend::put_data(const offset_t offset, const std::vector<uint8_t> & d, int *const err)
{
	*err = 0;

	block b(d);

	put_data(offset, b, err);
}

std::string storage_backend::get_identifier() const
{
	return id;
}
