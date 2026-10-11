// voxel_browser_server — authoritative, headless.
//
// Real transport (GameNetworkingSockets over UDP, spec §8.1) + world/session
// wiring landed in Phase 1.2: this binary generates terrain, streams it, and
// simulates players exactly like the `--singleplayer` integrated path, just
// over a real listen socket instead of loopback. Lua content (Phase 4) still
// needs to land before anything gameplay-facing is pack-defined.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include "vb/assetsync/manifest.hpp"
#include "vb/auth/config.hpp"
#include "vb/auth/http.hpp"
#include "vb/auth/server_glue.hpp"
#include "vb/auth/service.hpp"
#include "vb/core/build_info.hpp"
#include "vb/core/cli.hpp"
#include "vb/core/config.hpp"
#include "vb/core/log.hpp"
#include "vb/core/version_req.hpp"
#include "vb/net/gns_transport.hpp"
#include "vb/net/session.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/physics/movement.hpp"
#include "vb/script/pack_loader.hpp"
#include "vb/script/pack_manifest.hpp"
#include "vb/script/pack_runtime.hpp"
#include "vb/world/block.hpp"
#include "vb/world/region_store.hpp"
#include "vb/world/world.hpp"
#include "vb/world/world_registry.hpp"
#include "vb/worldgen/generator.hpp"
#include "vb/worldgen/worker_pool.hpp"

#if VB_WITH_LUA
#include "check_pack.hpp"
#endif
#if defined(VB_WITH_AUTOMATION)
#include "automation_endpoint.hpp"
#include "vb/automation/host.hpp"
#endif

namespace {

std::atomic_bool g_stop{ false };

extern "C" void handle_signal(int) {
	g_stop.store(true, std::memory_order_relaxed);
}

// The asset manifest is hashed once, but host.asset_file_bytes reads the
// file again when a client asks for it. Runtime state the server itself
// writes (vb.storage, vb.db) is excluded from the manifest entirely
// (core::is_pack_runtime_state), so normally nothing it lists changes. If
// a listed file does change on disk anyway (someone edits the pack while
// the server runs), asset_file_bytes refuses to serve bytes that no longer
// match their advertised hash and marks the manifest stale, and the main
// loop swaps in a rebuilt one. host.asset_manifest/asset_file_bytes run on
// the same thread session.tick() does today, but a mutex costs nothing
// noticeable at this call rate and makes that assumption unnecessary to
// maintain.
struct ManifestHolder {
	mutable std::mutex mutex;
	std::shared_ptr<const vb::assetsync::Manifest> ptr;

	std::shared_ptr<const vb::assetsync::Manifest> get() const {
		std::lock_guard<std::mutex> lock(mutex);
		return ptr;
	}
	void set(std::shared_ptr<const vb::assetsync::Manifest> p) {
		std::lock_guard<std::mutex> lock(mutex);
		ptr = std::move(p);
		stale.store(false, std::memory_order_relaxed);
	}
	std::atomic_bool stale{ false }; // a listed file's bytes no longer match its hash
};

void print_usage() {
	std::cout << "Usage: voxel_browser_server [options]\n"
				 "\n"
				 "  --config <path>        server.toml to load (default server.toml)\n"
				 "  --bind <addr>          bind address override (currently always binds any interface)\n"
				 "  --port <n>             UDP port override\n"
				 "  --content-pack <dir>   content pack directory override\n"
				 "  --tick-rate <hz>       simulation tick rate override\n"
				 "  --max-players <n>      player cap override\n"
				 "  --seed <n>             world seed override (0 = random)\n"
				 "  --world-dir <dir>      world save directory override (server.toml world_dir)\n"
				 "  --motd <text>          message of the day override\n"
				 "  --ticks <n>            run n ticks then exit (0 = forever)\n"
				 "  --status-file <path>   rewrite this TOML file every few seconds with uptime,\n"
				 "                         tick rate, players and seed (read by `vb server status`)\n"
				 "  --stop-file <path>     stop cleanly (saving the world) once this file exists;\n"
				 "                         checked about once a second (how `vb server stop` works\n"
				 "                         on Windows, where a detached process gets no signals)\n"
#if defined(VB_WITH_AUTOMATION)
				 "  --automation stdio|tcp[:PORT]  drive via JSON lines on stdin/stdout, or on a token-protected\n"
				 "                        127.0.0.1-only socket (dev builds only)\n"
				 "  --automation-token <t>   fixed token for tcp (default: random)\n"
				 "  --automation-info <file> write {host,port,token,pid} here for tcp (default: print to stderr)\n"
				 "  --net-sim <spec>      fake lag/jitter/loss on sent packets, e.g. lag_ms=100,loss_pct=2 (dev builds only)\n"
#endif
#if !defined(VB_DISTRIBUTION)
				 "  --insecure-skip-auth  DEV ONLY: ignore the pack's auth.lua and admit unauthenticated\n"
				 "                        players (get_login() returns nil); not in distribution builds\n"
#endif
				 "  --check-pack <dir>     load the pack headless (no socket), print file:line diagnostics\n"
				 "                        and exit 0 (clean) / 1 (errors); add --json for machine output and\n"
				 "                        --strict to fail on warnings\n"
#if !defined(VB_DISTRIBUTION)
				 "  --ignore-engine-req   DEV ONLY: start (or --check-pack) even when pack.toml's\n"
				 "                        engine_version_req does not match this build\n"
#endif
				 "  --version             print build info and exit\n"
				 "  --help                show this help\n";
}

// Quotes `s` as a TOML basic string (player names are user-supplied).
std::string toml_string(const std::string &s) {
	std::string out = "\"";
	for (const char c : s) {
		switch (c) {
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					out += '?';
				} else {
					out += c;
				}
		}
	}
	return out + "\"";
}

