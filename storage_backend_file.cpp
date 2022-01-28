#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "error.h"
#include "io.h"
#include "logging.h"
#include "storage_backend_file.h"
#include "str.h"


storage_backend_file::storage_backend_file(const std::string & id, const std::string & file, const int block_size, const std::vector<mirror *> & mirrors) : storage_backend(id, block_size, mirrors), file(file)
{
	fd = open(file.c_str(), O_RDWR);
	if (fd == -1)
		throw myformat("storage_backend_file(%s): failed to access \"%s\": %s", id.c_str(), file.c_str(), strerror(errno));

	struct stat st { 0 };
	if (fstat(fd, &st) == -1)
		throw myformat("storage_backend_file(%s): failed to retrieve meta data from \"%s\": %s", id.c_str(), file.c_str(), strerror(errno));

	if (st.st_size & 4095)
		throw myformat("storage_backend_file(%s): file not a multiple of 4096 in size (%ld)", id.c_str(), st.st_size);

	size = st.st_size;

	dolog(ll_debug, "storage_backend_file(%s): size is %zu bytes", id.c_str(), size);

	if (!verify_mirror_sizes())
		throw myformat("storage_backend_compressed_dir(%s): mirrors sanity check failed", id.c_str());
}

storage_backend_file::~storage_backend_file()
{
	close(fd);
}

storage_backend_file * storage_backend_file::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string id = cfg["id"].as<std::string>();

	std::vector<mirror *> mirrors;
	YAML::Node y_mirrors = cfg["mirrors"];
	for(YAML::const_iterator it = y_mirrors.begin(); it != y_mirrors.end(); it++)
		mirrors.push_back(mirror::load_configuration(it->as<YAML::Node>()));

	std::string file = cfg["file"].as<std::string>();
	int block_size = cfg["block-size"].as<int>();

	return new storage_backend_file(id, file, block_size, mirrors);
}

YAML::Node storage_backend_file::emit_configuration() const
{
	std::vector<YAML::Node> out_mirrors;
	for(auto m : mirrors)
		out_mirrors.push_back(m->emit_configuration());

	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["file"] = file;
	out_cfg["mirrors"] = out_mirrors;
	out_cfg["block-size"] = block_size;

	YAML::Node out;
	out["type"] = "storage-backend-file";
	out["cfg"] = out_cfg;

	return out;
}

offset_t storage_backend_file::get_size() const
{
	return size;
}

bool storage_backend_file::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	const offset_t offset = block_nr * block_size;
	if (lseek(fd, offset, SEEK_SET) == -1) {
		dolog(ll_error, "storage_backend_file::get_block(%s): failed to seek in file to offset %ld", id.c_str(), offset);
		return false;
	}

	*data = static_cast<uint8_t *>(malloc(block_size));
	if (!*data) {
		dolog(ll_error, "storage_backend_file::get_block(%s): cannot allocated %u bytes of memory", id.c_str(), size);
		return false;
	}

	int rc = READ(fd, *data, block_size);
	if (rc != block_size) {
		free(*data);
		dolog(ll_error, "storage_backend_file::get_block(%s): failed to read from file at offset %ld: expected %d, got %d", id.c_str(), offset, block_size, rc);
		return false;
	}

	return true;
}

bool storage_backend_file::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	const offset_t offset = block_nr * block_size;
	if (lseek(fd, offset, SEEK_SET) == -1) {
		dolog(ll_error, "storage_backend_file::put_block(%s): failed to seek in file to offset %ld", id.c_str(), offset);
		return false;
	}

	if (WRITE(fd, data, block_size) != block_size) {
		dolog(ll_error, "storage_backend_file::put_block(%s): failed to write (%zu bytes) to file at offset %lu", id.c_str(), block_size, offset);
		return false;
	}

	return true;
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

	if (do_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "storage_backend_file::trim_zero(%s): failed to send to mirror(s)", id.c_str());
		return false;
	}

	return true;
}
