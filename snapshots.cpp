#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <optional>
#include <string>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "io.h"
#include "logging.h"
#include "snapshots.h"
#include "str.h"
#include "yaml-helpers.h"


snapshot_state::snapshot_state(storage_backend *const src, const std::string & complete_filename, const int block_size, const bool sparse_files) : src(src), complete_filename(complete_filename), block_size(block_size)
{
	// target file
	fd = open(complete_filename.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd == -1)
		throw myformat("snapshot_state(%s): failed to create snapshot file: %s", complete_filename.c_str(), strerror(errno));

	if (ftruncate(fd, src->get_size()) == -1)
		throw myformat("snapshot_state(%s): failed to set size of snapshot: %s", complete_filename.c_str(), strerror(errno));

	// bitmap
	offset_t bitmap_size = (src->get_size() + block_size - 1) / (block_size * 8);  // each byte has 8 block-flags

	bitmap = reinterpret_cast<uint64_t *>(calloc(1, bitmap_size));
	if (bitmap == nullptr)
		throw myformat("snapshot_state(%s): failed to allocate bitmap memory: %s", complete_filename.c_str(), strerror(errno));

	// sparse files helper block
	if (sparse_files)
		sparse_block_compare = reinterpret_cast<uint8_t *>(calloc(1, block_size));

	th = new std::thread(std::ref(*this));
}

snapshot_state::~snapshot_state()
{
	stop_flag = true;

	if (th) {
		th->join();
		delete th;
	}

	free(sparse_block_compare);

	free(bitmap);
}

std::string snapshot_state::get_filename() const
{
	return complete_filename;
}

bool snapshot_state::has_finished() const
{
	return copy_finished;
}

bool snapshot_state::get_set_block_state(const block_nr_t block_nr)
{
	uint64_t mask = 1ll << (block_nr & 63);
	uint64_t bitmap_idx = block_nr / 64;

	std::lock_guard<std::mutex> lck(lock);

	bool unwritten = !(bitmap[bitmap_idx] & mask);

	if (unwritten) {
		bitmap[bitmap_idx] |= mask;

		if (copy_block(block_nr) == false) {

			dolog(ll_error, "snapshot_state::get_set_block_state(%s): failed to copy block %ld to snapshot", complete_filename.c_str(), block_nr);
			return false;
		}
	}

	return true;
}

bool snapshot_state::copy_block(const block_nr_t block_nr)
{
	offset_t off = block_nr * block_size;

	int      err = 0;
	block   *b = nullptr;
	src->get_data(off, block_size, &b, &err);

	if (err) {
		dolog(ll_error, "snapshot_state::copy_block(%s): failed to get block (get_data) from source: %s", complete_filename.c_str(), strerror(errno));
		return false;
	}

	const uint8_t *p = b->get_data();
	size_t         todo = b->get_size();

	if (sparse_block_compare && memcmp(sparse_block_compare, p, block_size) == 0) {
		delete b;
		return true;
	}

	if (todo != block_size) {
		dolog(ll_error, "snapshot_state::copy_block(%s): data-block is %ld bytes, expecting %d", complete_filename.c_str(), todo, block_size);
		delete b;
		return false;
	}

	ssize_t rc = PWRITE(fd, p, todo, off);

	if (rc == -1) {
		dolog(ll_error, "snapshot_state::copy_block(%s): failed to write block to snapshot: %s", complete_filename.c_str(), strerror(errno));
		delete b;
		return false;
	}
	else if (rc == 0) {
		dolog(ll_error, "snapshot_state::copy_block(%s): write 0 bytes to snapshot: disk full?", complete_filename.c_str());
		delete b;
		return false;
	}

	delete b;

	return true;
}

bool snapshot_state::put_block(const block_nr_t block_nr)
{
	if (get_set_block_state(block_nr) == false) {
		dolog(ll_error, "snapshot_state::put_block(%s): failed to copy block %ld to snapshot", complete_filename.c_str(), block_nr);
		return false;
	}

	return true;
}

void snapshot_state::operator()()
{
	block_nr_t       block_nr = 0;
	const block_nr_t n_blocks = (src->get_size() + block_size - 1) / block_size;

	dolog(ll_debug, "snapshot_state::operator: snapshot size: %ld bytes, %ld blocks (block size: %d)", src->get_size(), n_blocks, block_size);

	while(!stop_flag && block_nr < n_blocks) {
		if (get_set_block_state(block_nr) == false) {
			dolog(ll_error, "snapshot_state::operator(%s): failed to copy block to snapshot", complete_filename.c_str());
			break;
		}

		block_nr++;
	}

	if (block_nr != n_blocks)
		dolog(ll_error, "snapshot_state::operator(%s): not all data was snapshotted! (%ld of %ld blocks)", complete_filename.c_str(), block_nr, n_blocks);

	if (fsync(fd) == -1)
		dolog(ll_error, "snapshot_state::operator(%s): failed to fsync snapshot: %s", complete_filename.c_str(), strerror(errno));
	else
		dolog(ll_info, "snapshot_state::operator(%s): snapshot finished", complete_filename.c_str());

	close(fd);

	copy_finished = true;
}

