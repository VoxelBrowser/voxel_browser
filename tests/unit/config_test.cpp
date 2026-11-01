#include <doctest/doctest.h>

#include <ostream>

#include <filesystem>
#include <string>
#include <vector>

#include "vb/core/cli.hpp"
#include "vb/core/config.hpp"

using namespace vb::core;

namespace {

Args make_args(std::initializer_list<const char *> argv) {
	std::vector<char *> v;
	for (const char *s : argv) {
		v.push_back(const_cast<char *>(s));
	}
	return Args(static_cast<int>(v.size()), v.data());
}

} // namespace

TEST_CASE("server config: defaults when the document is empty") {
	auto c = parse_server_config("");
	REQUIRE(c);
	CHECK(c->port == 27015);
	CHECK(c->tick_rate == 20);
	CHECK(c->auth_mode == ConfigAuthMode::kNone);
	CHECK(c->content_pack == "content/base");
	CHECK(c->max_connections_per_ip == 0); // 0 = unlimited
	CHECK(c->max_messages_per_second == doctest::Approx(0.0)); // 0 = unlimited
	CHECK(c->chunk_send_budget_bytes_per_tick == 0); // 0 = unlimited
	CHECK(c->handshake_timeout_seconds == doctest::Approx(10.0));
}

TEST_CASE("server config: values are read from TOML") {
	auto c = parse_server_config(R"(
		bind_address = "127.0.0.1"
		port = 28000
		max_players = 4
		handshake_timeout_seconds = 30.0
		tick_rate = 30
		world_seed = 123456789
		gravity = 19.5
		void_kill_y = -32.0
		day_length_seconds = 600.0
		max_connections_per_ip = 3
		max_messages_per_second = 50.0
		chunk_send_budget_bytes_per_tick = 65536
		auth_mode = "token"
		motd = "hi"
	)");
	REQUIRE(c);
	CHECK(c->bind_address == "127.0.0.1");
	CHECK(c->port == 28000);
	CHECK(c->max_players == 4);
	CHECK(c->handshake_timeout_seconds == doctest::Approx(30.0));
	CHECK(c->tick_rate == 30);
	CHECK(c->world_seed == 123456789u);
	CHECK(c->gravity == doctest::Approx(19.5));
	CHECK(c->void_kill_y == doctest::Approx(-32.0));
	CHECK(c->day_length_seconds == doctest::Approx(600.0));
	CHECK(c->max_connections_per_ip == 3);
	CHECK(c->max_messages_per_second == doctest::Approx(50.0));
	CHECK(c->chunk_send_budget_bytes_per_tick == 65536);
	CHECK(c->auth_mode == ConfigAuthMode::kToken);
	CHECK(c->motd == "hi");
}

TEST_CASE("server config: a non-positive handshake_timeout_seconds falls back to 10") {
	auto c = parse_server_config("handshake_timeout_seconds = 0\n");
	REQUIRE(c);
	CHECK(c->handshake_timeout_seconds == doctest::Approx(10.0));
}

TEST_CASE("server config: [auth] overrides parse; absent table leaves them empty") {
	auto none = parse_server_config("");
	REQUIRE(none);
	CHECK(none->auth.issuer.empty());
	CHECK(none->auth.client_id.empty());

	auto c = parse_server_config(R"(
		[auth]
		issuer = "https://staging.example.com/realms/game"
		client_id = "voxel-staging"
		project_id = "p"
		api_key = "k"
		bogus = 1
	)");
	REQUIRE(c);
	CHECK(c->auth.issuer == "https://staging.example.com/realms/game");
	CHECK(c->auth.client_id == "voxel-staging");
	CHECK(c->auth.project_id == "p");
	CHECK(c->auth.api_key == "k");
	// auth_mode stays independent: the wire mode is derived from auth.lua.
	CHECK(c->auth_mode == ConfigAuthMode::kNone);
}

TEST_CASE("server config: day_length_seconds defaults to 1200 (Phase 6.8)") {
	auto c = parse_server_config("");
	REQUIRE(c);
	CHECK(c->day_length_seconds == doctest::Approx(1200.0));
}

TEST_CASE("server config: malformed TOML is an error") {
	auto c = parse_server_config("port = = 5");
	CHECK_FALSE(c);
	CHECK(c.error() == CoreError::kParseError);
}

