#include <stdint.h>
#include <string>


std::string get_endpoint_name(int fd);

ssize_t READ(int fd, uint8_t *whereto, size_t len);
ssize_t WRITE(int fd, const uint8_t *whereto, size_t len);