// Writes via a temp file + rename so a reader never sees a half-written file.
void write_status_file(const std::filesystem::path &path, const std::string &text) {
	std::filesystem::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out) {
			return; // status is best-effort; never take the server down over it
		}
	}
	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
}

std::uint64_t random_seed() {
	std::random_device rd;
	return (static_cast<std::uint64_t>(rd()) << 32) | static_cast<std::uint64_t>(rd());
}

} // namespace

int main(int argc, char **argv) {
	// Run under `vb server start` the output goes to a log file, where stdio
	// would otherwise block-buffer it: joins/leaves would only appear at exit,
	// defeating `vb server logs -f`. Output here is low-volume, so flush each write.
	std::cout << std::unitbuf;
	// Before the (slow) world/pack setup, so a stop request that arrives while
	// starting up -- `vb server stop` right after `start`, a watch restart --
	// ends the run cleanly instead of killing the process mid-setup.
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);
	const vb::core::Args args(argc, argv);

	if (args.has("help", 'h')) {
		print_usage();
		return EXIT_SUCCESS;
	}
	if (args.has("version", 'v')) {
		std::cout << vb::core::describe_build() << '\n';
		return EXIT_SUCCESS;
	}

	if (args.has("check-pack")) {
#if VB_WITH_LUA
		vb::server::CheckOptions options;
		options.strict = args.has("strict");
#if !defined(VB_DISTRIBUTION)
		options.ignore_engine_req = args.has("ignore-engine-req");
#endif
		const std::string dir = args.value_or("check-pack", "");
		if (dir.empty() || dir == "true") {
			std::cerr << "server: --check-pack needs a pack directory\n";
			return 2;
		}
		const vb::server::CheckReport report = vb::server::check_pack(dir, options);
		std::cout << (args.has("json") ? vb::server::format_json(report, options.strict)
									   : vb::server::format_human(report, options.strict))
				  << '\n';
		return report.ok(options.strict) ? EXIT_SUCCESS : EXIT_FAILURE;
#else
		std::cerr << "server: --check-pack needs a build with VB_WITH_LUA\n";
		return EXIT_FAILURE;
#endif
	}

#if defined(VB_WITH_AUTOMATION)
	std::unique_ptr<vb::automation::Host> automation;
	if (args.has("automation")) {
		std::string automation_error;
		automation = vb::automation::Host::open_spec(args.value_or("automation", ""),
				args.value_or("automation-token", ""), args.value_or("automation-info", ""), automation_error);
		if (!automation) {
			std::cerr << "server: " << automation_error << '\n';
			return EXIT_FAILURE;
		}
	}
	if (args.has("net-sim")) {
		std::string error;
		const auto sim = vb::net::parse_net_sim(args.value_or("net-sim", ""), error);
		if (!sim) {
			std::cerr << "server: " << error << '\n';
			return EXIT_FAILURE;
		}
		if (!vb::net::GnsTransport::set_net_sim(*sim)) {
			std::cerr << "server: --net-sim needs a build with VB_WITH_NET\n";
			return EXIT_FAILURE;
		}
	}
#else
	if (args.has("automation") || args.has("net-sim")) {
		// Never silently ignored: a misconfigured test setup must fail loudly.
		std::cerr << "server: built without VB_WITH_AUTOMATION\n";
		return EXIT_FAILURE;
	}