TEST_CASE("server config: out-of-range tick rate is clamped") {
	auto c = parse_server_config("tick_rate = 9000");
	REQUIRE(c);
	CHECK(c->tick_rate == 240);
}

TEST_CASE("server config: an out-of-range port is ignored, default kept") {
	auto c = parse_server_config("port = 99999");
	REQUIRE(c);
	CHECK(c->port == 27015);
}

TEST_CASE("client config: reads values and recent servers list") {
	auto c = parse_client_config(R"(
		window_width = 1920
		vsync = false
		fov = 90
		mouse_sensitivity = 0.2
		player_name = "Ada"
		recent_servers = ["a.example:27015", "b.example:27016"]
	)");
	REQUIRE(c);
	CHECK(c->window_width == 1920);
	CHECK_FALSE(c->vsync);
	CHECK(c->fov == doctest::Approx(90.0));
	CHECK(c->mouse_sensitivity == doctest::Approx(0.2));
	CHECK(c->player_name == "Ada");
	REQUIRE(c->recent_servers.size() == 2);
	CHECK(c->recent_servers[1] == "b.example:27016");
}

TEST_CASE("client config: keybindings default and round-trip through TOML") {
	auto defaults = parse_client_config("");
	REQUIRE(defaults);
	CHECK(defaults->key_forward == 87); // KEY_W
	CHECK(defaults->key_back == 83); // KEY_S
	CHECK(defaults->key_left == 65); // KEY_A
	CHECK(defaults->key_right == 68); // KEY_D
	CHECK(defaults->key_jump == 32); // KEY_SPACE
	CHECK(defaults->key_sprint == 340); // KEY_LEFT_SHIFT

	ClientConfig cfg = *defaults;
	cfg.key_forward = 265; // KEY_UP
	cfg.key_jump = 341; // KEY_LEFT_CONTROL
	REQUIRE(save_client_config("test_keybinds.toml", cfg));
	auto loaded = load_client_config("test_keybinds.toml");
	REQUIRE(loaded);
	CHECK(loaded->key_forward == 265);
	CHECK(loaded->key_back == 83);
	CHECK(loaded->key_jump == 341);
	CHECK(loaded->key_sprint == 340);
	std::filesystem::remove("test_keybinds.toml");
}

TEST_CASE("CLI overrides win over the file") {
	auto c = parse_server_config("port = 28000\ntick_rate = 30");
	REQUIRE(c);
	ServerConfig cfg = *c;

	const auto args = make_args({ "srv", "--port", "40000", "--motd", "cli" });
	apply_cli_overrides(cfg, args);
	CHECK(cfg.port == 40000);
	CHECK(cfg.tick_rate == 30); // untouched by CLI
	CHECK(cfg.motd == "cli");
}

TEST_CASE("client CLI overrides") {
	ClientConfig cfg;
	const auto args = make_args({ "cl", "--name", "Zed", "--fov", "100" });
	apply_cli_overrides(cfg, args);
	// --name is a session-only override that ClientApp applies itself; it must never land in the
	// config that Settings / the join path write back to client.toml (Phase 12.4).
	CHECK(cfg.player_name == "Player");
	CHECK(cfg.fov == doctest::Approx(100.0));
}

TEST_CASE("recent servers: most recent first, de-duplicated, capped at kMaxServerHistory") {
	ClientConfig cfg;
	record_recent_server(cfg, "a:1");
	record_recent_server(cfg, "b:2");
	record_recent_server(cfg, "a:1"); // moves to the front instead of duplicating
	REQUIRE(cfg.recent_servers.size() == 2);
	CHECK(cfg.recent_servers[0] == "a:1");
	CHECK(cfg.recent_servers[1] == "b:2");

	for (std::size_t i = 0; i < vb::core::kMaxServerHistory + 20; ++i) {
		record_recent_server(cfg, "host" + std::to_string(i) + ":27015");
	}
	CHECK(cfg.recent_servers.size() == vb::core::kMaxServerHistory);
	CHECK(cfg.recent_servers.front() == "host" + std::to_string(vb::core::kMaxServerHistory + 19) + ":27015");
	CHECK(vb::core::kMaxServerHistory == 50);
}
