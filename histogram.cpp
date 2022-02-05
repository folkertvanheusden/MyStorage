#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "histogram.h"
#include "logging.h"
#include "str.h"


histogram::histogram(const uint64_t max_value, const int n_slots) : n_slots(n_slots)
{
	counters = reinterpret_cast<uint64_t *>(calloc(n_slots, sizeof(uint64_t)));
	if (!counters)
		throw myformat("histogram: cannot allocate memory for %d counter slots", n_slots);

	divider = (max_value + 1) / n_slots;
}

histogram::~histogram()
{
	free(counters);
}

void histogram::count(const uint64_t value)
{
	// NOT thread-safe
	counters[value / divider]++;
}

uint64_t histogram::get_count(const int slot) const
{
	return counters[slot];
}

int histogram::get_n_slots() const
{
	return n_slots;
}

void histogram::dump(const std::string & filename)
{
	FILE *fh = fopen(filename.c_str(), "w");
	if (!fh) {
		dolog(ll_error, "histogram::dump: cannot create \"%s\": %s", filename.c_str(), strerror(errno));
		return;
	}

	for(int i=0; i<n_slots; i++)
		fprintf(fh, "%ld\n", counters[i]);

	fclose(fh);
}
