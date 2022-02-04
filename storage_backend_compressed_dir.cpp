#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <optional>
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


storage_backend_compressed_dir::storage_backend_compressed_dir(const std::string & id, const std::string & dir, const int block_size, const offset_t total_size, compresser *const c, const std::vector<mirror *> & mirrors) : storage_backend(id, block_size, mirrors), dir(dir), total_size(total_size), c(c)
{
	dir_structure = opendir(dir.c_str());
	if (!dir_structure)
		throw myformat("storage_backend_compressed_dir(%s): failed to open directory \"%s\": %s", id.c_str(), dir.c_str(), strerror(errno));

	dir_fd = dirfd(dir_structure);
	if (dir_fd == -1)
		throw myformat("storage_backend_compressed_dir(%s): failed to get file descriptor for open directory \"%s\": %s", id.c_str(), dir.c_str(), strerror(errno));

	if (!verify_mirror_sizes())
		throw myformat("storage_backend_compressed_dir(%s): mirrors sanity check failed", id.c_str());
}

storage_backend_compressed_dir::~storage_backend_compressed_dir()
{
	close(dir_fd);

	closedir(dir_structure);

	delete c;
}

YAML::Node storage_backend_compressed_dir::emit_configuration() const
{
	std::vector<YAML::Node> out_mirrors;
	for(auto m : mirrors)
		out_mirrors.push_back(m->emit_configuration());

	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["mirrors"] = out_mirrors;
	out_cfg["directory"] = dir;
	out_cfg["block-size"] = block_size;
	out_cfg["size"] = total_size;
	out_cfg["compresser"] = c->emit_configuration();

	YAML::Node out;
	out["type"] = "storage-backend-compressed-dir";
	out["cfg"] = out_cfg;

	return out;
}

storage_backend_compressed_dir * storage_backend_compressed_dir::load_configuration(const YAML::Node & node, const std::optional<uint64_t> size)
{
	dolog(ll_info, " * socket_backend_compressed_dir::load_configuration");

	const YAML::Node cfg = node["cfg"];

	std::string id = cfg["id"].as<std::string>();

	std::vector<mirror *> mirrors;
	YAML::Node y_mirrors = cfg["mirrors"];
	for(YAML::const_iterator it = y_mirrors.begin(); it != y_mirrors.end(); it++)
		mirrors.push_back(mirror::load_configuration(it->as<YAML::Node>()));

	std::string directory = cfg["directory"].as<std::string>();
	int block_size = cfg["block-size"].as<int>();

	offset_t size_final = size.has_value() ? size.value() : cfg["size"].as<uint64_t>();

	compresser *c = compresser::load_configuration(cfg["compresser"]);

	return new storage_backend_compressed_dir(id, directory, block_size, size_final, c, mirrors);
}

bool storage_backend_compressed_dir::can_do_multiple_blocks() const
{
	return false;
}

offset_t storage_backend_compressed_dir::get_size() const
{
	return total_size;
}

bool storage_backend_compressed_dir::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	std::string file = myformat("%s/%ld", dir.c_str(), block_nr);

	int fd = open(file.c_str(), O_RDONLY);
	if (fd == -1) {
		// a block that does not exist only contains 0x00
		if (errno == ENOENT) {
			*data = reinterpret_cast<uint8_t *>(calloc(1, block_size));
			return true;
		}

		dolog(ll_error, "storage_backend_compressed_dir::get_block(%s): failed to access \"%s\": %s", id.c_str(), file.c_str(), strerror(errno));
		return false;
	}

	struct stat st { 0 };
	if (fstat(fd, &st) == -1) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block(%s): fstat on \"%s\" failed: %s", id.c_str(), file.c_str(), strerror(errno));
		close(fd);
		return false;
	}

	uint8_t *temp = reinterpret_cast<uint8_t *>(malloc(st.st_size));
	if (!temp) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block(%s): cannot allocate %ld bytes or memory: %s", id.c_str(), st.st_size, strerror(errno));
		close(fd);
		return false;
	}

	if (READ(fd, temp, st.st_size) != st.st_size) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block(%s): short read on \"%s\"", id.c_str(), file.c_str());
		close(fd);
		return false;
	}

	size_t out_len = 0;
	bool rc = c->decompress(temp, st.st_size, data, &out_len);

	close(fd);
	free(temp);

	if (rc == false) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block(%s): failed to decompress block \"%s\"", id.c_str(), file.c_str());
		return false;
	}

	if (out_len != size_t(block_size)) {
		dolog(ll_error, "storage_backend_compressed_dir::get_block(%s): failed to decompress block \"%s\": output size (%zu) incorrect", id.c_str(), file.c_str(), out_len);
		free(*data);
		return false;
	}

	close(fd);

	return true;
}

