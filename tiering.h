#include <stdint.h>
#include <string>

#include "lock_group.h"
#include "mirror.h"
#include "storage_backend.h"


#define DESCRIPTORS_PER_BIN 2  // makes the structure exactly 64 bytes (1 cache line)
#define TF_dirty            1

typedef struct {
	uint64_t complete_block_nr_hash;
	uint64_t block_nr_slow_storage;
	uint64_t age;
	uint64_t flags;
} descriptor_t;

typedef struct {
	descriptor_t d[DESCRIPTORS_PER_BIN];
} descriptor_bin_t;

class tiering : public storage_backend
{
private:
	storage_backend *const fast_storage { nullptr };
	storage_backend *const slow_storage { nullptr };
	storage_backend *const meta_storage { nullptr };

	lock_group             lgt;

	std::atomic_uint64_t   age { 0 };  // die moet ook opgeslagen worden in een meta-data file TODO

	// indexed by block_nr_hash % n_entries
	uint64_t               map_n_entries { 0 };
	descriptor_bin_t      *map { nullptr };

	uint64_t hash_block_nr(const uint64_t block_nr);

	bool can_do_multiple_blocks() const override;

        bool get_block(const block_nr_t block_nr, uint8_t **const data) override;
        bool put_block(const block_nr_t block_nr, const uint8_t *const data) override;

public:
	tiering(const std::string & id, storage_backend *const fast_storage, storage_backend *const slow_storage, storage_backend *const meta_storage, const std::vector<mirror *> & mirrors);
	virtual ~tiering();

	static std::pair<uint64_t, int> get_meta_dimensions(const offset_t fast_storage_size, const int fast_storage_block_size);

	offset_t get_size() const override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	bool fsync() override;

	YAML::Node emit_configuration() const override;
	static tiering *load_configuration(const YAML::Node&);
};
