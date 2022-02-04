#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "block.h"
#include "storage_backend.h"


constexpr const char journal_signature[] = "MyStrg-J";

typedef struct
{
	uint32_t   crc;
	char       sig[8];  // verification if this is a journal
	int        block_size;  // at least 4kB
	block_nr_t n_elements;
	block_nr_t cur_n;

	int        read_pointer;
	int        write_pointer;
	bool       full;
} journal_meta_t;

typedef enum { JA_invalid = 0, JA_write = 1, JA_trim, JA_zero } journal_action_t;

// multiple of sizeof(journal_element_t) + block_size => journal_size
typedef struct
{
	uint32_t         crc;
	journal_action_t a;
	block_nr_t       target_block;
	uint8_t          data[0];
} journal_element_t;

class journal : public storage_backend
{
private:
	storage_backend *const data;
	storage_backend *const journal_;

	journal_meta_t         jm { 0 };
	int                    meta_dirty { 0 };

	std::mutex             lock;
	std::condition_variable_any cond_pull;
	std::condition_variable_any cond_push;

	lock_group             lgj;

	std::map<block_nr_t, std::pair<block, int64_t> > cache;

	std::atomic_bool       journal_commit_fatal_error { false };

	std::thread           *th { nullptr };
	const unsigned int     flush_interval { 5000 };

	bool update_journal_meta_data();

	bool put_in_cache(const journal_element_t *const je);
	bool push_action(const journal_action_t a, const block_nr_t block_nr, const block & data);

	bool transaction_start() override;
	bool transaction_end() override;

	void dump(const journal_element_t*);

protected:
        bool get_block(const block_nr_t block_nr, uint8_t **const data) override;
        bool put_block(const block_nr_t block_nr, const uint8_t *const data) override;

	bool can_do_multiple_blocks() const override;

public:
	journal(const std::string & id, storage_backend *const data, storage_backend *const journal_, const unsigned int flush_interval);
	virtual ~journal();

	bool fsync() override;

	offset_t get_size() const override;

	int get_maximum_transaction_size() const override;

	bool trim_zero(const offset_t offset, const uint32_t len, const bool trim, int *const err) override;

	void flush_journal();

	void operator()();

	YAML::Node emit_configuration() const override;
	static journal * load_configuration(const YAML::Node & node, std::optional<uint64_t> size);
};
