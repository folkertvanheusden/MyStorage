#include <stdint.h>
#include <string>

#include "sector.h"
#include "storage_backend.h"


class storage_backend_file : public storage_backend
{
private:
	int fd { -1 };
	uint64_t n_sectors { 0 };

public:
	storage_backend_file(const std::string & file);
	virtual ~storage_backend_file();

	uint64_t get_n_sectors() const override;
	sector get_sector(const uint64_t s_nr) override;
	void put_sector(const uint64_t s_nr, const sector & s) override;

	void fsync() override;
};
