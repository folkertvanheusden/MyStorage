#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "error.h"
#include "storage_backend_file.h"


storage_backend_file::storage_backend_file(const std::string & file)
{
	fd = open(file.c_str(), O_RDWR);
	if (fd == -1)
		error_exit(true, "storage_backend_file: failed to access \"%s\"", file.c_str());

	struct stat st { 0 };
	if (fstat(fd, &st) == -1)
		error_exit(true, "storage_backend_file: failed to retrieve meta data from \"%s\"", file.c_str());

	if (st.st_size & 4095)
		error_exit(true, "storage_backend_file: file not a multiple of 4096 in size (%ld)", st.st_size);

	size = st.st_size / 4096;
}

storage_backend_file::~storage_backend_file()
{
	close(fd);
}

offset_t storage_backend_file::get_size() const
{
	return size;
}

block * storage_backend_file::get_data(const offset_t offset, const uint32_t size)
{
	if (size == 0)
		error_exit(true, "storage_backend_file::get_data: requesting block of 0 bytes in size");

	if (lseek(fd, offset, SEEK_SET) == -1)
		error_exit(true, "storage_backend_file::get_data: failed to seek in file to offset %ld", offset);

	uint8_t *buffer = static_cast<uint8_t *>(malloc(size));
	if (read(fd, buffer, sizeof buffer) == -1)
		error_exit(true, "storage_backend_file::get_data: failed to read from file");

	return new block(buffer, size);
}

void storage_backend_file::put_data(const offset_t offset, const block & s)
{
	const uint8_t *p = s.get_data();
	const size_t len = s.get_size();

	if (lseek(fd, offset, SEEK_SET) == -1)
		error_exit(true, "storage_backend_file::put_data: failed to seek in file to offset %ld", offset);

	if (write(fd, p, len) == -1)
		error_exit(true, "storage_backend_file::put_data: failed to write (%zu bytes) to file at offset %lu", len, offset);
}

void storage_backend_file::fsync()
{
	if (fdatasync(fd) == -1)
		error_exit(true, "storage_backend_file::fsync: failed to sync data to disk");
}
