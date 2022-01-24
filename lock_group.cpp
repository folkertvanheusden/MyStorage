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

int lock_group::nr_to_lock(const uint64_t nr)
{
	return nr % locks.size();
}

void lock_group::lock_shared(const uint64_t nr)
{
	locks.at(nr_to_lock(nr))->lock_shared();
}

void lock_group::unlock_shared(const uint64_t nr)
{
	locks.at(nr_to_lock(nr))->unlock_shared();
}

void lock_group::lock_private(const uint64_t nr)
{
	locks.at(nr_to_lock(nr))->lock();
}

void lock_group::unlock_private(const uint64_t nr)
{
	locks.at(nr_to_lock(nr))->unlock();
}
