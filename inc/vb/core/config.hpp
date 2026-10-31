#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vb/core/error.hpp"
#include "vb/core/result.hpp"

// Layered configuration (spec §15): built-in defaults <- TOML file <- CLI
// overrides. A missing file is not an error (defaults are used); a malformed
// file is. Unknown keys are ignored.

namespace vb::core {

class Args; // fwd (vb/core/cli.hpp)

enum class ConfigAuthMode : std::uint8_t { kNone = 0,
	kToken = 1 };

// `[auth]` table in server.toml (architecture_spec/auth.md §4): overrides for
// the deployment-specific values of a pack's auth.lua, so one pack works
// against staging and production. Empty = not overridden. It can never
// disable authentication for a pack that ships auth.lua.
struct ServerAuthOverrides {
	std::string issuer;
	std::string client_id;
	std::string project_id;
	std::string api_key;
};

struct ServerConfig {
	std::string bind_address = "0.0.0.0";
	std::uint16_t port = 27015;
	std::string content_pack = "content/base";
	std::uint32_t max_players = 16;
	// Seconds a joining connection may sit in any one handshake step (asset
	// sync, waiting for Ready, ...) before it is dropped with "handshake
	// timeout". Signing in has its own, much longer limit. Raise it for slow
	// hosts, e.g. sanitizer builds (the e2e harness scales it with
	// VB_E2E_TIMEOUT_SCALE).
	double handshake_timeout_seconds = 10.0;
	std::uint32_t view_distance = 4; // chunks
	std::uint32_t tick_rate = 20;
	std::uint64_t world_seed = 0; // 0 = random at startup
	double gravity = 32.0;
	double void_kill_y = -64.0; // fall below this Y -> instant death, respawn at spawn
	// Real seconds for one full in-game day/night cycle (spec §5.4). Matches
	// ServerSession::day_length_seconds_'s own hardcoded default -- an unset
	// value here changes nothing.
	double day_length_seconds = 1200.0;
	std::uint32_t asset_max_file_mb = 32;
	std::uint32_t asset_max_total_mb = 512;
	// Per-IP connection cap (§8.3 hardening) -- 0 (default) = unlimited.
	// Only bites over a real GnsTransport (VB_WITH_NET); LoopbackTransport
	// has no network identity to cap on, see ServerSession::
	// set_max_connections_per_ip's own comment.
	std::uint32_t max_connections_per_ip = 0;
	// Per-connection message-rate flood guard (§8.3 hardening, defense in
	// depth on top of the closed-schema InputCmd/keybind bitset caps) -- 0
	// (default) = unlimited. A token bucket refilled at this many
	// post-join messages/second, capacity equal to one second's worth (a
	// short burst is tolerated, sustained flooding is not); see
	// ServerSession::set_max_messages_per_second's own comment.
	double max_messages_per_second = 0.0;
	ConfigAuthMode auth_mode = ConfigAuthMode::kNone;
	ServerAuthOverrides auth;
	std::string motd;
	// World persistence (ARCHITECTURE_SPEC.md §18's "region file format",
	// picked up 2026-09-25). `persist_world = false` keeps every prior
	// version's in-memory-forever behavior exactly (RegionStore is never
	// constructed at all -- not just skipped). `world_dir` is relative to the
	// server's working directory, same convention as `content_pack`.
	bool persist_world = true;
	std::string world_dir = "world";
	// Real seconds between full-world autosave sweeps; 0 disables periodic
	// autosave (edits still save on chunk unload and always at shutdown).
	double autosave_interval_seconds = 60.0;
	// Per-connection cap on S2C_ChunkAdd bytes sent per WorldReplicator::tick()
	// (STATE.md §6: a joining player's whole view box used to leave in one
	// uncapped burst). 0 (default) = unlimited, matching every other
	// "0 = unlimited" knob above. A chunk deferred by this budget is simply
	// retried next tick -- nothing is dropped, joining just spreads out over
	// more ticks once a view box is large enough to exceed it.
	std::uint32_t chunk_send_budget_bytes_per_tick = 0;
};

// Floor enforced by apply_cli_overrides(ClientConfig&, ...): below this,
// content/base/ui/hud.lua's ui_scale() has no headroom to work with and
// raylib itself refuses a 0-sized window. Width 688 is the actual
// requirement -- below it, ui_scale()'s own floor (never go under 1x) means
// a fully-stocked 9-slot hotbar's container (640px of slots + 16px padding)
// plus its 16px side margins no longer fits the screen and clips both
// edges; 720 leaves a little headroom above that line.
inline constexpr std::uint32_t kMinWindowWidth = 720;
inline constexpr std::uint32_t kMinWindowHeight = 360;

// The most recent servers kept in client.toml's `recent_servers` (the main screen shows the first
// three; Settings > History lists them all).
inline constexpr std::size_t kMaxServerHistory = 50;
// Longest accepted player name (the server rejects anything longer in the handshake).
inline constexpr std::size_t kMaxPlayerNameLength = 32;

struct ClientConfig {
	std::uint32_t window_width = 1280;
	std::uint32_t window_height = 720;
	bool vsync = true;
	double fov = 70.0;
	std::uint32_t render_distance = 4; // chunks, clamped to server view_distance
	double mouse_sensitivity = 0.12;
	std::uint32_t asset_cache_mb = 512;
	std::string asset_cache_dir; // empty = vb::core::user_cache_dir() / "assets"
	std::string player_name = "Player";
	std::vector<std::string> recent_servers;

