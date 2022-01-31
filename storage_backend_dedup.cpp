/*
 * - hash => data
 * - hash => use_count
 * - block => hash
 */
#include <fcntl.h>
#include <kcpolydb.h>
#include <optional>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "error.h"
#include "io.h"
#include "logging.h"
#include "storage_backend_dedup.h"
#include "str.h"
#include "yaml-helpers.h"


storage_backend_dedup::storage_backend_dedup(const std::string & id, const std::string & file, hash *const h, const std::vector<mirror *> & mirrors, const offset_t size, const int block_size) : storage_backend(id, block_size, mirrors), h(h), size(size), file(file)
{
	if (db.open(myformat("%s#*", file.c_str()), kyotocabinet::PolyDB::OWRITER | kyotocabinet::PolyDB::OCREATE) == false)
		throw myformat("storage_backend_dedup: failed to access DB-file \"%s\": %s", file.c_str(), db.error().message());

	if (!verify_mirror_sizes())
		throw myformat("storage_backend_dedup(%s): mirrors sanity check failed", file.c_str());
}

storage_backend_dedup::~storage_backend_dedup()
{
	db.close();

	delete h;

	dolog(ll_info, "~storage_backend_dedup: database closed");
}

storage_backend_dedup * storage_backend_dedup::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string id = cfg["id"].as<std::string>();

	std::vector<mirror *> mirrors;
	YAML::Node y_mirrors = cfg["mirrors"];
	for(YAML::const_iterator it = y_mirrors.begin(); it != y_mirrors.end(); it++)
		mirrors.push_back(mirror::load_configuration(it->as<YAML::Node>()));

	std::string file = yaml_get_string(cfg, "file", "deduplication store filename");

	offset_t size = yaml_get_uint64_t(cfg, "size", "size (in bytes) of the storage");

	int block_size = yaml_get_int(cfg, "block-size", "block size of store (bigger is faster, smaller is better de-duplication)");

	hash *h = hash::load_configuration(yaml_get_yaml_node(cfg, "hash", "hash-function selection"));

	return new storage_backend_dedup(id, file, h, mirrors, size, block_size);
}

YAML::Node storage_backend_dedup::emit_configuration() const
{
	std::vector<YAML::Node> out_mirrors;
	for(auto m : mirrors)
		out_mirrors.push_back(m->emit_configuration());

	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["mirrors"] = out_mirrors;
	out_cfg["file"] = file;
	out_cfg["size"] = size;
	out_cfg["block-size"] = block_size;
	out_cfg["hash"] = h->emit_configuration();

	YAML::Node out;
	out["type"] = "storage-backend-dedup";
	out["cfg"] = out_cfg;

	return out;
}

offset_t storage_backend_dedup::get_size() const
{
	return size;
}

// key for hash -> use_count
std::string storage_backend_dedup::get_usecount_key_for_hash(const std::string & hash)
{
	return myformat("use-count_%s", hash.c_str());
}

// key for block_nr -> hash
std::string storage_backend_dedup::get_hashforblocknr_key_for_blocknr(const block_nr_t block_nr)
{
	return myformat("block-to-hash_%lu", block_nr);
}

// key for hash -> data_block
std::string storage_backend_dedup::get_data_key_for_hash(const std::string & hash)
{
	return myformat("data-block_%s", hash.c_str());
}

