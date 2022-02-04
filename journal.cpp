#include <errno.h>
#include <optional>
#include <stdint.h>
#include <string.h>

#include "block.h"
#include "hash.h"
#include "journal.h"
#include "logging.h"
#include "str.h"
#include "time.h"
#include "yaml-helpers.h"


journal::journal(const std::string & id, storage_backend *const data, storage_backend *const journal_, const unsigned int flush_interval) : storage_backend(id, data->get_block_size(), { }), data(data), journal_(journal_), flush_interval(flush_interval)
{
	// retrieve journal meta data from storage
	block *b = nullptr;
	int err = 0;
	journal_->get_data(0, sizeof(jm), &b, &err);
	if (err)
		throw myformat("journal(%s): failed to retrieve meta-data: %s", id.c_str(), strerror(err));

	memcpy(&jm, b->get_data(), sizeof(jm));

	if (size_t(block_size) < 512 + sizeof(jm))
		throw myformat("journal(%s): block size (%d) too small, must be at least %ld bytes", id.c_str(), block_size, 512 + sizeof(jm));

	if (journal_->get_size() < offset_t(block_size))
		throw myformat("journal(%s): journal must be at least one block in size (%d bytes)", id.c_str(), block_size);

	if (jm.block_size == 0 && jm.n_elements == 0 && jm.crc == 0 && memcmp(jm.sig, journal_signature, sizeof jm.sig) != 0) {
		dolog(ll_info, "journal(%s): new journal", id.c_str());

		jm.block_size = block_size;
		jm.n_elements = (journal_->get_size() - (block_size + sizeof(journal_element_t))) / (sizeof(journal_element_t) + block_size);
		memcpy(jm.sig, journal_signature, sizeof jm.sig);
	}
	else {
		constexpr int crc_offset = sizeof(jm.crc);
		uint32_t crc = calc_crc(b->get_data() + crc_offset, b->get_size() - crc_offset);

		if (crc != jm.crc)
			throw myformat("journal(%s): CRC mismatch (expecting: %08x, retrieved: %08x)", id.c_str(), jm.crc, crc);
	}

	delete b;

	if (jm.n_elements == 0)
		throw myformat("journal(%s): journal of 0 elements in size?", id.c_str());

	dolog(ll_info, "journal(%s): %lu elements total, number filled: %lu", id.c_str(), jm.n_elements, jm.cur_n);

	// load journal in cache
	const size_t target_size = sizeof(journal_element_t) + jm.block_size;

	int work_read_pointer = jm.read_pointer;

	for(block_nr_t i=0; i<jm.cur_n; i++) {
		block *b = nullptr;
		int err = 0;
		journal_->get_data((work_read_pointer + 1) * target_size, target_size, &b, &err);
		if (err) {
			dolog(ll_error, "journal(%s): failed to retrieve journal action %d from storage", id.c_str(), work_read_pointer);
			break;
		}

		const journal_element_t *const je = reinterpret_cast<const journal_element_t *>(b->get_data());

		constexpr int crc_offset = sizeof(je->crc);
		uint32_t crc = calc_crc(b->get_data() + crc_offset, b->get_size() - crc_offset);

		if (crc != je->crc) {
			dolog(ll_error, "journal(%s): CRC mismatch (expecting: %08x, retrieved: %08x)", id.c_str(), je->crc, crc);
			dump(je);
			delete b;
			break;
		}

		put_in_cache(je);

		delete b;

		work_read_pointer = (work_read_pointer + 1) % jm.n_elements;
	}

	th = new std::thread(std::ref(*this));
}

journal::~journal()
{
	stop_flag = true;

	cond_push.notify_all();
	cond_pull.notify_all();

	if (th) {
		th->join();
		delete th;
	}
}

int journal::get_maximum_transaction_size() const
{
	return std::max(1ul, jm.n_elements * 3 / 4) * jm.block_size;
}

bool journal::transaction_start()
{
	return true;
}

