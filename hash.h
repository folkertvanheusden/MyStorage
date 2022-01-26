#pragma once
#include <optional>
#include <stdint.h>
#include <stdlib.h>
#include <string>


class hash
{
public:
	hash();
	virtual ~hash();

	virtual int get_size() const = 0;

	// return binary
	virtual void do_hash(const uint8_t *const in, const size_t len, uint8_t **const out) = 0;

	// return hex string
	std::optional<std::string> do_hash(const uint8_t *const in, const size_t len);
};
