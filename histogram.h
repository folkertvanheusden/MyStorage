#include <stdint.h>
#include <string>


class histogram
{
private:
	int       n_slots { 0 };
	uint64_t  divider { 0 };
	uint64_t *counters { nullptr };

public:
	histogram(const uint64_t max_value, const int n_slots);
	~histogram();

	void count(const uint64_t value);

	uint64_t get_count(const int slot) const;
	int get_n_slots() const;

	void dump(const std::string & filename);
};
