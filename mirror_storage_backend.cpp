#include <string.h>

#include "logging.h"
#include "mirror_storage_backend.h"
#include "storage_backend.h"
#include "types.h"


mirror_storage_backend::mirror_storage_backend(const std::string & id, storage_backend *const sb) : mirror(id), sb(sb)
{
	sb->acquire(this);
}

mirror_storage_backend::~mirror_storage_backend()
{
	sb->release(this);

	if (sb->obj_in_use_by().empty())
		delete sb;
}

YAML::Node mirror_storage_backend::emit_configuration() const
{
	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["storage-backend"] = sb->emit_configuration();

	YAML::Node out;
	out["type"] = "mirror-storage-backend";
	out["cfg"] = out_cfg;

	return out;
}

mirror_storage_backend * mirror_storage_backend::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string id = cfg["id"].as<std::string>();

	storage_backend *sb = storage_backend::load_configuration(cfg["storage-backend"]);

	return new mirror_storage_backend(id, sb);
}

offset_t mirror_storage_backend::get_size() const
{
	return sb->get_size();
}

bool mirror_storage_backend::put_block(const offset_t o, const block & b)
{
	int err = 0;
	sb->put_data(o, b, &err);

	if (err) {
		dolog(ll_error, "mirror_storage_backend::put_block(%s): cannot put block, reason: %s", id.c_str(), strerror(err));
		return false;
	}

	return true;
}

bool mirror_storage_backend::sync()
{
	if (sb->fsync() == false) {
		dolog(ll_error, "mirror_storage_backend::put_block(%s): cannot fsync", id.c_str());
		return false;
	}

	return true;
}

bool mirror_storage_backend::trim_zero(const offset_t offset, const uint32_t len, const bool trim)
{
	int err = 0;
	if (sb->trim_zero(offset, len, trim, &err) == false) {
		dolog(ll_error, "mirror_storage_backend::trim_zero(%s): failed: %s", id.c_str(), strerror(err));
		return false;
	}

	return true;
}
