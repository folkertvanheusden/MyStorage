#include <set>
#include <shared_mutex>
#include <vector>


class lock_group
{
private:
	std::vector<std::shared_mutex *> locks;

	std::set<uint64_t> nrs_to_locks(const std::vector<uint64_t> & nrs);

public:
	lock_group(const int n_locks = 128);
	virtual ~lock_group();

	void lock_shared(const std::vector<uint64_t> & nrs);
	void unlock_shared(const std::vector<uint64_t> & nrs);

	void lock_private(const std::vector<uint64_t> & nrs);
	void unlock_private(const std::vector<uint64_t> & nrs);
};
