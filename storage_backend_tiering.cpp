#include <assert.h>
#include <cstdint>
#include <string.h>
#include <arpa/inet.h>

#include "hash.h"
#include "logging.h"
#include "mirror.h"
#include "str.h"
#include "storage_backend_tiering.h"
#include "yaml-helpers.h"


storage_backend_tiering::storage_backend_tiering(const std::string & id, storage_backend *const fast_storage, storage_backend *const slow_storage, storage_backend *const meta_storage, const std::vector<mirror *> & mirrors) :
	storage_backend(id, fast_storage->get_block_size(), mirrors),
	fast_storage(fast_storage), slow_storage(slow_storage), meta_storage(meta_storage)
{
	if (fast_storage->get_block_size() != slow_storage->get_block_size())
		throw myformat("storage_backend_tiering(%s): slow- and fast storage must have the same block size", id.c_str());

	map_n_entries = meta_storage->get_size() / sizeof(descriptor_bin_t);

	dolog(ll_debug, "storage_backend_tiering(%s): %ld meta data slots", id.c_str(), map_n_entries);

	auto md = get_meta_dimensions(fast_storage->get_size(), fast_storage->get_block_size());
	uint64_t expecting_n = md.first;

	if (expecting_n != map_n_entries) {
		if (expecting_n > map_n_entries)
			throw myformat("storage_backend_tiering: meta storage too small, must be at least %ld bytes", expecting_n, expecting_n * md.second);

		dolog(ll_info, "storage_backend_tiering: mismatch between expected number of meta-entries (%ld) and configured number (%ld)", expecting_n, map_n_entries);
	}

	meta_hist = new histogram(map_n_entries, 1024);
	slow_hist = new histogram(slow_storage->get_size() / slow_storage->get_block_size(), 1024);
}

storage_backend_tiering::~storage_backend_tiering()
{
	delete slow_hist;
	delete meta_hist;

	delete fast_storage;
	delete slow_storage;
	delete meta_storage;
}

std::pair<uint64_t, int> storage_backend_tiering::get_meta_dimensions(const offset_t fast_storage_size, const int fast_storage_block_size)
{
	return { (fast_storage_size + fast_storage_block_size - 1) / fast_storage_block_size, sizeof(descriptor_bin_t) };
}

uint64_t storage_backend_tiering::hash_block_nr(const uint64_t block_nr)
{
	// TODO: replace by 128 murmur3 (and fold it in half)
	uint32_t upper_word = htonl(block_nr >> 32);
	uint32_t lower_word = htonl(block_nr);

	uint32_t upper_hash = murmur3_32(reinterpret_cast<const uint8_t *>(&upper_word), sizeof(uint32_t), MURMUR3_32_SEED);
	uint32_t lower_hash = murmur3_32(reinterpret_cast<const uint8_t *>(&lower_word), sizeof(uint32_t), MURMUR3_32_SEED);

	return (uint64_t(upper_hash) << 32) | lower_hash;
}

