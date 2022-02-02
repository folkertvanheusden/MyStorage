#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
#include "types.h"


ssize_t READ(int fd, uint8_t *whereto, size_t len)
{
	ssize_t cnt=0;

	while(len > 0) {
		ssize_t rc = read(fd, whereto, len);

		if (rc == -1) {
			if (errno == EAGAIN) {
				dolog(ll_warning, "READ: %s", strerror(errno));
				continue;
			}

			return -1;
		}
		else if (rc == 0)
			break;
		else {
			whereto += rc;
			len -= rc;
			cnt += rc;
		}
	}

	return cnt;
}

ssize_t PREAD(int fd, uint8_t *whereto, size_t len, offset_t offset)
{
	ssize_t cnt=0;

	while(len > 0) {
		ssize_t rc = pread(fd, whereto, len, offset);

		if (rc == -1) {
			if (errno == EAGAIN) {
				dolog(ll_warning, "PREAD: %s", strerror(errno));
				continue;
			}

			return -1;
		}
		else if (rc == 0) {
			dolog(ll_warning, "PREAD: read 0 bytes, end of file reached?");
			break;
		}
		else {
			whereto += rc;
			len -= rc;
			cnt += rc;
			offset += rc;
		}
	}

	return cnt;
}

ssize_t WRITE(int fd, const uint8_t *whereto, size_t len)
{
	ssize_t cnt=0;

	while(len > 0) {
		ssize_t rc = write(fd, whereto, len);

		if (rc == -1) {
			if (errno == EAGAIN) {
				dolog(ll_warning, "WRITE: %s", strerror(errno));
				continue;
			}

			return -1;
		}
		else if (rc == 0)
			return -1;
		else {
			whereto += rc;
			len -= rc;
			cnt += rc;
		}
	}

	return cnt;
}

ssize_t PWRITE(int fd, const uint8_t *whereto, size_t len, offset_t offset)
{
	ssize_t cnt=0;

	while(len > 0) {
		ssize_t rc = pwrite(fd, whereto, len, offset);

		if (rc == -1) {
			if (errno == EAGAIN) {
				dolog(ll_warning, "WRITE: %s", strerror(errno));
				continue;
			}

			return -1;
		}
		else if (rc == 0) {
			dolog(ll_warning, "PWRITE: wrote 0 bytes, disk full?");
			return -1;
		}
		else {
			whereto += rc;
			len -= rc;
			cnt += rc;
			offset += rc;
		}
	}

	return cnt;
}
