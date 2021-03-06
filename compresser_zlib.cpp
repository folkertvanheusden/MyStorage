#include <zlib.h>

#include "compresser_zlib.h"
#include "error.h"
#include "logging.h"
#include "yaml-helpers.h"


compresser_zlib::compresser_zlib(const int compression_level) : compression_level(compression_level)
{
}

compresser_zlib::~compresser_zlib()
{
}

std::string compresser_zlib::get_type() const
{
	return "zlib";
}

YAML::Node compresser_zlib::emit_configuration() const
{
        YAML::Node out_cfg;
        out_cfg["compression-level"] = compression_level;

        YAML::Node out;
        out["type"] = "compresser-zlib";
        out["cfg"] = out_cfg;

        return out;
}

compresser_zlib * compresser_zlib::load_configuration(const YAML::Node & node)
{
	dolog(ll_info, " * compresser_zlib::load_configuration");

	const YAML::Node cfg = yaml_get_yaml_node(node, "cfg", "configuration of module");

	int compression_level = yaml_get_int(cfg, "compression-level", "compression level: 0-9");

	return new compresser_zlib(compression_level);
}

bool compresser_zlib::compress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len)
{
	if (in_len > UINT32_MAX) {
		dolog(ll_error, "compresser_zlib::compress: input too large (%zu)", in_len);
		return false;
	}

	unsigned long temp_len = compressBound(in_len) + 4 /* original size storage */;
	*out = reinterpret_cast<uint8_t *>(malloc(temp_len));
        if (!*out) {
                dolog(ll_error, "compresser_zlib::compress: cannot allocate %d bytes of memory", temp_len);
                return false;
        }

	(*out)[0] = in_len >> 24;
	(*out)[1] = in_len >> 16;
	(*out)[2] = in_len >>  8;
	(*out)[3] = in_len;

	unsigned long dest_len = temp_len;
	int rc = compress2(&(*out)[4], &dest_len, in, in_len, compression_level);

	if (rc != Z_OK) {
		dolog(ll_error, "compresser_zlib::compress: compress2 failed (%d)", rc);
		free(*out);
		return false;
	}

	*out_len = dest_len + 4;

	return true;
}

bool compresser_zlib::decompress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len)
{
	if (in_len > UINT32_MAX) {
		dolog(ll_error, "compresser_zlib::decompress: input too large (%zu)", in_len);
		return false;
	}

	*out_len = (in[0] << 24) | (in[1] << 16) | (in[2] << 8) | in[3];

	*out = reinterpret_cast<uint8_t *>(malloc(*out_len));
        if (!*out) {
                dolog(ll_error, "compresser_zlib::decompress: cannot allocate %d bytes of memory", *out_len);
                return false;
        }

	unsigned long dest_len = *out_len;
	unsigned long src_len = in_len - 4;

	int rc = uncompress2(*out, &dest_len, &in[4], &src_len);
	if (rc != Z_OK) {
		dolog(ll_error, "compresser_zlib::decompress: uncompress failed (%d)", rc);
		free(*out);
		return false;
	}

	if (dest_len != *out_len) {
		dolog(ll_error, "compresser_zlib::decompress: uncompress failed, unexpected output size");
		free(*out);
		return false;
	}

	return true;
}
