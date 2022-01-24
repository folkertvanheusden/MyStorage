#include "lock_group.h"


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

std::set<uint64_t> lock_group::nrs_to_locks(const std::vector<uint64_t> & nrs)
{
	std::set<uint64_t> unique_nrs;

	for(auto nr : nrs)
		unique_nrs.insert(nr % locks.size());

	return unique_nrs;
}

void lock_group::lock_shared(const std::vector<uint64_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->lock_shared();
}

void lock_group::unlock_shared(const std::vector<uint64_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->unlock_shared();
}

void lock_group::lock_private(const std::vector<uint64_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->lock();
}

void lock_group::unlock_private(const std::vector<uint64_t> & nrs)
{
	for(auto nr : nrs_to_locks(nrs))
		locks.at(nr)->unlock();
}
