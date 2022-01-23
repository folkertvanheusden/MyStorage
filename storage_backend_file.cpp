#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "error.h"
#include "io.h"
#include "logging.h"
#include "storage_backend_file.h"


storage_backend_file::storage_backend_file(const std::string & id, const std::string & file) : storage_backend(id)
{
	fd = open(file.c_str(), O_RDWR);
	if (fd == -1)
		error_exit(true, "storage_backend_file: failed to access \"%s\"", file.c_str());

	struct stat st { 0 };
	if (fstat(fd, &st) == -1)
		error_exit(true, "storage_backend_file: failed to retrieve meta data from \"%s\"", file.c_str());

	if (st.st_size & 4095)
		error_exit(true, "storage_backend_file: file not a multiple of 4096 in size (%ld)", st.st_size);

	size = st.st_size;

	dolog(ll_debug, "storage_backend_file: size is %zu bytes", size);
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
		dolog(ll_error, "storage_backend_file::get_data: requesting block of 0 bytes in size");
		return;
	}

	if (lseek(fd, offset, SEEK_SET) == -1) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::get_data: failed to seek in file to offset %ld", offset);
		return;
	}

	uint8_t *buffer = static_cast<uint8_t *>(malloc(size));
	if (READ(fd, buffer, size) != size) {
		*err = errno;
		free(buffer);
		dolog(ll_error, "storage_backend_file::get_data: failed to read from file");
		return;
	}

	*b = new block(buffer, size);

	free(buffer);
}

void storage_backend_file::put_data(const offset_t offset, const block & s, int *const err)
{
	const uint8_t *p = s.get_data();
	const ssize_t len = s.get_size();

	*err = 0;

	if (lseek(fd, offset, SEEK_SET) == -1) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::put_data: failed to seek in file to offset %ld", offset);
		return;
	}

	if (WRITE(fd, p, len) != len) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::put_data: failed to write (%zu bytes) to file at offset %lu", len, offset);
		return;
	}
}

void storage_backend_file::fsync()
{
	if (fdatasync(fd) == -1)
		error_exit(true, "storage_backend_file::fsync: failed to sync data to disk");
}

bool storage_backend_file::trim(const offset_t offset, const uint32_t len, int *const err)
{
	*err = 0;

#ifdef linux
	if (fallocate(fd, FALLOC_FL_ZERO_RANGE, offset, size) == -1) {
		dolog(ll_error, "storage_backend_file::trim: failed to trim (%zu bytes) at offset %lu", len, offset);
		*err = errno;
	}
#else
	if (lseek(fd, offset, SEEK_SET) == -1) {
		*err = errno;
		dolog(ll_error, "storage_backend_file::trim: failed to seek in file to offset %ld", offset);
		return false;
	}

	uint8_t buffer[4096] { 0 };

	uint32_t work_len = len;
	offset_t current_offset = offset;
	while(work_len > 0) {
		uint32_t current_len = std::min(uint32_t(sizeof buffer), work_len);

		if (WRITE(fd, buffer, current_len) != current_len) {
			*err = errno;
			dolog(ll_error, "storage_backend_file::trim: failed to write (%zu bytes) to file at offset %lu", current_len, current_offset);
			return false;
		}

		work_len -= current_len;
		current_offset += current_len;
	}
#endif

	return true;
}