bool journal::transaction_end()
{
	std::unique_lock<std::mutex> lck(lock);

	if (update_journal_meta_data() == false) {
		dolog(ll_error, "journal::push_action(%s): failed to write journal meta-data to disk", id.c_str());
		return false;
	}

	return true;
}

// 'lock' must be locked
bool journal::update_journal_meta_data()
{
	if (meta_dirty == 0)
		return true;

	dolog(ll_debug, "journal::update_journal_meta_data(%s): prevented %d write(s)", id.c_str(), meta_dirty);

	constexpr int crc_offset = sizeof(uint32_t);
	jm.crc = calc_crc(reinterpret_cast<const uint8_t *>(&jm) + crc_offset, sizeof(journal_meta_t) - crc_offset);

	block jm_b(reinterpret_cast<uint8_t *>(&jm), sizeof(journal_meta_t), false);

	int err = 0;
	journal_->put_data(0, jm_b, &err);
	if (err) {
		dolog(ll_error, "journal::update_journal_meta_data(%s): failed to update journal meta: %s", id.c_str(), strerror(err));
		return false;
	}

	// make sure journal is on disk
	if (journal_->fsync() == false) {
		dolog(ll_error, "journal::update_journal_meta_data(%s): failed to sync journal to disk", id.c_str());
		return false;
	}

	meta_dirty = 0;

	return true;
}

// 'lock' must be locked
bool journal::put_in_cache(const journal_element_t *const je)
{
	dolog(ll_debug, "journal::put_in_cache(%s): put element %ld in cache", id.c_str(), je->target_block);

	auto it = cache.find(je->target_block);

	uint8_t *data = reinterpret_cast<uint8_t *>(calloc(1, jm.block_size));
	if (!data) {
		dolog(ll_error, "journal::put_in_cache(%s): cannot allocate %zu bytes of memory", id.c_str(), jm.block_size);
		return false;
	}

	if (je->a == JA_write)
		memcpy(data, je->data, jm.block_size);
	else if (je->a == JA_trim || je->a == JA_zero) {
		// calloc takes care of this
	}
	else {
		dolog(ll_error, "journal::put_in_cache(%s): unknown action %d", id.c_str(), je->a);
		free(data);
		return false;
	}

	block b(data, jm.block_size);

	int n = 0;

	if (it != cache.end()) {
		n = it->second.second;
		cache.erase(it);
	}

	n++;

	cache.insert({ je->target_block, { b, n } });

	return true;
}

bool journal::push_action(const journal_action_t a, const block_nr_t block_nr, const block & data)
{
	if (journal_commit_fatal_error) {
		dolog(ll_error, "journal::push_action(%s): cannot put; journal in error state", id.c_str());
		return false;
	}

	if (stop_flag) {
		dolog(ll_error, "journal::push_action(%s): cannot put; journal stopped", id.c_str());
		return false;
	}

	// verify that blocks of data in the journal are the same size as the backend ones
	if (data.get_size() != size_t(jm.block_size) && data.empty() == false) {
		dolog(ll_error, "journal::push_action(%s): data size (%zu) not expected size (%d)", id.c_str(), data.get_size(), jm.block_size);
		return false;
	}

	std::unique_lock<std::mutex> lck(lock);

	while(jm.full && !stop_flag)
		cond_pull.wait(lck);

	if (stop_flag) {
		dolog(ll_error, "journal::push_action(%s): action terminated early", id.c_str());
		return false;
	}

	// create element
	const size_t target_size = sizeof(journal_element_t) + jm.block_size;

	uint8_t *j_element = reinterpret_cast<uint8_t *>(calloc(1, target_size));
	if (!j_element) {
		dolog(ll_error, "journal::push_action(%s): cannot allocate %zu bytes of memory", id.c_str(), target_size);
		return false;
	}

	reinterpret_cast<journal_element_t *>(j_element)->a            = a;  // action
	reinterpret_cast<journal_element_t *>(j_element)->target_block = block_nr;

	if (a == JA_write)
		memcpy(reinterpret_cast<journal_element_t *>(j_element)->data, data.get_data(), jm.block_size);  // any data

	constexpr int crc_offset = sizeof(uint32_t);
	uint32_t crc = calc_crc(j_element + crc_offset, target_size - crc_offset);
	reinterpret_cast<journal_element_t *>(j_element)->crc          = crc;

	block b(j_element, target_size);

	// store element in journal
	int err = 0;
	journal_->put_data(offset_t(jm.write_pointer + 1) * target_size, b, &err);

	if (err) {
		dolog(ll_error, "journal::push_action(%s): failed to write journal element (%d bytes in size) %d: %s", id.c_str(), target_size, jm.write_pointer, strerror(err));
		return false;
	}

	// update meta-data
	jm.write_pointer++;
	jm.write_pointer %= jm.n_elements;

	jm.full = jm.write_pointer == jm.read_pointer;

	jm.cur_n++;

	meta_dirty++;

	if (jm.cur_n > jm.n_elements) {
		dolog(ll_error, "journal::push_action(%s): more (%d) elements in journal than it can contain (%d)", id.c_str(), jm.cur_n, jm.n_elements);
		return false;
	}

	put_in_cache(reinterpret_cast<journal_element_t *>(j_element));

	cond_push.notify_all();

	return true;
}

