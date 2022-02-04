#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "compresser_lzo.h"
#include "hash_sha384.h"
#include "journal.h"
#include "logging.h"
#include "snapshots.h"
#include "socket_client_ipv4.h"
#include "storage_backend_file.h"
#include "storage_backend_dedup.h"
#include "storage_backend_nbd.h"
#include "tiering.h"
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

void test_integrity(storage_backend *const sb)
{
	dolog(ll_info, " -> integrity tests, \"%s\"", sb->get_id().c_str());

	try {
		const offset_t size = sb->get_size();
		const unsigned int block_size = sb->get_block_size();
		const uint64_t put_n = size / block_size;

		// Fill, fase 1
		dolog(ll_info, " * fill fase 1 (put %ld blocks)", put_n / 2);

		for(uint64_t i=0; i<put_n / 2; i++) {
			uint64_t offset = 0;

       			if (getrandom(&offset, sizeof(offset), 0) != sizeof(offset))
				throw "Problem getting random";

			offset %= size - block_size;
			offset &= ~7;

			uint32_t len = 0;

       			if (getrandom(&len, sizeof(len), 0) != sizeof(len))
				throw "Problem getting random";

			len %= std::min(size - offset, offset_t(262144));
			len &= ~7;

			if (len == 0)
				continue;

			uint8_t *data = reinterpret_cast<uint8_t *>(malloc(len));

			for(uint32_t j=0; j<len; j += 8)
				*reinterpret_cast<uint64_t *>(data + j) = offset + j;

			block b(data, len);

			int err = 0;
			sb->put_data(offset, b, &err);

			assert(err == 0);
		}

		// Fill, fase 2
		dolog(ll_info, " * fill fase 2");
		
		for(uint64_t i=0; i<size; i += block_size * 16) {
			uint8_t *d = nullptr;
			int err = 0;
			sb->get_data(i, block_size * 16, &d, &err);

			assert(err == 0);

			bool set = false;

			for(unsigned int j=0; j<block_size * 16; j += 8) {
				if (*reinterpret_cast<uint64_t *>(d + j) == 0) {
					set = true;
					*reinterpret_cast<uint64_t *>(d + j) = i + j;
				}
			}

			if (set) {
				block b(d, block_size * 16);
				sb->put_data(i, b, &err);
			}
			else {
				free(d);
			}

			assert(err == 0);
		}

		// Verify
		dolog(ll_info, " * verify");
		
		for(uint64_t i=0; i<size; i += block_size * 16) {
			uint8_t *d = nullptr;
			int err = 0;
			sb->get_data(i, block_size * 16, &d, &err);

			assert(err == 0);

			for(unsigned int j=0; j<block_size * 16; j += 8)
				assert(*reinterpret_cast<uint64_t *>(d + j) == i + j);

			free(d);
		}
	}
	catch(const std::string & error) {
		fprintf(stderr, "exception: %s\n", error.c_str());
		assert(0);
	}
}

storage_backend *create_sb_instance()
{
	constexpr int block_size = 4096;

	storage_backend *fast = new storage_backend_file("fast", "test/fast.dat", 1 * 1024 * 1024, block_size, { });
	storage_backend *slow = new storage_backend_file("slow", "test/slow.dat", 5 * 1024 * 1024, block_size, { });

	auto md = tiering::get_meta_dimensions(fast->get_size(), fast->get_block_size());
	storage_backend *meta = new storage_backend_file("meta", "test/meta.dat", md.first * md.second, md.second, { });

	return new tiering("tiering", fast, slow, meta, { });
}

bool check_contents(const block *const b, const uint8_t v)
{
	for(size_t i=0; i<b->get_size(); i++) {
		if (b->get_data()[i] != v)
			return false;
	}

	return true;
}

void test_integrity_basic()
{
	{
		storage_backend *sb = create_sb_instance();

		// empty at start
		int err = 0;
		block *b = nullptr;
		sb->get_data(0, sb->get_block_size(), &b, &err);
		assert(err == 0);

		assert(check_contents(b, 0x00));
		delete b;

		// data written is the same when read
		uint8_t *bv = reinterpret_cast<uint8_t *>(malloc(sb->get_block_size()));
		memset(bv, 0xff, sb->get_block_size());
		block b2(bv, sb->get_block_size());

		sb->put_data(0, b2, &err);
		assert(err == 0);

		sb->get_data(0, sb->get_block_size(), &b, &err);
		assert(err == 0);

		// data written again is the same when read
		sb->put_data(0, b2, &err);
		assert(err == 0);

		sb->get_data(0, sb->get_block_size(), &b, &err);
		assert(err == 0);

		assert(*b == b2);

		delete b;

		// overwritten with something else
		uint8_t *bv3 = reinterpret_cast<uint8_t *>(malloc(sb->get_block_size()));
		memset(bv3, 0x80, sb->get_block_size());
		block b3(bv3, sb->get_block_size());

		sb->put_data(0, b3, &err);
		assert(err == 0);

		sb->get_data(0, sb->get_block_size(), &b, &err);
		assert(err == 0);

		assert(b3 == *b);

		delete b;

		delete sb;

		//
		sb = create_sb_instance();
		sb->get_data(0, sb->get_block_size(), &b, &err);
		assert(err == 0);
		delete sb;

		assert(b3 == *b);

		//
	}

	os_assert(unlink("test/meta.dat"));
	os_assert(unlink("test/slow.dat"));
	os_assert(unlink("test/fast.dat"));
}

