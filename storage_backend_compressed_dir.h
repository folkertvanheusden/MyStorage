#include <stdint.h>
#include <string>

#include "block.h"
#include "compresser.h"
#include "lock_group.h"
#include "storage_backend.h"


class storage_backend_compressed_dir : public storage_backend
{
private:
	const std::string       dir;
	const int               block_size;
	const offset_t          total_size;
	compresser       *const c;
	lock_group              lg;

	bool get_block(const uint64_t block_nr, uint8_t **const data);
	bool put_block(const uint64_t block_nr, const uint8_t *const data);

	void un_lock_block_group(const offset_t offset, const uint32_t size, const bool do_lock, const bool shared);

public:
	storage_backend_compressed_dir(const std::string & id, const std::string & dir, const int block_size, const offset_t total_size, compresser *const c);
	virtual ~storage_backend_compressed_dir();

	offset_t get_size() const override;
	void get_data(const offset_t offset, const uint32_t size, block **const b, int *const err) override;
	void put_data(const offset_t offset, const block & b, int *const err) override;

	void fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;
};
