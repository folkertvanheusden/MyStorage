#include <optional>
#include <stdint.h>
#include <string>
#include <vector>


std::string get_endpoint_name(int fd);

void add_uint64(std::vector<uint8_t> & target, const uint64_t v);
void add_uint32(std::vector<uint8_t> & target, const uint32_t v);
void add_uint16(std::vector<uint8_t> & target, const uint16_t v);
void add_uint8(std::vector<uint8_t> & target, const uint8_t v);

std::optional<uint64_t> receive_uint64(const int fd);
std::optional<uint32_t> receive_uint32(const int fd);
std::optional<uint16_t> receive_uint16(const int fd);
std::optional<std::vector<uint8_t> > receive_n_uint8(const int fd, const size_t n);

bool str_to_mac(const std::string & in, uint8_t *const out);
