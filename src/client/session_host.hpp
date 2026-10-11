// Client-side connection plumbing and small draw helpers shared by the
// windowed/headless client loop -- moved verbatim out of src/client/main.cpp
// (phase E1 of docs/e2e-automation.md), no behavior change.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include <raygui.h>
#include <raylib.h>

#include "vb/assetsync/cache.hpp"
#include "vb/auth/config.hpp"
#if defined(VB_WITH_AUTH)
#include "vb/auth/http.hpp"
#include "vb/auth/server_glue.hpp"
#endif
#include "vb/core/build_info.hpp"
#include "vb/core/cli.hpp"
#include "vb/core/config.hpp"
#include "vb/core/ids.hpp" // kChunkDim
#include "vb/core/log.hpp"
#include "vb/core/paths.hpp"
#include "vb/core/version_req.hpp"
#include "vb/net/gns_transport.hpp"
#include "vb/net/loopback.hpp"
#include "vb/net/session.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/physics/movement.hpp"
#include "vb/protocol/input.hpp"
#include "vb/protocol/world.hpp"
#include "vb/render/camera.hpp"
#include "vb/render/chunk_renderer.hpp"
#include "vb/render/crack_atlas.hpp"
#include "vb/render/crack_overlay.hpp"
#include "vb/render/entity_renderer.hpp"
#include "vb/render/input.hpp"
#include "vb/render/main_menu.hpp"
#include "vb/render/ui_renderer.hpp"
#include "vb/render/window.hpp"
#include "vb/script/pack_loader.hpp"
#include "vb/script/pack_manifest.hpp"
#include "vb/script/pack_runtime.hpp"
#include "vb/script/ui_runtime.hpp"
#include "vb/world/block.hpp"
#include "vb/world/daynight.hpp"
#include "vb/world/raycast.hpp"
#include "vb/world/region_store.hpp"
#include "vb/world/world.hpp"
#include "vb/world/world_registry.hpp"
#include "vb/worldgen/generator.hpp"
#include "vb/worldgen/worker_pool.hpp"

