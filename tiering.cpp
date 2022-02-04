#include <cstdint>
#include <string.h>
#include <sys/random.h>

#include "logging.h"
#include "mirror.h"
#include "tiering.h"
#include "yaml-helpers.h"


tiering::tiering(const std::string & id, storage_backend *const fast_storage, storage_backend *const slow_storage, storage_backend *const meta_storage, const int block_size, const std::vector<mirror *> & mirrors) :
	storage_backend(id, block_size, mirrors),
	fast_storage(fast_storage), slow_storage(slow_storage), meta_storage(meta_storage)
{
	map_n_entries = meta_storage->get_size() / sizeof(descriptor_bin_t);
}

tiering::~tiering()
{
}

void tiering::init_zobrist()
{
	getrandom(&zobrist[0], sizeof(uint64_t) * 256, 0);
}

uint64_t tiering::perform_zobrist(const uint64_t block_nr)
{
	uint64_t out = 0;

	for(int i=0; i<8; i++) {
		uint8_t v = block_nr >> (8 * i);

		out ^= zobrist[v];
	}

	return out;
}

bool tiering::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	bool     ok = true;
	uint64_t complete_block_nr_hash = perform_zobrist(block_nr);
	// map_index is also the fast-storage index
	uint64_t map_index = complete_block_nr_hash % map_n_entries;   // TODO make sure every bin gets visited as often as others

	uint64_t now = ++age;

	lgt.un_lock_block_group(map_index, 1, 1, true, false);

	uint8_t *dp = nullptr;
	int err = 0;
	meta_storage->get_data(map_index, sizeof(descriptor_bin_t), &dp, &err);

	if (err) {
		lgt.un_lock_block_group(map_index, 1, 1, false, false);

		dolog(ll_error, "tiering::get_block(%s): failed retrieving block from meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));
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
		fast_storage->get_data(map_index * block_size, block_size, data, &err);

		if (err) {
			dolog(ll_error, "tiering::get_block(%s): failed retrieving block from fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(err));
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
			fast_storage->get_data(map_index * block_size, block_size, &dirty_block, &g_err);
			if (g_err) {
				dolog(ll_error, "tiering::get_block(%s): failed retrieving block from fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(g_err));
				ok = false;
			}
			else {
				// put in slow storage
				int p_err = 0;
				slow_storage->put_data(d->d[replace_slot].block_nr_slow_storage * block_size, *dirty_block, &p_err);
				if (p_err) {
					dolog(ll_error, "tiering::get_block(%s): failed writing dirty block to slow storage (%s): %s", id.c_str(), slow_storage->get_id().c_str(), strerror(p_err));
					ok = false;
				}
			}

			// reset "dirty"-flag
			d->d[replace_slot].flags &= ~TF_dirty;

			delete dirty_block;
		}

		int g_err = 0;
		slow_storage->get_data(block_nr * block_size, block_size, data, &g_err);
		if (g_err) {
			dolog(ll_error, "tiering::get_block(%s): failed retrieving block from slow storage (%s): %s", id.c_str(), slow_storage->get_id().c_str(), strerror(g_err));
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
	meta_storage->put_data(map_index, bd, &err);

	if (err) {
		dolog(ll_error, "tiering::get_block(%s): failed storing block into meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));

		ok = false;
	}

	lgt.un_lock_block_group(map_index, 1, 1, false, false);

	return ok;
}

bool tiering::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	bool     ok = true;
	uint64_t complete_block_nr_hash = perform_zobrist(block_nr);
	// map_index is also the fast-storage index
	uint64_t map_index = complete_block_nr_hash % map_n_entries;   // TODO make sure every bin gets visited as often as others

	uint64_t now = ++age;

	lgt.un_lock_block_group(map_index, 1, 1, true, false);

	uint8_t *dp = nullptr;
	int err = 0;
	meta_storage->get_data(map_index, sizeof(descriptor_bin_t), &dp, &err);

	if (err) {
		lgt.un_lock_block_group(map_index, 1, 1, false, false);

		dolog(ll_error, "tiering::put_block(%s): failed retrieving block from meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));
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
		block b(data, block_size, false);

		int err = 0;
		fast_storage->put_data(map_index * block_size, b, &err);

		if (err) {
			dolog(ll_error, "tiering::put_block(%s): failed stroing block in fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(err));
			ok = false;
		}
		else {
			d->d[replace_slot].age    = now;
			d->d[replace_slot].flags |= TF_dirty;
		}
	}
	else {
		// when a dirty block will be replaced (written to), move it slow storage first
		if (d->d[replace_slot].flags & TF_dirty) {
			block *dirty_block = nullptr;

			// get from fast storage
			int g_err = 0;
			fast_storage->get_data(map_index * block_size, block_size, &dirty_block, &g_err);
			if (g_err) {
				dolog(ll_error, "tiering::put_block(%s): failed retrieving block from fast storage (%s): %s", id.c_str(), fast_storage->get_id().c_str(), strerror(g_err));
				ok = false;
			}
			else {
				// put in slow storage
				int p_err = 0;
				slow_storage->put_data(d->d[replace_slot].block_nr_slow_storage * block_size, *dirty_block, &p_err);
				if (p_err) {
					dolog(ll_error, "tiering::put_block(%s): failed writing dirty block to slow storage (%s): %s", id.c_str(), slow_storage->get_id().c_str(), strerror(p_err));
					ok = false;
				}
			}

			// reset "dirty"-flag
			d->d[replace_slot].flags &= ~TF_dirty;

			delete dirty_block;
		}

		block new_data(data, block_size, false);

		int p_err = 0;
		fast_storage->put_data(block_nr * block_size, new_data, &p_err);
		if (p_err) {
			dolog(ll_error, "tiering::put_block(%s): failed storing block in fast storage (%s): %s", id.c_str(), slow_storage->get_id().c_str(), strerror(p_err));
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
	meta_storage->put_data(map_index, bd, &err);

	if (err) {
		dolog(ll_error, "tiering::put_block(%s): failed storing block into meta storage (%s): %s", id.c_str(), meta_storage->get_id().c_str(), strerror(err));

		ok = false;
	}

	lgt.un_lock_block_group(map_index, 1, 1, false, false);

	return ok;
}

offset_t tiering::get_size() const
{
	return slow_storage->get_size();
}

bool tiering::fsync()
{
	bool ok = true;

	if (fast_storage->fsync() == false) {
		dolog(ll_error, "tiering::fsync(%s): failed fsync on fast storage (%s)", id.c_str(), fast_storage->get_id().c_str());
		ok = false;
	}

	if (slow_storage->fsync() == false) {
		dolog(ll_error, "tiering::fsync(%s): failed fsync on slow storage (%s)", id.c_str(), slow_storage->get_id().c_str());
		ok = false;
	}

	if (meta_storage->fsync() == false) {
		dolog(ll_error, "tiering::fsync(%s): failed fsync on meta storage (%s)", id.c_str(), meta_storage->get_id().c_str());
		ok = false;
	}

	return ok;
}

bool tiering::can_do_multiple_blocks() const
{
	return false;
}

bool tiering::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

	uint8_t *b0x00 = reinterpret_cast<uint8_t *>(calloc(1, block_size));

	offset_t work_offset = offset;
	size_t work_size = len;

	lg.un_lock_block_group(offset, len, block_size, true, false);

	while(work_size > 0) {
		block_nr_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		int current_size = std::min(work_size, size_t(block_size - block_offset));

		if (current_size == block_size) {
			bool rc = put_block(block_nr, b0x00);

			if (rc == false) {
				*err = EIO;
				dolog(ll_error, "tiering::trim_zero(%s): failed to %s block %ld", id.c_str(), trim ? "trim" : "zero", block_nr);
				break;
			}
		}
		else {
			uint8_t *temp = nullptr;

			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "tiering::trim_zero(%s): failed to retrieve block %ld", id.c_str(), id.c_str(), block_nr);
				*err = EIO;
				break;
			}

			memset(&temp[block_offset], 0x00, current_size);

			if (!put_block(block_nr, temp)) {
				dolog(ll_error, "tiering::trim_zero(%s): failed to update block %ld", id.c_str(), id.c_str(), block_nr);
				*err = EIO;
				free(temp);
				break;
			}

			free(temp);
		}

		work_offset += current_size;
		work_size -= current_size;
	}

	lg.un_lock_block_group(offset, len, block_size, false, false);

	free(b0x00);

	if (do_mirror_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "tiering::trim_zero(%s): failed to send to mirror(s)", id.c_str(), id.c_str());
		return false;
	}

	return *err == 0;
}

YAML::Node tiering::emit_configuration() const
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
	out["type"] = "tiering";
	out["cfg"] = out_cfg;

	return out;
}

tiering * tiering::load_configuration(const YAML::Node & node)
{
	dolog(ll_info, " * tiering::load_configuration");

	const YAML::Node cfg = yaml_get_yaml_node(node, "cfg", "tiering configuration");

	std::string id = yaml_get_string(cfg, "id", "module id");

	std::vector<mirror *> mirrors;
	YAML::Node y_mirrors = cfg["mirrors"];
	for(YAML::const_iterator it = y_mirrors.begin(); it != y_mirrors.end(); it++)
		mirrors.push_back(mirror::load_configuration(it->as<YAML::Node>()));

	storage_backend *sb_slow = storage_backend::load_configuration(cfg["storage-backend-slow"]);
	storage_backend *sb_fast = storage_backend::load_configuration(cfg["storage-backend-fast"]);

	storage_backend *meta = storage_backend::load_configuration(cfg["storage-backend-meta"]);

	int block_size = yaml_get_int(cfg, "block-size", "block size");

	return new tiering(id, sb_fast, sb_slow, meta, block_size, mirrors);
}