#endif

	const std::string config_path = args.value_or("config", "server.toml");
	auto loaded = vb::core::load_server_config(config_path);
	if (!loaded) {
		std::cerr << "server: failed to load " << config_path << ": "
				  << vb::core::message(loaded.error()) << '\n';
		return EXIT_FAILURE;
	}
	vb::core::ServerConfig config = *loaded;
	vb::core::apply_cli_overrides(config, args);

	// pack.toml's engine_version_req is enforced (architecture_spec/dev-experience.md
	// §3.7): a pack that needs a newer engine fails here with a clear message rather
	// than mid-game with "attempt to call a nil value". Fail closed on a bad value.
	const vb::script::PackManifest pack_manifest = vb::script::read_pack_manifest(config.content_pack);
	{
		const vb::core::EngineReqCheck req = vb::script::check_pack_engine_req(pack_manifest, vb::core::engine_version());
#if defined(VB_DISTRIBUTION)
		const bool ignore_req = false;
#else
		const bool ignore_req = args.has("ignore-engine-req");
#endif
		if (!req.ok && ignore_req) {
			VB_WARN("script", "*** --ignore-engine-req: ", req.message, " (continuing; development only) ***");
		} else if (!req.ok) {
			std::cerr << "server: " << req.message << '\n';
			std::cerr << "hint: install a matching engine (`vb install`), or fix engine_version_req in "
					  << config.content_pack << "/pack.toml\n";
			return EXIT_FAILURE;
		} else if (req.warning) {
			VB_WARN("script", req.message);
		}
	}

	// In-engine authentication (architecture_spec/auth.md §4): a pack-root
	// auth.lua makes authentication mandatory. Fail closed -- any problem
	// here ends startup, never a silent downgrade to no-auth.
	std::optional<vb::auth::AuthConfig> active_auth;
	{
		vb::auth::AuthOverrides overrides;
		overrides.issuer = config.auth.issuer;
		overrides.client_id = config.auth.client_id;
		overrides.project_id = config.auth.project_id;
		overrides.api_key = config.auth.api_key;
		const vb::auth::AuthLoad auth =
				vb::auth::load_auth_lua(config.content_pack, overrides);
		const bool skip_auth = args.has("insecure-skip-auth");
#if defined(VB_DISTRIBUTION)
		if (skip_auth) {
			std::cerr << "server: --insecure-skip-auth is not available in this build\n";
			return EXIT_FAILURE;
		}
#endif
		if (!auth.present) {
			if (skip_auth) {
				std::cerr << "server: --insecure-skip-auth given but the content pack declares "
							 "no auth.lua; nothing to skip\n";
			}
		} else if (skip_auth) {
			VB_WARN("auth", "*** --insecure-skip-auth: '", config.content_pack,
					"' declares auth.lua but authentication is DISABLED; every player is "
					"admitted unverified and get_login() returns nil. Development only. ***");
		} else if (!auth.error.empty()) {
			std::cerr << "server: " << config.content_pack << ": " << auth.error << '\n';
			return EXIT_FAILURE;
		} else if (!vb::auth::kBuiltWithAuth) {
			std::cerr << "server: content pack '" << config.content_pack
					  << "' requires authentication (auth.lua); rebuild with VB_WITH_AUTH\n";
			return EXIT_FAILURE;
		} else {
			VB_INFO("auth", "authentication required: ", vb::auth::describe(*auth.config));
			if (!vb::auth::kVerifierAvailable) {
				std::cerr << "server: content pack '" << config.content_pack
						  << "' requires authentication, but this build has no token "
							 "verifier; refusing to start\n";
				return EXIT_FAILURE;
			}
			active_auth = *auth.config;
		}
	}

	const long long max_ticks = args.int_or("ticks", 0);
	// Empty = disabled. A stale file from a previous run is the launcher's to
	// remove (`vb` does) -- deleting it here would make "stop requested" and
	// "stop honoured" indistinguishable to whoever wrote it.
	const std::filesystem::path stop_file = args.value_or("stop-file", "");
	const std::filesystem::path status_file = args.value_or("status-file", "");
	const std::uint64_t seed = config.world_seed != 0 ? config.world_seed : random_seed();

	vb::net::GnsTransport transport;
	auto listen_status = transport.listen(config.port);
	if (!listen_status) {
		// A build without VB_WITH_NET can still run the simulation (worldgen,
		// ticking, etc.) for testing/CI purposes -- it just can never accept a
		// real connection. Any other failure (port in use, bind denied) is
		// fatal: there is no point ticking a server nobody can ever reach.
		if (listen_status.error() == vb::core::NetError::kBackendUnavailable) {
			std::cerr << "server: built without VB_WITH_NET -- running the "
						 "simulation with no network backend (no one can connect)\n";
		} else {
			std::cerr << "server: failed to listen on " << config.bind_address
					  << ':' << config.port << ": "
					  << vb::core::message(listen_status.error()) << '\n';
			return EXIT_FAILURE;
		}
	}

	// One shared, mutable registry: PackRuntime may extend it with
	// vb.register_block before it's frozen and copied into World/WorldGenerator
	// (Phase 4.2). Phase 5.1's content/base pack re-declares the Phase 2 base
	// set by name (`add_or_get` is idempotent), so ids are unchanged unless the
	// pack adds something new.
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime pack_runtime(transport, registry,
			std::filesystem::path(config.content_pack) / "storage.json");
	// Phase 6.13: read-only vb.config.get(key) -- set before load_content_pack
	// so it's already visible to registration-time (module-scope) pack code.
	pack_runtime.set_server_config(config);
	pack_runtime.set_auth_required(active_auth.has_value());
	if (!vb::script::load_content_pack(pack_runtime, config.content_pack)) {
		std::cerr << "server: content pack '" << config.content_pack
				  << "' failed to load, aborting\n";
		return EXIT_FAILURE;
	}
	pack_runtime.freeze();
	if (const auto worldgen_check = pack_runtime.validate_worldgen(); !worldgen_check) {
		std::cerr << "server: content pack '" << config.content_pack
				  << "' has invalid worldgen data: " << worldgen_check.message << "\n";
		return EXIT_FAILURE;
	}
	// Persist any vb.storage write load_content_pack's init.lua made (e.g.
	// content/base's own boot_count demo) now rather than on the first tick.
	// storage.json used to be part of the asset manifest below, and this
	// flush landing after the manifest was hashed broke every join with
	// "hash mismatch" (2026-09-18); it is runtime state and excluded now.
	pack_runtime.flush_storage();

	// Asset manifest (Phase 4.4): built once at startup from the content
	// pack, handed to every connection by reference. A build without
	// VB_WITH_COMPRESSION (kDisabled) skips asset sync for every client with
	// a startup warning; any other failure means the pack itself is
	// broken/hostile and is fatal.
	const vb::assetsync::AssetSizeCaps asset_caps{
		static_cast<std::uint64_t>(config.asset_max_file_mb) * 1024ull * 1024ull,
		static_cast<std::uint64_t>(config.asset_max_total_mb) * 1024ull * 1024ull
	};
	// A world save directory configured inside the pack (`world_dir = "<pack>/
	// world"`) is runtime state too: never offered to clients.
	std::vector<std::filesystem::path> asset_exclude_dirs;
	if (config.persist_world) {
		asset_exclude_dirs.emplace_back(config.world_dir);
	}
	auto manifest_result = vb::assetsync::build_manifest(
			config.content_pack, asset_caps, asset_exclude_dirs);
	ManifestHolder manifest_holder;
	if (manifest_result) {
		manifest_holder.set(std::make_shared<const vb::assetsync::Manifest>(
				std::move(*manifest_result)));
	} else if (manifest_result.error() == vb::core::AssetSyncError::kDisabled) {
		// Not fatal (a headless test server needs no assets), but never
		// silent: a joining client then has no ui/ screens or HUD -- so no
		// visible chat or hotbar -- and renders untextured blocks.
		std::cerr << "server: WARNING: asset sync disabled (built without "
					 "VB_WITH_COMPRESSION): clients will not receive ui/ scripts "
					 "or textures\n";
	} else {
		std::cerr << "server: failed to build asset manifest for "
				  << config.content_pack << ": "
				  << vb::core::message(manifest_result.error()) << '\n';
		return EXIT_FAILURE;
	}
	// Only meaningful when the manifest above actually built (kDisabled --
	// no VB_WITH_COMPRESSION -- means asset sync is off entirely and this
	// revision is never consulted again).
	const bool asset_sync_enabled = manifest_holder.get() != nullptr;

	vb::world::World world(registry);
	// World persistence (opt-in via server.toml's persist_world, default on).
	// nullptr when disabled -- WorldReplicator/ChunkLifecycleSystem treat that
	// exactly like every other unset optional seam (no-op), so this is the one
	// place the feature can be switched off entirely, not just left idle.
	std::unique_ptr<vb::world::RegionStore> region_store;
	if (config.persist_world) {
		// Saved chunks are raw block ids: refuse a world whose ids belong to a
		// different pack's registry rather than load its terrain with the
		// wrong (and untextured) blocks.
		const auto check = vb::world::check_world_registry(config.world_dir, registry,
				pack_manifest.name.empty() ? config.content_pack : pack_manifest.name);
		if (!check.ok) {
			std::cerr << "server: " << check.message << '\n';
			return EXIT_FAILURE;
		}
		if (!check.message.empty()) {
			std::cerr << "server: WARNING: " << check.message << '\n';
		}
		region_store = std::make_unique<vb::world::RegionStore>(config.world_dir);
	}
	vb::worldgen::WorldGenParams gen_params;
	gen_params.seed = seed;
	// Phase 6.14: nullptr unless a pack ever called vb.worldgen.set_pipeline,
	// in which case WorldGenerator uses the pack-driven pipeline instead of
	// its fixed default -- same opt-in shape as effective_move_params above.
	const auto worldgen_pipeline = pack_runtime.build_worldgen_pipeline(gen_params);
	const vb::worldgen::WorldGenerator generator(gen_params, registry, worldgen_pipeline);
	vb::worldgen::WorldGenWorkerPool pool(generator); // copies into the pool

	vb::net::HandshakeServerConfig hs_config;
	hs_config.pack_name = "base";
	hs_config.tick_rate = static_cast<std::uint16_t>(config.tick_rate);
	hs_config.view_distance = config.view_distance;
	hs_config.motd = config.motd;
	hs_config.engine_version_req = pack_manifest.engine_version_req.value_or("");
	hs_config.third_person_allowed = pack_runtime.third_person_allowed();
	hs_config.auth_mode = static_cast<vb::protocol::AuthMode>(config.auth_mode);
	if (active_auth) {
		// Derived from auth.lua's presence, never configured (auth.md §4).
#if defined(VB_WITH_AUTH)
		vb::auth::apply_external_auth(hs_config, *active_auth);
#endif
	}
	hs_config.max_players = config.max_players;
	hs_config.handshake_timeout_seconds = config.handshake_timeout_seconds;
	hs_config.world_seed = seed;

	// JoinGrant::spawn_pos otherwise defaults to a fixed {0, 64, 0} regardless
	// of seed -- with base_height=64 and amplitude=28 the real surface ranges
	// roughly [36, 92], so a fixed Y can land at or below it and spawn the
	// player embedded in solid terrain outright (no fall involved).
	// Computed once: the search generates real chunks to rule out spots
	// inside trees/boulders/caves, which is too slow to redo per join.
	const vb::core::Vec3d spawn = vb::worldgen::default_spawn_position(generator);
	vb::net::HandshakeServerHost host;
	host.on_ready = [spawn](std::string_view) {
		vb::net::JoinGrant grant;
		grant.spawn_pos = spawn;
		return grant;
	};
	// Phase 4.3: advertise the (possibly Lua-extended) registry to every
	// joining client so custom blocks aren't invisible client-side.
	host.block_registry =
			[&registry]() -> std::optional<std::vector<vb::protocol::BlockRegistryRecord>> {
		std::vector<vb::protocol::BlockRegistryRecord> out;
		out.reserve(registry.size());
		for (std::size_t i = 0; i < registry.size(); ++i) {
			const auto &t = registry.get(static_cast<vb::core::BlockId>(i));
			// Real bug fixed here: max_damage/crack_texture used to be
			// silently dropped by this aggregate-init (only the first 6 of
			// BlockRegistryRecord's fields were listed) -- no client ever
			// saw a nonzero max_damage, so client.break_progress() (Phase
			// 6.5) always read `registry.get(block).max_damage == 0` and
			// stayed permanently nullopt for every max_damage>0 block.
			out.push_back({ t.name, t.solid, t.opaque, t.liquid, t.light_emission,
					t.texture, t.max_damage, t.crack_texture });
		}
		return out;
	};
	// Phase 6.7: fold the operator's server.toml gravity in as the base, then
	// let a pack's vb.physics.set_params{...} override on top of it (only the
	// fields it actually set) -- effective_move_params() returns `base`
	// unchanged when no pack ever calls it. Advertised to joining clients so
	// their prediction uses the exact same tunables as this authoritative
	// simulation, mirroring host.block_registry's pattern.
	vb::physics::MoveParams move_params;
	move_params.gravity = config.gravity;
	move_params = pack_runtime.effective_move_params(move_params);
	host.move_params = [move_params]() -> std::optional<vb::protocol::S2CMoveParams> {
		return vb::protocol::S2CMoveParams{ move_params.half_width, move_params.height,
			move_params.eye_height, move_params.walk_speed, move_params.sprint_speed,
			move_params.accel, move_params.air_accel, move_params.friction,
			move_params.gravity, move_params.jump_speed, move_params.terminal_velocity,
			move_params.step_height, move_params.fly_speed, move_params.fly };
	};
	// Phase 6.8: advertise a pack's vb.daynight.set_curve{...} override the
	// same way -- nullopt (no pack ever called it) sends no frame, leaving
	// every client on vb::world::default_day_night_curve() unchanged.
	const std::optional<vb::world::DayNightCurve> day_night_curve =
			pack_runtime.effective_day_night_curve();
	host.day_night_curve =
			[day_night_curve]() -> std::optional<std::vector<vb::protocol::DayNightKeyframeRecord>> {
		if (!day_night_curve) {
			return std::nullopt;
		}
		std::vector<vb::protocol::DayNightKeyframeRecord> out;
		out.reserve(day_night_curve->keyframes.size());
		for (const auto &k : day_night_curve->keyframes) {
			out.push_back({ k.tick, k.brightness, k.color.r, k.color.g, k.color.b });
		}
		return out;
	};
	// Phase 7.2: advertise a pack's vb.render.set_fog{...} override the
	// same way -- nullopt (no pack ever called it) sends no frame, leaving
	// every client to compute its own default fog distance from its own
	// view_distance config.
	const std::optional<vb::protocol::S2CFogParams> fog_params =
			pack_runtime.effective_fog_params();
	host.fog_params = [fog_params]() -> std::optional<vb::protocol::S2CFogParams> {
		return fog_params;
	};
	host.asset_manifest = [&manifest_holder] { return manifest_holder.get(); };
	host.asset_file_bytes = [&manifest_holder, content_pack = config.content_pack](
									vb::core::AssetHash h) -> std::optional<std::vector<std::byte>> {
		auto manifest_ptr = manifest_holder.get();
		if (!manifest_ptr) {
			return std::nullopt;
		}
		const auto *e = manifest_ptr->find(h);
		if (e == nullptr) {
			return std::nullopt;
		}
		std::ifstream f(std::filesystem::path(content_pack) / e->path, std::ios::binary);
		if (!f) {
			return std::nullopt;
		}
		std::vector<std::byte> buf(static_cast<std::size_t>(e->size));
		if (e->size > 0) {
			f.read(reinterpret_cast<char *>(buf.data()),
					static_cast<std::streamsize>(e->size));
		}
		// The file changed since it was hashed: sending these bytes would only
		// fail the client's check ("asset transfer failed (hash mismatch ...)")
		// with nothing logged here. Refuse, say why, and have the main loop
		// rebuild the manifest so the next join gets the current file.
		if (!f || f.peek() != std::ifstream::traits_type::eof() ||
				vb::assetsync::hash_bytes(buf) != h) {
			std::cerr << "server: asset '" << e->path
					  << "' changed on disk since the manifest was built; refusing "
						 "to serve it and rebuilding the manifest\n";
			manifest_holder.stale.store(true, std::memory_order_relaxed);
			return std::nullopt;
		}
		return buf;
	};
	pack_runtime.install_keybind_registry(host); // before ServerSession copies `host` in
	pack_runtime.install_entity_kind_registry(host); // before ServerSession copies `host` in
	pack_runtime.install_join_veto(host); // before ServerSession copies `host` in