bool storage_backend_dedup::put_key_value(const std::string & key, const uint8_t *const value, const int value_len)
{
	if (db.set(key.c_str(), key.size(), reinterpret_cast<const char *>(value), value_len) == false) {
		dolog(ll_error, "storage_backend_dedup::put_key_value(%s): failed to retrieve value for key \"%s\": %s", id.c_str(), key.c_str(), db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::get_key_value(const std::string & key, uint8_t *const value, const int value_len, bool *const not_found)
{
	int rc = db.get(key.c_str(), key.size(), reinterpret_cast<char *>(value), value_len);

	if (rc == -1) {
		if (db.error().code() == kyotocabinet::BasicDB::Error::Code::NOREC) {
			*not_found = true;
			return true;
		}

		dolog(ll_error, "storage_backend_dedup::get_key_value(%s): failed to retrieve value for key \"%s\": %s", id.c_str(), key.c_str(), db.error().message());
		return false;
	}

	if (rc != value_len) {
		dolog(ll_error, "storage_backend_dedup::get_key_value(%s): value for key \"%s\" is not expected (%d) size (%d): %s", id.c_str(), key.c_str(), value_len, rc, db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::decrease_use_count(const std::string block_hash, int64_t *const new_count)
{
	bool not_found = false;
	if (get_key_value(block_hash, reinterpret_cast<uint8_t *>(new_count), sizeof(*new_count), &not_found) == false || not_found) {
		dolog(ll_error, "storage_backend_dedup::decrease_use_count(%s): failed to retrieve use count for hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	(*new_count)--;

	if (put_key_value(block_hash, reinterpret_cast<const uint8_t *>(new_count), sizeof(*new_count)) == false) {
		dolog(ll_error, "storage_backend_dedup::decrease_use_count(%s): failed to update use count for hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::get_use_count(const std::string block_hash, int64_t *const new_count)
{
	bool not_found = false;
	if (get_key_value(block_hash, reinterpret_cast<uint8_t *>(new_count), sizeof(*new_count), &not_found) == false) {
		if (not_found) {
			*new_count = 0;
			return true;
		}

		dolog(ll_error, "storage_backend_dedup::get_use_count(%s): failed to retrieve use count for hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::set_use_count(const std::string block_hash, const int64_t new_count)
{
	if (put_key_value(block_hash, reinterpret_cast<const uint8_t *>(&new_count), sizeof(new_count)) == false) {
		dolog(ll_error, "storage_backend_dedup::set_use_count(%s): failed to update use count for hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::delete_block_counter_by_hash(const std::string block_hash)
{
	if (db.remove(block_hash) == false) {
		dolog(ll_error, "storage_backend_dedup::delete_block_counter_by_hash(%s): failed to delete counter by hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::delete_block_by_hash(const std::string block_hash)
{
	if (db.remove(block_hash) == false) {
		dolog(ll_error, "storage_backend_dedup::delete_block_by_hash(%s): failed to delete block by hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::increase_use_count(const std::string block_hash, int64_t *const new_count)
{
	bool not_found = false;
	if (get_key_value(block_hash, reinterpret_cast<uint8_t *>(new_count), sizeof(*new_count), &not_found) == false || not_found) {
		dolog(ll_error, "storage_backend_dedup::increase_use_count(%s): failed to retrieve use count for hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	(*new_count)++;

	if (put_key_value(block_hash, reinterpret_cast<const uint8_t *>(new_count), sizeof(*new_count)) == false) {
		dolog(ll_error, "storage_backend_dedup::increase_use_count(%s): failed to update use count for hash \"%s\": %s", id.c_str(), block_hash.c_str(), db.error().message());
		return false;
	}

	return true;
}

std::optional<std::string> storage_backend_dedup::get_hash_for_block(const block_nr_t block_nr)
{
	// block-to-hash key
	std::string bth_key = get_hashforblocknr_key_for_blocknr(block_nr);

	int bh_size = h->get_size() * 2;
	uint8_t *block_hash = reinterpret_cast<uint8_t *>(calloc(1, bh_size));  // is/will be a plain ascii string

	bool not_found = false;
	if (get_key_value(bth_key, block_hash, bh_size, &not_found) == false) {
		free(block_hash);

		dolog(ll_error, "storage_backend_dedup::get_hash_for_block(%s): failed to retrieve, number %ld: %s", id.c_str(), block_nr, db.error().message());
		return { };
	}
	
	if (not_found) {
		free(block_hash);
		return "";
	}

	return std::string(reinterpret_cast<const char *>(block_hash), bh_size);
}

bool storage_backend_dedup::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	const offset_t offset = block_nr * block_size;

	lgdd.un_lock_block_group(offset, block_size, block_size, true, true);

	bool rc = get_block_int(block_nr, data);

	lgdd.un_lock_block_group(offset, block_size, block_size, false, true);

	return rc;
}

bool storage_backend_dedup::get_block_int(const block_nr_t block_nr, uint8_t **const data)
{
	// hash for block
	auto hfb = get_hash_for_block(block_nr);
	if (hfb.has_value() == false) {
		dolog(ll_error, "storage_backend_dedup::get_block_int(%s): failed to retrieve hash for block %ld: %s", id.c_str(), block_nr, db.error().message());
		return false;
	}

	*data = reinterpret_cast<uint8_t *>(calloc(1, block_size));

	// block does not exist yet, return 0x00
	if (hfb.value().empty())
		return true;

	// hash-to-data key
	std::string htd_key = get_data_key_for_hash(hfb.value().c_str());

	int rc2 = db.get(htd_key.c_str(), htd_key.size(), reinterpret_cast<char *>(*data), block_size);

	if (rc2 == -1) {
		dolog(ll_error, "storage_backend_dedup::get_block_int(%s): failed to retrieve block data for hash \"%s\": %s", id.c_str(), htd_key.c_str(), db.error().message());
		free(*data);
		return false;
	}

	return true;
}

bool storage_backend_dedup::map_blocknr_to_hash(const block_nr_t block_nr, const std::string & new_block_hash)
{
	return put_key_value(get_hashforblocknr_key_for_blocknr(block_nr), reinterpret_cast<const uint8_t *>(new_block_hash.c_str()), new_block_hash.size());
}

#define ABORT_TRANSACTION(db, where) 				\
	do {							\
		if ((db).end_transaction(false) == false) {	\
			dolog(ll_error, "%s failed aborting transaction: %s", where, db.error().message()); \
			return false;				\
		}						\
	}							\
	while(0)


bool storage_backend_dedup::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	const offset_t offset = block_nr * block_size;

	lgdd.un_lock_block_group(offset, block_size, block_size, true, false);

	bool rc = put_block_int(block_nr, data);

	lgdd.un_lock_block_group(offset, block_size, block_size, false, false);

	return rc;
}

bool storage_backend_dedup::put_block_int(const block_nr_t block_nr, const uint8_t *const data_in)
{
	if (db.begin_transaction() == false) {
		ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));
		return false;
	}

	// get hash for block (get_hash_for_block())
	auto cur_hash_for_blocknr = get_hash_for_block(block_nr);
	if (cur_hash_for_blocknr.has_value() == false) {
		ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

		dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to get hash for blocknr %ld", id.c_str(), block_nr);

		return false;
	}

	// - overwriting a block? (hash.empty() == false)
	if (cur_hash_for_blocknr.has_value() && cur_hash_for_blocknr.value().empty() == false) {
		// - decrease use count
		int64_t new_use_count = 0;
		if (decrease_use_count(cur_hash_for_blocknr.value(), &new_use_count) == false) {
			ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

			dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to retrieve use-count for hash \"%s\"", id.c_str(), cur_hash_for_blocknr.value().c_str());

			return false;
		}

		// - if use count is 0:
		if (new_use_count == 0) {
			// - delete block for that hash
			if (delete_block_by_hash(cur_hash_for_blocknr.value()) == false) {
				ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

				dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to delete block for hash \"%s\"", id.c_str(), cur_hash_for_blocknr.value().c_str());

				return false;
			}

			// delete counter
			if (delete_block_counter_by_hash(get_hashforblocknr_key_for_blocknr(block_nr)) == false) {
				ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

				dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to delete counter for hash \"%s\"", id.c_str(), cur_hash_for_blocknr.value().c_str());

				return false;
			}
		}
		else if (new_use_count < 0) {
			ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));


			dolog(ll_error, "storage_backend_dedup::put_block_int(%s): new_use_count < 0! (%ld) dataset is corrupt!", id.c_str(), new_use_count);
			return false;
		}
	}

	// - calc hash over new-block
	auto new_block_hash = h->do_hash(data_in, block_size);
	if (!new_block_hash.has_value()) {
		ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

		dolog(ll_error, "storage_backend_dedup::put_block_int(%s): cannot calculate hash", id.c_str());

		return false;
	}

	int64_t new_block_use_count = 0;
	if (get_use_count(new_block_hash.value(), &new_block_use_count) == false) {
		ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

		dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to retrieve use-count for hash \"%s\"", id.c_str(), new_block_hash.value().c_str());

		return false;
	}

	// - new-block-hash exists? (count > 0)
	if (new_block_use_count > 0) {
		// - increase count for new-block-hash
		int64_t temp = 0;
		if (increase_use_count(new_block_hash.value(), &temp) == false) {
			ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

			dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to increase use-count for data block with hash \"%s\"", id.c_str(), new_block_hash.value().c_str());

			return false;
		}

		if (temp != new_block_use_count + 1) {
			ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

			dolog(ll_error, "storage_backend_dedup::put_block_int(%s): new count (%ld) not as expected (%ld) hash \"%s\"", id.c_str(), temp, new_block_use_count + 1, new_block_hash.value().c_str());

			return false;
		}
	}
	else {  // new block
		// - count == 0:
		//   - set count to 1
		if (set_use_count(new_block_hash.value(), 1) == false) {
			ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

			dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to set use-count for data block with hash \"%s\" to 1", id.c_str(), new_block_hash.value().c_str());

			return false;
		}

		// - put block
		if (put_key_value(get_data_key_for_hash(new_block_hash.value()), data_in, block_size) == false) {
			ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

			dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to store data block with hash \"%s\"", id.c_str(), new_block_hash.value().c_str());

			return false;
		}
	}

	// - put mapping blocknr to new-block-hash
	if (map_blocknr_to_hash(block_nr, new_block_hash.value()) == false) {
		ABORT_TRANSACTION(db, myformat("storage_backend_dedup::put_block_int(%s):", id.c_str()));

		dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed to map blocknr %ld to hash \"%s\"", id.c_str(), block_nr, new_block_hash.value().c_str());

		return false;
	}

	// commit
	if (db.end_transaction(true) == false) {
		dolog(ll_error, "storage_backend_dedup::put_block_int(%s): failed committing transaction: %s", db.error().message());
		return false;
	}

	return true;
}

bool storage_backend_dedup::fsync()
{
	if (db.synchronize(true) == false) {
		dolog(ll_error, "storage_backend_dedup::fsync(%s): failed to kyotocabinet store to disk", id.c_str());
		return false;
	}

	if (do_sync_mirrors() == false) {
		dolog(ll_error, "storage_backend_dedup::fsync(%s): failed to sync data to mirror(s)", id.c_str());
		return false;
	}

	return true;
}

bool storage_backend_dedup::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

	uint8_t *b0x00 = reinterpret_cast<uint8_t *>(calloc(1, block_size));

	lgdd.un_lock_block_group(offset, len, block_size, true, false);

	offset_t work_offset = offset;
	size_t work_size = len;

	while(work_size > 0) {
		block_nr_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		int current_size = std::min(work_size, size_t(block_size - block_offset));

		if (current_size == block_size)
			put_block_int(block_nr, b0x00);
		else {
			uint8_t *temp = nullptr;

			if (!get_block_int(block_nr, &temp)) {
				dolog(ll_error, "storage_backend_dedup::trim_zero(%s): failed to retrieve block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				break;
			}

			memset(&temp[block_offset], 0x00, current_size);

			if (!put_block_int(block_nr, temp)) {
				dolog(ll_error, "storage_backend_dedup::trim_zero(%s): failed to update block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				free(temp);
				break;
			}

			free(temp);
		}

		work_offset += current_size;
		work_size -= current_size;
	}

	lgdd.un_lock_block_group(offset, len, block_size, false, false);

	free(b0x00);

	if (do_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "storage_backend_dedup::trim_zero(%s): failed to send to mirror(s)", id.c_str());
		return false;
	}

	return *err == 0;
}
