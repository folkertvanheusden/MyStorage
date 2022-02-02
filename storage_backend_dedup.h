#include <kcpolydb.h>
#include <optional>
#include <stdint.h>
#include <string>
#include <yaml-cpp/yaml.h>

#include "block.h"
#include "compresser.h"
#include "hash.h"
#include "lock_group.h"
#include "storage_backend.h"


class storage_backend_dedup : public storage_backend
{
private:
	hash          *const h { nullptr };
	compresser    *const c { nullptr };
	offset_t             size { 0 };
	lock_group           lgdd;
	kyotocabinet::PolyDB db;
	const std::string    file;

	bool get_block(const block_nr_t block_nr, uint8_t **const data) override;  // with locking
	bool get_block_int(const block_nr_t block_nr, uint8_t **const data);  // without locking
	bool put_block(const block_nr_t block_nr, const uint8_t *const data_in) override;  // with locking
	bool put_block_int(const block_nr_t block_nr, const uint8_t *const data_in);  // without locking

	void un_lock_block_group(const offset_t offset, const uint32_t size, const bool do_lock, const bool shared);

	bool put_key_value(const std::string & key, const uint8_t *const value, const int value_len);
	bool get_key_value(const std::string & key, uint8_t *const value, const int value_len, bool *const not_found);

	std::string get_usecount_key_for_hash(const std::string & hash);
	std::string get_hashforblocknr_key_for_blocknr(const block_nr_t block_nr);
	std::string get_data_key_for_hash(const std::string & hash);

	std::optional<std::string> get_hash_for_block(const block_nr_t block_nr);
	bool get_use_count(const std::string block_hash, int64_t *const new_count);
	bool set_use_count(const std::string block_hash, const int64_t new_count);
	bool increase_use_count(const std::string block_hash, int64_t *const new_count);
	bool decrease_use_count(const std::string block_hash, int64_t *const new_count);
	bool delete_block_by_hash(const std::string block_hash);
	bool delete_block_counter_by_hash(const std::string block_hash);
	bool map_blocknr_to_hash(const block_nr_t block_nr, const std::string & new_block_hash);

protected:
	bool can_do_multiple_blocks() const override;

public:
	storage_backend_dedup(const std::string & id, const std::string & file, hash *const h, compresser *const c, const std::vector<mirror *> & mirrors, const offset_t size, const int block_size);
	virtual ~storage_backend_dedup();

	offset_t get_size() const override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	static storage_backend_dedup * load_configuration(const YAML::Node & node);
	YAML::Node emit_configuration() const override;
};
