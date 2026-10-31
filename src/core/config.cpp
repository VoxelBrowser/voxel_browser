#include "vb/core/config.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

#include <toml++/toml.hpp>

#include "vb/core/cli.hpp"
#include "vb/core/log.hpp"

namespace vb::core {

namespace {

template <typename T>
void read_uint(const toml::table &tbl, std::string_view key, T &out) {
	if (auto v = tbl[key].template value<std::int64_t>()) {
		if (*v >= 0 &&
				static_cast<std::uint64_t>(*v) <=
						static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
			out = static_cast<T>(*v);
		} else {
			VB_WARN("config", "value for '", key, "' out of range, ignored");
		}
	}
}

void read_double(const toml::table &tbl, std::string_view key, double &out) {
	if (auto d = tbl[key].value<double>()) {
		out = *d;
	} else if (auto i = tbl[key].template value<std::int64_t>()) {
		out = static_cast<double>(*i);
	}
}

void read_string(const toml::table &tbl, std::string_view key, std::string &out) {
	if (auto s = tbl[key].value<std::string>()) {
		out = *s;
	}
}

void read_bool(const toml::table &tbl, std::string_view key, bool &out) {
	if (auto b = tbl[key].value<bool>()) {
		out = *b;
	}
}

int as_int(const std::optional<std::string> &s, int fallback) {
	if (!s) {
		return fallback;
	}
	try {
		return std::stoi(*s);
	} catch (...) {
		return fallback;
	}
}

ServerConfig server_from_table(const toml::table &tbl) {
	ServerConfig c;
	read_string(tbl, "bind_address", c.bind_address);
	read_uint(tbl, "port", c.port);
	read_string(tbl, "content_pack", c.content_pack);
	read_uint(tbl, "max_players", c.max_players);
	read_double(tbl, "handshake_timeout_seconds", c.handshake_timeout_seconds);
	if (!(c.handshake_timeout_seconds > 0.0)) {
		VB_WARN("config", "handshake_timeout_seconds must be > 0, using 10");
		c.handshake_timeout_seconds = 10.0;
	}
	read_uint(tbl, "view_distance", c.view_distance);
	read_uint(tbl, "tick_rate", c.tick_rate);
	read_uint(tbl, "world_seed", c.world_seed);
	read_double(tbl, "gravity", c.gravity);
	read_double(tbl, "void_kill_y", c.void_kill_y);
	read_double(tbl, "day_length_seconds", c.day_length_seconds);
	read_uint(tbl, "asset_max_file_mb", c.asset_max_file_mb);
	read_uint(tbl, "asset_max_total_mb", c.asset_max_total_mb);
	read_uint(tbl, "max_connections_per_ip", c.max_connections_per_ip);
	read_double(tbl, "max_messages_per_second", c.max_messages_per_second);
	read_string(tbl, "motd", c.motd);
	read_bool(tbl, "persist_world", c.persist_world);
	read_string(tbl, "world_dir", c.world_dir);
	read_double(tbl, "autosave_interval_seconds", c.autosave_interval_seconds);
	read_uint(tbl, "chunk_send_budget_bytes_per_tick", c.chunk_send_budget_bytes_per_tick);
	if (auto mode = tbl["auth_mode"].value<std::string>()) {
		if (*mode == "token") {
			c.auth_mode = ConfigAuthMode::kToken;
		} else if (*mode == "none") {
			c.auth_mode = ConfigAuthMode::kNone;
		} else {
			VB_WARN("config", "unknown auth_mode '", *mode, "', using 'none'");
		}
	}
	if (const toml::table *auth = tbl["auth"].as_table()) {
		read_string(*auth, "issuer", c.auth.issuer);
		read_string(*auth, "client_id", c.auth.client_id);
		read_string(*auth, "project_id", c.auth.project_id);
		read_string(*auth, "api_key", c.auth.api_key);
		for (const auto &[key, value] : *auth) {
			(void)value;
			const std::string_view k = key.str();
			if (k != "issuer" && k != "client_id" && k != "project_id" && k != "api_key") {
				VB_WARN("config", "unknown [auth] key '", k, "' ignored");
			}
		}
	}
	if (c.tick_rate < 1 || c.tick_rate > 240) {
		VB_WARN("config", "tick_rate ", c.tick_rate, " out of 1..240, clamped");
		c.tick_rate = c.tick_rate < 1 ? 1u : 240u;
	}
	return c;
}

ClientConfig client_from_table(const toml::table &tbl) {
	ClientConfig c;
	read_uint(tbl, "window_width", c.window_width);
	read_uint(tbl, "window_height", c.window_height);
	read_bool(tbl, "vsync", c.vsync);
	read_double(tbl, "fov", c.fov);
	read_uint(tbl, "render_distance", c.render_distance);
	read_double(tbl, "mouse_sensitivity", c.mouse_sensitivity);
	read_uint(tbl, "asset_cache_mb", c.asset_cache_mb);
	read_string(tbl, "asset_cache_dir", c.asset_cache_dir);
	read_string(tbl, "player_name", c.player_name);
	read_uint(tbl, "key_forward", c.key_forward);
	read_uint(tbl, "key_back", c.key_back);
	read_uint(tbl, "key_left", c.key_left);
	read_uint(tbl, "key_right", c.key_right);
	read_uint(tbl, "key_jump", c.key_jump);
	read_uint(tbl, "key_sprint", c.key_sprint);
	if (auto arr = tbl["recent_servers"].as_array()) {
		c.recent_servers.clear();
		for (const auto &node : *arr) {
			if (auto s = node.value<std::string>()) {
				c.recent_servers.push_back(*s);
			}
		}
	}
	return c;
}

template <typename Cfg, typename FromTable>
Result<Cfg, CoreError> parse_text(std::string_view text, FromTable from_table) {
	try {
		return from_table(toml::parse(text));
	} catch (const toml::parse_error &e) {
		VB_ERROR("config", "TOML parse error: ", e.description());
		return Err{ CoreError::kParseError };
	}
}

template <typename Cfg>
Result<Cfg, CoreError> load_file(const std::string &path,
		Result<Cfg, CoreError> (*parse)(std::string_view)) {
	std::error_code ec;
	if (!std::filesystem::exists(path, ec) || ec) {
		VB_INFO("config", "no config at '", path, "', using defaults");
		return Cfg{};
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return Err{ CoreError::kIoError };
	}
	const std::string text((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
	return parse(text);
}

} // namespace

Result<ServerConfig, CoreError> parse_server_config(std::string_view text) {
	return parse_text<ServerConfig>(text, server_from_table);
}

Result<ClientConfig, CoreError> parse_client_config(std::string_view text) {
	return parse_text<ClientConfig>(text, client_from_table);
}

Result<ServerConfig, CoreError> load_server_config(const std::string &path) {
	return load_file<ServerConfig>(path, parse_server_config);
}

Result<ClientConfig, CoreError> load_client_config(const std::string &path) {
	return load_file<ClientConfig>(path, parse_client_config);
}

Result<void, CoreError> save_client_config(const std::string &path, const ClientConfig &config) {
	toml::table tbl;
	tbl.insert_or_assign("window_width", static_cast<std::int64_t>(config.window_width));
	tbl.insert_or_assign("window_height", static_cast<std::int64_t>(config.window_height));
	tbl.insert_or_assign("vsync", config.vsync);
	tbl.insert_or_assign("fov", config.fov);
	tbl.insert_or_assign("render_distance", static_cast<std::int64_t>(config.render_distance));
	tbl.insert_or_assign("mouse_sensitivity", config.mouse_sensitivity);
	tbl.insert_or_assign("asset_cache_mb", static_cast<std::int64_t>(config.asset_cache_mb));
	if (!config.asset_cache_dir.empty()) {
		tbl.insert_or_assign("asset_cache_dir", config.asset_cache_dir);
	}
	tbl.insert_or_assign("player_name", config.player_name);
	tbl.insert_or_assign("key_forward", static_cast<std::int64_t>(config.key_forward));
	tbl.insert_or_assign("key_back", static_cast<std::int64_t>(config.key_back));
	tbl.insert_or_assign("key_left", static_cast<std::int64_t>(config.key_left));
	tbl.insert_or_assign("key_right", static_cast<std::int64_t>(config.key_right));
	tbl.insert_or_assign("key_jump", static_cast<std::int64_t>(config.key_jump));
	tbl.insert_or_assign("key_sprint", static_cast<std::int64_t>(config.key_sprint));
	toml::array recent;
	for (const auto &s : config.recent_servers) {
		recent.push_back(s);
	}
	tbl.insert_or_assign("recent_servers", std::move(recent));

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		VB_ERROR("config", "could not open '", path, "' for writing");
		return Err{ CoreError::kIoError };
	}
	out << tbl;
	if (!out) {
		return Err{ CoreError::kIoError };
	}
	return {};
}

void apply_cli_overrides(ServerConfig &config, const Args &args) {
	config.bind_address = args.value_or("bind", config.bind_address);
	config.content_pack = args.value_or("content-pack", config.content_pack);
	config.motd = args.value_or("motd", config.motd);
	config.world_dir = args.value_or("world-dir", config.world_dir);
	config.port = static_cast<std::uint16_t>(
			args.int_or("port", static_cast<int>(config.port)));
	config.tick_rate = static_cast<std::uint32_t>(
			args.int_or("tick-rate", static_cast<int>(config.tick_rate)));
	config.max_players = static_cast<std::uint32_t>(
			args.int_or("max-players", static_cast<int>(config.max_players)));
	if (auto seed = args.value("seed")) {
		try {
			config.world_seed = std::stoull(*seed);
		} catch (...) {
			VB_WARN("config", "--seed is not a number, ignored");
		}
	}
}

void apply_cli_overrides(ClientConfig &config, const Args &args) {
	// `--name` is deliberately not applied here: it is a session-only override handled by ClientApp,
	// so it can never be written back to client.toml (Settings is the only writer of player_name).
	config.window_width = static_cast<std::uint32_t>(
			args.int_or("width", static_cast<int>(config.window_width)));
	config.window_height = static_cast<std::uint32_t>(
			args.int_or("height", static_cast<int>(config.window_height)));
	config.render_distance = static_cast<std::uint32_t>(args.int_or(
			"render-distance", static_cast<int>(config.render_distance)));
	config.fov = static_cast<double>(as_int(args.value("fov"),
			static_cast<int>(config.fov)));
	config.asset_cache_dir = args.value_or("asset-cache-dir", config.asset_cache_dir);

	// Runs after both the TOML value and any --width/--height override, so
	// both paths are covered by one clamp (see kMinWindowWidth/Height).
	if (config.window_width < kMinWindowWidth || config.window_height < kMinWindowHeight) {
		VB_WARN("config", "window size ", config.window_width, "x", config.window_height,
				" below the ", kMinWindowWidth, "x", kMinWindowHeight, " floor, clamped");
		config.window_width = std::max(config.window_width, kMinWindowWidth);
		config.window_height = std::max(config.window_height, kMinWindowHeight);
	}
}

} // namespace vb::core
