#pragma once
#include <atomic>
#include <set>
#include <yaml-cpp/yaml.h>


class base
{
private:
	std::set<const base *> obj_used_by;

protected:
	std::string      id;
	std::atomic_bool stop_flag { false };

public:
	base(const std::string & id);
	virtual ~base();

	std::string get_id() const;

	const std::set<const base *> & obj_in_use_by() const;
	void acquire(const base *const b);
	void release(const base *const b);

	void stop();

	virtual YAML::Node emit_configuration() const = 0;
};
