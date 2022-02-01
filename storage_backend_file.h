#include <stdint.h>
#include <string>
#include <yaml-cpp/yaml.h>

#include "block.h"
#include "storage_backend.h"


class storage_backend_file : public storage_backend
{
private:
	int               fd { -1 };
	offset_t          size { 0 };
	const std::string file;

protected:
	bool get_block(const block_nr_t block_nr, uint8_t **const data) override;
	bool put_block(const block_nr_t block_nr, const uint8_t *const data) override;

	bool can_do_multiple_blocks() const override;
	bool get_multiple_blocks(const block_nr_t block_nr, const block_nr_t blocks_to_do, uint8_t *const to) override;

public:
	storage_backend_file(const std::string & id, const std::string & file, const int block_size, const std::vector<mirror *> & mirrors);
	virtual ~storage_backend_file();

	offset_t get_size() const override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	static storage_backend_file * load_configuration(const YAML::Node & node);
	YAML::Node emit_configuration() const override;
};
