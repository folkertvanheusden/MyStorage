#include <vector>
#include <yaml-cpp/yaml.h>

#include "aoe-common.h"
#include "mirror.h"
#include "storage_backend.h"


class storage_backend_aoe : public storage_backend
{
private:
	const std::string dev_name;
	uint8_t           my_mac[6] { 0 };
	const uint16_t    major;
	const uint8_t     minor;
	mutable struct {
		int       fd { -1 };
		uint8_t   tgt_mac[6] { 0 };
		offset_t  size { 0 };
		int       mtu_size { 1500 };
	}                 connection;

	bool connect() const;
	bool do_ata_command(aoe_ata_t *const aa_in, const int len, uint8_t *const recv_buffer, const int rb_size, int *const err);
	void wait_for_packet(uint8_t *const recv_buffer, const int rb_size, bool *const error, bool *const timeout, int *const n_data) const;

	bool can_do_multiple_blocks() const override;

        bool get_block(const block_nr_t block_nr, uint8_t **const data) override;
        bool put_block(const block_nr_t block_nr, const uint8_t *const data) override;

public:
	storage_backend_aoe(const std::string & id, const std::vector<mirror *> & mirrors, const std::string & dev_name, const uint8_t my_mac[6], const uint16_t major, const uint8_t minor, const int mtu_size, const int block_size);
	virtual ~storage_backend_aoe();

	offset_t get_size() const override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	YAML::Node emit_configuration() const override;
	static storage_backend_aoe * load_configuration(const YAML::Node & node);
};