namespace vb::client {

inline vb::worldgen::WorldGenerator make_generator(
		std::uint64_t seed, const vb::world::BlockRegistry &registry,
		std::shared_ptr<const vb::worldgen::PackWorldGenPipeline> pipeline = nullptr) {
	vb::worldgen::WorldGenParams p;
	p.seed = seed;
	return vb::worldgen::WorldGenerator(p, registry, std::move(pipeline));
}

inline vb::net::HandshakeServerConfig sp_server_config(std::uint64_t seed, int view_distance,
		const std::optional<vb::auth::AuthConfig> &auth = std::nullopt) {
	vb::net::HandshakeServerConfig c;
#if defined(VB_WITH_AUTH)
	if (auth) {
		vb::auth::apply_external_auth(c, *auth); // the same handshake as a dedicated server
	}
#else
	(void)auth;
#endif
	c.pack_name = "base";
	c.motd = "integrated singleplayer";
	c.world_seed = seed;
	c.view_distance = static_cast<std::uint32_t>(view_distance);
	return c;
}

// Copies the pack's handshake-visible settings (vb.render.set_third_person)
// into a server config, as src/server/main.cpp does for a dedicated server.
inline vb::net::HandshakeServerConfig with_pack_settings(vb::net::HandshakeServerConfig c,
		const vb::script::PackRuntime &pack_runtime) {
	c.third_person_allowed = pack_runtime.third_person_allowed();
	return c;
}

inline vb::net::HandshakeClientConfig sp_client_config(const std::string &name) {
	vb::net::HandshakeClientConfig c;
	c.player_name = name;
	c.client_version = vb::kVersionString;
	return c;
}

// JoinGrant::spawn_pos otherwise defaults to a fixed {0, 64, 0} regardless of
// seed -- with base_height=64 and amplitude=28 the real surface ranges
// roughly [36, 92], so a fixed Y can land at or below it and spawn the player
// embedded in solid terrain outright (no fall involved). Compute a real one
// instead, from the generator the world itself uses (pack pipeline
// included): a pipeline-less stand-in has a different height field and used
// to spawn singleplayer players underground. Computed once, not per join.
inline vb::net::HandshakeServerHost sp_server_host(
		const vb::worldgen::WorldGenerator &generator) {
	const vb::core::Vec3d spawn = vb::worldgen::default_spawn_position(generator);
	vb::net::HandshakeServerHost host;
	host.on_ready = [spawn](std::string_view) {
		vb::net::JoinGrant grant;
		grant.spawn_pos = spawn;
		return grant;
	};
	return host;
}

// Default content pack --singleplayer loads (matches server.toml.example's
// own default). `--content-pack <dir>` overrides it, and when the default isn't
// found relative to the working directory it falls back to
// `<exe dir>/content/base` (set once at the top of main(), before anything
// reads it) so an installed client works from any directory -- `vb launch`
// relies on that. Not `constexpr` for that reason, but still process-wide
// state that is written once at startup and only read afterwards.
inline std::string kSingleplayerContentPack = "content/base";

// Phase 7.6 landed world persistence for the dedicated server only
// (RegionStore wired into src/server/main.cpp) -- --singleplayer's
// integrated server had no RegionStore at all, so every edit made in a
// singleplayer session was lost the moment the process exited (the world
// regenerated from scratch, including any placed/broken blocks, on the next
// launch). Fixed the same way the dedicated server's own persist_world
// default works: a fixed relative directory (no client.toml surface for this
// yet, same "hardcoded, not configurable" posture kSingleplayerContentPack
// already has above), distinct from a dedicated server's own default "world"
// dir so running both from the same working directory never collide.
// Deliberately not scoped per-seed: a dedicated server's own world_dir isn't
// either (REMAINING_TASKS.md never asked for that), so a singleplayer world
// re-launched with a different --seed just serves whatever was saved under
// this same directory for any chunk already edited, regenerating the rest
// with the new seed -- identical in spirit to how a dedicated server behaves
// if its own world_seed config changes with saved chunks already on disk.
//
// `--world-dir <dir>` overrides it (`vb launch` points it at a per-user
// directory so worlds outlive the installed version that created them).
inline std::string kSingleplayerWorldDir = "world_singleplayer";

// Dev only (compiled out under VB_DISTRIBUTION): ignore the pack's auth.lua, so
// singleplayer admits the local player unverified and get_login() is nil.
inline bool kSingleplayerSkipAuth = false;

// Dev only (compiled out under VB_DISTRIBUTION): load the singleplayer pack even when its
// pack.toml engine_version_req does not match this build.
inline bool kSingleplayerIgnoreEngineReq = false;

// `auth_out` is set iff the pack declares a valid auth.lua (and auth is not
// skipped): singleplayer then runs the real sign-in, same as a dedicated server.
inline vb::script::PackRuntime make_singleplayer_pack_runtime(
		vb::net::Transport &transport, vb::world::BlockRegistry &registry,
		std::optional<vb::auth::AuthConfig> &auth_out) {
	vb::script::PackRuntime rt(transport, registry,
			std::filesystem::path(kSingleplayerContentPack) / "storage.json");
	// A pack that declares auth.lua makes authentication mandatory
	// (architecture_spec/auth.md): fail closed -- a broken declaration, or a
	// build without the verifier, falls back to the hardcoded base set exactly
	// like a pack that failed to load, never to an unauthenticated pack.
	bool pack_blocked = false;
	// pack.toml's engine_version_req is enforced here too (dev-experience.md §3.7): a pack
	// that needs a newer engine falls back to the base set, like any other blocked pack.
	{
		const vb::core::EngineReqCheck req = vb::script::check_pack_engine_req(
				vb::script::read_pack_manifest(kSingleplayerContentPack), vb::core::engine_version());
		if (!req.ok && kSingleplayerIgnoreEngineReq) {
			std::cerr << "client: *** --ignore-engine-req: " << req.message
					  << " (continuing; development only) ***\n";
		} else if (!req.ok) {
			std::cerr << "client: singleplayer content pack '" << kSingleplayerContentPack << "': " << req.message
					  << " -- running with the hardcoded base block set only\n";
			pack_blocked = true;
		}
	}
	const vb::auth::AuthLoad auth = vb::auth::load_auth_lua(kSingleplayerContentPack);
	if (auth.present && kSingleplayerSkipAuth) {
		std::cerr << "client: *** --insecure-skip-auth: '" << kSingleplayerContentPack
				  << "' declares auth.lua but authentication is DISABLED (development only) ***\n";
	} else if (auth.present && !auth.error.empty()) {
		std::cerr << "client: singleplayer content pack '" << kSingleplayerContentPack
				  << "': " << auth.error << " -- running with the hardcoded base block set only\n";
		pack_blocked = true;
	} else if (auth.present && !vb::auth::kBuiltWithAuth) {
		std::cerr << "client: singleplayer content pack '" << kSingleplayerContentPack
				  << "' requires authentication (auth.lua); rebuild with VB_WITH_AUTH -- "
				  << "running with the hardcoded base block set only\n";
		pack_blocked = true;
	} else if (auth.present) {
		auth_out = auth.config;
		rt.set_auth_required(true); // before the pack loads: vb.auth.required() is visible to init.lua
	}
	if (pack_blocked) {
		// fall through to freeze() with just the base set
	} else if (!vb::script::load_content_pack(rt, kSingleplayerContentPack)) {
		std::cerr << "client: singleplayer content pack '"
				  << kSingleplayerContentPack << "' failed to load -- "
				  << "running with the hardcoded base block set only\n";
	}
	rt.freeze();
	if (const auto worldgen_check = rt.validate_worldgen(); !worldgen_check) {
		std::cerr << "client: singleplayer content pack has invalid worldgen data ("
				  << worldgen_check.message << ") -- decoration disabled\n";
	}
	return rt;
}

// Real texture/atlas system, --singleplayer only: no Asset Sync exists on
// this in-process path (see RemoteConnection's own asset_cache for the real-
// multiplayer equivalent), so texture bytes are read straight off disk
// instead of resolved through a synced virtual FS -- only the handful of
// paths the registry actually references, not the whole content pack tree.
inline vb::render::VirtualFs load_textures_from_disk(
		const vb::world::BlockRegistry &registry, const std::filesystem::path &content_root) {
	vb::render::VirtualFs vfs;
	for (std::size_t i = 0; i < registry.size(); ++i) {
		const vb::world::BlockType &type = registry.get(static_cast<vb::core::BlockId>(i));
		if (type.texture.empty()) {
			continue;
		}
		std::ifstream f(content_root / type.texture, std::ios::binary | std::ios::ate);
		if (!f) {
			continue; // missing on disk -- TextureAtlas::build() falls back gracefully
		}
		const auto size = static_cast<std::size_t>(f.tellg());
		f.seekg(0);
		std::vector<std::byte> bytes(size);
		f.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
		vfs[type.texture] = std::move(bytes);
	}
	return vfs;
}

// Same idea as load_textures_from_disk() above, for entity kinds' `visual`
// spritesheets (entity-management follow-up) instead of block textures --
// --singleplayer has no asset-sync virtual FS to read these back out of, so
// this reads them straight off the same on-disk content pack the integrated
// server loaded from.
inline vb::render::VirtualFs load_entity_textures_from_disk(
		const std::vector<vb::protocol::EntityKindRegistryRecord> &kinds,
		const std::filesystem::path &content_root) {
	vb::render::VirtualFs vfs;
	for (const auto &kind : kinds) {
		if (!kind.visual) {
			continue;
		}
		std::ifstream f(content_root / kind.visual->texture, std::ios::binary | std::ios::ate);
		if (!f) {
			continue; // missing on disk -- set_kind_visual() falls back to the placeholder
		}
		const auto size = static_cast<std::size_t>(f.tellg());
		f.seekg(0);
		std::vector<std::byte> bytes(size);
		f.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
		vfs[kind.visual->texture] = std::move(bytes);
	}
	return vfs;
}

inline vb::net::HandshakeServerHost make_singleplayer_host(
		const vb::worldgen::WorldGenerator &generator,
		vb::script::PackRuntime &pack_runtime,
		const vb::world::BlockRegistry &registry,
		const vb::physics::MoveParams &move_params,
		const std::optional<vb::auth::AuthConfig> &auth = std::nullopt,
		std::shared_ptr<void> *auth_service_out = nullptr) {
	vb::net::HandshakeServerHost host = sp_server_host(generator);
	pack_runtime.install_join_veto(host); // before ServerSession copies `host`
#if defined(VB_WITH_AUTH)
	if (auth) {
		// The same wiring as the dedicated server (src/server/main.cpp).
		auto svc = vb::auth::install_external_auth(host, *auth,
				std::shared_ptr<vb::auth::HttpFetcher>(vb::auth::make_curl_fetcher()));
		if (auth_service_out != nullptr) {
			*auth_service_out = svc; // keeps the key-refresh worker alive with the session
		}
	}
#else
	(void)auth;
	(void)auth_service_out;
#endif
	// Entity-management follow-up to Phase 6.1: mirrors src/server/main.cpp's
	// own install_entity_kind_registry call exactly -- without this,
	// --singleplayer's script entities would render as the flat placeholder
	// regardless of what a pack's vb.register_entity{width=, height=} asked
	// for, the same "silently missing" gap host.block_registry above closes
	// for custom blocks.
	pack_runtime.install_entity_kind_registry(host); // before ServerSession copies `host`
	// Phase 6.7: mirrors src/server/main.cpp's own host.move_params exactly --
	// without this, --singleplayer's client-side prediction would silently
	// keep vb::physics::MoveParams's hardcoded defaults even when a pack
	// overrides them via vb.physics.set_params.
	host.move_params =
			[move_params]() -> std::optional<vb::protocol::S2CMoveParams> {
		return vb::protocol::S2CMoveParams{ move_params.half_width,
			move_params.height, move_params.eye_height, move_params.walk_speed,
			move_params.sprint_speed, move_params.accel, move_params.air_accel,
			move_params.friction, move_params.gravity, move_params.jump_speed,
			move_params.terminal_velocity, move_params.step_height,
			move_params.fly_speed, move_params.fly };
	};
	// Phase 4.3: without this, a joining client stays on its own base()
	// registry and any pack-added block (planks/sticks from crafting.lua)
	// resolves to nothing client-side -- name lookups fall back to "?" in
	// the hotbar even though give()/take() work fine either way (inventory
	// slots are just numeric ids). Mirrors src/server/main.cpp's own
	// host.block_registry callback exactly.
	host.block_registry =
			[&registry]() -> std::optional<std::vector<vb::protocol::BlockRegistryRecord>> {
		std::vector<vb::protocol::BlockRegistryRecord> out;
		out.reserve(registry.size());
		for (std::size_t i = 0; i < registry.size(); ++i) {
			const auto &t = registry.get(static_cast<vb::core::BlockId>(i));
			// Same real bug fix as src/server/main.cpp's own copy of this
			// callback (see its comment): max_damage/crack_texture were
			// silently dropped by the old 6-field aggregate-init.
			out.push_back({ t.name, t.solid, t.opaque, t.liquid, t.light_emission,
					t.texture, t.max_damage, t.crack_texture });
		}
		return out;
	};
	// Phase 6.8: mirrors src/server/main.cpp's own host.day_night_curve
	// exactly -- nullopt (no pack called vb.daynight.set_curve) sends no
	// frame, leaving --singleplayer on the same built-in gradient a
	// dedicated server's clients get.
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
	return host;
}

// Owns the in-process world for --singleplayer, kept alive for the whole
// session (spec §3: the integrated server is a library, not a child
// process). Built directly from LoopbackNetwork/ServerSession/ClientSession
// -- the same pieces `vb::net::IntegratedGame` wraps -- rather than through
// IntegratedGame itself: a `PackRuntime` needs the raw server-side
// `Transport&` (to send chat/give/etc. messages) and needs
// `install_join_veto()` to run *before* `ServerSession` is constructed,
// neither of which IntegratedGame's all-in-one constructor exposes a hook
// for. This closes the "`--singleplayer` doesn't asset-sync/run the content
// pack's Lua at all" gap `REMAINING_TASKS.md` had tracked since Phase 5.1 --
// singleplayer now loads and runs `content/base` exactly like a real
// dedicated server does (chat, crafting, item drops, custom blocks), just
// over a loopback transport instead of real UDP.
struct Singleplayer {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	// Set iff the pack declares auth.lua (see make_singleplayer_pack_runtime);
	// declared before pack_runtime/server so their initializers can read it.
	std::optional<vb::auth::AuthConfig> auth_config;
	std::shared_ptr<void> auth_service; // AuthService, outlives `server`'s use of it
	vb::script::PackRuntime pack_runtime;
	// Phase 6.7: no ServerConfig/server.toml on this in-process path, so the
	// engine default (vb::physics::MoveParams{}) is the base a pack's
	// vb.physics.set_params{...} overrides on top of -- computed right after
	// pack_runtime (declaration order == init order) so it's ready both for
	// make_singleplayer_host below and set_move_params() in the body.
	vb::physics::MoveParams move_params;
	vb::world::World world;
	// Phase 6.14: mirrors src/server/main.cpp's own build_worldgen_pipeline
	// call -- the pipeline is nullptr unless a pack called
	// vb.worldgen.set_pipeline, in which case --singleplayer's terrain
	// matches a dedicated server's. Shared by `pool` and the spawn search.
	vb::worldgen::WorldGenerator generator;
	vb::worldgen::WorldGenWorkerPool pool;
	// Phase 7.6 follow-up: declared before `server` so it outlives the
	// WorldReplicator that `server` owns (which holds a raw, non-owning
	// pointer to it via ChunkLifecycleSystem::set_region_store) -- members are
	// destroyed in reverse declaration order, so this stays alive for the
	// whole time `server`'s replicator could still touch it.
	std::unique_ptr<vb::world::RegionStore> region_store;
	// Set when the saved world can't be used with this pack (see
	// vb::world::check_world_registry); the session must not be played then.
	std::string world_error;
	vb::net::ServerSession server;
	std::optional<vb::net::ClientSession> client_session;

