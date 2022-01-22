#pragma once
#include <stdint.h>
#include <stdlib.h>


class block {
private:
	uint8_t *const data;
	const size_t len;

public:
	block(const uint8_t *const data, const size_t len);
	virtual ~block();

	size_t get_size() const;

	void get_data(uint8_t *const target) const;
};
