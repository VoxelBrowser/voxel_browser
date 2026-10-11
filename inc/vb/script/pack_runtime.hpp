#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>

#include "vb/core/config.hpp"
#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"
#include "vb/net/handshake.hpp"
#include "vb/net/session.hpp"
#include "vb/net/transport.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/physics/movement.hpp"
#include "vb/protocol/world.hpp" // S2CFogParams
#include "vb/script/vm.hpp"
#include "vb/world/block.hpp"
#include "vb/world/daynight.hpp"
#include "vb/worldgen/generator.hpp"

// Server-side Lua registration + runtime API (spec §10.3, Phase 4.2). Owns a
// Vm, the frozen-after-load content registries, the event bus, and
// vb.after/vb.every timers.
//
// Layering: PackRuntime depends *down* on Transport/ServerSession/
// WorldReplicator/BlockRegistry -- none of them ever include this header
// (WorldReplicator's block-edit veto seam is a plain std::function struct,
// vb/net/world_replicator.hpp's BlockEditHooks).
//
// Construction is split because a join veto must wrap HandshakeServerHost
// *before* the ServerSession that owns it exists, while runtime dispatch
// needs that same ServerSession by reference:
//
//   PackRuntime rt(transport, registry, storage_path);
//   rt.load_pack_file(pack_source);
//   rt.freeze();
//   rt.install_join_veto(host);              // before constructing session
//   net::ServerSession session(transport, cfg, host);
//   net::WorldReplicator replicator(world, pool, registry, view_dist);
//   session.set_world_replicator(...);
//   rt.attach_world(*session.world_replicator());
//   rt.attach_session(session);
//   // main loop, once per tick, after session.tick(dt):
//   for (auto &j : session.take_joins())  rt.dispatch_player_join_completed(j);
//   for (auto &l : session.take_leaves()) rt.dispatch_player_leave(l);
//   rt.dispatch_tick(dt);
//
// Single-threaded: construct and drive every method from the same thread
// that calls ServerSession::tick() (the server main loop). No locking.

namespace vb::script {

class PackRuntime {
public:
	// `storage_path` is the JSON file backing vb.storage.
	PackRuntime(net::Transport &transport, world::BlockRegistry &registry,
			std::filesystem::path storage_path, VmLimits limits = {});
	~PackRuntime();

#if defined(VB_WITH_AUTOMATION)
	// Development-only (docs/e2e-automation.md §5.3): puts `count` of the block
	// registered as `item_name` into a player's inventory and syncs it to
	// them, exactly like `player:give{}`. False for an unknown item name, a
	// zero count, or a build without scripting.
	bool admin_give(core::NetId player, std::string_view item_name, std::uint16_t count);
#endif
	PackRuntime(PackRuntime &&) noexcept;
	PackRuntime &operator=(PackRuntime &&) noexcept;
	PackRuntime(const PackRuntime &) = delete;
	PackRuntime &operator=(const PackRuntime &) = delete;

	// Run one pack Lua file (pack-load time only: registration calls are
	// allowed). Call freeze() once every pack file is loaded, before
	// attach_world()/attach_session().
	ScriptResult load_pack_file(std::string_view code,
			std::string_view chunk_name = "pack");
	void freeze();

	// Structure editor S1: call after freeze() (every block is registered by
	// then). Resolves each registered structure's palette and every biome's
	// decoration entries, so an unknown block or structure name is reported
	// as a pack load error naming the offender instead of surfacing later,
	// when the pipeline is built. build_worldgen_pipeline() runs the same
	// checks; on failure it logs the message and builds the pipeline with no
	// decoration.
	ScriptResult validate_worldgen() const;

	// Phase 4.1: installs (or replaces) the pack's virtual module filesystem
	// for a sandboxed `require` inside pack Lua code -- see
	// vb::script::Vm::install_require for the exact resolution/caching rules.
	// Call any time before a pack script might call require(), typically once
	// before the first load_pack_file() (vb::script::load_content_pack does
	// this automatically from its own directory walk).
	void set_pack_modules(std::unordered_map<std::string, std::string> modules);

	// Wraps host.authenticate so vb.on("player_join", handler) can veto a join
	// before it completes. Call after freeze(), before constructing the
	// ServerSession that will own `host`.
	void install_join_veto(net::HandshakeServerHost &host);