snapshots::snapshots(const std::string & id, const std::string & storage_directory, const std::string & filename_template, storage_backend *const sb, const bool sparse_files) : storage_backend(id, sb->get_block_size(), { }), storage_directory(storage_directory), filename_template(filename_template), sb(sb), sparse_files(sparse_files)
{
	if (sparse_files)
		dolog(ll_info, "snapshots: sparse snapshot files enabled");
}

snapshots::~snapshots()
{
	for(auto s : running_snapshots)
		delete s;
}

std::optional<std::string> snapshots::trigger_snapshot()
{
	char      buffer[PATH_MAX] { 0 };
	struct tm tm { 0 };
	time_t    t = time(nullptr);

	localtime_r(&t, &tm);

	size_t rc = strftime(buffer, sizeof buffer, filename_template.c_str(), &tm);
	if (rc == 0) {
		dolog(ll_error, "snapshots::trigger_snapshot: problem generating snapshot filename");
		return { };
	}

	snapshot_state *ss = nullptr;

	try {
		ss = new snapshot_state(sb, buffer, block_size, sparse_files);

		lock.lock();
		running_snapshots.push_back(ss);
		lock.unlock();
	}
	catch(const std::string & exception) {
		dolog(ll_error, "snapshots::trigger_snapshot: failed to start snapshot");
		delete ss;
		return { };
	}

	dolog(ll_info, "snapshots::trigger_snapshot: snapshotting to %s", buffer);

	return buffer;
}

bool snapshots::can_do_multiple_blocks() const
{
	return false;
}

bool snapshots::trigger_range(const offset_t offset, const uint32_t len)
{
	bool ok = true;

	for(auto ss : running_snapshots) {
		for(offset_t o = offset; o<offset + len; o += block_size) {
			block_nr_t bo = o / block_size;

			if (ss->put_block(bo) == false) {
				ok = false;
				dolog(ll_error, "snapshots::trigger_range(%s): failed to write block %ld to snapshot", id.c_str(), bo);
				break;
			}
		}
	}

	return ok;
}

bool snapshots::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	return sb->get_block(block_nr, data);
}

bool snapshots::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	if (trigger_range(block_nr * sb->get_block_size(), sb->get_block_size()) == false) {
		dolog(ll_error, "snapshots::put_block(%s): failed to write block %ld to snapshot", id.c_str(), block_nr);
		return false;
	}

	return sb->put_block(block_nr, data);
}

offset_t snapshots::get_size() const
{
	return sb->get_size();
}

bool snapshots::fsync()
{
	return sb->fsync();
}

bool snapshots::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	if (trigger_range(offset, len) == false) {
		dolog(ll_error, "snapshots::trim_zero(%s): failed to write block %ld to snapshot", id.c_str(), offset);
		return false;
	}

	return sb->trim_zero(offset, len, trim, err);
}

void snapshots::put_data(const offset_t offset, const block & b, int *const err)
{
	if (trigger_range(offset, b.get_size()) == false) {
		dolog(ll_error, "snapshots::put_data(%s): failed to write block %ld to snapshot", id.c_str(), offset);
		*err = EIO;
	}
	else {
		sb->put_data(offset, b, err);
	}
}

void snapshots::put_data(const offset_t offset, const std::vector<uint8_t> & d, int *const err)
{
	if (trigger_range(offset, d.size()) == false) {
		dolog(ll_error, "snapshots::put_data(%s): failed to write block %ld to snapshot", id.c_str(), offset);
		*err = EIO;
	}
	else {
		sb->put_data(offset, d, err);
	}
}

snapshots * snapshots::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string id = yaml_get_string(cfg, "id", "id of snapshot object");
	std::string storage_directory = yaml_get_string(cfg, "storage-directory", "directory where snapshots are placed");
	std::string filename_template = yaml_get_string(cfg, "filename-template", "template of snapshot filenames (see \"man strftime\")");
	storage_backend *sb = storage_backend::load_configuration(cfg["storage-backend"]);
	bool sparse_files = yaml_get_bool(cfg, "sparse-files", "create snapshots with holes where possible (to reduce diskspace)");

	return new snapshots(id, storage_directory, filename_template, sb, sparse_files);
}

YAML::Node snapshots::emit_configuration() const
{
	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["storage-directory"] = storage_directory;
	out_cfg["filename-template"] = filename_template;
	out_cfg["storage-backend"] = sb->emit_configuration();
	out_cfg["sparse-files"] = sparse_files;

	YAML::Node out;
	out["type"] = "snapshots";
	out["cfg"] = out_cfg;

	return out;
}
