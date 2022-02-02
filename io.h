#include <stdint.h>

#include "types.h"


ssize_t READ(int fd, uint8_t *whereto, size_t len);
ssize_t WRITE(int fd, const uint8_t *whereto, size_t len);
ssize_t PREAD(int fd, uint8_t *whereto, size_t len, offset_t offset);
ssize_t PWRITE(int fd, const uint8_t *whereto, size_t len, offset_t offset);
