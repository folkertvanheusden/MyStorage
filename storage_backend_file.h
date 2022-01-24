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
	storage_backend_file(const std::string & id, const std::string & file);
	virtual ~storage_backend_file();

	offset_t get_size() const override;
	void get_data(const offset_t offset, const uint32_t size, block **const b, int *const err) override;
	void put_data(const offset_t offset, const block & b, int *const err) override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;
};