void journal::dump(const journal_element_t *const je)
{
	dolog(ll_warning, "CRC: %08x, action: %d, target block: %ld, dump: %s", je->crc, je->a, je->target_block, bin_to_text(je->data, 16).c_str());
}

void journal::operator()()
{
	dolog(ll_info, "journal::operator(%s): thread started", id.c_str());

	uint64_t last_write = 0;

	while(!stop_flag && !journal_commit_fatal_error) {
		std::unique_lock<std::mutex> lck(lock);

		while(jm.read_pointer == jm.write_pointer && !jm.full && !stop_flag)
			cond_push.wait(lck);

		if (stop_flag) {
			dolog(ll_debug, "journal::operator(%s): thread terminating", id.c_str());
			break;
		}

		// try to group
		uint64_t now = get_ms();
		if (now - last_write < flush_interval && jm.cur_n < jm.n_elements / 8)
			continue;

		last_write = now;

		// retrieve action(s) from journal
		const size_t target_size = sizeof(journal_element_t) + jm.block_size;

		uint8_t          *buffer = nullptr;
		journal_action_t  ja = JA_invalid;
		block_nr_t        first_target_block = -1;
		block_nr_t        prev = -1;
		int               combine_n = 0;
		int               work_read_pointer = jm.read_pointer;

		for(;!journal_commit_fatal_error;) {
			// get
			block *local_b = nullptr;
			int err = 0;
			journal_->get_data((work_read_pointer + 1) * target_size, target_size, &local_b, &err);
			if (err) {
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator(%s): failed to retrieve journal action from storage", id.c_str());
				break;
			}

			const journal_element_t *const je = reinterpret_cast<const journal_element_t *>(local_b->get_data());

			// verify
			constexpr int crc_offset = sizeof(uint32_t);
			uint32_t crc = calc_crc(local_b->get_data() + crc_offset, local_b->get_size() - crc_offset);

			if (crc != je->crc) {
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator(%s): CRC mismatch (expecting: %08x, retrieved: %08x)", id.c_str(), je->crc, crc);
				dump(je);
				delete local_b;
				break;
			}

			// see if this entry is the same kind (action type) and
			// "connects" to the previous one
			if (ja == JA_invalid) {
				ja = je->a;
				first_target_block = prev = je->target_block;
				combine_n = 1;
			}
			else if (ja == je->a && je->target_block == prev + 1) {
				prev = je->target_block;
				combine_n++;
			}
			else {
				delete local_b;
				break;
			}

			if (ja == JA_write) {
				buffer = reinterpret_cast<uint8_t *>(realloc(buffer, combine_n * size_t(jm.block_size)));

				memcpy(&buffer[(combine_n - 1) * jm.block_size], je->data, jm.block_size);
			}

			delete local_b;

			work_read_pointer++;
			work_read_pointer %= jm.n_elements;

			if (work_read_pointer == jm.write_pointer)
				break;
		}

		if (journal_commit_fatal_error)
			break;

		if (ja == JA_write) {
			dolog(ll_debug, "journal::operator(%s): writing %d block(s) %lu", id.c_str(), combine_n, first_target_block);

			block b(buffer, combine_n * size_t(jm.block_size));

			int err = 0;
			data->put_data(first_target_block * jm.block_size, b, &err);

			if (err) {
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator(%s): failed to write data to storage: %s", id.c_str(), strerror(err));
				break;
			}
		}
		else if (ja == JA_trim || ja == JA_zero) {
			dolog(ll_debug, "journal::operator(%s): %s %d block(s) starting from %lu", id.c_str(), ja == JA_trim ? "trim" : "zero", combine_n, first_target_block);

			int err = 0;
			if (data->trim_zero(first_target_block * jm.block_size, combine_n * jm.block_size, ja == JA_trim, &err) == false) {
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator(%s): failed to trim/zero storage: %s", id.c_str(), strerror(err));
				break;
			}
		}
		else {
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator(%s): unknown action %d", id.c_str(), ja);
			break;
		}

		// update journal meta-data
		int clean_element = jm.read_pointer;
		jm.read_pointer += combine_n;
		jm.read_pointer %= jm.n_elements;

		jm.full = 0;

		jm.cur_n -= combine_n;

		meta_dirty += combine_n;

		// remove from cache
		block_nr_t work_target_block = first_target_block;

		for(int i=0; i<combine_n; i++) {
			auto it = cache.find(work_target_block);
			if (it == cache.end()) {
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator(%s): block %lu not in RAM cache", id.c_str(), work_target_block);
				break;
			}

			it->second.second--;

			if (it->second.second == 0) {
				dolog(ll_debug, "journal::operator(%s): remove block %lu from RAM cache", id.c_str(), work_target_block);
				cache.erase(it);
			}

			work_target_block++;
		}

		// update journal meta data (read/write pointers etc)
		if (update_journal_meta_data() == false) {
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator(%s): failed to write journal meta-data to storage", id.c_str());
			break;
		}

		// clear journal
		while(combine_n > 0) {
			int cur_n = std::min(jm.n_elements - clean_element, block_nr_t(combine_n));

			int err = 0;
			if (journal_->trim_zero((clean_element + 1) * target_size, target_size * cur_n, true, &err) == false) {
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator(%s): failed to clear journal entry/entries", id.c_str());
				break;
			}

			combine_n -= cur_n;

			clean_element += cur_n;
			clean_element %= jm.n_elements;
		}

		cond_pull.notify_all();
	}

	dolog(ll_info, "journal::operator(%s): thread terminated with %lu items in journal", id.c_str(), jm.cur_n);
}