	// Whether this server authenticates players (the pack ships auth.lua and
	// it is active). Backs `vb.auth.required()`. Call before the pack loads so
	// module-scope code sees the right answer. When false, every
	// `player:get_login()` is nil.
	void set_auth_required(bool required);

	// Wraps host.keybind_registry so every vb.register_keybind name reaches
	// joining clients as S2C_KeybindRegistry (Phase 6.3). Same calling
	// convention as install_join_veto: call after freeze(), before
	// constructing the ServerSession that will copy `host`.
	void install_keybind_registry(net::HandshakeServerHost &host);

	// Wraps host.entity_kind_registry so every vb.register_entity kind reaches
	// joining clients as S2C_EntityKindRegistry (entity-management follow-up
	// to Phase 6.1), letting render::EntityRenderer size script-entity
	// billboards per kind instead of one flat placeholder for all of them.
	// Same calling convention as install_join_veto/install_keybind_registry:
	// call after freeze(), before constructing the ServerSession that will
	// copy `host`.
	void install_entity_kind_registry(net::HandshakeServerHost &host);

	// Call once each object exists to enable the block-edit veto/on_break/
	// on_place hooks and the entity/player runtime API respectively.
	void attach_world(net::WorldReplicator &replicator);
	void attach_session(net::ServerSession &session);

	// Phase 6.13: makes the operator's server.toml/CLI settings readable via
	// `vb.config.get(key)` -- read-only, deliberately not an override surface
	// like 6.6-6.11 (a pack should not be able to silently change
	// `max_players` out from under the operator running the server). Call any
	// time before a pack might call vb.config.get, i.e. right after
	// construction, before load_pack_file(). Never called at all (e.g.
	// --singleplayer's in-process PackRuntime, which has no ServerConfig/
	// server.toml) means vb.config.get returns nil for every key.
	void set_server_config(const core::ServerConfig &config);

	// Phase 6.7: applies a pack's `vb.physics.set_params{...}` on top of
	// `base` -- only the fields the pack actually set replace `base`'s value,
	// everything else keeps it. `base` is the engine/operator default (e.g.
	// ServerConfig::gravity already folded in), so a pack that never calls
	// vb.physics.set_params gets `base` back unchanged. Call after freeze(),
	// once, before constructing the ServerSession (its result also belongs on
	// HandshakeServerHost::move_params so the client mirrors it exactly).
	physics::MoveParams effective_move_params(physics::MoveParams base) const;

	// Phase 6.18: applies a pack's `vb.combat.set_params{...}` on top of
	// `base` -- same "only the fields the pack actually set replace base's
	// value" shape as effective_move_params() above. Call after freeze(),
	// once, before wiring the result onto ServerSession::set_punch_params().
	net::ServerSession::PunchParams effective_punch_params(
			net::ServerSession::PunchParams base) const;

	// Phase 6.21: applies a pack's `vb.action.set_params{reach=...}` on top of
	// `base` -- same shape as effective_punch_params()/effective_move_params()
	// above. Unifies what used to be WorldReplicator's own hardcoded, non-
	// overridable reach constant and PunchParams::reach into the one value
	// both WorldReplicator::set_reach() and ServerSession::punch() now read.
	// Call after freeze(), once, before wiring the result onto
	// WorldReplicator::set_reach().
	net::ActionParams effective_action_params(net::ActionParams base) const;

	// REMAINING_TASKS.md's hunger gap: applies a pack's
	// `vb.hunger.set_params{decay_per_second=, starvation_damage_per_second=}`
	// on top of `base` -- same shape as effective_punch_params() above. Call
	// after freeze(), once, before wiring the result onto
	// ServerSession::set_hunger_params().
	net::ServerSession::HungerParams effective_hunger_params(
			net::ServerSession::HungerParams base) const;

	// Phase 6.8: a pack's `vb.daynight.set_curve{keyframes = {...}}`, if it
	// ever called it -- `nullopt` (default) means no pack ever overrode the
	// curve, so the caller should send no S2C_DayNightCurve frame at all and
	// let the client keep vb::world::default_day_night_curve() (same "no
	// frame, no behavior change" posture as effective_move_params() feeding
	// HandshakeServerHost::move_params).
	std::optional<world::DayNightCurve> effective_day_night_curve() const;

