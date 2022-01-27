#include <stdint.h>

#include "compresser.h"


class compresser_lzo : public compresser
{
public:
	compresser_lzo();
	virtual ~compresser_lzo();

	bool compress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) override;
	bool decompress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) override;

	YAML::Node emit_configuration() const override;
	static compresser_lzo * load_configuration(const YAML::Node & node);
};