	Singleplayer(std::uint64_t seed, const std::string &name, int view_distance) : pack_runtime(make_singleplayer_pack_runtime(net.server(), registry, auth_config)),
																				   move_params(pack_runtime.effective_move_params(vb::physics::MoveParams{})),
																				   world(registry),
																				   generator(make_generator(seed, registry,
																						   pack_runtime.build_worldgen_pipeline(
																								   vb::worldgen::WorldGenParams{ seed }))),
																				   pool(generator),
																				   server(net.server(), with_pack_settings(sp_server_config(seed, view_distance, auth_config), pack_runtime),
																						   make_singleplayer_host(generator, pack_runtime, registry, move_params,
																								   auth_config, &auth_service)) {
		server.set_move_params(move_params);
		// Phase 6.18: mirrors src/server/main.cpp's own set_punch_params call.
		server.set_punch_params(pack_runtime.effective_punch_params(
				vb::net::ServerSession::PunchParams{}));
		// Mirrors src/server/main.cpp's own set_hunger_params call
		// (REMAINING_TASKS.md's hunger gap).
		server.set_hunger_params(pack_runtime.effective_hunger_params(
				vb::net::ServerSession::HungerParams{}));
		// Phase 6.8: no server.toml on this in-process path either, so
		// ServerSession's own hardcoded default (kDefaultDayLengthSeconds,
		// matching its member initializer) is the base a pack's
		// vb.daynight.set_day_length(...) overrides on top of (mirrors
		// move_params above).
		server.set_day_length_seconds(pack_runtime.effective_day_length_seconds(
				vb::net::kDefaultDayLengthSeconds));
		auto listening = net.server().listen(0);
		(void)listening; // loopback listen never fails on a fresh network

		// Phase 7.6 follow-up: mirrors src/server/main.cpp's own
		// region_store construction -- unlike the dedicated server there's no
		// persist_world config toggle to check here yet (no client.toml
		// surface for it, same fixed-default posture kSingleplayerWorldDir's
		// own comment already has).
		// Same registry check as the dedicated server: a world saved by another
		// pack is refused (world_error, shown instead of joining) rather than
		// loaded with the wrong blocks.
		const auto pack_info = vb::script::read_pack_manifest(kSingleplayerContentPack);
		const auto check = vb::world::check_world_registry(kSingleplayerWorldDir, registry,
				pack_info.name.empty() ? kSingleplayerContentPack : pack_info.name);
		if (!check.ok) {
			world_error = check.message;
		} else {
			if (!check.message.empty()) {
				VB_WARN("world", check.message);
			}
			region_store = std::make_unique<vb::world::RegionStore>(kSingleplayerWorldDir);
		}

		auto replicator = std::make_unique<vb::net::WorldReplicator>(
				world, pool, registry, view_distance, 3);
		pack_runtime.attach_world(*replicator);
		// Phase 6.21: mirrors src/server/main.cpp's own set_reach call -- the
		// same value both this replicator's block-edit reach and
		// server.punch()'s combat reach read.
		replicator->set_reach(
				pack_runtime.effective_action_params(vb::net::ActionParams{}).reach);
		replicator->set_region_store(region_store.get());
		server.set_world_replicator(std::move(replicator));
		pack_runtime.attach_session(server);

		vb::net::Transport &client_transport = net.create_client();
		auto conn = client_transport.connect("integrated", 0);
		if (conn) {
			client_session.emplace(
					client_transport, *conn, sp_client_config(name));
		} else {
			std::cerr << "client: singleplayer failed to connect to its own "
						 "loopback server\n";
		}
	}

