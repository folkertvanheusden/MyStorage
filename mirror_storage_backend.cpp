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

bool mirror_storage_backend::put_block(const offset_t o, const block & b)
{
	int err = 0;
	sb->put_data(o, b, &err);

	if (err) {
		dolog(ll_error, "mirror_storage_backend::put_block: cannot put block, reason: %s", strerror(err));
		return false;
	}

	return true;
}

bool mirror_storage_backend::sync()
{
	if (sb->fsync() == false) {
		dolog(ll_error, "mirror_storage_backend::put_block: cannot fsync");
		return false;
	}

	return true;
}
