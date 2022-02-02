#include <thread>
#include <yaml-cpp/yaml.h>

#include "server.h"
#include "storage_backend.h"


typedef struct {
	std::string  dev_name;
	uint8_t      my_mac[6];
	uint8_t      allowed_mac[6] { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };  // broadcast address for everyone
	int          mtu_size { 1500 };
	uint8_t      configuration[1024] { 0 };
	int          fd { -1 };
	std::thread *th { nullptr };
} aoe_path_t;

class aoe : public server
{
private:
	storage_backend  *const sb { nullptr };
	const int               major { 0x0001 };
	const int               minor { 0x11 };
	const int               firmware_version { 0x4001 };

	std::vector<aoe_path_t> paths;

	bool announce(const aoe_path_t & ap);
	void worker_thread(aoe_path_t & ap);

public:
	aoe(storage_backend *const sb, const uint16_t major, const uint8_t minor, const std::vector<aoe_path_t> & paths);
	virtual ~aoe();

	YAML::Node emit_configuration() const override;
	static aoe * load_configuration(const YAML::Node & node, const std::vector<storage_backend *> & storage);
};