void journal::flush_journal()
{
	std::unique_lock<std::mutex> lck(lock);

	// TODO trigger operator() to not wait for interval, but just write the blocks

	while(jm.cur_n > 0 && !stop_flag)
		cond_pull.wait(lck);

	dolog(ll_info, "journal::flush_journal(%s): journal empty, terminating thread", id.c_str());

	stop_flag = true;

	cond_pull.notify_all();
	cond_push.notify_all();
}

offset_t journal::get_size() const
{
	return data->get_size();
}

bool journal::can_do_multiple_blocks() const
{
	return false;
}

bool journal::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	bool rc = false;

	std::unique_lock<std::mutex> lck(lock);

	auto it = cache.find(block_nr);
	if (it != cache.end()) {
		*data = reinterpret_cast<uint8_t *>(malloc(it->second.first.get_size()));
		if (!*data)
			dolog(ll_error, "journal::get_block(%s): cannot allocate %zu bytes of memory", id.c_str(), it->second.first.get_size());
		else {
			memcpy(*data, it->second.first.get_data(), it->second.first.get_size());
			rc = true;
		}
	}
	else {
		int err = 0;
		this->data->get_data(block_nr * block_size, block_size, data, &err);

		if (err)
			dolog(ll_error, "journal::get_block(%s): failed to retrieve block %ld from storage: %s", id.c_str(), block_nr, strerror(errno));
		else
			rc = true;
	}

	return rc;
}

