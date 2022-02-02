#include <errno.h>
#include <netdb.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "io.h"
#include "logging.h"
#include "net.h"
#include "str.h"


std::string get_endpoint_name(int fd)
{
	char host[256] { "?" };
	char serv[256] { "?" };
	struct sockaddr_in6 addr { 0 };
	socklen_t addr_len = sizeof addr;

	if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == -1)
		dolog(ll_warning, "get_endpoint_name: failed to find name of fd %d", fd);
	else
		getnameinfo((struct sockaddr *)&addr, addr_len, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);

	return myformat("[%s]:%s", host, serv);
}

void add_uint64(std::vector<uint8_t> & target, const uint64_t v)
{
	target.push_back(v >> 56);
	target.push_back(v >> 48);
	target.push_back(v >> 40);
	target.push_back(v >> 32);
	target.push_back(v >> 24);
	target.push_back(v >> 16);
	target.push_back(v >>  8);
	target.push_back(v);
}

void add_uint32(std::vector<uint8_t> & target, const uint32_t v)
{
	target.push_back(v >> 24);
	target.push_back(v >> 16);
	target.push_back(v >>  8);
	target.push_back(v);
}

void add_uint16(std::vector<uint8_t> & target, const uint16_t v)
{
	target.push_back(v >>  8);
	target.push_back(v);
}

void add_uint8(std::vector<uint8_t> & target, const uint8_t v)
{
	target.push_back(v);
}

std::optional<uint64_t> receive_uint64(const int fd)
{
	uint8_t buffer[8] { 0 };
	if (READ(fd, buffer, sizeof buffer) != sizeof buffer)
		return { };
	
	return (uint64_t(buffer[0]) << 56) | (uint64_t(buffer[1]) << 48) | (uint64_t(buffer[2]) << 40) | (uint64_t(buffer[3]) << 32) | (uint64_t(buffer[4]) << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
}

std::optional<uint32_t> receive_uint32(const int fd)
{
	uint8_t buffer[4] { 0 };
	if (READ(fd, buffer, sizeof buffer) != sizeof buffer)
		return { };
	
	return (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
}

std::optional<uint16_t> receive_uint16(const int fd)
{
	uint8_t buffer[2] { 0 };
	if (READ(fd, buffer, sizeof buffer) != sizeof buffer)
		return { };
	
	return (buffer[0] << 8) | buffer[1];
}

std::optional<std::vector<uint8_t> > receive_n_uint8(const int fd, const size_t n)
{
	std::vector<uint8_t> out;
	out.resize(n);

	if (READ(fd, out.data(), n) != ssize_t(n))
		return { };

	return out;
}

bool str_to_mac(const std::string & in, uint8_t *const out)
{
	auto parts = split(str_tolower(in), ":");

	if (parts.size() != 6) {
		dolog(ll_error, "str_to_mac: \"%s\" does not contain 6 hex values seperated by ':'", in.c_str());
		return false;
	}

	for(size_t i=0; i<parts.size(); i++) {
		if (parts.at(i).size() != 2) {
			dolog(ll_error, "str_to_mac: \"%s\" (from \"%s\") is not 2 digits in length", parts.at(i).c_str(), in.c_str());
			return false;
		}

		int v = 0;

		char c1 = parts.at(i).at(0);
		if (c1 >= 'a' && c1 <= 'f')
			v += c1 - 'a' + 10;
		else if (c1 >= 0 && c1 <= '9')
			v += c1 - '0';
		else {
			dolog(ll_error, "str_to_mac: \"%s\" (from \"%s\") is not valid hex", parts.at(i).c_str(), in.c_str());
			return false;
		}

		v <<= 4;

		char c2 = parts.at(i).at(1);
		if (c2 >= 'a' && c2 <= 'f')
			v += c2 - 'a' + 10;
		else if (c2 >= 0 && c2 <= '9')
			v += c2 - '0';
		else {
			dolog(ll_error, "str_to_mac: \"%s\" (from \"%s\") is not valid hex", parts.at(i).c_str(), in.c_str());
			return false;
		}

		out[i] = v;
	}

	return true;
}

std::string mac_to_str(const uint8_t in[6])
{
	char buffer[18] { 0 };

	snprintf(buffer, sizeof buffer, "%02x:%02x:%02x:%02x:%02x:%02x", in[0], in[1], in[2], in[3], in[4], in[5]);

	return buffer;
}