	// Phase 5.3: physical-key bindings for the client-local movement/action
	// axes (MovementBindings in src/client/main.cpp) that the Settings ->
	// Keybindings screen rebinds. Stored as raw raylib KEY_* integer values --
	// vb_core doesn't depend on raylib, but these are stable, public,
	// GLFW-derived constants, so hardcoding their current values here (rather
	// than pulling in raylib.h) is safe. Distinct from Phase 6.19's
	// vb.register_keybind name registry, which is server-driven and unrelated
	// to which physical key a player presses.
	std::int32_t key_forward = 87; // KEY_W
	std::int32_t key_back = 83; // KEY_S
	std::int32_t key_left = 65; // KEY_A
	std::int32_t key_right = 68; // KEY_D
	std::int32_t key_jump = 32; // KEY_SPACE
	std::int32_t key_sprint = 340; // KEY_LEFT_SHIFT
};

// Parse from an in-memory TOML document (used by tests and callers that already
// have the text).
Result<ServerConfig, CoreError> parse_server_config(std::string_view toml_text);
Result<ClientConfig, CoreError> parse_client_config(std::string_view toml_text);

// Load from a file path. A non-existent path yields defaults; a parse failure
// yields CoreError::kParseError.
Result<ServerConfig, CoreError> load_server_config(const std::string &path);
Result<ClientConfig, CoreError> load_client_config(const std::string &path);

// Overwrite `path` with `config` serialized as TOML (used by the client
// Settings screen + recent-servers list, spec §5.3). Comments in an existing
// file are not preserved -- this regenerates the file from scratch.
// Moves `server` ("host:port") to the front of `config.recent_servers`, dropping any earlier copy
// and trimming the list to kMaxServerHistory.
void record_recent_server(ClientConfig &config, const std::string &server);

Result<void, CoreError> save_client_config(const std::string &path, const ClientConfig &config);

// Apply recognised CLI flags on top of a config (mutates in place).
//   server: --bind --port --content-pack --tick-rate --max-players --seed --motd --world-dir
//   client: --name --width --height --fov --render-distance --asset-cache-dir
void apply_cli_overrides(ServerConfig &config, const Args &args);
void apply_cli_overrides(ClientConfig &config, const Args &args);

} // namespace vb::core
