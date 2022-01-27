#include "lock_group.h"
#include "types.h"


lock_group::lock_group(const int n_locks)
{
	for(int i=0; i<n_locks; i++)
		locks.push_back(new std::shared_mutex());
}

lock_group::~lock_group()
{
	while(!locks.empty()) {
		delete locks.at(0);

		locks.erase(locks.begin());
	}
}

std::set<uint64_t> lock_group::nrs_to_locks(const std::vector<block_nr_t> & nrs)
{
	std::set<uint64_t> unique_nrs;

	for(auto nr : nrs)
		unique_nrs.insert(nr % locks.size());

	return unique_nrs;
}

void lock_group::lock_shared(const std::vector<block_nr_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->lock_shared();
}

void lock_group::unlock_shared(const std::vector<block_nr_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->unlock_shared();
}

void lock_group::lock_private(const std::vector<block_nr_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->lock();
}

void lock_group::unlock_private(const std::vector<block_nr_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->unlock();
}

void lock_group::un_lock_block_group(const offset_t offset, const uint32_t size, const int block_size, const bool do_lock, const bool shared)
{
	std::vector<block_nr_t> block_nrs;

	for(offset_t o=offset; o<offset + size; o += block_size)
		block_nrs.push_back(o / block_size);

	if (do_lock) {
		if (shared)
			lock_shared(block_nrs);
		else
			lock_private(block_nrs);
	}
	else {
		if (shared)
			unlock_shared(block_nrs);
		else
			unlock_private(block_nrs);
	}
}
