#include <stdint.h>


ssize_t READ(int fd, uint8_t *whereto, size_t len);
ssize_t WRITE(int fd, const uint8_t *whereto, size_t len);
