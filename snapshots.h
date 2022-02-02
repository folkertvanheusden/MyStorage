#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <stdint.h>
#include <thread>
#include <vector>

#include "storage_backend.h"


class snapshot_state
{
private:
        storage_backend  *const src;
	const std::string complete_filename;
	const int         block_size { 4096 };

        int               fd { -1 };
        uint64_t         *bitmap { nullptr };
        std::mutex        lock;  // for the bitmap
	std::thread      *th { nullptr };
	std::atomic_bool  stop_flag { false };
	std::atomic_bool  copy_finished { false };

	bool get_set_block_state(const block_nr_t block_nr);
	bool copy_block(const block_nr_t block_nr);

public:
	snapshot_state(storage_backend *const src, const std::string & complete_filename, const int block_size);
	virtual ~snapshot_state();

	bool has_finished() const;

	std::string get_filename() const;

	bool put_block(const block_nr_t block_nr);

	void operator()();
};

class snapshots : public storage_backend
{
private:
	const std::string             storage_directory;
	const std::string             filename_template;
	storage_backend        *const sb { nullptr };
	std::vector<snapshot_state *> running_snapshots;
	std::mutex                    lock;  // for running_snapshots
	std::thread                  *th { nullptr };

	bool trigger_range(const offset_t offset, const uint32_t len);

protected:
	bool can_do_multiple_blocks() const override;

        bool get_block(const block_nr_t block_nr, uint8_t **const data) override;
        bool put_block(const block_nr_t block_nr, const uint8_t *const data) override;

public:
	// filename_template: %-escapes are from strftime
	snapshots(const std::string & id, const std::string & storage_directory, const std::string & filename_template, storage_backend *const sb);
	virtual ~snapshots();

	std::optional<std::string> trigger_snapshot();

	offset_t get_size() const override;

	bool fsync() override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	void put_data(const offset_t offset, const block & b, int *const err) override;
	void put_data(const offset_t offset, const std::vector<uint8_t> & d, int *const err) override;

	static snapshots * load_configuration(const YAML::Node & node);
	YAML::Node emit_configuration() const override;

	// periodically checks if a snapshot has finished
	void operator()();
};