	// Phase 6.8: applies a pack's `vb.daynight.set_day_length(seconds)` on top
	// of `base` (the operator's `server.toml` day_length_seconds, or
	// ServerSession's own hardcoded default) -- returns `base` unchanged if no
	// pack ever calls it, same "engine/operator default, pack overrides on
	// top" shape as effective_move_params()/ServerConfig::gravity.
	double effective_day_length_seconds(double base) const;

	// Phase 7.2: a pack's `vb.render.set_fog{start=, end=}`, if it ever
	// called it -- `nullopt` (default) means no pack ever overrode the fog
	// distance, so the caller should send no S2C_FogParams frame at all and
	// let each client compute its own default from its own view_distance
	// config (same "no frame, no behavior change" posture as
	// effective_day_night_curve(), except there's no server-side universal
	// default to fall back on here -- the server doesn't know each client's
	// view_distance).
	std::optional<protocol::S2CFogParams> effective_fog_params() const;
	// vb.render.set_third_person(allowed); true unless the pack forbade it.
	// Copy into HandshakeServerConfig::third_person_allowed.
	bool third_person_allowed() const;

	// Phase 6.14: compiles a pack's `vb.worldgen.set_pipeline{...}` call (plus
	// every `vb.register_biome` entry) into an immutable
	// `worldgen::PackWorldGenPipeline` on top of `base` (the operator/engine
	// default `WorldGenParams` -- only `seed` is ever externally overridden
	// today). Returns `nullptr` if no pack ever called `set_pipeline`, same
	// "no call, no pipeline, generator keeps its fixed default" opt-in shape
	// as `effective_day_night_curve`. Call once, main thread, after
	// `freeze()`, before constructing `WorldGenerator`/`WorldGenWorkerPool` --
	// the returned pipeline is immutable and safe to share across worker
	// threads from then on.
	std::shared_ptr<const worldgen::PackWorldGenPipeline> build_worldgen_pipeline(
			const worldgen::WorldGenParams &base) const;

	// Drive from the main loop, once per tick, after ServerSession::tick():
	void dispatch_player_join_completed(const net::SessionPlayerJoined &j);
	void dispatch_player_leave(const net::SessionPlayerLeft &l);
	// Periodic re-auth changed an allowlisted claim (auth.md §5.6): swaps the
	// player's frozen login table and fires vb.on("login_changed", ...).
	void dispatch_login_changed(const net::SessionLoginChanged &c);
	void dispatch_tick(double dt_seconds);

	// Generic bus hook for player_interact (no C2S message yet) -- exposed so
	// a future message handler can call it without knowing anything about
	// Lua. Returns false if any handler vetoed.
	bool dispatch_player_interact(core::NetId player, core::IVec3 target);

	// Phase 5.4/6.10: runs every vb.on("chat", handler) in registration
	// order, chaining text replacements (each handler sees the prior one's
	// output) and short-circuiting on the first `false` veto -- same shape as
	// run_player_input/ServerSession::InputHookResult.
	net::ServerSession::ChatHookResult dispatch_chat(
			core::NetId sender, std::string_view text);

	// Wired automatically by attach_session() to ServerSession's
	// C2S_UiEvent handler (Phase 4.5) -- fires vb.on("ui_event", handler)
	// non-vetoably; spec gives no veto semantics for UI events.
	void dispatch_ui_event(core::NetId player, const protocol::C2SUiEvent &event);

	bool storage_dirty() const;
	void flush_storage(); // write vb.storage to storage_path if dirty

	// Bumped every time flush_storage() actually writes storage_path to disk
	// (both this explicit call and dispatch_tick()'s own internal auto-flush
	// when storage_dirty()). Lets an embedder (e.g. the dedicated server's
	// asset manifest, which hashes storage.json's on-disk bytes) detect a
	// runtime write with a cheap integer comparison instead of re-hashing the
	// file speculatively every tick -- see REMAINING_TASKS.md's "Manifest
	// staleness" item.
	std::uint64_t storage_revision() const;

	// Dotted names of the `vb.*` surface plus every registered usertype
	// method (`Player:give`), for the API drift test (sdk/lua/api_index.txt).
	std::vector<std::string> describe_api();

	// Names of the Lua globals that exist now (after the pack loaded), so `--check-pack` can
	// tell run-time-defined globals (`_G[name] = ...`) from typos.
	std::vector<std::string> global_names();

	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace vb::script
