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


storage_backend_dedup::storage_backend_dedup(const std::string & id, const std::string & file, hash *const h, const std::vector<mirror *> & mirrors, const offset_t size, const int block_size) : storage_backend(id, mirrors), h(h), size(size), block_size(block_size)
{
	if (db.open(myformat("%s#*", file.c_str()), kyotocabinet::PolyDB::OWRITER | kyotocabinet::PolyDB::OCREATE) == false)
		error_exit(false, "storage_backend_dedup: failed to access DB-file \"%s\": %s", file.c_str(), db.error().message());

	verify_mirror_sizes();
}

storage_backend_dedup::~storage_backend_dedup()
{
	db.close();
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
std::string storage_backend_dedup::get_hashforblocknr_key_for_blocknr(const uint64_t block_nr)
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

std::optional<std::string> storage_backend_dedup::get_hash_for_block(const uint64_t block_nr)
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

bool storage_backend_dedup::get_block(const uint64_t block_nr, uint8_t **const data)
{
	// hash for block
	auto hfb = get_hash_for_block(block_nr);
	if (hfb.has_value() == false) {
		dolog(ll_error, "storage_backend_dedup::get_block(%s): failed to retrieve hash for block %ld: %s", id.c_str(), block_nr, db.error().message());
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
		dolog(ll_error, "storage_backend_dedup::get_block(%s): failed to retrieve block data for hash \"%s\": %s", id.c_str(), htd_key.c_str(), db.error().message());
		free(*data);
		return false;
	}

	return true;
}

bool storage_backend_dedup::map_blocknr_to_hash(const uint64_t block_nr, const std::string & new_block_hash)
{
	return put_key_value(get_hashforblocknr_key_for_blocknr(block_nr), reinterpret_cast<const uint8_t *>(new_block_hash.c_str()), new_block_hash.size());
}

bool storage_backend_dedup::put_block(const uint64_t block_nr, const uint8_t *const data_in)
{
	// get hash for block (get_hash_for_block())
	auto cur_hash_for_blocknr = get_hash_for_block(block_nr);
	if (cur_hash_for_blocknr.has_value() == false) {
		dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to hash for blocknr %ld", id.c_str(), block_nr);
		return false;
	}

	// - overwriting a block? (hash.empty() == false)
	if (cur_hash_for_blocknr.has_value() && cur_hash_for_blocknr.value().empty() == false) {
		// - decrease use count
		int64_t new_use_count = 0;
		if (decrease_use_count(cur_hash_for_blocknr.value(), &new_use_count) == false) {
			dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to retrieve use-count for hash \"%s\"", id.c_str(), cur_hash_for_blocknr.value().c_str());
			return false;
		}

		// - if use count is 0:
		if (new_use_count == 0) {
		// - delete block for that hash
			if (delete_block_by_hash(cur_hash_for_blocknr.value()) == false) {
				dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to delete block for hash \"%s\"", id.c_str(), cur_hash_for_blocknr.value().c_str());
				return false;
			}
		}
		else if (new_use_count < 0) {
			dolog(ll_error, "storage_backend_dedup::put_block(%s): new_use_count < 0! (%ld) dataset is corrupt!", id.c_str(), new_use_count);
			return false;
		}
	}

	// - calc hash over new-block
	std::string new_block_hash = h->do_hash(data_in, block_size);

	int64_t new_block_use_count = 0;
	if (get_use_count(new_block_hash, &new_block_use_count) == false) {
		dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to retrieve use-count for hash \"%s\"", id.c_str(), new_block_hash.c_str());
		return false;
	}

	// - new-block-hash exists? (count > 0)
	if (new_block_use_count > 0) {
		// - increase count for new-block-hash
		int64_t temp = 0;
		if (increase_use_count(new_block_hash, &temp) == false) {
			dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to increase use-count for data block with hash \"%s\"", id.c_str(), new_block_hash.c_str());
			return false;
		}

		if (temp != new_block_use_count + 1) {
			dolog(ll_error, "storage_backend_dedup::put_block(%s): new count (%ld) not as expected (%ld) hash \"%s\"", id.c_str(), temp, new_block_use_count + 1, new_block_hash.c_str());
			return false;
		}
	}
	else {  // new block
		// - count == 0:
		//   - set count to 1
		if (set_use_count(new_block_hash, 1) == false) {
			dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to set use-count for data block with hash \"%s\" to 1", id.c_str(), new_block_hash.c_str());
			return false;
		}

		// - put block
		if (put_key_value(get_data_key_for_hash(new_block_hash), data_in, block_size) == false) {
			dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to store data block with hash \"%s\"", id.c_str(), new_block_hash.c_str());
			return false;
		}
	}

	// - put mapping blocknr to new-block-hash
	if (map_blocknr_to_hash(block_nr, new_block_hash) == false) {
		dolog(ll_error, "storage_backend_dedup::put_block(%s): failed to map blocknr %ld to hash \"%s\"", id.c_str(), block_nr, new_block_hash.c_str());
		return false;
	}

	return true;
}

