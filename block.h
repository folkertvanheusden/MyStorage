#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <vector>


// This class is a wrapper around a pointer/size pair.
// The owner ship of the data it wraps is moved to the block class!
// (unless the not_free constructor is used)
class block {
private:
	const uint8_t *const data;
	const size_t         len;
	const bool           do_free;

public:
	block(const uint8_t *const data, const size_t len, const bool do_free);
	block(const uint8_t *const data, const size_t len);
	block(const std::vector<uint8_t> & data);
	block(const block & other);
	virtual ~block();

	bool empty() const;

	size_t get_size() const;

	const uint8_t * get_data() const;
};
