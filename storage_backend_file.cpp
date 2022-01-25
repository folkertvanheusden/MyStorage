#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "error.h"
#include "io.h"
#include "logging.h"
#include "storage_backend_file.h"


storage_backend_file::storage_backend_file(const std::string & id, const std::string & file, const std::vector<mirror *> & mirrors) : storage_backend(id, mirrors)
{
	fd = open(file.c_str(), O_RDWR);
	if (fd == -1)
		error_exit(true, "storage_backend_file(%s): failed to access \"%s\"", id.c_str(), file.c_str());

	struct stat st { 0 };
	if (fstat(fd, &st) == -1)
		error_exit(true, "storage_backend_file(%s): failed to retrieve meta data from \"%s\"", id.c_str(), file.c_str());

	if (st.st_size & 4095)
		error_exit(true, "storage_backend_file(%s): file not a multiple of 4096 in size (%ld)", id.c_str(), st.st_size);

	size = st.st_size;

	dolog(ll_debug, "storage_backend_file(%s): size is %zu bytes", id.c_str(), size);
}

storage_backend_file::~storage_backend_file()
{
	close(fd);
}

offset_t storage_backend_file::get_size() const
{
	return size;
}

void storage_backend_file::get_data(const offset_t offset, const uint32_t size, block **const b, int *const err)
{
	*err = 0;
	*b = nullptr;

	if (size == 0) {
		*err = EINVAL;
		dolog(ll_error, "storage_backend_file::get_data(%s): requesting block of 0 bytes in size", id.c_str());
		return;
	}

	if (lseek(fd, offset, SEEK_SET) == -1) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::get_data(%s): failed to seek in file to offset %ld", id.c_str(), offset);
		return;
	}

	uint8_t *buffer = static_cast<uint8_t *>(malloc(size));
	if (READ(fd, buffer, size) != size) {
		*err = errno;
		free(buffer);
		dolog(ll_error, "storage_backend_file::get_data(%s): failed to read from file", id.c_str());
		return;
	}

	*b = new block(buffer, size);

	free(buffer);
}

void storage_backend_file::put_data(const offset_t offset, const block & b, int *const err)
{
	const uint8_t *p = b.get_data();
	const ssize_t len = b.get_size();

	*err = 0;

	if (lseek(fd, offset, SEEK_SET) == -1) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::put_data(%s): failed to seek in file to offset %ld", id.c_str(), offset);
		return;
	}

	if (WRITE(fd, p, len) != len) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::put_data(%s): failed to write (%zu bytes) to file at offset %lu", id.c_str(), len, offset);
		return;
	}

	if (do_mirror(offset, b) == false) {
		*err = EIO;
		dolog(ll_error, "storage_backend_file::put_data(%s): failed to send block (%zu bytes) to mirror(s) at offset %lu", id.c_str(), len, offset);
		return;
	}

	if (do_mirror(offset, b) == false) {
		*err = EIO;
		dolog(ll_error, "storage_backend_file::put_data(%s): failed to send block (%zu bytes) to mirror(s) at offset %lu", id.c_str(), len, offset);
		return;
	}
}

bool storage_backend_file::fsync()
{
	if (fdatasync(fd) == -1) {
		dolog(ll_error, "storage_backend_file::fsync(%s): failed to sync data to disk", id.c_str());
		return false;
	}

	if (do_sync_mirrors() == false) {
		dolog(ll_error, "storage_backend_file::fsync(%s): failed to sync data to mirror(s)", id.c_str());
		return false;
	}

	return true;
}

bool storage_backend_file::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

#ifdef linux
	if (fallocate(fd, (trim ? FALLOC_FL_PUNCH_HOLE : FALLOC_FL_ZERO_RANGE) | FALLOC_FL_KEEP_SIZE, offset, len) == -1) {
		dolog(ll_error, "storage_backend_file::trim(%s): failed to trim (%zu bytes) at offset %lu", id.c_str(), len, offset);
		*err = errno;
	}
#else
	if (lseek(fd, offset, SEEK_SET) == -1) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::trim(%s): failed to seek in file to offset %ld", id.c_str(), offset);
		return false;
	}

	uint8_t buffer[4096] { 0 };

	uint32_t work_len = len;
	offset_t current_offset = offset;
	while(work_len > 0) {
		uint32_t current_len = std::min(uint32_t(sizeof buffer), work_len);

		if (WRITE(fd, buffer, current_len) != current_len) {
			*err = errno;
			dolog(ll_error, "storage_backend_file::trim(%s): failed to write (%zu bytes) to file at offset %lu", id.c_str(), current_len, current_offset);
			return false;
		}

		work_len -= current_len;
		current_offset += current_len;
	}
#endif

	if (do_trim_zero(offset, len) == false) {
		dolog(ll_error, "storage_backend_file::trim_zero(%s): failed to send to mirror(s)", id.c_str());
		return false;
	}

	return true;
}
