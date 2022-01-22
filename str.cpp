#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "logging"

std::string myformat(const char *const fmt, ...)
{
	char *buffer = nullptr;
        va_list ap;

        va_start(ap, fmt);

        if (vasprintf(&buffer, fmt, ap) == -1) {
		va_end(ap);

		dolog(ll_warning, "myformat: failed to convert string with format \"%s\"", fmt);

		return fmt;
	}

        va_end(ap);

	std::string result = buffer;
	free(buffer);

	return result;
}
