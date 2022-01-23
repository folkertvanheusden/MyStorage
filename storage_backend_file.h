#include <stdint.h>
#include <string>

#include "block.h"
#include "storage_backend.h"


class storage_backend_file : public storage_backend
{
private:
	int fd { -1 };
	offset_t size { 0 };

public:
	storage_backend_file(const std::string & file);
	virtual ~storage_backend_file();

	offset_t get_size() const override;
	block * get_data(const offset_t offset, const uint32_t size) override;
	void put_data(const offset_t offset, const block & b) override;

	void fsync() override;
};
