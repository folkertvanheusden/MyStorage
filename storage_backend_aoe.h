#include <vector>

#include "mirror.h"
#include "storage_backend.h"


class storage_backend_aoe : public storage_backend
{
private:
	const std::string dev_name;
	uint8_t           my_mac[6] { 0 };
	const uint16_t    major;
	const uint8_t     minor;
	offset_t          size { 0 };
	mutable struct {
		int       fd { -1 };
		uint8_t   tgt_mac[6] { 0 };
		offset_t  size { 0 };
	}                 connection;

	bool connect() const;

public:
	storage_backend_aoe(const std::string & id, const std::vector<mirror *> & mirrors, const std::string & dev_name, const uint8_t my_mac[6], const uint16_t major, const uint8_t minor);
	virtual ~storage_backend_aoe();

	offset_t get_size() const override;

	void get_data(const offset_t offset, const uint32_t size, block **const b, int *const err) override;
	void put_data(const offset_t offset, const block & b, int *const err) override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;
};