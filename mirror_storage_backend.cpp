#include <string.h>

#include "logging.h"
#include "mirror_storage_backend.h"
#include "storage_backend.h"
#include "types.h"


mirror_storage_backend::mirror_storage_backend(const std::string & id, storage_backend *const sb) : mirror(id), sb(sb)
{
}

mirror_storage_backend::~mirror_storage_backend()
{
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