#if defined(VB_WITH_AUTH)
	// External authentication (auth.md §5): the key set loads in the
	// background (failure is logged and retried; joins fail closed until a key
	// set exists); the handshake polls a ticket per connection. Name
	// collisions and the pack's join veto then run in the handshake FSM.
	std::shared_ptr<vb::auth::AuthService> auth_service;
	if (active_auth) {
		auth_service = vb::auth::install_external_auth(host, *active_auth,
				std::shared_ptr<vb::auth::HttpFetcher>(vb::auth::make_curl_fetcher()));
	}
#endif

	vb::net::ServerSession session(transport, hs_config, host);
	auto replicator = std::make_unique<vb::net::WorldReplicator>(world, pool,
			registry, static_cast<int>(config.view_distance), 3);
	pack_runtime.attach_world(*replicator);
	replicator->set_region_store(region_store.get());
	// STATE.md §6: bounds the send side of streaming a player's view box --
	// see ServerConfig::chunk_send_budget_bytes_per_tick's own comment.
	replicator->set_send_budget_bytes(config.chunk_send_budget_bytes_per_tick);
	// Half of each tick for lighting newly generated chunks: a view box
	// filling (a player joining or exploring) can't hold a tick -- and with it
	// every player's input -- for hundreds of milliseconds.
	replicator->set_chunk_ingest_time_budget(0.5 * 1000.0 / static_cast<double>(config.tick_rate));
	session.set_world_replicator(std::move(replicator));
	pack_runtime.attach_session(session);

	session.set_move_params(move_params);
	// Phase 6.18: a pack's vb.combat.set_params{...} overrides player:punch()'s
	// reach/hit_radius/player_damage defaults, same config-then-pack-override
	// shape as move_params above.
	session.set_punch_params(pack_runtime.effective_punch_params(
			vb::net::ServerSession::PunchParams{}));
	// Phase 6.21: a pack's vb.action.set_params{reach=...} overrides the one
	// value both WorldReplicator's block-edit reach check and
	// ServerSession::punch() now share, same config-then-pack-override shape
	// as move_params/punch_params above.
	session.world_replicator()->set_reach(pack_runtime.effective_action_params(
															  vb::net::ActionParams{})
					.reach);
	// REMAINING_TASKS.md's hunger gap: a pack's vb.hunger.set_params{...}
	// overrides decay/starvation-damage rates, same config-then-pack-
	// override shape as move_params/punch_params above -- 0/0 (the
	// engine's own default) means hunger decay stays disabled unless a
	// pack opts in.
	session.set_hunger_params(pack_runtime.effective_hunger_params(
			vb::net::ServerSession::HungerParams{}));
	session.set_void_kill_y(config.void_kill_y);
	// §8.3 hardening: 0 (server.toml's own default) means unlimited, same as
	// today's unset behavior -- only bites once an operator opts in.
	session.set_max_connections_per_ip(
			static_cast<int>(config.max_connections_per_ip));
	// Same "0 = unlimited, only bites once an operator opts in" posture as
	// max_connections_per_ip just above -- defense in depth on top of the
	// closed-schema per-message caps (Phase 3.2/6.3's tracked flood-guard
	// item), not a replacement for them.
	session.set_max_messages_per_second(config.max_messages_per_second);
	// Phase 6.8: fold server.toml's day_length_seconds in as the base, then
	// let a pack's vb.daynight.set_day_length(...) override on top of it --
	// mirrors move_params.gravity's config-then-pack-override precedent.
	session.set_day_length_seconds(
			pack_runtime.effective_day_length_seconds(config.day_length_seconds));

	std::cout << vb::core::describe_build() << '\n'
			  << "server: bind " << config.bind_address << ':' << config.port
			  << " (bound port " << transport.bound_port() << "), pack '"
			  << config.content_pack << "', " << config.tick_rate << " Hz, max "
			  << config.max_players << " players, seed " << seed << ", world "
			  << (config.persist_world ? std::filesystem::absolute(config.world_dir).string()
									   : std::string("(not saved)"))
			  << '\n';

	const auto tick_dt =
			std::chrono::nanoseconds(std::chrono::seconds(1)) / config.tick_rate;
	const double tick_dt_seconds = std::chrono::duration<double>(tick_dt).count();
	auto next = std::chrono::steady_clock::now();
	long long tick = 0;

	// A chunk that stays loaded forever (a player idling in one spot) never
	// goes through ChunkLifecycleSystem's own unload-triggers-save path, so
	// its edits would otherwise only reach disk at shutdown -- this sweep is
	// the periodic durability backstop for that case. 0 (or persistence
	// disabled entirely) means "only unload/shutdown save", same as leaving
	// autosave off outright.
	const long long autosave_ticks = (region_store && config.autosave_interval_seconds > 0.0)
			? std::max<long long>(1,
					  std::llround(config.autosave_interval_seconds * config.tick_rate))
			: 0;
	auto autosave_sweep = [&] {
		if (!region_store) {
			return;
		}
		for (vb::core::ChunkCoord coord : world.loaded_coords()) {
			if (const vb::world::Chunk *chunk = world.find_chunk(coord)) {
				region_store->save_if_dirty(*chunk);
			}
		}
		region_store->flush();
	};

	// Rebuilds the manifest once asset_file_bytes has found a listed file
	// changed on disk (see ManifestHolder). Checked about once a second, same
	// interval-in-ticks shape as autosave_ticks above.
	const long long manifest_check_ticks =
			asset_sync_enabled ? std::max<long long>(1, config.tick_rate) : 0;
	auto manifest_refresh_check = [&] {
		if (!asset_sync_enabled || !manifest_holder.stale.load(std::memory_order_relaxed)) {
			return;
		}
		auto rebuilt = vb::assetsync::build_manifest(
				config.content_pack, asset_caps, asset_exclude_dirs);
		if (rebuilt) {
			manifest_holder.set(std::make_shared<const vb::assetsync::Manifest>(
					std::move(*rebuilt)));
		} else {
			// A pack whose manifest built fine at startup failing to rebuild
			// later (disk full, a file deleted mid-run, ...) is a real
			// operational problem, but not a reason to crash a running
			// server over -- keep serving the last-known-good manifest, warn,
			// and try again next check.
			std::cerr << "server: failed to rebuild asset manifest: "
					  << vb::core::message(rebuilt.error()) << '\n';
		}
	};

	// --status-file: who is online and how the tick loop is keeping up. Written
	// at start, every kStatusIntervalSeconds, and once more at shutdown
	// (running = false), always atomically.
	constexpr double kStatusIntervalSeconds = 5.0;
	// Ticks of lag the loop may replay back to back before it resyncs to the clock.
	constexpr int kMaxTickCatchUp = 5;
	std::map<std::uint32_t, std::string> online;
	const auto status_started = std::chrono::steady_clock::now();
	auto window_start = status_started;
	long long window_ticks = 0;
	double achieved_tick_rate = 0.0;
	const auto emit_status = [&](bool running) {
		if (status_file.empty()) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const double window = std::chrono::duration<double>(now - window_start).count();
		if (window >= 1.0 && window_ticks > 0) {
			achieved_tick_rate = static_cast<double>(window_ticks) / window;
		}
		window_start = now;
		window_ticks = 0;
		std::ostringstream os;
		os << "running = " << (running ? "true" : "false") << "\n"
		   << "updated = "
		   << std::chrono::duration_cast<std::chrono::seconds>(
					  std::chrono::system_clock::now().time_since_epoch())
						.count()
		   << "\n"
		   << "uptime_seconds = " << std::chrono::duration_cast<std::chrono::seconds>(now - status_started).count()
		   << "\n"
		   << "tick = " << tick << "\n"
		   << "target_tick_rate = " << config.tick_rate << "\n"
		   << "tick_rate = " << std::fixed << std::setprecision(1) << achieved_tick_rate
		   << std::defaultfloat << "\n"
		   << "max_players = " << config.max_players << "\n"
		   << "seed = \"" << seed << "\"\n" // string: u64 can exceed TOML's signed 64-bit integers
		   << "motd = " << toml_string(config.motd) << "\n"
		   << "players = [";
		bool first = true;
		for (const auto &[id, name] : online) {
			os << (first ? "" : ", ") << toml_string(name);
			first = false;
		}
		os << "]\n";
		write_status_file(status_file, os.str());
	};
	const long long status_ticks = status_file.empty()
			? 0
			: std::max<long long>(1, std::llround(kStatusIntervalSeconds * config.tick_rate));
	emit_status(true);

	// Same once-a-second cadence as manifest_check_ticks: a stat() per tick
	// would be harmless, but a one-second stop latency is fine and cheaper.
	const long long stop_file_check_ticks =
			stop_file.empty() ? 0 : std::max<long long>(1, config.tick_rate);