	// Phase 7.6 follow-up: unconditional final save on the way out, mirroring
	// src/server/main.cpp's own unconditional autosave_sweep() call right
	// before it returns -- without this, only edits already covered by the
	// periodic sweep below (or a chunk that happened to unload while playing)
	// would survive; anything edited since the last sweep and still loaded
	// when the session ends (quitting to the main menu, or closing the whole
	// app) would be silently lost.
	~Singleplayer() { save_all_dirty(); }

	void save_all_dirty() {
		if (!region_store) {
			return;
		}
		for (vb::core::ChunkCoord coord : world.loaded_coords()) {
			if (const vb::world::Chunk *chunk = world.find_chunk(coord)) {
				region_store->save_if_dirty(*chunk);
			}
		}
		region_store->flush();
	}

	vb::net::ClientSession &client() { return *client_session; }

	// Server tick rate (spec §7's fixed simulation rate; matches
	// HandshakeServerConfig::tick_rate's default -- sp_server_config() doesn't
	// override it). A dedicated server (src/server/main.cpp) sleep_until()s
	// between ticks, so it's naturally paced at this rate; the integrated
	// server here is instead driven by the client's render loop, so tick()
	// accumulates the variable frame dt and steps the server at this fixed
	// rate itself -- otherwise physics/worldgen determinism and replication
	// cadence would depend on framerate, unlike every other server.
	static constexpr double kFixedDt = 1.0 / 20.0;
	// Caps how many fixed steps one frame will catch up on (e.g. after a
	// stall from asset loading or a debugger breakpoint) -- runs behind at
	// that point instead of spiralling into an ever-growing catch-up burst.
	static constexpr int kMaxStepsPerFrame = 5;
	double tick_accum_ = 0.0;

