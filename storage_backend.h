#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "base.h"
#include "block.h"
#include "lock_group.h"
#include "mirror.h"
#include "types.h"


class storage_backend : public base
{
private:
	bool do_mirror(const offset_t offset, const block & b);

protected:
	friend class snapshots;

	const int                   block_size { 4096 };
	const std::vector<mirror *> mirrors;
	lock_group                  lg;

	bool do_sync_mirrors();
	bool verify_mirror_sizes();
	bool do_mirror_trim_zero(const offset_t offset, const uint32_t size, const bool trim);

	virtual bool can_do_multiple_blocks() const = 0;
	virtual bool get_multiple_blocks(const block_nr_t block_nr, const block_nr_t blocks_to_do, uint8_t *const to);

        virtual bool get_block(const block_nr_t block_nr, uint8_t **const data) = 0;
        virtual bool put_block(const block_nr_t block_nr, const uint8_t *const data) = 0;

	// used by put_data
	virtual bool transaction_start();
	virtual bool transaction_end();

public:
	storage_backend(const std::string & id, const int block_size, const std::vector<mirror *> & mirrors);
	virtual ~storage_backend();

	virtual offset_t get_size() const = 0;
	int get_block_size() const;

	virtual int get_maximum_transaction_size() const;

	void get_data(const offset_t offset, const uint32_t size, uint8_t **const d, int *const err);
	void get_data(const offset_t offset, const uint32_t size, block **const b, int *const err);
	virtual void put_data(const offset_t offset, const block & b, int *const err);
	virtual void put_data(const offset_t offset, const std::vector<uint8_t> & d, int *const err);

	virtual bool fsync() = 0;

	virtual bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) = 0;

	static storage_backend * load_configuration(const YAML::Node & node);
};