#if defined(VB_WITH_AUTOMATION)
	std::unique_ptr<vb::server::ServerAutomationEndpoint> automation_endpoint;
	if (automation) {
		automation_endpoint = std::make_unique<vb::server::ServerAutomationEndpoint>(
				session, world, registry, pack_runtime, tick, transport.bound_port());
	}
#endif

	while (!g_stop.load(std::memory_order_relaxed)) {
		++tick;
		session.tick(tick_dt_seconds);
		if (stop_file_check_ticks > 0 && tick % stop_file_check_ticks == 0) {
			std::error_code stop_ec;
			if (std::filesystem::exists(stop_file, stop_ec)) {
				std::cout << "server: stop file '" << stop_file.string() << "' found, stopping\n";
				break;
			}
		}

		++window_ticks;
		for (const auto &joined : session.take_joins()) {
			online[static_cast<std::uint32_t>(joined.net_id)] = joined.name;
#if defined(VB_WITH_AUTOMATION)
			if (automation_endpoint) {
				automation_endpoint->player_joined(joined.net_id, joined.name);
			}
#endif
			pack_runtime.dispatch_player_join_completed(joined);
			std::cout << "server: '" << joined.name << "' joined (net id "
					  << static_cast<std::uint32_t>(joined.net_id) << ")\n";
		}
		for (const auto &changed : session.take_login_changes()) {
			pack_runtime.dispatch_login_changed(changed);
		}
		for (const auto &left : session.take_leaves()) {
			online.erase(static_cast<std::uint32_t>(left.net_id));
#if defined(VB_WITH_AUTOMATION)
			if (automation_endpoint) {
				automation_endpoint->player_left(left.net_id);
			}
#endif
			pack_runtime.dispatch_player_leave(left);
			std::cout << "server: a player left (" << left.reason << ")\n";
		}

		if (status_ticks > 0 && tick % status_ticks == 0) {
			emit_status(true);
		}
		if (autosave_ticks > 0 && tick % autosave_ticks == 0) {
			autosave_sweep();
		}
		if (manifest_check_ticks > 0 && tick % manifest_check_ticks == 0) {
			manifest_refresh_check();
		}

#if defined(VB_WITH_AUTOMATION)
		if (automation && !automation->pump(*automation_endpoint)) {
			break; // `quit`, or the harness closed our stdin
		}
#endif

		if (max_ticks > 0 && tick >= max_ticks) {
			break;
		}

		next += tick_dt;
		// Bound the catch-up after a stall (or while ticks cost more than a tick period):
		// otherwise the loop replays the whole backlog back to back, and everything that
		// counts simulated time -- handshake timeouts above all -- sees seconds pass in
		// milliseconds, dropping clients that are in fact fine. Falling behind just means
		// the simulation runs slower than real time, which is the honest outcome.
		if (const auto now = std::chrono::steady_clock::now(); now - next > kMaxTickCatchUp * tick_dt) {
			next = now;
		}
		std::this_thread::sleep_until(next);
	}

	// Always save on the way out, regardless of the autosave interval -- this
	// is the difference between "at most autosave_interval_seconds of edits
	// lost on a clean shutdown" and "up to that much lost on *every* stop".
	autosave_sweep();
	emit_status(false);

	std::cout << "server: stopped after " << tick << " ticks\n";
	return EXIT_SUCCESS;
}
