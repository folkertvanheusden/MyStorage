#include "journal.h"
#include "logging.h"
#include "storage_backend.h"
#include "storage_backend_compressed_dir.h"
#include "storage_backend_dedup.h"
#include "storage_backend_file.h"
#include "str.h"
#include "types.h"


storage_backend::storage_backend(const std::string & id, const int block_size, const std::vector<mirror *> & mirrors) : base(id), block_size(block_size), mirrors(mirrors)
{
	for(auto m : mirrors)
		m->acquire(this);
}

storage_backend::~storage_backend()
{
	for(auto m : mirrors) {
		m->release(this);

		if (m->obj_in_use_by().empty())
			delete m;
	}
}

storage_backend * storage_backend::load_configuration(const YAML::Node & node)
{
        const std::string type = str_tolower(node["type"].as<std::string>());

	if (type == "storage-backend-file")
		return storage_backend_file::load_configuration(node);
	else if (type == "storage-backend-dedup")
		return storage_backend_dedup::load_configuration(node);
	else if (type == "storage-backend-compressed-dir")
		return storage_backend_compressed_dir::load_configuration(node);
	else if (type == "journal")
		return journal::load_configuration(node);

	dolog(ll_error, "storage_backend::load_configuration: storage type \"%s\" is not known", type.c_str());

	return nullptr;
}

int storage_backend::get_block_size() const
{
	return block_size;
}

bool storage_backend::verify_mirror_sizes()
{
	for(auto m : mirrors) {
		if (m->get_size() < get_size()) {
			dolog(ll_error, "storage_backend::verify_mirror_sizes(%s): mirror %s is too small", id.c_str(), m->get_id().c_str());
			return false;
		}
	}

	return true;
}

bool storage_backend::do_mirror(const offset_t offset, const block & b)
{
	bool ok = true;

	for(auto m : mirrors) {
		if (m->put_block(offset, b) == false) {
			ok = false;
			dolog(ll_error, "storage_backend::do_mirror(%s): failed writing to mirror %s", id.c_str(), m->get_id().c_str());
		}
	}

	return ok;
}

bool storage_backend::do_sync_mirrors()
{
	bool ok = true;

	for(auto m : mirrors) {
		if (m->sync() == false) {
			ok = false;
			dolog(ll_error, "storage_backend::do_sync_mirrors(%s): failed syncing mirror %s", id.c_str(), m->get_id().c_str());
		}
	}

	return ok;
}

bool storage_backend::do_trim_zero(const offset_t offset, const uint32_t size, const bool trim)
{
	bool ok = true;

	for(auto m : mirrors) {
		if (m->trim_zero(offset, size, trim) == false) {
			ok = false;
			dolog(ll_error, "storage_backend::do_trim_zero(%s): failed trim/zero mirror %s", id.c_str(), m->get_id().c_str());
		}
	}

	return ok;
}

void storage_backend::put_data(const offset_t offset, const std::vector<uint8_t> & d, int *const err)
{
	*err = 0;

	block b(d);

	put_data(offset, b, err);
}

void storage_backend::get_data(const offset_t offset, const uint32_t size, block **const b, int *const err)
{
	uint8_t *out = nullptr;
	get_data(offset, size, &out, err);

	*b = new block(out, size);
}

void storage_backend::get_data(const offset_t offset, const uint32_t size, uint8_t **const out, int *const err)
{
	*err = 0;

	lg.un_lock_block_group(offset, size, block_size, true, true);

	*out = nullptr;
	uint32_t out_size = 0;

	offset_t work_offset = offset;
	uint32_t work_size = size;

	while(work_size > 0) {
		block_nr_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		uint32_t current_size = std::min(work_size, block_size - block_offset);

		uint8_t *temp = nullptr;

		if (!get_block(block_nr, &temp)) {
			dolog(ll_error, "storage_backend::get_data(%s): failed to retrieve block %ld", id.c_str(), block_nr);
			*err = EINVAL;
			free(out);
			break;
		}

		*out = reinterpret_cast<uint8_t *>(realloc(*out, out_size + current_size));
		memcpy(&(*out)[out_size], &temp[block_offset], current_size);
		out_size += current_size;

		free(temp);

		work_offset += current_size;
		work_size -= current_size;
	}

	lg.un_lock_block_group(offset, size, block_size, false, true);
}

void storage_backend::put_data(const offset_t offset, const block & b, int *const err)
{
	*err = 0;

	if (transaction_start() == false) {
		dolog(ll_error, "storage_backend::put_data(%s): failed to start transaction", id.c_str());
		*err = EINVAL;
		return;
	}

	lg.un_lock_block_group(offset, b.get_size(), block_size, true, false);

	offset_t work_offset = offset;

	const uint8_t *input = b.get_data();
	size_t work_size = b.get_size();

	while(work_size > 0) {
		block_nr_t block_nr = work_offset / block_size;
		uint32_t block_offset = work_offset % block_size;

		int current_size = std::min(work_size, size_t(block_size - block_offset));

		uint8_t *temp = nullptr;

		if (block_offset == 0 && current_size == block_size)
			temp = reinterpret_cast<uint8_t *>(malloc(block_size));
		else {
			if (!get_block(block_nr, &temp)) {
				dolog(ll_error, "storage_backend::put_data(%s): failed to retrieve block %ld", id.c_str(), block_nr);
				*err = EINVAL;
				break;
			}
		}

		memcpy(&temp[block_offset], input, current_size);

		if (!put_block(block_nr, temp)) {
			dolog(ll_error, "storage_backend::put_data(%s): failed to update block %ld", id.c_str(), block_nr);
			*err = EINVAL;
			free(temp);
			break;
		}

		free(temp);

		work_offset += current_size;
		work_size -= current_size;
		input += current_size;
	}

	if (transaction_end() == false) {
		dolog(ll_error, "storage_backend::put_data(%s): failed to end transaction", id.c_str());
		*err = EINVAL;
	}

	lg.un_lock_block_group(offset, b.get_size(), block_size, false, false);

	if (*err == 0 && do_mirror(offset, b) == false) {
		*err = EIO;
		dolog(ll_error, "storage_backend::put_data(%s): failed to send block (%zu bytes) to mirror(s) at offset %lu", id.c_str(), b.get_size(), offset);
	}
}

int storage_backend::get_maximum_transaction_size() const
{
	return 1 << 31;
}

bool storage_backend::transaction_start()
{
	return true;
}

bool storage_backend::transaction_end()
{
	return true;
}