void test_integrities()
{
	try {
		if (1) {
			test_integrity_basic();
		}

		if (0) {
			const std::string test_data_file = "test/data.dat";

			constexpr offset_t data_size = 5l * 1024l * 1024l * 1024l;
			constexpr int block_size = 4096;

			make_file(test_data_file, data_size);  // 1GB data file

			storage_backend_file sbf_data("data", test_data_file, data_size, block_size, { });

			test_integrity(&sbf_data);

			os_assert(unlink(test_data_file.c_str()));
		}

		if (0) {
			try {
				socket_client_ipv4 sc("192.168.122.115", 10809);

				storage_backend_nbd nbd("nbd-test", &sc, "test", 131072, { });

				test_integrity(&nbd);
			}
			catch(const std::string & error) {
				fprintf(stderr, "exception: %s\n", error.c_str());
				assert(0);
			}
		}

		if (0) {
			try {
				const std::string test_data_file = "test/data.kch";

				constexpr offset_t data_size = 1024l * 1024l * 1024l;
				constexpr int block_size = 4096;

				hash *h = new hash_sha384();
				compresser *c = new compresser_lzo();
				storage_backend_dedup sbf_data("data", test_data_file, h, c, { }, data_size, block_size);

				test_integrity(&sbf_data);

				os_assert(unlink(test_data_file.c_str()));
			}
			catch(const std::string & error) {
				fprintf(stderr, "exception: %s\n", error.c_str());
				assert(0);
			}
		}

		if (0) {
			const std::string test_data_file = "test/data.dat";

			constexpr offset_t data_size = 5l * 1024l * 1024l * 1024l;
			constexpr int block_size = 4096;

			storage_backend_file sbf_data("data", test_data_file, data_size, block_size, { });

			snapshots s("snapshot", ".", "snapshut.img", &sbf_data, true);

			test_integrity(&s);

			os_assert(unlink(test_data_file.c_str()));
		}

		if (1) {
			constexpr int block_size = 4096;

			storage_backend *fast = new storage_backend_file("fast", "test/fast.dat", 1 * 1024 * 1024, block_size, { });
			storage_backend *slow = new storage_backend_file("slow", "test/slow.dat", 5 * 1024 * 1024, block_size, { });

			auto md = tiering::get_meta_dimensions(fast->get_size(), fast->get_block_size());
			storage_backend *meta = new storage_backend_file("meta", "test/meta.dat", md.first * md.second, md.second, { });

			tiering t("tiering", fast, slow, meta, { });

			test_integrity(&t);

			os_assert(unlink("test/meta.dat"));
			os_assert(unlink("test/slow.dat"));
			os_assert(unlink("test/fast.dat"));
		}
	}
	catch(const std::string & error) {
		fprintf(stderr, "test_integritie exception: %s\n", error.c_str());
		assert(0);
	}
}

void test_journal()
{
	dolog(ll_info, " -> journal tests");

	try {
		const std::string test_data_file = "test/data.dat";
		const std::string test_journal_file = "test/journal.dat";

		constexpr offset_t data_size = 32l * 1024l * 1024l;
		constexpr int block_size = 4096;

		constexpr uint64_t put_n = data_size / block_size;

		make_file(test_data_file, data_size);  // 1GB data file

		constexpr offset_t journal_size = 1024 * 1024;

		// test if what goes in, goes out
		dolog(ll_info, " * 001 smoke test");
		storage_backend_file sbf_data("data", test_data_file, data_size, block_size, { });
		storage_backend_file sbf_journal("journal", test_journal_file, journal_size, block_size, { });

		{
			journal *j = new journal("journal", &sbf_data, &sbf_journal, 5);
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
			journal *j = new journal("journal", &sbf_data, &sbf_journal, 5);
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
		dolog(ll_info, " * 002 concurrency");
		{
			uint64_t start = get_us();
			journal *j = new journal("journal", &sbf_data, &sbf_journal, 5);

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

		// concurrency test for trim
		dolog(ll_info, " * 003 concurrent trim & write");
		{
			journal *j = new journal("journal", &sbf_data, &sbf_journal, 5);

			std::atomic_bool stop = false;

			std::thread t([j, &stop] {
					uint8_t *data = reinterpret_cast<uint8_t *>(calloc(1, block_size));
					block b(data, block_size);

					while(!stop) {
						uint64_t i = rand() % put_n;

						*reinterpret_cast<uint64_t *>(data) = i;

						int err = 0;
						j->put_data(i * block_size, b, &err);
						assert(err == 0);
					}
				});

			std::thread t2([j, &stop] {
					while(!stop) {
						uint64_t i = rand() % put_n;

						block *b = nullptr;

						int err = 0;
						j->get_data(i * block_size, block_size, &b, &err);
						assert(err == 0);

						const uint64_t v = *reinterpret_cast<const uint64_t *>(b->get_data());
						assert(v == 0 || v == i);

						delete b;
					}
				});

			// trim
			int err = 0;
			j->trim_zero(0, data_size, true, &err);
			assert(err == 0);

			stop = true;
			t2.join();
			t.join();

			delete j;
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

	test_integrities();

//	test_journal();

	dolog(ll_info, " *** ALL FINE ***");

	rmdir("test");

	return 0;
}
