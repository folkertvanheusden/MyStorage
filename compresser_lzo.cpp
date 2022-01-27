#include <lzo/lzo1.h>

#include "compresser_lzo.h"
#include "error.h"
#include "logging.h"


compresser_lzo::compresser_lzo()
{
        if (lzo_init() != LZO_E_OK)
                throw "compresser_lzo: lzo_init failed";
}

compresser_lzo::~compresser_lzo()
{
}

YAML::Node compresser_lzo::emit_configuration() const
{
        YAML::Node out;
        out["type"] = "compresser-lzo";

        return out;
}

compresser_lzo * compresser_lzo::load_configuration(const YAML::Node & node)
{
	return new compresser_lzo();
}

bool compresser_lzo::compress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len)
{
	if (in_len > UINT32_MAX) {
		dolog(ll_error, "compresser_lzo::compress: input too large (%zu)", in_len);
		return false;
	}

	unsigned long temp_len = in_len + 128 /* TODO: what is the maximum overhead for uncompressible data? */ + 4 /* original size storage */;
	*out = reinterpret_cast<uint8_t *>(malloc(temp_len));
	if (!*out) {
		dolog(ll_error, "compresser_lzo::compress: cannot allocate %d bytes of memory", temp_len);
		return false;
	}

	(*out)[0] = in_len >> 24;
	(*out)[1] = in_len >> 16;
	(*out)[2] = in_len >>  8;
	(*out)[3] = in_len;

        uint8_t buffer[LZO1_MEM_COMPRESS] { 0 };

        int rc = lzo1_compress(in, in_len, &(*out)[4], out_len, buffer);
        if (rc != LZO_E_OK) {
                dolog(ll_error, "compresser_lzo::compress: lzo1_compress failed (%d)", rc);
                return false;
        }

	*out_len += 4;  // size

	return true;
}

bool compresser_lzo::decompress(const uint8_t *const in, const size_t in_len, uint8_t **const out, size_t *const out_len)
{
	if (in_len > UINT32_MAX) {
		dolog(ll_error, "compresser_lzo::decompress: input too large (%zu)", in_len);
		return false;
	}

	*out_len = (in[0] << 24) | (in[1] << 16) | (in[2] << 8) | in[3];

	*out = reinterpret_cast<uint8_t *>(malloc(*out_len));
	if (!*out) {
		dolog(ll_error, "compresser_lzo::decompress: cannot allocate %d bytes of memory", *out_len);
		return false;
	}

	int rc = lzo1_decompress(&in[4], in_len - 4, *out, out_len, nullptr);
	if (rc != LZO_E_OK) {
		dolog(ll_error, "compresser_lzo::decompress: lzo1_compress failed (%d)", rc);
		free(*out);
		return false;
	}

	return true;
}
