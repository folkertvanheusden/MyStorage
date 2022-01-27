#pragma once
#include <set>
#include <shared_mutex>
#include <vector>

#include "types.h"


class lock_group
{
private:
	std::vector<std::shared_mutex *> locks;

	std::set<uint64_t> nrs_to_locks(const std::vector<block_nr_t> & nrs);

public:
	lock_group(const int n_locks = 128);
	virtual ~lock_group();

	void lock_shared(const std::vector<block_nr_t> & nrs);
	void unlock_shared(const std::vector<block_nr_t> & nrs);

	void lock_private(const std::vector<block_nr_t> & nrs);
	void unlock_private(const std::vector<block_nr_t> & nrs);

	void un_lock_block_group(const offset_t offset, const uint32_t size, const int block_size, const bool do_lock, const bool shared);
};
