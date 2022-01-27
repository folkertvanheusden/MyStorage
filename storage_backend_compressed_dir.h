#include <dirent.h>
#include <stdint.h>
#include <string>
#include <sys/types.h>
#include <yaml-cpp/yaml.h>

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
	DIR                    *dir_structure { nullptr };
	int                     dir_fd { -1 };

	bool get_block(const block_nr_t block_nr, uint8_t **const data);
	bool put_block(const block_nr_t block_nr, const uint8_t *const data);

	void un_lock_block_group(const offset_t offset, const uint32_t size, const bool do_lock, const bool shared);

public:
	storage_backend_compressed_dir(const std::string & id, const std::string & dir, const int block_size, const offset_t total_size, compresser *const c, const std::vector<mirror *> & mirrors);
	virtual ~storage_backend_compressed_dir();

	offset_t get_size() const override;
	void get_data(const offset_t offset, const uint32_t size, block **const b, int *const err) override;
	void put_data(const offset_t offset, const block & b, int *const err) override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	YAML::Node emit_configuration() const override;
	static storage_backend_compressed_dir * load_configuration(const YAML::Node & node);
};