	// Advances the client every frame (render-rate prediction/interpolation),
	// and the server + pack runtime's join/leave/tick dispatch at the fixed
	// rate above -- mirrors src/server/main.cpp's own tick loop so a pack
	// behaves identically whether it's driven by a real dedicated server or
	// this in-process one.
	// Per-real-tick chunk ingest+relight budget (mirrors
	// chunk_lifecycle.cpp's own kIngestBudgetPerTick default) -- see
	// set_ingest_budget()'s doc for why this needs shrinking per catch-up
	// step below instead of being spent in full on every one of them.
	static constexpr std::size_t kBaseChunkIngestBudget = 32;
	// The same, as wall time per rendered frame: the integrated server runs
	// inside the client's frame, so chunk lighting here is a frame hitch.
	static constexpr double kFrameChunkIngestMs = 20.0;

	// Phase 7.6 follow-up: mirrors src/server/main.cpp's own
	// autosave_interval_seconds default (60s) -- a chunk that stays loaded
	// the whole session (the player never wandering far enough to trigger
	// ChunkLifecycleSystem's own unload-triggers-save path) would otherwise
	// only ever reach disk in the final save_all_dirty() the destructor
	// above does, e.g. never for a session that crashes instead of exiting
	// cleanly. No client.toml surface to configure this yet, same
	// hardcoded-default posture as kSingleplayerWorldDir itself.
	static constexpr double kAutosaveIntervalSeconds = 60.0;
	double autosave_accum_ = 0.0;

