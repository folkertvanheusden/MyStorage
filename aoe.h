#include <thread>
#include <yaml-cpp/yaml.h>

#include "server.h"
#include "storage_backend.h"


class aoe : public server
{
private:
	const std::string      dev_name;
	storage_backend *const sb { nullptr };
	uint8_t                my_mac[6];
	int                    fd { -1 };
	int                    mtu_size { 1500 };
	std::thread           *th { nullptr };
	uint8_t                configuration[1024] { 0 };
	const int              major { 0x0001 };
	const int              minor { 0x11 };
	const int              firmware_version { 0x4001 };

	bool announce();

public:
	aoe(const std::string & dev_name, storage_backend *const sb, const uint8_t my_mac[6], const int mtu_size_in, const uint16_t major, const uint8_t minor);
	virtual ~aoe();

	void operator()();

	YAML::Node emit_configuration() const override;
	static aoe * load_configuration(const YAML::Node & node, const std::vector<storage_backend *> & storage);
};