bool journal::put_block(const block_nr_t block_nr, const uint8_t *const data)
{
	block b(data, block_size, false);

	if (push_action(JA_write, block_nr, b) == false)
	{
		dolog(ll_error, "journal::put_block(%s): cannot write block %ld", id.c_str(), block_nr);
		return false;
	}

	return true;
}

bool journal::fsync()
{
	return true;
}

bool journal::trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err)
{
	*err = 0;

	uint8_t *b0x00 = reinterpret_cast<uint8_t *>(calloc(1, block_size));

	block empty_block(nullptr, 0);

	lgj.un_lock_block_group(offset, len, block_size, true, false);

	offset_t work_offset = offset;
	size_t work_size = len;

	while(work_size > 0) {
		block_nr_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		int current_size = std::min(work_size, size_t(block_size - block_offset));

		if (current_size == block_size) {
			bool rc = false;

			if (trim)
				rc = push_action(JA_trim, block_nr, empty_block);
			else
				rc = push_action(JA_zero, block_nr, empty_block);

			if (rc == false) {
				*err = EIO;
				dolog(ll_error, "journal::trim_zero(%s): failed to %s block %ld", id.c_str(), trim ? "trim" : "zero", block_nr);
				break;
			}
		}
		else {
			uint8_t *temp = nullptr;

			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "journal::trim_zero(%s): failed to retrieve block %ld", id.c_str(), id.c_str(), block_nr);
				*err = EIO;
				break;
			}

			memset(&temp[block_offset], 0x00, current_size);

			if (!put_block(block_nr, temp)) {
				dolog(ll_error, "journal::trim_zero(%s): failed to update block %ld", id.c_str(), id.c_str(), block_nr);
				*err = EIO;
				free(temp);
				break;
			}

			free(temp);
		}

		work_offset += current_size;
		work_size -= current_size;
	}

	lgj.un_lock_block_group(offset, len, block_size, false, false);

	free(b0x00);

	if (do_mirror_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "journal::trim_zero(%s): failed to send to mirror(s)", id.c_str(), id.c_str());
		return false;
	}

	return *err == 0;
}

YAML::Node journal::emit_configuration() const
{
	YAML::Node out_cfg;
	out_cfg["id"] = id;
	out_cfg["storage-backend_data"] = data->emit_configuration();
	out_cfg["storage-backend_journal"] = journal_->emit_configuration();
	out_cfg["flush-interval"] = flush_interval;

	YAML::Node out;
	out["type"] = "journal";
	out["cfg"] = out_cfg;

	return out;
}

journal * journal::load_configuration(const YAML::Node & node, std::optional<uint64_t> size, std::optional<int> block_size)
{
	dolog(ll_info, " * journal::load_configuration");

	const YAML::Node cfg = yaml_get_yaml_node(node, "cfg", "journal configuration");

	std::string id = yaml_get_string(cfg, "id", "module id");

	storage_backend *sb_data = storage_backend::load_configuration(cfg["storage-backend_data"], size, block_size);
	storage_backend *sb_journal = storage_backend::load_configuration(cfg["storage-backend_journal"], { }, { });

	int flush_interval = yaml_get_int(cfg, "flush-interval", "after how many milliseconds (max) to flush journalled blocks to disk");

	return new journal(id, sb_data, sb_journal, flush_interval);
}
