#include <stdint.h>
#include <yaml-cpp/yaml.h>

#include "compresser.h"


class compresser_zlib : public compresser
{
private:
	const int compression_level;

public:
	compresser_zlib(const int compression_level);
	virtual ~compresser_zlib();

	std::string get_type() const override;

	bool compress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) override;
	bool decompress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len) override;

	YAML::Node emit_configuration() const override;
	static compresser_zlib * load_configuration(const YAML::Node & node);
};
