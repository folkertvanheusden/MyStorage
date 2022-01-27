#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "base.h"
#include "block.h"
#include "mirror.h"
#include "types.h"


class storage_backend : public base
{
protected:
	const std::vector<mirror *> mirrors;

	bool do_mirror(const offset_t offset, const block & b);
	bool do_sync_mirrors();
	bool do_trim_zero(const offset_t offset, const uint32_t size, const bool trim);
	bool verify_mirror_sizes();

public:
	storage_backend(const std::string & id, const std::vector<mirror *> & mirrors);
	virtual ~storage_backend();

	virtual offset_t get_size() const = 0;

	virtual void get_data(const offset_t offset, const uint32_t size, block **const b, int *const err) = 0;
	virtual void put_data(const offset_t offset, const block & b, int *const err) = 0;
	void put_data(const offset_t offset, const std::vector<uint8_t> & d, int *const err);

	virtual bool fsync() = 0;

	virtual bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) = 0;

	static storage_backend * load_configuration(const YAML::Node & node);
};