bool storage_backend_compressed_dir::put_block(const block_nr_t block_nr, const uint8_t *const data_in)
{
	uint8_t *data_out = nullptr;
	size_t out_len = 0;
	bool rc = c->compress(data_in, block_size, &data_out, &out_len);

	if (rc == false) {
		dolog(ll_error, "storage_backend_compressed_dir::put_block(%s): failed to compress block \"%lu\"", id.c_str(), block_nr);
		return false;
	}

	std::string file = myformat("%s/%ld", dir.c_str(), block_nr);

	int fd = open(file.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		dolog(ll_error, "storage_backend_compressed_dir::put_block(%s): failed to access \"%s\": %s", id.c_str(), file.c_str(), strerror(errno));
		return false;
	}

	dolog(ll_debug, "storage_backend_compressed_dir::put_block(%s): writing %zu byte(s) to \"%s\"", id.c_str(), out_len, file.c_str());

	if (WRITE(fd, data_out, out_len) != ssize_t(out_len)) {
		dolog(ll_error, "storage_backend_compressed_dir::put_block(%s): failed to write to \"%s\": %s", id.c_str(), file.c_str(), strerror(errno));
		free(data_out);
		close(fd);
		return false;
	}

	free(data_out);

	close(fd);

	return true;
}

bool storage_backend_compressed_dir::fsync()
{
	if (::fsync(dir_fd) == -1) {
		dolog(ll_error, "storage_backend_compressed_dir::fsync(%s): fsync callf failed on directory \"%s\": %s", id.c_str(), dir.c_str(), strerror(errno));
		return false;
	}

	if (do_sync_mirrors() == false) {
		dolog(ll_error, "storage_backend_file::fsync(%s): failed to sync data to mirror(s)", id.c_str());
		return false;
	}

	return true;
}

bool storage_backend_compressed_dir::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

	lg.un_lock_block_group(offset, len, block_size, true, false);

	offset_t work_offset = offset;
	size_t work_size = len;

	while(work_size > 0) {
		block_nr_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		int current_size = std::min(work_size, size_t(block_size - block_offset));

		if (current_size == block_size) {
			// delete file
			std::string file = myformat("%s/%ld", dir.c_str(), block_nr);

			if (unlink(file.c_str()) == -1) {
				if (errno != ENOENT) {
					*err = errno;
					dolog(ll_error, "storage_backend_compressed_dir::trim_zero(%s): failed to delete file \"%s\": %s", id.c_str(), file.c_str(), strerror(*err));
					break;
				}
			}
		}
		else {
			uint8_t *temp = nullptr;

			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "storage_backend_compressed_dir::trim_zero(%s): failed to retrieve block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				break;
			}

			memset(&temp[block_offset], 0x00, current_size);

			if (!put_block(block_nr, temp)) {
				dolog(ll_error, "storage_backend_compressed_dir::trim_zero(%s): failed to update block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				free(temp);
				break;
			}

			free(temp);
		}

		work_offset += current_size;
		work_size -= current_size;
	}

	lg.un_lock_block_group(offset, len, block_size, false, false);

	if (do_mirror_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "storage_backend_compressed_dir::trim_zero(%s): failed to send to mirror(s)", id.c_str());
		return false;
	}

	return *err == 0;
}
