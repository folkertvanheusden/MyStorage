#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "journal.h"
#include "logging.h"
#include "storage_backend_file.h"
#include "time.h"
#include "types.h"

void os_assert(int v)
{
	if (v == -1) {
		fprintf(stderr, "error: %s\n", strerror(errno));
		assert(0);
	}
}

void make_file(const std::string & name, const off_t size)
{
	int fd = open(name.c_str(), O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	os_assert(fd);

	os_assert(ftruncate(fd, size));

	os_assert(close(fd));
}

void test_journal()
{
	dolog(ll_info, " -> journal tests");

	try {
		const std::string test_data_file = "test/data.dat";
		const std::string test_journal_file = "test/journal.dat";

		constexpr offset_t data_size = 16 * 1024 * 1024;
		constexpr int block_size = 4096;

		constexpr uint64_t put_n = data_size / block_size;

		make_file(test_data_file.c_str(), data_size);  // 1GB data file
		make_file(test_journal_file.c_str(), 512 * 1024);  // 512kB journal file, 4 kB header

		// test if what goes in, goes out
		dolog(ll_info, " * 001 smoke test");
		storage_backend_file sbf_data("data", test_data_file, block_size, { });
		storage_backend_file sbf_journal("journal", test_journal_file, block_size, { });

		{
			journal *j = new journal("journal", &sbf_data, &sbf_journal);
			uint8_t *data = reinterpret_cast<uint8_t *>(calloc(1, block_size));
			block b(data, block_size);

			for(uint64_t i=0; i<put_n; i++) {
				*reinterpret_cast<uint64_t *>(data) = i;

				int err = 0;
				j->put_data(i * block_size, b, &err);
				assert(err == 0);
			}

			delete j;
		}

		{
			journal *j = new journal("journal", &sbf_data, &sbf_journal);
			j->flush_journal();
			delete j;
		}

		for(uint64_t i=0; i<put_n; i++) {
			int err = 0;
			block *b = nullptr;
			sbf_data.get_data(i * block_size, block_size, &b, &err);

			assert(err == 0);

			assert(*reinterpret_cast<const uint64_t *>(b->get_data()) == i);

			delete b;
		}

		// concurrency test
		dolog(ll_info, " * 001 concurrency");
		{
			uint64_t start = get_us();
			journal *j = new journal("journal", &sbf_data, &sbf_journal);

			uint64_t put_count = 0, get_count = 0;

			std::thread t([start, j, &put_count] {
					uint8_t *data = reinterpret_cast<uint8_t *>(calloc(1, block_size));
					block b(data, block_size);

					while(get_us() - start < 1000000) {
						uint64_t i = rand() % put_n;

						*reinterpret_cast<uint64_t *>(data) = i;

						int err = 0;
						j->put_data(i * block_size, b, &err);
						assert(err == 0);

						put_count++;
					}
				});


			while(get_us() - start < 1000000) {
				uint64_t i = rand() % put_n;

				uint64_t *v = 0;
				int err = 0;
				j->get_data(i * block_size, uint32_t(sizeof(v)), reinterpret_cast<uint8_t **>(&v), &err);
				assert(err == 0);

				assert(*v == i || *v == 0);

				free(v);

				get_count++;
			}

			t.join();

			delete j;

			dolog(ll_info, "gets: %ld, puts: %ld", get_count, put_count);
		}

		os_assert(unlink(test_journal_file.c_str()));
		os_assert(unlink(test_data_file.c_str()));
	}
	catch(const std::string & error) {
		fprintf(stderr, "journal exception: %s\n", error.c_str());
		assert(0);
	}
}

void setup()
{
	setlog("test-mystorage.log", ll_debug, ll_info);
	dolog(ll_info, "test-MyStorage starting");

	os_assert(mkdir("test", 0770));
}

int main(int argc, char *argv[])
{
	dolog(ll_info, " *** TESTS STARTED ***\n");

	setup();

	test_journal();

	dolog(ll_info, " *** ALL FINE ***");

	return 0;
}
