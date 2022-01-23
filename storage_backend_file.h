#include <stdint.h>
#include <string>

#include "block.h"
#include "storage_backend.h"


class storage_backend_file : public storage_backend
{
private:
	int fd { -1 };
	uint64_t size { 0 };

public:
	storage_backend_file(const std::string & file);
	virtual ~storage_backend_file();

	uint64_t get_size() const override;
	block * get_data(const uint64_t offset, const uint32_t size) override;
	void put_data(const uint64_t offset, const block & b) override;

	void fsync() override;
};