	void tick(double dt) {
		if (client_session) {
			client_session->tick(dt);
		}

		tick_accum_ += dt;
		// How many fixed steps this frame is about to run, capped the same way
		// the loop below caps itself -- known up front since it's a pure
		// function of tick_accum_/kFixedDt, so the ingest budget can be spread
		// across them before the first step runs instead of after the fact.
		const int expected_steps = std::min(kMaxStepsPerFrame,
				static_cast<int>(tick_accum_ / kFixedDt));
		if (vb::net::WorldReplicator *wr = server.world_replicator()) {
			const std::size_t per_step_budget = expected_steps > 1
					? std::max<std::size_t>(1,
							  kBaseChunkIngestBudget / static_cast<std::size_t>(expected_steps))
					: kBaseChunkIngestBudget;
			wr->set_chunk_ingest_budget(per_step_budget);
			wr->set_chunk_ingest_time_budget(
					kFrameChunkIngestMs / static_cast<double>(std::max(1, expected_steps)));
		}
		int steps = 0;
		while (tick_accum_ >= kFixedDt && steps < kMaxStepsPerFrame) {
			server.tick(kFixedDt);
			for (auto &j : server.take_joins()) {
				pack_runtime.dispatch_player_join_completed(j);
			}
			for (auto &c : server.take_login_changes()) {
				pack_runtime.dispatch_login_changed(c);
			}
			for (auto &l : server.take_leaves()) {
				pack_runtime.dispatch_player_leave(l);
			}
			tick_accum_ -= kFixedDt;
			++steps;
		}
		if (steps == kMaxStepsPerFrame) {
			tick_accum_ = 0.0; // drop the backlog rather than spiral
		}

		if (region_store) {
			autosave_accum_ += dt;
			if (autosave_accum_ >= kAutosaveIntervalSeconds) {
				autosave_accum_ = 0.0;
				save_all_dirty();
			}
		}
	}
};

// Real multiplayer: a GnsTransport dialing a dedicated voxel_browser_server
// (spec §8.1). `session` is empty if connect() itself failed (bad address, or
// built without VB_WITH_NET); a *later* handshake failure instead shows up as
// client->failed() once the join-wait loop runs.
struct RemoteConnection {
	vb::net::GnsTransport transport;
	vb::assetsync::ClientAssetCache asset_cache;
	std::optional<vb::net::ClientSession> session;

