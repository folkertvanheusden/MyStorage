#include <optional>
#include <thread>
#include <vector>

#include "socket_listener.h"
#include "storage_backend.h"


class nbd
{
private:
	socket_listener *const sl;
	const std::vector<storage_backend *> & storage_backends;

	std::thread *th { nullptr };

	void handle_client(const int fd);
	bool send_option_reply(const int fd, const uint32_t opt, const uint32_t reply_type, const std::vector<uint8_t> & data);
	bool send_cmd_reply(const int fd, const uint32_t err, const uint64_t handle, const std::vector<uint8_t> & data);
	std::optional<size_t> find_storage_backend_by_id(const std::string & id);

public:
	nbd(socket_listener *const sl, const std::vector<storage_backend *> & storage_backends);
	virtual ~nbd();

	void operator()();
};