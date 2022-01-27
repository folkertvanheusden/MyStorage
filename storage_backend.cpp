#include "logging.h"
#include "storage_backend.h"
#include "storage_backend_compressed_dir.h"
#include "storage_backend_dedup.h"
#include "storage_backend_file.h"
#include "str.h"


storage_backend::storage_backend(const std::string & id, const std::vector<mirror *> & mirrors) : base(id), mirrors(mirrors)
{
	for(auto m : mirrors)
		m->acquire(this);
}

storage_backend::~storage_backend()
{
	for(auto m : mirrors) {
		m->release(this);

		if (m->obj_in_use_by().empty())
			delete m;
	}
}

storage_backend * storage_backend::load_configuration(const YAML::Node & node)
{
        const std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "storage-backend-file")
		return storage_backend_file::load_configuration(node);
	else if (type == "storage-backend-dedup")
		return storage_backend_dedup::load_configuration(node);
	else if (type == "storage-backend-compressed-dir")
		return storage_backend_compressed_dir::load_configuration(node);

	dolog(ll_error, "storage_backend::load_configuration: storage type \"%s\" is not known", type.c_str());

	return nullptr;
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

bool storage_backend::do_trim_zero(const offset_t offset, const uint32_t size, const bool trim)
{
	bool ok = true;

	for(auto m : mirrors) {
		if (m->trim_zero(offset, size, trim) == false) {
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
