#include <stdint.h>
#include <string>
#include <yaml-cpp/yaml.h>

#include "block.h"
#include "socket_client.h"
#include "storage_backend.h"


class storage_backend_nbd : public storage_backend
{
private:
	int                  fd { -1 };
	offset_t             size { 0 };
	socket_client *const sc { nullptr };
	const std::string    export_name;
	uint64_t             seq_nr { 0 };

	bool reconnect();

protected:
	bool get_block(const block_nr_t block_nr, uint8_t **const data) override;
	bool get_multiple_blocks(const block_nr_t block_nr, const block_nr_t blocks_to_do, uint8_t *to) override;

	bool put_block(const block_nr_t block_nr, const uint8_t *const data) override;

	bool can_do_multiple_blocks() const override;

public:
	storage_backend_nbd(const std::string & id, socket_client *const sc, const std::string & export_name, const int block_size, const std::vector<mirror *> & mirrors);
	virtual ~storage_backend_nbd();

	offset_t get_size() const override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	static storage_backend_nbd * load_configuration(const YAML::Node & node);
	YAML::Node emit_configuration() const override;
};