bool storage_backend_tiering::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	bool     ok = true;
	uint64_t complete_block_nr_hash = hash_block_nr(block_nr);
	// map_index is also the fast-storage index
	uint64_t map_index = complete_block_nr_hash % map_n_entries;   // TODO make sure every bin gets visited as often as others
	int      f_s_block_size = fast_storage->get_block_size();

	meta_hist->count(map_index);

	uint64_t now = ++age;

	lgt.un_lock_block_group(map_index, 1, 1, true, false);

	uint8_t *dp = nullptr;
	int err = 0;
	meta_storage->get_data(map_index * sizeof(descriptor_bin_t), sizeof(descriptor_bin_t), &dp, &err);

	if (err) {
		lgt.un_lock_block_group(map_index, 1, 1, false, false);

		dolog(ll_error, "storage_backend_tiering::get_block(%s): failed retrieving block from meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));
		return false;
	}

	descriptor_bin_t *d = reinterpret_cast<descriptor_bin_t *>(dp);

	int replace_slot = -1;
	uint64_t oldest = UINT64_MAX;
	bool match = false;

	for(int i=0; i<DESCRIPTORS_PER_BIN; i++) {
		if (d->d[i].complete_block_nr_hash == complete_block_nr_hash) {
			replace_slot = i;
			match = true;
			break;
		}

		if (d->d[i].age < oldest)
			replace_slot = i;
	}

	if (match) {
		int err = 0;
		fast_storage->get_data(map_index * f_s_block_size, f_s_block_size, data, &err);

		if (err) {
			dolog(ll_error, "storage_backend_tiering::get_block(%s): failed retrieving block from fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(err));
			ok = false;
		}
		else {
			d->d[replace_slot].age = now;
		}
	}
	else {
		// when a dirty block will be replaced (written to), move it slow storage first
		if (d->d[replace_slot].flags & TF_dirty) {
			block *dirty_block = nullptr;

			// get from fast storage
			int g_err = 0;
			fast_storage->get_data(map_index * f_s_block_size, f_s_block_size, &dirty_block, &g_err);
			if (g_err) {
				dolog(ll_error, "storage_backend_tiering::get_block(%s): failed retrieving block from fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(g_err));
				ok = false;
			}
			else {
				slow_hist->count(d->d[replace_slot].block_nr_slow_storage);

				// put in slow storage
				int p_err = 0;
				slow_storage->put_data(d->d[replace_slot].block_nr_slow_storage * f_s_block_size, *dirty_block, &p_err);
				if (p_err) {
					dolog(ll_error, "storage_backend_tiering::get_block(%s): failed writing dirty block to slow storage (%s): %s", id.c_str(), slow_storage->get_id().c_str(), strerror(p_err));
					ok = false;
				}
			}

			// reset "dirty"-flag
			d->d[replace_slot].flags &= ~TF_dirty;

			delete dirty_block;
		}

		slow_hist->count(block_nr);

		int g_err = 0;
		slow_storage->get_data(block_nr * f_s_block_size, f_s_block_size, data, &g_err);
		if (g_err) {
			dolog(ll_error, "storage_backend_tiering::get_block(%s): failed retrieving block from slow storage (%s): %s", id.c_str(), slow_storage->get_id().c_str(), strerror(g_err));
			ok = false;
		}
		else {
			d->d[replace_slot].complete_block_nr_hash = complete_block_nr_hash;
			d->d[replace_slot].block_nr_slow_storage  = block_nr;
			d->d[replace_slot].age   = now;
			d->d[replace_slot].flags = 0;
		}
	}

	block bd(dp, sizeof(descriptor_bin_t));
	meta_storage->put_data(map_index * sizeof(descriptor_bin_t), bd, &err);  // TODO alleen 'replace_slot', niet de hele descriptor_bin_t

	if (err) {
		dolog(ll_error, "storage_backend_tiering::get_block(%s): failed storing block into meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));

		ok = false;
	}

	lgt.un_lock_block_group(map_index, 1, 1, false, false);

	return ok;
}

bool storage_backend_tiering::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	bool     ok = true;
	uint64_t complete_block_nr_hash = hash_block_nr(block_nr);
	// map_index is also the fast-storage index
	uint64_t map_index = complete_block_nr_hash % map_n_entries;   // TODO make sure every bin gets visited as often as others
	int      f_s_block_size = fast_storage->get_block_size();

	meta_hist->count(map_index);

	uint64_t now = ++age;

	lgt.un_lock_block_group(map_index, 1, 1, true, false);

	uint8_t *dp = nullptr;
	int err = 0;
	meta_storage->get_data(map_index * sizeof(descriptor_bin_t), sizeof(descriptor_bin_t), &dp, &err);

	if (err) {
		lgt.un_lock_block_group(map_index, 1, 1, false, false);

		dolog(ll_error, "storage_backend_tiering::put_block(%s): failed retrieving block from meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));
		return false;
	}

	descriptor_bin_t *d = reinterpret_cast<descriptor_bin_t *>(dp);

	int replace_slot = -1;
	uint64_t oldest = UINT64_MAX;
	bool match = false;

	for(int i=0; i<DESCRIPTORS_PER_BIN; i++) {
		if (d->d[i].complete_block_nr_hash == complete_block_nr_hash) {
			match = d->d[i].block_nr_slow_storage == block_nr;  // if not 'true': hash-collision
			replace_slot = i;
			break;
		}

		if (d->d[i].age < oldest)
			replace_slot = i;
	}

	if (match) {  // updating
		block b(data, f_s_block_size, false);

		int err = 0;
		fast_storage->put_data(map_index * f_s_block_size, b, &err);

		if (err) {
			dolog(ll_error, "storage_backend_tiering::put_block(%s): failed storing block in fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(err));
			ok = false;
		}
		else {
			d->d[replace_slot].age    = now;
			d->d[replace_slot].flags |= TF_dirty;
		}
	}
	else {  // replacing
		// when a dirty block will be replaced (written to), move the old current contents to slow storage first
		if (d->d[replace_slot].flags & TF_dirty) {
			block *dirty_block = nullptr;

			// get from fast storage
			int g_err = 0;
			fast_storage->get_data(map_index * f_s_block_size, f_s_block_size, &dirty_block, &g_err);
			if (g_err) {
				dolog(ll_error, "storage_backend_tiering::put_block(%s): failed retrieving block from fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(g_err));
				ok = false;
			}
			else {
				// put in slow storage
				slow_hist->count(d->d[replace_slot].block_nr_slow_storage);

				int p_err = 0;
				slow_storage->put_data(d->d[replace_slot].block_nr_slow_storage * f_s_block_size, *dirty_block, &p_err);
				if (p_err) {
					dolog(ll_error, "storage_backend_tiering::put_block(%s): failed writing dirty block to slow storage (%s): %s", id.c_str(), slow_storage->get_id().c_str(), strerror(p_err));
					ok = false;
				}
			}

			// reset "dirty"-flag
			d->d[replace_slot].flags &= ~TF_dirty;

			delete dirty_block;
		}

		block new_data(data, f_s_block_size, false);

		int p_err = 0;
		fast_storage->put_data(map_index * f_s_block_size, new_data, &p_err);
		if (p_err) {
			dolog(ll_error, "storage_backend_tiering::put_block(%s): failed storing block in fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(p_err));
			ok = false;
		}
		else {
			d->d[replace_slot].complete_block_nr_hash = complete_block_nr_hash;
			d->d[replace_slot].block_nr_slow_storage  = block_nr;
			d->d[replace_slot].age   = now;
			d->d[replace_slot].flags = TF_dirty;
		}
	}

	block bd(dp, sizeof(descriptor_bin_t));
	meta_storage->put_data(map_index * sizeof(descriptor_bin_t), bd, &err);

	if (err) {
		dolog(ll_error, "storage_backend_tiering::put_block(%s): failed storing block into meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));

		ok = false;
	}

	lgt.un_lock_block_group(map_index, 1, 1, false, false);

	return ok;
}

offset_t storage_backend_tiering::get_size() const
{
	return slow_storage->get_size();
}

bool storage_backend_tiering::fsync()
{
	bool ok = true;

	if (fast_storage->fsync() == false) {
		dolog(ll_error, "storage_backend_tiering::fsync(%s): failed fsync on fast storage (%s)", id.c_str(), fast_storage->get_id().c_str());
		ok = false;
	}

	if (slow_storage->fsync() == false) {
		dolog(ll_error, "storage_backend_tiering::fsync(%s): failed fsync on slow storage (%s)", id.c_str(), slow_storage->get_id().c_str());
		ok = false;
	}

	if (meta_storage->fsync() == false) {
		dolog(ll_error, "storage_backend_tiering::fsync(%s): failed fsync on meta storage (%s)", id.c_str(), meta_storage->get_id().c_str());
		ok = false;
	}

	return ok;
}

bool storage_backend_tiering::can_do_multiple_blocks() const
{
	return false;
}

bool storage_backend_tiering::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

	int      f_s_block_size = fast_storage->get_block_size();

	uint8_t *b0x00 = reinterpret_cast<uint8_t *>(calloc(1, f_s_block_size));

	offset_t work_offset = offset;
	size_t work_size = len;

	lg.un_lock_block_group(offset, len, f_s_block_size, true, false);

	while(work_size > 0) {
		block_nr_t block_nr = work_offset / f_s_block_size;
		uint32_t block_offset = work_offset % f_s_block_size;

		int current_size = std::min(work_size, size_t(f_s_block_size - block_offset));

		if (current_size == f_s_block_size) {
			bool rc = put_block(block_nr, b0x00);

			if (rc == false) {
				*err = EIO;
				dolog(ll_error, "storage_backend_tiering::trim_zero(%s): failed to %s block %ld", id.c_str(), trim ? "trim" : "zero", block_nr);
				break;
			}
		}
		else {
			uint8_t *temp = nullptr;

			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "storage_backend_tiering::trim_zero(%s): failed to retrieve block %ld", id.c_str(), id.c_str(), block_nr);
				*err = EIO;
				break;
			}

			memset(&temp[block_offset], 0x00, current_size);

			if (!put_block(block_nr, temp)) {
				dolog(ll_error, "storage_backend_tiering::trim_zero(%s): failed to update block %ld", id.c_str(), id.c_str(), block_nr);
				*err = EIO;
				free(temp);
				break;
			}

			free(temp);
		}

		work_offset += current_size;
		work_size -= current_size;
	}

	lg.un_lock_block_group(offset, len, f_s_block_size, false, false);

	free(b0x00);

	if (do_mirror_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "storage_backend_tiering::trim_zero(%s): failed to send to mirror(s)", id.c_str(), id.c_str());
		return false;
	}

	return *err == 0;
}

YAML::Node storage_backend_tiering::emit_configuration() const
{
	std::vector<YAML::Node> out_mirrors;
	for(auto m : mirrors)
		out_mirrors.push_back(m->emit_configuration());

	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["mirrors"] = out_mirrors;
	out_cfg["storage-backend-slow"] = slow_storage->emit_configuration();
	out_cfg["storage-backend-fast"] = fast_storage->emit_configuration();
	out_cfg["storage-backend-meta"] = meta_storage->emit_configuration();

	YAML::Node out;
	out["type"] = "storage-backend-tiering";
	out["cfg"] = out_cfg;

	return out;
}

storage_backend_tiering * storage_backend_tiering::load_configuration(const YAML::Node & node, const std::optional<uint64_t> size, std::optional<int> block_size)
{
	dolog(ll_info, " * storage_backend_tiering::load_configuration");

	if (size.has_value())
		dolog(ll_info, " * storage_backend_tiering::load_configuration: cannot override size");

	const YAML::Node cfg = yaml_get_yaml_node(node, "cfg", "storage-backend-tiering configuration");

	std::string id = yaml_get_string(cfg, "id", "module id");

	std::vector<mirror *> mirrors;
	YAML::Node y_mirrors = cfg["mirrors"];
	for(YAML::const_iterator it = y_mirrors.begin(); it != y_mirrors.end(); it++)
		mirrors.push_back(mirror::load_configuration(it->as<YAML::Node>()));

	storage_backend *sb_slow = storage_backend::load_configuration(cfg["storage-backend-slow"], { }, { });
	storage_backend *sb_fast = storage_backend::load_configuration(cfg["storage-backend-fast"], { }, { });

	auto md = storage_backend_tiering::get_meta_dimensions(sb_fast->get_size(), sb_fast->get_block_size());
	storage_backend *meta = storage_backend::load_configuration(cfg["storage-backend-meta"], md.first * md.second, md.second);

	return new storage_backend_tiering(id, sb_fast, sb_slow, meta, mirrors);
}

void storage_backend_tiering::dump_stats(const std::string & base_filename)
{
	meta_hist->dump(base_filename + id + "_meta-histogram.dat");

	slow_hist->dump(base_filename + id + "_slow-histogram.dat");
}
