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

	n_sectors = st.st_size / 4096;
}

storage_backend_file::~storage_backend_file()
{
	close(fd);
}

uint64_t storage_backend_file::get_n_sectors() const
{
	return n_sectors;
}

sector storage_backend_file::get_sector(const uint64_t s_nr)
{
	if (lseek(fd, s_nr * 4096, SEEK_SET) == -1)
		error_exit(true, "storage_backend_file::get_sector: failed to seek in file to offset %ld", s_nr * 4096);

	uint8_t buffer[4096] { 0 };
	if (read(fd, buffer, sizeof buffer) == -1)
		error_exit(true, "storage_backend_file::get_sector: failed to read from file");

	return sector(buffer);
}

void storage_backend_file::put_sector(const uint64_t s_nr, const sector & s)
{
	uint8_t buffer[4096] { 0 };
	s.get_data(buffer);

	if (lseek(fd, s_nr * 4096, SEEK_SET) == -1)
		error_exit(true, "storage_backend_file::put_sector: failed to seek in file to offset %ld", s_nr * 4096);

	if (write(fd, buffer, sizeof buffer) == -1)
		error_exit(true, "storage_backend_file::get_sector: failed to write to file");
}

void storage_backend_file::fsync()
{
	if (fdatasync(fd) == -1)
		error_exit(true, "storage_backend_file::get_sector: failed to sync data to disk");
}
