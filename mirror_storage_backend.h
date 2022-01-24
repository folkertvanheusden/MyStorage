#pragma once
#include <string>

#include "block.h"
#include "mirror.h"
#include "storage_backend.h"
#include "types.h"


class mirror_storage_backend : public mirror
{
private:
	const std::string id;
	storage_backend *const sb;

public:
	mirror_storage_backend(const std::string & id, storage_backend *const sb);
	virtual ~mirror_storage_backend();

	bool put_block(const offset_t o, const block & b) override;

	bool sync() override;
};
