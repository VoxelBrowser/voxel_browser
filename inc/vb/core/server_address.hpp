#pragma once

#include <optional>
#include <string>
#include <string_view>

// Parsing of the main menu's single address field (Phase 12.2): `host`, `host:port`, `[v6]` or
// `[v6]:port`. The port defaults to kDefaultServerPort.

namespace vb::core {

inline constexpr int kDefaultServerPort = 27015;

struct ServerAddress {
	std::string host; // without any brackets
	int port = kDefaultServerPort;
};

// Trims surrounding whitespace first. On failure returns nullopt and, when `error` is non-null,
// stores a short player-facing reason ("Enter a server address", "Port must be 1-65535", ...).
std::optional<ServerAddress> parse_server_address(std::string_view text, std::string *error = nullptr);

// The inverse, for filling the field: omits the default port, brackets IPv6 hosts.
std::string format_server_address(std::string_view host, int port);

} // namespace vb::core
