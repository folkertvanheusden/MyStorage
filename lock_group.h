#include <shared_mutex>
#include <vector>


class lock_group
{
private:
	std::vector<std::shared_mutex *> locks;

	int nr_to_lock(const uint64_t nr);

public:
	lock_group(const int n_locks = 128);
	virtual ~lock_group();

	void lock_shared(const uint64_t nr);
	void unlock_shared(const uint64_t nr);

	void lock_private(const uint64_t nr);
	void unlock_private(const uint64_t nr);
};