void storage_backend_dedup::un_lock_block_group(const offset_t offset, const uint32_t size, const bool do_lock, const bool shared)
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

void storage_backend_dedup::get_data(const offset_t offset, const uint32_t size, block **const b, int *const err)
{
	*err = 0;
	*b = nullptr;

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
			dolog(ll_error, "storage_backend_dedup::get_data(%s): failed to retrieve block %ld", id.c_str(), block_nr);
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

	un_lock_block_group(offset, size, false, true);
}

void storage_backend_dedup::put_data(const offset_t offset, const block & b, int *const err)
{
	*err = 0;

	// TODO start transaction

	un_lock_block_group(offset, size, true, false);

	offset_t work_offset = offset;

	const uint8_t *input = b.get_data();
	size_t work_size = b.get_size();

	while(work_size > 0) {
		uint64_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		int current_size = std::min(work_size, size_t(block_size - block_offset));

		uint8_t *temp = nullptr;

		if (block_offset == 0 && current_size == block_size)
			temp = reinterpret_cast<uint8_t *>(calloc(1, block_size));
		else {
			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "storage_backend_dedup::put_data(%s): failed to retrieve block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				break;
			}
		}

		memcpy(&temp[block_offset], input, current_size);

		if (!put_block(block_nr, temp)) {
			dolog(ll_error, "storage_backend_dedup::put_data(%s): failed to update block %ld", id.c_str(), block_nr);
			*err = EINVAL;
			free(temp);
			break;
		}

		free(temp);

		work_offset += current_size;
		work_size -= current_size;
		input += current_size;
	}

	un_lock_block_group(offset, size, false, false);

	// TODO finish transaction

	if (do_mirror(offset, b) == false) {
		*err = EIO;
		dolog(ll_error, "storage_backend_dedup::put_data(%s): failed to send block (%zu bytes) to mirror(s) at offset %lu", id.c_str(), size, offset);
		return;
	}
}

bool storage_backend_dedup::fsync()
{
	// TODO

	if (do_sync_mirrors() == false) {
		dolog(ll_error, "storage_backend_dedup::fsync(%s): failed to sync data to mirror(s)", id.c_str());
		return false;
	}

	return true;
}

bool storage_backend_dedup::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

	un_lock_block_group(offset, len, true, false);

	offset_t work_offset = offset;
	size_t work_size = len;

	while(work_size > 0) {
		uint64_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		int current_size = std::min(work_size, size_t(block_size - block_offset));

		if (current_size == block_size) {
			// TODO
		}
		else {
			uint8_t *temp = nullptr;

			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "storage_backend_dedup::trim_zero(%s): failed to retrieve block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				break;
			}

			memset(&temp[block_offset], 0x00, current_size);

			if (!put_block(block_nr, temp)) {
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

	un_lock_block_group(offset, len, false, false);

	if (do_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "storage_backend_dedup::trim_zero(%s): failed to send to mirror(s)", id.c_str());
		return false;
	}

	return *err == 0;
}
