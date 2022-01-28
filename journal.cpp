#include <errno.h>
#include <string.h>

#include "block.h"
#include "hash.h"
#include "journal.h"
#include "logging.h"
#include "str.h"


journal::journal(const std::string & id, storage_backend *const data, storage_backend *const journal_) : storage_backend(id, data->get_block_size(), { }), data(data), journal_(journal_)
{
	// retrieve journal meta data from storage
	block *b = nullptr;
	int err = 0;
	journal_->get_data(0, sizeof(jm), &b, &err);
	if (err)
		throw myformat("journal: failed to retrieve meta-data: %s", strerror(err));

	memcpy(&jm, b->get_data(), sizeof(jm));

	if (size_t(block_size) < 512 + sizeof(jm))
		throw myformat("journal: block size (%d) too small, must be at least %d bytes", block_size, 512 + sizeof(jm));

	if (journal_->get_size() < offset_t(block_size))
		throw myformat("journal: journal must be at least one block in size (%d bytes)", block_size);

	if (jm.block_size == 0 && jm.n_elements == 0 && jm.crc == 0 && memcmp(jm.sig, journal_signature, sizeof jm.sig) != 0) {
		dolog(ll_info, "journal: new journal");

		jm.block_size = block_size;
		jm.n_elements = (journal_->get_size() - block_size) / (sizeof(journal_element_t) + block_size);
		memcpy(jm.sig, journal_signature, sizeof jm.sig);
	}
	else {
		constexpr int crc_offset = sizeof(jm.crc);
		uint32_t crc = calc_crc(b->get_data() + crc_offset, b->get_size() - crc_offset);

		if (crc != jm.crc)
			throw myformat("journal: CRC mismatch (expecting: %08x, retrieved: %08x)", jm.crc, crc);
	}

	delete b;

	if (jm.n_elements == 0)
		throw myformat("journal: journal of 0 elements in size?");

	if (jm.block_size != journal_->get_block_size())
		throw myformat("journal: mismatch between the block sizes of the journal storage and the journal dump file");

	dolog(ll_info, "journal: %lu elements total, number filled: %lu", jm.n_elements, jm.cur_n);

	// load journal in cache
	const size_t target_size = sizeof(journal_element_t) + jm.block_size;

	int work_read_pointer = jm.read_pointer;

	for(block_nr_t i=0; i<jm.cur_n; i++) {
		block *b = nullptr;
		int err = 0;
		journal_->get_data((work_read_pointer + 1) * target_size, target_size, &b, &err);
		if (err) {
			dolog(ll_error, "journal: failed to retrieve journal action %d from storage", work_read_pointer);
			break;
		}

		const journal_element_t *const je = reinterpret_cast<const journal_element_t *>(b->get_data());

		constexpr int crc_offset = sizeof(je->crc);
		uint32_t crc = calc_crc(b->get_data() + crc_offset, b->get_size() - crc_offset);

		if (crc != je->crc) {
			delete b;
			dolog(ll_error, "journal: CRC mismatch (expecting: %08x, retrieved: %08x)", je->crc, crc);
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

// 'lock' must be locked
bool journal::update_journal_meta_data()
{
	constexpr int crc_offset = sizeof(uint32_t);
	jm.crc = calc_crc(reinterpret_cast<const uint8_t *>(&jm) + crc_offset, sizeof(journal_meta_t) - crc_offset);

	block jm_b(reinterpret_cast<uint8_t *>(&jm), sizeof(journal_meta_t), false);

	int err = 0;
	journal_->put_data(0, jm_b, &err);
	if (err) {
		dolog(ll_error, "journal::update_journal_meta_data: failed to update journal meta: %s", strerror(err));
		return false;
	}

	return true;
}

// 'lock' must be locked
bool journal::put_in_cache(const journal_element_t *const je)
{
	dolog(ll_debug, "journal: put element %ld in cache", je->target_block);

	uint8_t *data = reinterpret_cast<uint8_t *>(calloc(1, jm.block_size));
	if (!data) {
		dolog(ll_error, "journal::put_in_cache: cannot allocate %zu bytes of memory", jm.block_size);
		return false;
	}

	if (je->a == JA_write)
		memcpy(data, je->data, jm.block_size);
	else if (je->a == JA_trim || je->a == JA_zero) {
		// calloc takes care of this
	}
	else {
		dolog(ll_error, "journal::put_in_cache: unknown action %d", je->a);
		free(data);
		return false;
	}

	block b(data, jm.block_size);

	auto it = cache.find(je->target_block);
	if (it == cache.end())
		cache.insert({ je->target_block, { b, 1 } });
	else
		it->second.second++;

	return true;
}

bool journal::push_action(const journal_action_t a, const block_nr_t block_nr, const block & data)
{
	if (journal_commit_fatal_error) {
		dolog(ll_error, "journal::push_action: cannot put; journal in error state");
		return false;
	}

	if (stop_flag) {
		dolog(ll_error, "journal::push_action: cannot put; journal stopped");
		return false;
	}

	// verify that blocks of data in the journal are the same size as the backend ones
	if (data.get_size() != size_t(jm.block_size) && data.empty() == false) {
		dolog(ll_error, "journal::push_action: data size (%zu) not expected size (%d)", data.get_size(), jm.block_size);
		return false;
	}

	std::unique_lock<std::mutex> lck(lock);

	while(jm.full && !stop_flag)
		cond_pull.wait(lck);

	if (stop_flag) {
		dolog(ll_error, "journal::push_action: action terminated early");
		return false;
	}

	// create element
	const size_t target_size = sizeof(journal_element_t) + jm.block_size;

	uint8_t *j_element = reinterpret_cast<uint8_t *>(calloc(1, target_size));
	if (!j_element) {
		dolog(ll_error, "journal::push_action: cannot allocate %zu bytes of memory", target_size);
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
	journal_->put_data((jm.write_pointer + 1) * target_size, b, &err);

	if (err) {
		dolog(ll_error, "journal::push_action: failed to write journal element: %s", strerror(err));
		return false;
	}

	// update meta-data
	jm.write_pointer++;
	jm.write_pointer %= jm.n_elements;

	jm.full = jm.write_pointer == jm.read_pointer;

	jm.cur_n++;

	put_in_cache(reinterpret_cast<journal_element_t *>(j_element));

	if (update_journal_meta_data() == false) {
		dolog(ll_error, "journal::push_action: failed to write journal meta-data to disk");
		return false;
	}

	// make sure journal is on disk
	if (journal_->fsync() == false) {
		dolog(ll_error, "journal::push_action: failed to sync journal to disk");
		return false;
	}

	cond_push.notify_all();

	return true;
}

void journal::operator()()
{
	dolog(ll_info, "journal::operator: thread started");

	while(!stop_flag) {
		std::unique_lock<std::mutex> lck(lock);

		while(jm.read_pointer == jm.write_pointer && !jm.full && !stop_flag)
			cond_push.wait(lck);

		if (stop_flag) {
			dolog(ll_debug, "journal::operator: thread terminating");
			break;
		}

		// retrieve action from journal
		const size_t target_size = sizeof(journal_element_t) + jm.block_size;

		block *b = nullptr;
		int err = 0;
		journal_->get_data((jm.read_pointer + 1) * target_size, target_size, &b, &err);
		if (err) {
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator: failed to retrieve journal action from storage");
			break;
		}

		const journal_element_t *const je = reinterpret_cast<const journal_element_t *>(b->get_data());

		constexpr int crc_offset = sizeof(uint32_t);
		uint32_t crc = calc_crc(b->get_data() + crc_offset, b->get_size() - crc_offset);

		if (crc != je->crc) {
			delete b;
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator: CRC mismatch (expecting: %08x, retrieved: %08x)", je->crc, crc);
			break;
		}

		dolog(ll_debug, "journal::operator: storing block %lu", je->target_block);

		if (je->a == JA_write) {
			block b2(je->data, jm.block_size, false);

			int err = 0;
			data->put_data(je->target_block * jm.block_size, b2, &err);

			if (err) {
				delete b;
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator: failed to write data to storage: %s", strerror(err));
				break;
			}
		}
		else if (je->a == JA_trim || je->a == JA_zero) {
			int err = 0;
			if (data->trim_zero(je->target_block * jm.block_size, jm.block_size, je->a == JA_trim, &err) == false) {
				delete b;
				journal_commit_fatal_error = true;
				dolog(ll_error, "journal::operator: failed to trim/zero storage: %s", strerror(err));
				break;
			}
		}
		else {
			delete b;
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator: unknown action %d", je->a);
			break;
		}

		// update journal meta-data
		int clean_element = jm.read_pointer;
		jm.read_pointer++;
		jm.read_pointer %= jm.n_elements;

		jm.full = 0;

		jm.cur_n--;

		auto it = cache.find(je->target_block);
		if (it == cache.end()) {
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator: block %lu not in RAM cache", je->target_block);
			delete b;
			break;
		}

		it->second.second--;

		if (it->second.second == 0) {
			dolog(ll_debug, "journal::operator: remove block %lu from RAM cache", je->target_block);
			cache.erase(je->target_block);
		}

		delete b;

		if (update_journal_meta_data() == false) {
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator: failed to write journal meta-data to storage");
			break;
		}

		// make sure journal is on disk
		if (journal_->fsync() == false) {
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator: failed to sync journal to disk");
			break;
		}

		if (journal_->trim_zero((clean_element + 1) * target_size, target_size, true, &err) == false) {
			journal_commit_fatal_error = true;
			dolog(ll_error, "journal::operator: failed to clear journal entry");
			break;
		}

		cond_pull.notify_all();
	}

	dolog(ll_info, "journal::operator: thread terminated with %lu items in journal", jm.cur_n);
}

void journal::flush_journal()
{
	std::unique_lock<std::mutex> lck(lock);

	while(jm.cur_n > 0 && !stop_flag)
		cond_pull.wait(lck);

	dolog(ll_info, "journal::flush_journal: journal empty, terminating thread");

	stop_flag = true;

	cond_pull.notify_all();
	cond_push.notify_all();
}

offset_t journal::get_size() const
{
	return data->get_size();
}

bool journal::get_block(const block_nr_t block_nr, uint8_t **const data)
{
	bool rc = false;

	std::unique_lock<std::mutex> lck(lock);

	auto it = cache.find(block_nr);
	if (it != cache.end()) {
		*data = reinterpret_cast<uint8_t *>(malloc(it->second.first.get_size()));
		if (!*data)
			dolog(ll_error, "journal::get_block: cannot allocate %zu bytes of memory", it->second.first.get_size());
		else {
			memcpy(*data, it->second.first.get_data(), it->second.first.get_size());
			rc = true;
		}
	}
	else {
		int err = 0;
		this->data->get_data(block_nr * block_size, block_size, data, &err);

		if (err)
			dolog(ll_error, "journal::get_block: failed to retrieve block %ld from storage: %s", block_nr, strerror(errno));
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
		dolog(ll_error, "journal::put_block: cannot write block %ld", block_nr);
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
				dolog(ll_error, "journal::trim_zero: failed to %s block %ld", block_nr, trim ? "trim" : "zero");
				break;
			}
		}
		else {
			uint8_t *temp = nullptr;

			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "journal::trim_zero: failed to retrieve block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				break;
			}

			memset(&temp[block_offset], 0x00, current_size);

			if (!put_block(block_nr, temp)) {
				dolog(ll_error, "journal::trim_zero: failed to update block %ld", id.c_str(), block_nr);
				*err = EINVAL;
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

	if (do_trim_zero(offset, len, trim) == false) {
		dolog(ll_error, "journal::trim_zero: failed to send to mirror(s)", id.c_str());
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

	YAML::Node out;
	out["type"] = "journal";
	out["cfg"] = out_cfg;

	return out;
}

journal * journal::load_configuration(const YAML::Node & node)
{
	const YAML::Node cfg = node["cfg"];

	std::string id = cfg["id"].as<std::string>();

	storage_backend *sb_data = storage_backend::load_configuration(cfg["storage-backend_data"]);
	storage_backend *sb_journal = storage_backend::load_configuration(cfg["storage-backend_journal"]);

	return new journal(id, sb_data, sb_journal);
}