	RemoteConnection(const std::string &host, std::uint16_t port,
			const std::string &name, const vb::core::ClientConfig &config) : asset_cache(config.asset_cache_dir.empty()
																							 ? vb::core::user_cache_dir() / "assets"
																							 : std::filesystem::path(config.asset_cache_dir),
																					 static_cast<std::uint64_t>(config.asset_cache_mb) * 1024ull * 1024ull) {
		auto conn = transport.connect(host, port);
		if (!conn) {
			return;
		}
		vb::net::HandshakeClientConfig c;
		c.player_name = name;
		c.client_version = vb::kVersionString;
		session.emplace(transport, *conn, std::move(c), &asset_cache);
	}
};

inline Camera3D to_camera(const vb::render::FirstPersonController &c, float fovy) {
	const vb::core::Vec3d p = c.position();
	const vb::core::Vec3d t = c.target();
	Camera3D cam{};
	cam.position = { static_cast<float>(p.x), static_cast<float>(p.y),
		static_cast<float>(p.z) };
	cam.target = { static_cast<float>(t.x), static_cast<float>(t.y),
		static_cast<float>(t.z) };
	cam.up = { 0.0f, 1.0f, 0.0f };
	cam.fovy = fovy;
	cam.projection = CAMERA_PERSPECTIVE;
	return cam;
}

inline void draw_overlay(const vb::render::FirstPersonController &c,
		const std::string &status, std::size_t chunk_count,
		std::size_t entity_count, bool mouse_captured,
		std::uint32_t time_of_day) {
	const vb::core::Vec3d p = c.position();
	char line[160];
	DrawText("voxel_browser", 12, 12, 20, RAYWHITE);
	DrawText(status.c_str(), 12, 38, 18, Color{ 170, 200, 170, 255 });
	std::snprintf(line, sizeof(line),
			"pos  %.1f  %.1f  %.1f   chunks %zu   entities %zu", p.x, p.y, p.z,
			chunk_count, entity_count);
	DrawText(line, 12, 64, 18, Color{ 170, 170, 180, 255 });
	std::snprintf(line, sizeof(line), "look yaw %.0f  pitch %.0f", c.yaw(),
			c.pitch());
	DrawText(line, 12, 86, 18, Color{ 170, 170, 180, 255 });
	// kTicksPerDay (24000) / 24h conveniently gives 1000 ticks/hour.
	std::snprintf(line, sizeof(line), "time %02u:%02u",
			time_of_day / 1000u, (time_of_day % 1000u) * 60u / 1000u);
	DrawText(line, 12, 108, 16, Color{ 170, 170, 180, 255 });
	DrawText(mouse_captured ? "mouse captured (Tab to release)"
							: "click to capture mouse",
			12, 130, 16, Color{ 140, 140, 150, 255 });
	DrawFPS(12, 154);
}

inline const char *connecting_status_text(vb::net::ClientHandshakeStatus s) {
	using vb::net::ClientHandshakeStatus;
	switch (s) {
		case ClientHandshakeStatus::kConnecting:
			return "Connecting...";
		case ClientHandshakeStatus::kAwaitingChallenge:
			return "Waiting for sign-in request...";
		case ClientHandshakeStatus::kSigningIn:
			return "Signing in...";
		case ClientHandshakeStatus::kAuthenticating:
			return "Authenticating...";
		case ClientHandshakeStatus::kAwaitingAssetManifest:
			return "Requesting content manifest...";
		case ClientHandshakeStatus::kSyncingAssets:
			return "Downloading content pack...";
		case ClientHandshakeStatus::kSyncing:
			return "Syncing world...";
		case ClientHandshakeStatus::kJoined:
			return "Joined.";
		case ClientHandshakeStatus::kFailed:
			return "Failed.";
	}
	return "Connecting...";
}

} // namespace vb::client
