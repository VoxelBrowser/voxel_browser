#include "vb/core/server_address.hpp"

#include <charconv>

namespace vb::core {

namespace {

std::optional<ServerAddress> fail(std::string *error, const char *why) {
	if (error != nullptr) {
		*error = why;
	}
	return std::nullopt;
}

bool parse_port(std::string_view s, int &out) {
	if (s.empty() || s.size() > 5) {
		return false;
	}
	int v = 0;
	const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	if (ec != std::errc{} || ptr != s.data() + s.size() || v < 1 || v > 65535) {
		return false;
	}
	out = v;
	return true;
}

bool has_space(std::string_view s) {
	return s.find_first_of(" \t\r\n") != std::string_view::npos;
}

} // namespace

std::optional<ServerAddress> parse_server_address(std::string_view text, std::string *error) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
		text.remove_prefix(1);
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
		text.remove_suffix(1);
	}
	if (text.empty()) {
		return fail(error, "Enter a server address");
	}
	ServerAddress out;
	std::string_view host = text;
	std::string_view port_text;
	bool has_port = false;
	if (text.front() == '[') {
		const std::size_t close = text.find(']');
		if (close == std::string_view::npos) {
			return fail(error, "Missing ']' in the IPv6 address");
		}
		host = text.substr(1, close - 1);
		const std::string_view rest = text.substr(close + 1);
		if (!rest.empty()) {
			if (rest.front() != ':') {
				return fail(error, "Unexpected text after ']'");
			}
			port_text = rest.substr(1);
			has_port = true;
		}
	} else {
		const std::size_t first = text.find(':');
		if (first != std::string_view::npos) {
			if (text.find(':', first + 1) != std::string_view::npos) {
				return fail(error, "Wrap IPv6 addresses in [brackets]");
			}
			host = text.substr(0, first);
			port_text = text.substr(first + 1);
			has_port = true;
		}
	}
	if (host.empty() || has_space(host)) {
		return fail(error, "Enter a server address");
	}
	if (has_port && !parse_port(port_text, out.port)) {
		return fail(error, "Port must be 1-65535");
	}
	out.host = std::string(host);
	return out;
}

std::string format_server_address(std::string_view host, int port) {
	std::string out;
	if (host.find(':') != std::string_view::npos) {
		out = "[" + std::string(host) + "]";
	} else {
		out = std::string(host);
	}
	if (port != kDefaultServerPort) {
		out += ':' + std::to_string(port);
	}
	return out;
}

} // namespace vb::core
