#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "compresser.h"
#include "error.h"
#include "io.h"
#include "logging.h"
#include "storage_backend_compressed_dir.h"
#include "str.h"


storage_backend_compressed_dir::storage_backend_compressed_dir(const std::string & id, const std::string & dir, const int block_size, const offset_t total_size, compresser *const c) : storage_backend(id), dir(dir), block_size(block_size), total_size(total_size), c(c)
{
}

storage_backend_compressed_dir::~storage_backend_compressed_dir()
{
}

offset_t storage_backend_compressed_dir::get_size() const
{
	return total_size;
}

bool storage_backend_compressed_dir::get_block(const uint64_t block_nr, uint8_t **const data)
{
	std::string file = myformat("%s/%ld", dir.c_str(), block_nr);

	int fd = open(file.c_str(), O_RDONLY);
	if (fd == -1) {
		// a block that does not exist only contains 0x00
		if (errno == ENOENT) {
			*data = reinterpret_cast<uint8_t *>(calloc(1, block_size));
			return true;
		}

		dolog(ll_error, "storage_backend_compressed_dir::get_block: failed to access \"%s\": %s", file.c_str(), strerror(errno));
		return false;
	}

	struct stat st { 0 };
	if (fstat(fd, &st) == -1) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block: fstat on \"%s\" failed: %s", file.c_str(), strerror(errno));
		close(fd);
		return false;
	}

	uint8_t *temp = reinterpret_cast<uint8_t *>(malloc(st.st_size));
	if (READ(fd, temp, st.st_size) != st.st_size) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block: short read on \"%s\"", file.c_str());
		close(fd);
		return false;
	}

	size_t out_len = 0;
	bool rc = c->decompress(temp, st.st_size, data, &out_len);

	close(fd);
	free(temp);

	if (rc == false) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block: failed to decompress block \"%s\"", file.c_str());
		return false;
	}

	if (out_len != size_t(block_size)) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block: failed to decompress block \"%s\": output size (%zu) incorrect", file.c_str(), out_len);
		free(*data);
		return false;
	}

	close(fd);

	return true;
}

bool storage_backend_compressed_dir::put_block(const uint64_t block_nr, const uint8_t *const data_in)
{
	uint8_t *data_out = nullptr;
	size_t out_len = 0;
	bool rc = c->compress(data_in, block_size, &data_out, &out_len);

	if (rc == false) {
		dolog(ll_error, "storage_backend_compressed_dir::put_block: failed to compress block \"%lu\"", block_nr);
		return false;
	}

	std::string file = myformat("%s/%ld", dir.c_str(), block_nr);

	int fd = open(file.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		dolog(ll_error, "storage_backend_compressed_dir::put_block: failed to access \"%s\": %s", file.c_str(), strerror(errno));
		return false;
	}

	dolog(ll_debug, "storage_backend_compressed_dir::put_block: writing %zu byte(s) to \"%s\"", out_len, file.c_str());

	if (WRITE(fd, data_out, out_len) != ssize_t(out_len)) {
		dolog(ll_error, "storage_backend_compressed_dir::put_block: failed to write to \"%s\": %s", file.c_str(), strerror(errno));
		free(data_out);
		close(fd);
		return false;
	}

	free(data_out);

	close(fd);

	return true;
}

void storage_backend_compressed_dir::un_lock_block_group(const offset_t offset, const uint32_t size, const bool do_lock, const bool shared)
{
	std::vector<uint64_t> block_nrs;

	for(offset_t o=offset; o<offset + size; o += block_size)
		block_nrs.push_back(o);

	if (do_lock) {
		if (shared)
			lg.lock_shared(block_nrs);
		else
			lg.lock_private(block_nrs);
	}
	else {
		if (shared)
			lg.unlock_shared(block_nrs);
		else
			lg.unlock_private(block_nrs);
	}
}

void storage_backend_compressed_dir::get_data(const offset_t offset, const uint32_t size, block **const b, int *const err)
{
	*err = 0;

	un_lock_block_group(offset, size, true, true);

	uint8_t *out = nullptr;
	uint32_t out_size = 0;

	offset_t work_offset = offset;
	uint32_t work_size = size;

	while(work_size > 0) {
		uint64_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		uint32_t current_size = std::min(work_size, block_size - block_offset);

		uint8_t *temp = nullptr;

		if (!get_block(block_nr, &temp)) {
			dolog(ll_error, "storage_backend_compressed_dir::get_data: failed to retrieve block %ld", block_nr);
			*err = EINVAL;
			free(out);
			break;
		}

		out = reinterpret_cast<uint8_t *>(realloc(out, out_size + current_size));
		memcpy(&out[out_size], &temp[block_offset], current_size);
		out_size += current_size;

		free(temp);

		work_offset += current_size;
		work_size -= current_size;
	}

	*b = new block(out, out_size);

	free(out);

	un_lock_block_group(offset, size, false, true);
}

void storage_backend_compressed_dir::put_data(const offset_t offset, const block & s, int *const err)
{
	*err = 0;

	un_lock_block_group(offset, s.get_size(), true, false);

	offset_t work_offset = offset;

	const uint8_t *input = s.get_data();
	size_t work_size = s.get_size();

	while(work_size > 0) {
		uint64_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		uint32_t current_size = std::min(work_size, size_t(block_size - block_offset));

		uint8_t *temp = nullptr;

		// TODO if offset == 0 && current_size == block_size, do a calloc instead
		if (!get_block(block_nr, &temp)) {
			dolog(ll_error, "storage_backend_compressed_dir::put_data: failed to retrieve block %ld", block_nr);
			*err = EINVAL;
			break;
		}

		memcpy(&temp[block_offset], input, current_size);

		if (!put_block(block_nr, temp)) {
			dolog(ll_error, "storage_backend_compressed_dir::put_data: failed to update block %ld", block_nr);
			*err = EINVAL;
			free(temp);
			break;
		}

		free(temp);

		work_offset += current_size;
		work_size -= current_size;
		input += current_size;
	}

	un_lock_block_group(offset, s.get_size(), false, false);
}

void storage_backend_compressed_dir::fsync()
{
	// TODO sync directory
}

bool storage_backend_compressed_dir::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	uint8_t *zero = reinterpret_cast<uint8_t *>(calloc(1, len));

	*err = 0;

	// TODO: delete files if possible
	storage_backend_compressed_dir::put_data(offset, block(zero, len), err);

	free(zero);

	return *err != 0;
}