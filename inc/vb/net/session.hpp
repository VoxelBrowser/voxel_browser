#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <memory>
#include <unordered_map>
#include <utility>

#include <entt/entt.hpp>

#include "vb/assetsync/cache.hpp"
#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"
#include "vb/ecs/components.hpp"
#include "vb/ecs/system_runner.hpp"
#include "vb/net/block_damage_tracker.hpp"
#include "vb/net/handshake.hpp"
#include "vb/net/server_time_estimator.hpp"
#include "vb/net/transport.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/physics/movement.hpp"
#include "vb/protocol/chat.hpp"
#include "vb/protocol/handshake.hpp"
#include "vb/protocol/input.hpp"
#include "vb/protocol/inventory.hpp"
#include "vb/protocol/snapshot.hpp"
#include "vb/replication/interest.hpp"
#include "vb/world/block_damage.hpp"
#include "vb/world/client_chunk_store.hpp"
#include "vb/world/daynight.hpp"
#include "vb/world/item_drops.hpp"

// Sessions glue a Transport to the handshake FSMs and present a small
// game-facing API: the server loop pulls join/leave events, the client loop
// polls a status. Both are driven by one tick() per frame/tick.
//
// Transport backend is injected, so the same ServerSession runs behind a
// LoopbackTransport (integrated singleplayer, tests) or a GnsTransport
// (dedicated server) with no code change.

namespace vb::net {

// Day/night cycle (spec §5.4): ServerSession's built-in real-seconds-per-day
// default, also the base --singleplayer's in-process host feeds into
// PackRuntime::effective_day_length_seconds() (Phase 6.8) since it has no
// server.toml to read a config value from.
inline constexpr double kDefaultDayLengthSeconds = 1200.0;

// --- server ---------------------------------------------------------------

struct SessionPlayerJoined {
	ConnId conn = ConnId::kInvalid;
	core::NetId net_id = core::NetId::kInvalid;
	std::string name;
	// External auth: the verified identity (null when the server isn't
	// authenticating). User data only; the token never reaches here.
	std::shared_ptr<const LoginData> login;
};

// Periodic re-auth succeeded and an allowlisted claim changed (auth.md §5.6).
struct SessionLoginChanged {
	core::NetId net_id = core::NetId::kInvalid;
	std::shared_ptr<const LoginData> login; // the new login (same name as before)
};

struct SessionPlayerLeft {
	ConnId conn = ConnId::kInvalid;
	core::NetId net_id = core::NetId::kInvalid;
	std::string reason;
	// Captured just before the player's entity is destroyed: by the time a
	// leave is taken, the session can no longer answer for this net id.
	std::string name;
	std::optional<physics::MoveState> last_state;
};

class ServerSession {
public:
	ServerSession(Transport &transport, HandshakeServerConfig config,
			HandshakeServerHost host = {});

	void tick(double dt_seconds);

	std::vector<SessionPlayerJoined> take_joins();
	std::vector<SessionPlayerLeft> take_leaves();
	std::vector<SessionLoginChanged> take_login_changes();

	std::size_t player_count() const { return playing_; }
	std::size_t pending_count() const { return conns_.size() - playing_; }
	std::uint32_t server_tick() const { return server_tick_; }

	// Feed authoritative entity state into the replication grid (Phase 3's
	// movement systems will call this; tests set it directly).
	void set_player_state(core::NetId id, core::Vec3d pos, core::Vec2f rot = {},
			core::Vec3f vel = {});

	void set_interest_radius_cells(int cells) { interest_radius_cells_ = cells; }

	// Day/night cycle (spec §5.4): real seconds for one full in-game day.
	// Default matches world::advance_time_of_day's expectations; set before
	// players join if a pack/host wants a different pace.
	void set_day_length_seconds(double seconds) { day_length_seconds_ = seconds; }
	std::uint32_t time_of_day() const {
		return static_cast<std::uint32_t>(time_of_day_ticks_);
	}

	// Movement tunables applied to every player (spec §7.3). Set before players
	// join; a Lua pack overrides per entity kind in Phase 4.
	void set_move_params(physics::MoveParams p) { move_params_ = p; }

	// Authoritative feet position of a player (for tests / teleports). Input
	// batches are the normal drive path. nullopt if `id` isn't a playing
	// connection.
	std::optional<physics::MoveState> player_move_state(core::NetId id) const;

	// Current / max health of a playing connection; nullopt if `id` isn't one.
	// Always available: `player:get_health()` (Lua) reads it.
	std::optional<std::pair<float, float>> player_health(core::NetId id) const;

	// Moves a player to `pos` (feet), zeroing velocity and keeping their look
	// direction; the client reconciles to it like any authoritative
	// correction. Always available: `player:set_pos()` (Lua) and the
	// automation `teleport` command both call it. False if `id` isn't a
	// playing connection.
	bool teleport_player(core::NetId id, core::Vec3d pos);

#if defined(VB_WITH_AUTOMATION)
	// --- development-only automation primitives (docs/e2e-automation.md §5.3) ---
	// Compiled out of production builds with the rest of the automation code;
	// nothing in the engine itself calls them.

	// Sets health exactly. Lowering goes through damage_player (so reaching 0
	// kills + respawns as usual, cause "automation"); raising just heals.
	// False if `id` isn't a playing connection.
	bool set_player_health(core::NetId id, float value);
	// Jumps the day/night clock and tells every client immediately.
	void set_time_of_day(std::uint32_t ticks);
	// Closes a playing connection with `reason`.
	bool kick_player(core::NetId id, std::string_view reason);
	// Simulates `seconds` of tick time passing for the player's periodic re-auth
	// (auth.md §5.6): subtracts it from the timer until the next request and, while
	// a request is outstanding, from its grace countdown. The next tick then runs the
	// normal system_reauth path. False if `id` isn't a playing, signed-in connection.
	bool advance_reauth(core::NetId id, double seconds);
#endif

	// Entity-management follow-up (held item / hotbar selection, Phase
	// 6.20): the latest `InputCmd::selected_slot` reported by a playing
	// connection (post `vb.on("player_input", ...)` override, if any), or 0
	// if `id` isn't a playing connection. Meaningless on its own -- a
	// script host resolves it against that player's actual inventory
	// (`PlayerHandle::get_held_item()`).
	std::uint8_t selected_slot(core::NetId id) const;

	// Phase 4.2 (Lua entity/player API): directly set a connected player's
	// authoritative velocity. No-op if `id` isn't a playing connection.
	void set_player_velocity(core::NetId id, core::Vec3d vel);

	// Transport-level connection for a playing net id (kInvalid if not
	// found/not playing) — lets a script host send arbitrary framed messages
	// (chat / open_ui) without ServerSession knowing their contents.
	ConnId conn_for_player(core::NetId id) const;

	// Phase 6.17: performs a block edit exactly as if it had arrived as a
	// real C2S_BlockEdit from `editor`, without a wire frame -- lets a script
	// host implement its own breaking/placing policy (e.g. content/base's
	// Lua hold-to-break; the engine no longer has a built-in one) while
	// reusing the one validated pipeline (reach check, block-edit hooks,
	// drops, relight, delta fan-out to every mirroring client, including
	// the editor's own). Returns the resulting S2C_BlockEditResult's
	// `accepted` flag; false (no-op) without a WorldReplicator attached.
	bool apply_script_block_edit(core::NetId editor,
			protocol::BlockEditAction action, core::IVec3 pos,
			core::BlockId block = core::BlockId::kAir);

	// Display name of a playing net id ("" if not found/not playing).
	std::string_view player_name(core::NetId id) const;
	// Verified identity of a playing player; null when the server isn't
	// authenticating (or the player is gone).
	std::shared_ptr<const LoginData> player_login(core::NetId id) const;

	// Optional: attach world replication (chunk streaming). Without it the
	// session only replicates entities.
	void set_world_replicator(std::unique_ptr<WorldReplicator> replicator) {
		replicator_ = std::move(replicator);
	}
	WorldReplicator *world_replicator() { return replicator_.get(); }

	// Phase 4.5: routes a playing connection's C2S_UiEvent up to a script
	// host, without ServerSession knowing anything about Lua. Unset (the
	// default) leaves UI events silently ignored, same posture as the
	// "unknown post-join message" comment this replaces for kC2SUiEvent.
	void set_ui_event_handler(
			std::function<void(core::NetId, const protocol::C2SUiEvent &)> handler) {
		on_ui_event_ = std::move(handler);
	}

	// Phase 6.10: veto-or-replace shape mirroring PlayerInputOverride/
	// InputHookResult below -- a pack can suppress a chat line outright
	// (`veto`) or rewrite its text (`replacement_text`, e.g. profanity
	// filtering or custom formatting) before ServerSession broadcasts it.
	struct ChatHookResult {
		bool veto = false;
		std::optional<std::string> replacement_text;
	};

	// Phase 5.4/6.10: routes a playing connection's C2S_Chat up to a script
	// host before ServerSession broadcasts it. Unset (the default -- e.g.
	// `--singleplayer`, which has no PackRuntime) means every chat line is
	// broadcast unchanged, same "no handler, no side effect" posture as
	// set_input_handler.
	void set_chat_handler(
			std::function<ChatHookResult(core::NetId, std::string_view)> handler) {
		on_chat_ = std::move(handler);
	}

	// Death/respawn (spec §5.4): a player whose feet fall below this world Y
	// (the "void") is killed instantly and respawned at their spawn point.
	// Health also generically triggers a respawn at 0 -- nothing decrements
	// it yet besides the void check (no combat system exists), but the path
	// is shared so a future damage source gets respawn for free.
	void set_void_kill_y(double y) { void_kill_y_ = y; }

	// Per-IP connection cap (§8.3 hardening, tracked in REMAINING_TASKS.md's
	// 1.3): `0` (default) = unlimited, matching every other optional policy
	// knob in this class. Enforced in tick()'s kConnected handling, using
	// Transport::remote_address() -- LoopbackTransport always returns
	// nullopt there (no real network identity in-process), so this is
	// effectively a no-op over loopback/singleplayer regardless of the
	// configured value; it only bites over a real GnsTransport.
	void set_max_connections_per_ip(int n) { max_connections_per_ip_ = n; }
	// External auth (auth.md §8): at most `n` sign-in attempts (C2S_Auth) per
	// peer IP per minute, so a client cannot use the server as a free token-
	// verification/JWKS-refresh oracle. `0` = unlimited. Like the connection
	// cap it needs Transport::remote_address(), so it only bites on a real
	// network transport. Default 30.
	void set_max_auth_attempts_per_minute_per_ip(int n) { max_auth_attempts_per_minute_ = n; }

	// Per-connection message-rate flood guard (§8.3 hardening, tracked in
	// REMAINING_TASKS' "per-player rate limit / flood guard" item): defense
	// in depth on top of the closed-schema caps that already exist per
	// message type (C2SInputBatch::kMaxCmds, the fixed-width keybind
	// bitset) -- those bound how much damage *one* message can do, not how
	// *often* a connection can send messages at all. A token bucket per
	// playing connection, refilled by `n` tokens/second (capacity == `n`,
	// so up to one second's worth of burst is tolerated) in
	// system_network_io()'s own per-tick refill step; a message arriving
	// with an empty bucket is silently dropped (not disconnected -- a
	// transient burst, e.g. a lag spike replaying a backlog, shouldn't cost
	// a real player their connection) rather than processed. Applies
	// generically to every post-join C2S message type alike (input batch,
	// block edit, chat, UI event, block-break begin/stop) -- one token per
	// message regardless of type, since the goal is bounding total
	// dispatch/bandwidth cost per connection, not policing any one message
	// kind. `0` (default) = unlimited, matching every other optional policy
	// knob in this class; set before players join.
	void set_max_messages_per_second(double n) { max_messages_per_second_ = n; }

	// Phase 6.6: generic damage primitive -- the only way to reduce a
	// player's health besides the void-kill check above. `cause` is opaque
	// to the engine (e.g. "fall", "pvp", "void") and threaded through
	// unchanged to the respawn handler below. No-op if `id` isn't a playing
	// connection or is already at 0 health awaiting this tick's respawn.
	void damage_player(core::NetId id, float amount, std::string_view cause);

	// Authoritative feet position `id` was granted at join (spec §8.3's
	// JoinGrant::spawn_pos) -- the same fixed point respawns used to reuse
	// forever before the respawn handler below existed. A pack's respawn
	// handler can read this as a default respawn point, or ignore it
	// entirely for its own checkpoint/bed logic. {} if `id` isn't playing.
	core::Vec3d spawn_point(core::NetId id) const;

	// Phase 6.6: what to do once a player's health reaches 0, decided by the
	// respawn handler below rather than hardcoded. Inventory-drop and any
	// other side effect is the handler's own business (ServerSession has no
	// concept of inventory) -- it gets the full decision here, but the
	// *doing* of a drop, if any, happens on the handler's side before it
	// returns.
	struct RespawnDecision {
		float heal_to = 0.0f;
		core::Vec3d pos{};
		std::string message; // sent as a private S2C_Chat line if non-empty
	};

	// Fires once per respawn (health reaching 0, for any cause -- void-kill
	// included) with (player, cause, health_before), and decides the new
	// health/position/message. Unset (the default -- e.g. `--singleplayer`
	// before a PackRuntime attaches one) falls back to the original
	// behavior: full heal, teleport to the join spawn point, fixed message
	// -- so every pre-6.6 caller/test is unaffected.
	void set_respawn_handler(std::function<RespawnDecision(
					core::NetId, std::string_view, float)>
					handler) {
		on_respawn_ = std::move(handler);
	}

	// Phase 6.3 (vb.on("player_input", handler)): a handler-chosen override
	// for one InputCmd's move/yaw/pitch/buttons, applied before movement
	// integration. `veto` drops this cmd's effect on movement/rotation
	// entirely (its seq is still consumed/acked, so it isn't reprocessed
	// forever); `replacement`, if set, replaces the cmd used for this tick.
	struct PlayerInputOverride {
		core::Vec3f move;
		float yaw = 0.0f;
		float pitch = 0.0f;
		std::uint8_t buttons = 0;
		std::uint32_t keybinds = 0;
		std::uint8_t selected_slot = 0;
	};
	struct InputHookResult {
		bool veto = false;
		std::optional<PlayerInputOverride> replacement;
	};

	// Unset (the default -- e.g. `--singleplayer` before a PackRuntime
	// attaches one) means every InputCmd passes through unchanged, same
	// "no handler, no side effect" posture as set_chat_handler.
	void set_input_handler(std::function<InputHookResult(
					core::NetId, const protocol::InputCmd &)>
					handler) {
		on_input_ = std::move(handler);
	}

	// Dropped-item entities (spec §5.1). Spawns one at `pos`, replicated
	// generically through the interest grid like any other entity -- no
	// dedicated wire message. Auto-collected when a player's feet come
	// within the system's pickup radius (see ItemDropSystem); the pickup
	// handler below is how that reaches a player's actual inventory.
	core::NetId spawn_item_drop(
			core::Vec3d pos, core::BlockId item, std::uint16_t count);

	// Entity-management follow-up: lets a pack-registered `vb.register_entity`
	// kind stand in, cosmetically, for a player or a dropped item -- neither
	// goes through vb.world.spawn (players are joined via the handshake,
	// drops via spawn_item_drop() above), so neither ever gets a real
	// EntityKindId to look up width/height/visual by unless one is set here.
	// Unset (the default) means players replicate with
	// EntityKindId::kInvalid and drops with the reserved world::kItemDropKind
	// sentinel, exactly as before either setter existed.
	void set_player_visual_kind(core::EntityKindId kind) {
		player_visual_kind_ = kind;
	}
	void set_item_drop_visual_kind(core::EntityKindId kind) {
		item_drop_visual_kind_ = kind;
	}

	// Phase 5.1: routes a player walking over a dropped item up to a script
	// host's inventory, without ServerSession knowing anything about Lua.
	// Unset (the default -- e.g. `--singleplayer`, which has no PackRuntime)
	// means picked-up items vanish with no effect, same "no handler, no
	// side effect" posture as set_chat_handler/set_ui_event_handler.
	void set_item_pickup_handler(
			std::function<void(core::NetId, core::BlockId, std::uint16_t)> handler) {
		on_item_pickup_ = std::move(handler);
	}

	// Phase 6.5 (spec §10.7): shared block-damage breaking hooks. Unset
	// fields mean the corresponding half is a no-op -- e.g. no `begin` means
	// every C2S_BlockBreakBegin is rejected outright (no pack attached), and
	// no `tick_damage` means damage never accrues even for an accepted
	// begin, matching "the engine ships zero built-in policy" (§10.7).
	struct BlockBreakHooks {
		std::function<bool(core::NetId, core::IVec3, core::BlockId)> begin;
		std::function<float(
				core::NetId, core::IVec3, core::BlockId, std::uint16_t)>
				tick_damage;
		std::function<std::optional<float>(
				core::IVec3, core::BlockId, float, std::uint16_t, std::uint64_t)>
				health_tick;
	};
	void set_block_break_hooks(BlockBreakHooks hooks) {
		block_break_hooks_ = std::move(hooks);
	}

	// Phase 7.3 (spec: REMAINING_TASKS' "generic region-enter/exit hook"):
	// fires once per player per crossing of a `BlockType::region` block's
	// boundary, not per-tick -- water is the first block to opt in, but this
	// is a generic occupancy tracker, not a liquid-specific one. Unset
	// fields (or both unset) mean update_region_occupancy() below is a no-op
	// -- same "no handler installed, zero side effect and zero extra
	// per-tick cost" posture as BlockBreakHooks above.
	struct RegionHooks {
		std::function<void(core::NetId, core::IVec3, core::BlockId)> enter;
		std::function<void(core::NetId, core::IVec3, core::BlockId)> exit;
	};
	void set_region_hooks(RegionHooks hooks) { region_hooks_ = std::move(hooks); }

	// Phase 6.22 (REMAINING_TASKS' "no fall damage" gap): fires once per
	// player exactly on the tick their vertical fall is arrested by hitting
	// ground (on_ground flips false -> true with a downward velocity), never
	// per-tick while airborne or while already grounded. `impact_speed` is
	// the player's downward speed (m/s, always > 0) the instant before
	// landing -- the engine computes and reports this raw value only; it
	// ships zero built-in fall-damage policy (no threshold, no formula), same
	// "mechanism, not policy" posture as BlockBreakHooks/RegionHooks above.
	// Unset means landing is simply never reported -- e.g. no PackRuntime
	// attached.
	void set_landed_hook(std::function<void(core::NetId, double)> handler) {
		on_landed_ = std::move(handler);
	}

	// Formalizes REMAINING_TASKS' Phase 4 "EntityKind tick/spawn/hit/death
	// callbacks wired into real systems" gap now that SystemRunner (Phase
	// 3.1) actually exists: PackRuntime::dispatch_tick (global `tick` event +
	// entity on_tick/on_hit/on_death dispatch + timers) used to be called as
	// a separate step in each embedder's own loop, strictly *after*
	// session.tick() had already run every SystemRunner phase for that tick
	// (server/main.cpp, client/main.cpp's Singleplayer::tick) -- meaning a
	// script-driven entity move only reached sync_interest/broadcast_snapshots
	// one tick late. This hook lets build_systems() run it as a real,
	// ordered phase instead, same "ServerSession has no idea this is
	// Lua-backed" decoupling as spawn_script_entity below -- PackRuntime
	// wires it in attach_session(). Unset means no script runtime is
	// attached (e.g. a test building a bare ServerSession), same "unset = no
	// side effect" posture as every other hook here.
	void set_script_tick_hook(std::function<void(double)> handler) {
		on_script_tick_ = std::move(handler);
	}

	// Phase 6.1 (vb.register_entity / vb.world.spawn): a generic Lua-kind
	// entity, replicated the exact same way spawn_item_drop's entries are --
	// no dedicated wire message, just another interest-grid entry keyed by a
	// NetId from its own id range (disjoint from both players, which start at
	// 1, and item drops, which start at 0x8000'0000 -- see item_drops.hpp).
	// ServerSession has no idea these are Lua-backed; PackRuntime owns the
	// per-instance `self` table and on_spawn/on_tick/on_hit/on_death
	// dispatch entirely on its own side.
	core::NetId spawn_script_entity(core::EntityKindId kind, core::Vec3d pos);
	void set_script_entity_state(core::NetId id, core::Vec3d pos,
			core::Vec2f rot = {}, core::Vec3f vel = {});
	void remove_script_entity(core::NetId id);
	// Current server-side position of a script entity (moved by
	// update_attachments() while attached); nullopt for an unknown id.
	std::optional<core::Vec3d> script_entity_pos(core::NetId id) const;

	// Entity-management follow-up (spec architecture_spec/rendering.md
	// §11.3's "Per-instance override"): `vb.world.spawn`'s `visual_override`
	// option. `nullopt` clears it (no-op if never set). Attached to the one
	// S2C_EntitySnapshot record a given observing client receives when this
	// NetId first enters their interest set (protocol::EntityRecord::
	// visual_override, see ServerSession::to_record/broadcast_snapshots) --
	// fixed for the entity's whole replicated lifetime, same as `kind`; there
	// is no path yet to change it after a client has already seen the entity
	// (deliberately out of scope for this pass, see REMAINING_TASKS.md).
	void set_script_entity_visual_override(
			core::NetId id, std::optional<protocol::EntityVisualOverride> override_def);

	// Pack-set, changeable properties of a script entity, replicated in
	// S2C_EntityProps (not the per-tick snapshot): broadcast_snapshots() sends
	// all of them to each client the entity enters the interest set of, and
	// each change (or removal) to every client that already sees it, on the
	// next tick. `nullopt` clears the property (no-op if it was never set).
	//
	// The world-space text label (`vb.world.spawn`'s `text`, entity:set_text),
	// already fully resolved by PackRuntime.
	void set_script_entity_text(
			core::NetId id, std::optional<protocol::EntityText> text);
	// entity:set_clip(name): the animation clip every client plays for this
	// entity instead of the one it would pick from velocity/flags.
	void set_script_entity_clip(core::NetId id, std::optional<std::string> clip);
	// entity:attach_to(parent, ...): update_attachments() moves the entity to
	// its parent's position + offset every tick (clients do the same per
	// frame from the parent's interpolated position). When the parent is
	// gone, the orphan handler below runs (or the entity is removed if none
	// is set).
	void set_script_entity_attachment(
			core::NetId id, std::optional<protocol::EntityAttachment> attachment);
	// PackRuntime despawns an entity whose attachment parent disappeared
	// through this, so the pack's on_death and bookkeeping run as usual.
	void set_on_script_entity_orphaned(std::function<void(core::NetId)> fn) {
		on_script_entity_orphaned_ = std::move(fn);
	}

	// Phase 6.18 (Growtopia-style combat): tunables for punch() below. One
	// discrete swing per call -- edge-triggering (only calling punch() on a
	// rising "attack key" edge, not every tick it's held) is entirely the
	// caller's job, same posture as every other "engine ships a default,
	// pack can override individual fields" knob (vb.physics.set_params,
	// 6.7). Global, not per-entity-kind, since no entity kind besides the
	// player throws punches today.
	struct PunchParams {
		// Phase 6.21: reach moved out to the shared ActionParams::reach
		// (world_replicator.hpp) -- punch() reads world_replicator()->reach()
		// so combat reach and block-edit reach are always the same one
		// pack-overridable value (vb.action.set_params{reach=...}), not two
		// independently-overridable numbers that could drift apart.
		float hit_radius = 0.6f; // capsule radius around a player's torso point
		float player_damage = 1.0f; // PvP damage per punch landing on a player
		// Self-heal (spec-equivalent to 6.5's BlockDamageSystem heal hook, but
		// a built-in engine default here instead of "zero policy until a pack
		// supplies one" -- punching has no begin/stop lifecycle for a pack to
		// hang a heal policy off of, so the engine ships one directly).
		// A block with no punches landed on it idles indefinitely; once one
		// exists, `heal_after_seconds` of no *new* punches lets it start
		// healing, then it loses one punch every `heal_interval_seconds`
		// until back to 0 (fully repaired) or hit again (resets both timers).
		// A negative `heal_after_seconds` disables healing entirely -- punch
		// counts then only ever go away by actually breaking the block.
		double heal_after_seconds = 4.0;
		double heal_interval_seconds = 1.5;
		// REMAINING_TASKS.md's "no punch-rate cooldown enforced engine-side"
		// gap: minimum real time between two punch() calls from the same
		// puncher that actually resolve to a swing (armed the instant a call
		// clears the previous cooldown, regardless of whether that swing hits
		// anything -- a whiff still costs the same swing time a landed hit
		// would, matching how a real attack-rate cap works). A call still on
		// cooldown is a silent no-op (empty PunchResult), the same shape as
		// "nothing within reach" below -- not a disconnect/flood-guard
		// concern, just a gameplay cap. `<= 0.0` (the default) disables this
		// entirely, so every existing direct-punch() call site/test is
		// unaffected unless a pack opts in via vb.combat.set_params.
		double punch_cooldown_seconds = 0.0;
	};
	void set_punch_params(PunchParams p) { punch_params_ = p; }
	const PunchParams &punch_params() const { return punch_params_; }

	struct PunchResult {
		bool hit_player = false;
		core::NetId target = core::NetId::kInvalid; // valid iff hit_player
		bool hit_block = false;
		core::IVec3 block_pos{}; // valid iff hit_block
		std::uint16_t block_punches = 0; // accumulated hits, iff hit_block
		bool block_broken = false; // iff hit_block and this punch broke it
	};

	// Resolves one discrete punch from `puncher`: raycasts blocks and nearby
	// players along `puncher`'s current look direction (authoritative
	// yaw/pitch + eye position, not anything client-reported) and picks
	// whichever is closer -- "hit whatever's directly in front of you,"
	// same as the real client's own crosshair raycast, just without a
	// camera object. A player hit applies instant PvP damage
	// (damage_player()), unaffected by `block_damage` -- that knob is
	// mining-only (REMAINING_TASKS.md's "vary break time by block/tool"
	// gap); PvP's own balance stays entirely `vb.combat.set_params`'s
	// `player_damage`. A block hit increments a sparse per-position punch
	// counter by `block_damage` (default 1, matching every pre-existing
	// call site/test) and, once it reaches the target's
	// BlockType::max_damage (0 = break on the very first punch, unaffected
	// by `block_damage`, same "unset" meaning it always had), commits the
	// break through apply_script_block_edit() below -- the exact same
	// validated pipeline (reach check, hooks, drops, relight, fan-out) a
	// real C2S_BlockEdit uses. The engine has no notion of "tools" or
	// per-block hardness beyond max_damage itself -- a pack decides
	// `block_damage` however it likes (e.g. looking up the puncher's
	// currently held item), keeping the "generic primitive, not a
	// game-specific concept" posture every other combat knob here already
	// has. Returns a default/empty PunchResult (both `hit_player`/
	// `hit_block` false) if `puncher` isn't a playing connection or
	// nothing is within reach.
	PunchResult punch(core::NetId puncher, std::uint16_t block_damage = 1);

	// REMAINING_TASKS.md's "hunger has no primitive at all yet" gap.
	// Mechanism, not policy, same split as fall damage (6.22)/PvP: the
	// engine owns a per-player Hunger stat and its own real-time decay (no
	// begin/stop lifecycle for a pack to hang a decay policy off of, the
	// same reasoning punch()'s built-in self-heal default already used),
	// but ships every number at a value that changes nothing unless a pack
	// opts in.
	struct HungerParams {
		// Hunger points lost per real second. `<= 0.0` (the default)
		// disables hunger decay entirely -- every existing join/tick
		// call site is unaffected unless a pack calls
		// vb.hunger.set_params, matching PunchParams::punch_cooldown_seconds's
		// own "0 disables" posture.
		float decay_per_second = 0.0f;
		// Damage per real second applied (cause "hunger") while hunger is
		// at 0. `<= 0.0` (the default) means hunger can bottom out with no
		// starvation consequence at all -- a pack that only wants a hunger
		// *stat* (e.g. to gate sprinting) without starvation damage can set
		// decay_per_second alone.
		float starvation_damage_per_second = 0.0f;
	};
	void set_hunger_params(HungerParams p) { hunger_params_ = p; }
	const HungerParams &hunger_params() const { return hunger_params_; }

	// nullopt if `id` isn't a currently-playing connection (same shape as
	// player_move_state()).
	std::optional<float> player_hunger(core::NetId id) const;
	// Clamped to [0, Hunger::max]; a negative `amount` is a deliberate way
	// for a pack to spend hunger directly (e.g. sprint cost) without
	// waiting on decay_per_second. No-op if `id` isn't playing.
	void add_player_hunger(core::NetId id, float amount);

private:
	struct Conn {
		explicit Conn(ServerHandshake hs) : handshake(std::move(hs)) {}
		ServerHandshake handshake;
		// Set once, from Transport::remote_address() at kConnected time --
		// nullopt over LoopbackTransport (no real network identity), a real
		// IP string over GnsTransport. Used only for the per-IP connection
		// cap (set_max_connections_per_ip).
		std::optional<std::string> remote_address;
		// set_max_messages_per_second's token bucket. Seeded to a full bucket
		// at kConnected (a fresh connection starts with a full burst
		// allowance, not an empty one it has to wait a second to fill).
		// Meaningless (never drained) while max_messages_per_second_ == 0.
		double msg_tokens = 0.0;
		double age = 0.0;
		// PunchParams::punch_cooldown_seconds' per-puncher timer, counted down
		// once per tick in system_network_io() (mirrors msg_tokens' own
		// per-tick refill loop just above it). Meaningless while
		// punch_cooldown_seconds <= 0.0 (never armed, so never checked).
		double punch_cooldown_remaining = 0.0;
		bool playing = false;
		bool input_driven = false;
		core::NetId net_id = core::NetId::kInvalid;
		// The player's ecs::Position/Velocity/Rotation/Collider/PlayerInput/
		// Health/PlayerTag/NetReplicated components live in `registry_` -- see
		// the comment there. Only valid once `playing` (created on join
		// completion, destroyed on disconnect); entt::null until then.
		entt::entity entity{ entt::null };
		std::vector<core::NetId> last_visible;
		core::Vec3d spawn_pos{};
		// Set by apply_damage() the instant health reaches 0; consumed and
		// cleared by check_respawns() when it calls the respawn handler.
		std::string death_cause;
		float death_health_before = 0.0f;
		// Last S2C_PlayerStatus sent to this player; nullopt until the first
		// one (sent the tick after join), then re-sent only on change.
		std::optional<protocol::S2CPlayerStatus> last_status;
		// External auth: the live login (replaced by each successful re-auth)
		// and the periodic re-auth exchange (auth.md §5.6).
		std::shared_ptr<const LoginData> login;
		struct Reauth {
			double timer = 0.0; // seconds until the next S2C_ReauthRequest
			bool pending = false; // a request is outstanding
			double remaining = 0.0; // grace seconds left to answer it
			std::string nonce;
			std::int64_t sent_at = 0; // unix seconds when the request went out
			AuthTicket ticket; // verification of the C2S_Reauth, once received
		} reauth;
	};

	void drop(ConnId conn, const std::string &reason);
	const world::BlockSolidQuery &world_query() const;
	void handle_input_batch(Conn &conn, const protocol::C2SInputBatch &batch);
	void handle_block_edit(ConnId conn, Conn &state,
			const protocol::Frame &frame);
	void handle_chat(Conn &state, const protocol::Frame &frame);
	void handle_block_break_begin(ConnId conn, Conn &state,
			const protocol::Frame &frame);
	void handle_block_break_stop(Conn &state, const protocol::Frame &frame);
	void apply_damage(Conn &state, float amount, std::string_view cause);
	void update_hunger(double dt_seconds);
	void check_respawns();
	// Sends S2C_PlayerStatus to each playing player whose health/hunger
	// changed since the last one sent.
	void sync_player_status();
	void update_item_drops(double dt_seconds);
	void update_block_damage();
	void update_block_punch_healing(double dt_seconds);
	void update_region_occupancy();
	// Phase 6.5's deferred half (closed 2026-09-27): sends S2C_BlockDamage
	// to every playing connection that currently mirrors the chunk containing
	// `pos` (WorldReplicator::player_has_chunk), not just whoever's punching
	// it -- called from punch() on every punch-count change and from
	// update_block_punch_healing() on every heal step. `punches == 0` covers
	// both "healed back to full" and "just broke" alike.
	void broadcast_block_damage(core::IVec3 pos, std::uint16_t punches);
	void broadcast_snapshots();
	void broadcast_world();
	void broadcast_time_of_day();

	// SystemRunner (spec §6/§7.2): named, ordered phases of tick(), built
	// once on first tick() call. system_network_io/system_handshake_timeouts/
	// system_advance_time_of_day just give the equivalent former inline
	// tick() blocks a name+slot; system_sync_interest is the one genuinely
	// new system (see its definition).
	void build_systems();
	void system_network_io(double dt_seconds);
	void system_handshake_timeouts(double dt_seconds);
	void kick_duplicate_login(ConnId newcomer);
	void system_reauth(double dt_seconds);
	void finish_reauth(ConnId conn, Conn &state, const AuthOutcome &outcome);
	double reauth_interval_for(core::NetId id) const;
	void kick_with_message(ConnId conn, protocol::DisconnectReason reason,
			const std::string &message);
	void system_advance_time_of_day(double dt_seconds);
	void system_sync_interest();

	Transport &transport_;
	HandshakeServerConfig config_;
	HandshakeServerHost host_;
	// One entity per playing connection (spec §7.1's base components --
	// Position/Velocity/Rotation/Collider/PlayerInput/Health/PlayerTag/
	// NetReplicated), created in tick()'s join-completion handling and
	// destroyed on disconnect, plus one entity per spawned script entity
	// (Position/EntityKind/NetReplicated, see spawn_script_entity). Iterated
	// generically by system_sync_interest() via SystemRunner (systems_,
	// below) -- see ARCHITECTURE_SPEC.md §6/§7.2.
	entt::registry registry_;
	ecs::SystemRunner systems_;
	// spawn_script_entity()'s NetId -> registry entity, so
	// set_script_entity_state()/remove_script_entity() can find the entity
	// again by the id PackRuntime already tracks its Lua-side state under.
	std::unordered_map<core::NetId, entt::entity> script_entities_;
	// set_script_entity_visual_override()'s storage -- looked up by
	// broadcast_snapshots() only when building an `entered` record (see
	// to_record()); absent means the entity never set one.
	std::unordered_map<core::NetId, protocol::EntityVisualOverride>
			script_entity_visual_overrides_;
	// set_script_entity_text()/_clip()/_attachment()'s storage (an entity
	// with none of them has no entry), plus which properties of which ids
	// changed since the last broadcast_snapshots() (which sends and clears
	// them; protocol::EntityPropField bits).
	struct ScriptEntityProps {
		std::optional<protocol::EntityText> text;
		std::optional<std::string> clip;
		std::optional<protocol::EntityAttachment> attach;

		bool empty() const { return !text && !clip && !attach; }
	};
	std::unordered_map<core::NetId, ScriptEntityProps> script_entity_props_;
	std::unordered_map<core::NetId, std::uint8_t> dirty_entity_props_;
	std::function<void(core::NetId)> on_script_entity_orphaned_;
	template <typename T>
	void set_script_entity_prop(core::NetId id, std::optional<T> ScriptEntityProps::*field,
			std::optional<T> value, std::uint8_t bit);
	void update_attachments();
	std::map<ConnId, Conn> conns_;
	replication::InterestGrid interest_;
	std::unique_ptr<WorldReplicator> replicator_;
	std::function<void(core::NetId, const protocol::C2SUiEvent &)> on_ui_event_;
	std::function<ChatHookResult(core::NetId, std::string_view)> on_chat_;
	world::ItemDropSystem item_drops_;
	std::function<void(core::NetId, core::BlockId, std::uint16_t)> on_item_pickup_;
	world::BlockDamageSystem block_damage_;
	BlockBreakHooks block_break_hooks_;
	std::function<void(core::NetId, double)> on_landed_;
	std::function<void(double)> on_script_tick_;
	// Phase 7.3: last-known region-block occupancy per playing net id --
	// absent = "not currently inside a region block". Compared each tick in
	// update_region_occupancy() to fire enter/exit exactly on the crossing,
	// not every tick spent inside one.
	std::unordered_map<core::NetId, std::pair<core::IVec3, core::BlockId>>
			region_occupancy_;
	RegionHooks region_hooks_;
	// Entity-management follow-up: see set_player_visual_kind()/
	// set_item_drop_visual_kind() above. nullopt means "no pack opted in" --
	// join and spawn_item_drop() fall back to their pre-existing default kind
	// values exactly as before either setter existed.
	std::optional<core::EntityKindId> player_visual_kind_;
	std::optional<core::EntityKindId> item_drop_visual_kind_;
	// Phase 6.18: sparse pos -> punch/heal state, distinct from
	// world::BlockDamageSystem above -- that system's begin/tick/stop
	// lifecycle models a *held*, continuous action across many ticks (5.2's
	// original hold-to-break); a punch is one atomic event with no "holding"
	// concept at all, so it needs no begin/stop, just "add one, check the
	// threshold" plus this struct's own idle-based self-heal. An entry is
	// removed the instant it breaks (apply_script_block_edit erases the
	// world entry, this map along with it), heals fully back to 0 (nothing
	// left to track), or never added in the first place for a max_damage ==
	// 0 (instant-break) block.
	struct PunchDamageState {
		std::uint16_t punches = 0;
		double idle_seconds = 0.0; // time since the last punch landed here
		// Seconds accumulated toward the next -1 heal step, only once
		// idle_seconds has crossed PunchParams::heal_after_seconds.
		double heal_progress = 0.0;
	};
	std::unordered_map<core::IVec3, PunchDamageState> block_punch_counts_;
	PunchParams punch_params_;
	HungerParams hunger_params_;
	physics::MoveParams move_params_;
	int interest_radius_cells_ = 2;
	double time_of_day_ticks_ = 0.0;
	double day_length_seconds_ = kDefaultDayLengthSeconds; // 20 real minutes/day
	double time_of_day_broadcast_accum_ = 0.0;
	double void_kill_y_ = -64.0;
	int max_connections_per_ip_ = 0; // 0 = unlimited
	int max_auth_attempts_per_minute_ = 30; // 0 = unlimited
	double uptime_seconds_ = 0.0; // advanced by tick(); the rate limiter's clock
	std::unordered_map<std::string, std::deque<double>> auth_attempts_; // ip -> attempt times
	double max_messages_per_second_ = 0.0; // 0 = unlimited
	std::function<RespawnDecision(core::NetId, std::string_view, float)>
			on_respawn_;
	std::function<InputHookResult(core::NetId, const protocol::InputCmd &)>
			on_input_;
	std::uint32_t server_tick_ = 0;
	std::size_t playing_ = 0;
	std::uint32_t next_net_id_ = 1;
	// Phase 6.1: starts well past any plausible player NetId (1, counting up)
	// and well short of ItemDropSystem's 0x8000'0000 range, so all three id
	// spaces stay disjoint without sharing a counter.
	std::uint32_t next_script_entity_id_ = 0x4000'0000u;
	std::vector<TransportEvent> scratch_;
	std::vector<SessionPlayerJoined> joins_;
	std::vector<SessionPlayerLeft> leaves_;
	std::vector<SessionLoginChanged> login_changes_;
};

// --- client --------------------------------------------------------------

class ClientSession {
public:
	// `conn` is the ConnId returned by Transport::connect(). `cache` is
	// optional (default nullptr): when supplied, ClientHandshake's asset-sync
	// hooks are wired to it for real (Phase 4.4); when null, asset sync
	// behaves as already-synced (ClientHandshake's own no-op defaults).
	ClientSession(Transport &transport, ConnId conn, HandshakeClientConfig config,
			assetsync::ClientAssetCache *cache = nullptr);

	void tick(double dt_seconds);

	ClientHandshakeStatus status() const { return handshake_.status(); }
	bool joined() const {
		return handshake_.status() == ClientHandshakeStatus::kJoined;
	}
	bool failed() const {
		return handshake_.status() == ClientHandshakeStatus::kFailed ||
				!failure_reason_.empty();
	}
	const std::string &failure_reason() const { return failure_reason_; }

	// External auth (auth.md §7). The hook is called with the server's
	// challenge and returns a ticket polled each tick (status() ==
	// kSigningIn while pending). Set before the first tick().
	void set_sign_in_provider(
			std::function<TokenTicket(const protocol::S2CAuthChallenge &)> provider) {
		handshake_.set_sign_in_provider(std::move(provider));
	}
	const std::optional<protocol::S2CAuthChallenge> &auth_challenge() const {
		return handshake_.auth_challenge();
	}
	// Name the server accepted (external auth: the verified name_claim).
	const std::string &resolved_name() const { return handshake_.resolved_name(); }
	// Abort a pending sign-in; the join fails with "sign-in cancelled".
	void cancel_sign_in();
	// Periodic live re-auth (auth.md §5.6): on S2C_ReauthRequest the hook starts
	// a (usually silent) sign-in and returns a ticket; once it yields a token
	// it goes to the server as C2S_Reauth. A ticket that ends in an error just
	// drops the request -- the server kicks when its grace period runs out.
	void set_reauth_provider(
			std::function<TokenTicket(const protocol::S2CReauthRequest &)> provider) {
		reauth_provider_ = std::move(provider);
	}
	bool reauth_pending() const { return static_cast<bool>(reauth_ticket_); }

	const std::optional<protocol::S2CServerInfo> &server_info() const {
		return handshake_.server_info();
	}
	const std::optional<protocol::S2CJoinAccept> &join_accept() const {
		return handshake_.join_accept();
	}

	// Connect-screen byte-progress (spec gap: previously status-text-only).
	// Both 0 when there's no cache (headless/no-op asset sync) or nothing is
	// currently pending -- callers should treat 0/0 as "no progress to show"
	// rather than divide-by-zero into a fraction.
	std::uint64_t asset_sync_total_bytes() const {
		return asset_cache_ != nullptr ? asset_cache_->sync_total_bytes() : 0;
	}
	std::uint64_t asset_sync_received_bytes() const {
		return asset_cache_ != nullptr ? asset_cache_->sync_received_bytes() : 0;
	}

	// Replicated view of other entities (spec §8.4). Updated from every
	// S2C_EntitySnapshot once joined.
	const std::unordered_map<core::NetId, protocol::EntityRecord> &
	remote_entities() const {
		return remote_;
	}
	std::uint32_t last_server_tick() const { return last_server_tick_; }
#if defined(VB_WITH_AUTOMATION)
	// Development-only: transport round-trip time for the automation snapshot (`rtt_ms`).
	std::optional<double> rtt_seconds() const { return transport_.round_trip_time_seconds(conn_); }
#endif

	// Wall-clock estimate of the server's current tick, as a fractional
	// (not just integer last-received) value -- REMAINING_TASKS.md Phase
	// 3's "wall-clock server_time_est" gap. Feeds interpolated_pos()'s own
	// interpolation target so a remote entity keeps easing smoothly toward
	// its latest known sample every render frame, rather than only moving
	// on the frames a new snapshot happens to arrive. `false` before the
	// first snapshot of this session (join not yet complete).
	bool server_time_est_primed() const { return server_time_.primed(); }
	double server_time_est_seconds() const { return server_time_.estimate_seconds(); }

	// --- local-player prediction (spec §8.4) -----------------------------

	void set_move_params(physics::MoveParams p) { move_params_ = p; }
	// Whatever set_move_params() last set -- the engine default until (if
	// ever) a real S2C_MoveParams frame arrives and apply_move_params()
	// overwrites it (Phase 6.7). Callers that keep their own copy of
	// MoveParams for non-prediction purposes (e.g. eye height for the
	// camera) should re-read this rather than assume their local default
	// still matches what the session is actually predicting with.
	const physics::MoveParams &move_params() const { return move_params_; }
	// Seed the predicted state from S2C_JoinAccept spawn_pos.
	void set_local_feet(core::Vec3d feet) { predicted_.position = feet; }

	// Sample one input: predict locally against the chunk mirror, keep it in the
	// unacked history, and send a batch of recent commands on lane 4.
	void push_input(const protocol::InputCmd &cmd);

	// --- block editing (spec §5.2) --------------------------------------

	// Optimistically apply an edit to the local chunk mirror, remember it for
	// rollback, and send it. The server confirms with S2C_BlockEditResult and
	// the authoritative S2C_ChunkDelta.
	void push_block_edit(const protocol::C2SBlockEdit &edit);
	std::size_t pending_edit_count() const { return pending_edits_.size(); }

	// Phase 6.5 (spec §10.7): shared block-damage breaking. No optimistic
	// local apply here (unlike push_block_edit) -- there's nothing to predict
	// until the server actually commits the break, which arrives as an
	// ordinary S2C_ChunkDelta through the existing path. A future block-
	// selection UI sends `begin` once per newly-targeted max_damage>0 block
	// and `stop` when released/re-targeted/out of reach.
	void send_block_break_begin(core::IVec3 pos, core::IVec3 face) {
		send_message(transport_, conn_, protocol::C2SBlockBreakBegin{ pos, face });
	}
	void send_block_break_stop(core::IVec3 pos) {
		send_message(transport_, conn_, protocol::C2SBlockBreakStop{ pos });
	}

	const physics::MoveState &predicted_state() const { return predicted_; }
	core::Vec3d predicted_feet() const { return predicted_.position; }
	std::uint32_t last_acked_input_seq() const { return last_acked_seq_; }
	std::size_t unacked_input_count() const { return history_.size(); }

	// Remote entity position for rendering, interpolated at
	// server_time_est - 100 ms (spec §8.4). Falls back to the raw snapshot pos
	// when only one sample is known.
	core::Vec3d interpolated_pos(core::NetId id) const;

	// Replicated chunk mirror (spec §11.2), populated from S2C_Chunk* messages.
	const world::ClientChunkStore &chunk_store() const { return chunks_; }
	world::ClientChunkStore &chunk_store() { return chunks_; }

	// Virtual pack filesystem assembled by the asset-sync cache (Phase 4.4),
	// path -> bytes. Empty if no cache was supplied or sync hasn't finished.
	// Nothing consumes this yet (Lua require / texture loader land later).
	const std::unordered_map<std::string, std::vector<std::byte>> &
	virtual_pack_fs() const;

	// Pack-registered custom keybind names (spec §10.6, Phase 6.3), in
	// registration order == bit position for InputCmd::keybinds. Empty until
	// (and unless) an S2C_KeybindRegistry arrives -- a host that never opts
	// in leaves this empty forever, same "no frame, no behavior change"
	// posture as chunk_store()'s block registry.
	const std::vector<std::string> &registered_keybinds() const {
		return keybind_names_;
	}

	// Pack-registered `vb.register_entity{...}` kinds (entity-management
	// follow-up to Phase 6.1), index == EntityKindId - 1. Empty until (and
	// unless) an S2C_EntityKindRegistry arrives -- a host that never opts in
	// leaves this empty forever, same "no frame, no behavior change" posture
	// as registered_keybinds()/chunk_store()'s block registry.
	const std::vector<protocol::EntityKindRegistryRecord> &
	entity_kind_registry() const {
		return entity_kinds_;
	}

	// Looks up a script entity's registered kind record by EntityRecord::kind.
	// Returns nullptr for core::EntityKindId::kInvalid (every player) or an
	// id with no matching S2C_EntityKindRegistry entry (host never opted in,
	// or a stale id) -- callers fall back to a generic placeholder either way,
	// same "missing = default" posture as every other opt-in registry.
	const protocol::EntityKindRegistryRecord *entity_kind(
			core::EntityKindId id) const {
		if (id == core::EntityKindId::kInvalid) {
			return nullptr;
		}
		const auto index = static_cast<std::size_t>(id) - 1;
		if (index >= entity_kinds_.size()) {
			return nullptr;
		}
		return &entity_kinds_[index];
	}

	// Entity-management follow-up: a script entity's per-instance visual
	// override (`vb.world.spawn`'s `visual_override` option), learned once
	// from the S2C_EntitySnapshot record that first made this NetId visible
	// (protocol::EntityRecord::visual_override is only ever populated on an
	// `entered` record, see ServerSession::to_record) and cached here for the
	// rest of the entity's replicated lifetime. nullptr for any entity that
	// never set one, same "missing = default" posture as entity_kind().
	const protocol::EntityVisualOverride *entity_visual_override(
			core::NetId id) const {
		const auto it = entity_visual_overrides_.find(id);
		return it == entity_visual_overrides_.end() ? nullptr : &it->second;
	}

	// A script entity's S2C_EntityProps properties: its text label, the clip
	// the pack forced with entity:set_clip, and its attachment. nullptr /
	// nullopt when unset. Dropped when the entity leaves this client's
	// interest set.
	const protocol::EntityText *entity_text(core::NetId id) const {
		const auto it = entity_props_.find(id);
		return it == entity_props_.end() || !it->second.text ? nullptr : &*it->second.text;
	}
	const std::string *entity_clip(core::NetId id) const {
		const auto it = entity_props_.find(id);
		return it == entity_props_.end() || !it->second.clip ? nullptr : &*it->second.clip;
	}
	const protocol::EntityAttachment *entity_attachment(core::NetId id) const {
		const auto it = entity_props_.find(id);
		return it == entity_props_.end() || !it->second.attach ? nullptr : &*it->second.attach;
	}

	// The block a replicated dropped-item entity represents (learned from its
	// `entered` record, EntityRecord::item); nullopt for anything else.
	std::optional<core::BlockId> entity_item(core::NetId id) const {
		const auto it = entity_items_.find(id);
		if (it == entity_items_.end()) {
			return std::nullopt;
		}
		return static_cast<core::BlockId>(it->second);
	}

	// --- client UI VM (spec §10.4, Phase 4.5) ---------------------------

	// Drains a pending S2C_OpenUi, if one arrived since the last call.
	std::optional<protocol::S2COpenUi> take_open_ui();

	// Sends one C2S_UiEvent (a UiRuntime widget callback calling
	// ui.send_event/ui.close). No optimistic local state, unlike block
	// edits -- just a pass-through RPC to the server's Lua VM.
	void send_ui_event(const protocol::C2SUiEvent &event) {
		send_message(transport_, conn_, event);
	}

	// --- chat (spec §5.4) -------------------------------------------------

	// Sends one C2S_Chat line typed into the HUD chat box.
	void send_chat(std::string_view text) {
		send_message(transport_, conn_, protocol::C2SChat{ std::string(text) });
	}

	// Drains chat lines (already server-formatted "<name>: <text>") received
	// since the last call, oldest first. Join/leave notices also land here as
	// "* <name> joined/left the game" lines (spec §5.4's "join-leave
	// messages"), interleaved with real chat in receipt order.
	std::vector<std::string> take_chat_messages();

	// Everyone else currently known to be playing (net id -> display name),
	// kept in sync by S2C_PlayerList/S2C_PlayerJoin/S2C_PlayerLeave. Does not
	// include this client's own name.
	const std::unordered_map<core::NetId, std::string> &players() const {
		return players_;
	}

	// The day/night gradient to render with (spec §5.4, Phase 6.8): the
	// engine default until (if ever) a real S2C_DayNightCurve frame arrives
	// and apply_day_night_curve() overwrites it -- same "no frame, no
	// behavior change" posture as move_params()/registered_keybinds().
	const world::DayNightCurve &day_night_curve() const {
		return day_night_curve_;
	}

	// A pack's fog distance override (spec §7.2, Phase 7.2), if a real
	// S2C_FogParams frame ever arrived and apply_fog_params() set it --
	// `nullopt` otherwise, meaning the caller should compute its own default
	// fog distance from its own view_distance config (unlike
	// day_night_curve()/move_params() there's no server-side universal
	// default to fall back on here, so this stays optional rather than
	// defaulting to an empty struct).
	const std::optional<protocol::S2CFogParams> &fog_override() const {
		return fog_override_;
	}

	// Live block-damage replicated by the server (Phase 6.5's deferred half,
	// closed 2026-09-27): pos -> raw punch count, populated by S2C_BlockDamage
	// and only ever containing entries currently taking damage (a pos is
	// erased the instant its punches reach 0, never left at a stale 0 entry).
	// Callers divide by BlockType::max_damage (chunk_store().registry())
	// themselves for a fraction -- same "engine reports raw state,
	// presentation computes the rest" posture break_progress() always had.
	const std::unordered_map<core::IVec3, std::uint16_t> &block_damage() const {
		return block_damage_.punches();
	}

	// This player's inventory (spec §5.1), kept in sync by S2C_Inventory.
	// Empty until the first snapshot arrives (e.g. before any player:give()).
	const std::vector<protocol::InventorySlot> &inventory() const {
		return inventory_;
	}

	// This player's own health/hunger, kept in sync by S2C_PlayerStatus.
	// nullopt until the first one arrives (the tick after joining).
	const std::optional<protocol::S2CPlayerStatus> &player_status() const {
		return player_status_;
	}

	// Day/night cycle (spec §5.4): S2C_JoinAccept's value until the first
	// periodic S2C_TimeOfDay update arrives, then the latest of those. Ticks
	// into the day cycle -- see vb::world::daynight.hpp for the convention.
	std::uint32_t time_of_day() const {
		if (time_of_day_override_) {
			return *time_of_day_override_;
		}
		if (join_accept()) {
			return join_accept()->time_of_day;
		}
		return 0;
	}

private:
	// Handle a post-join gameplay message (snapshot / chunk). Returns true if
	// consumed.
	bool apply_gameplay_frame(const protocol::Frame &frame);
	void apply_block_registry(const protocol::S2CBlockRegistry &msg);
	void apply_keybind_registry(const protocol::S2CKeybindRegistry &msg);
	void apply_entity_kind_registry(const protocol::S2CEntityKindRegistry &msg);
	void apply_move_params(const protocol::S2CMoveParams &msg);
	void apply_day_night_curve(const protocol::S2CDayNightCurve &msg);
	void apply_fog_params(const protocol::S2CFogParams &msg);
	void apply_block_damage(const protocol::S2CBlockDamage &msg);
	void apply_entity_props(const protocol::S2CEntityProps &msg);
	void apply_snapshot(const protocol::S2CEntitySnapshot &snap);
	void reconcile(const protocol::EntityRecord &authoritative,
			std::uint32_t acked_seq);
	void handle_block_edit_result(const protocol::S2CBlockEditResult &res);
	void forget_pending_edits_for(core::ChunkCoord coord);

	struct PendingEdit {
		std::uint32_t seq = 0;
		core::IVec3 pos{};
		core::BlockId prev = core::BlockId::kAir;
		core::ChunkCoord coord{};
	};

	Transport &transport_;
	ConnId conn_;
	ClientHandshake handshake_;
	// Gameplay frames (snapshots, chunks, time of day, ...) that arrived after
	// C2S_Ready but before S2C_JoinAccept. They travel on other lanes with no
	// ordering against JoinAccept, which can be lost and resent; applied in
	// arrival order right after it. Bounded: see kMaxEarlyGameplayFrames.
	std::vector<std::pair<protocol::MessageHeader, std::vector<std::byte>>> early_gameplay_;
	std::function<TokenTicket(const protocol::S2CReauthRequest &)> reauth_provider_;
	TokenTicket reauth_ticket_;
	bool started_ = false;
	std::string failure_reason_;
	std::vector<TransportEvent> scratch_;
	std::unordered_map<core::NetId, protocol::EntityRecord> remote_;
	// Client-side lightweight ECS mirror (spec §6): one entity per remote
	// net id, holding ecs::InterpBuffer (prev/cur sample for render
	// interpolation -- replaces the old bespoke RemoteSample struct) and
	// ecs::EntityKind. remote_ above stays the flat "latest record per net
	// id" view (unchanged public remote_entities() API, still used by
	// entity_renderer.cpp and tests); this registry is the interpolation
	// bookkeeping's real home instead of an ad hoc parallel map.
	entt::registry entity_registry_;
	std::unordered_map<core::NetId, entt::entity> net_to_entity_;
	world::ClientChunkStore chunks_{ world::BlockRegistry::base() };
	std::uint32_t last_server_tick_ = 0;
	ServerTimeEstimator server_time_;
	assetsync::ClientAssetCache *asset_cache_ = nullptr; // not owned; may be null
	std::optional<protocol::S2COpenUi> pending_open_ui_;
	std::vector<std::string> pending_chat_;
	std::unordered_map<core::NetId, std::string> players_;
	std::optional<std::uint32_t> time_of_day_override_;
	std::vector<protocol::InventorySlot> inventory_;
	std::optional<protocol::S2CPlayerStatus> player_status_;
	std::vector<std::string> keybind_names_;
	std::vector<protocol::EntityKindRegistryRecord> entity_kinds_;
	// entity_visual_override()'s storage, populated from EntityRecord::
	// visual_override in apply_snapshot() and erased alongside remote_ on
	// removal.
	std::unordered_map<core::NetId, protocol::EntityVisualOverride>
			entity_visual_overrides_;
	std::unordered_map<core::NetId, std::uint16_t> entity_items_;
	// entity_text()/entity_clip()/entity_attachment()'s storage.
	// `server_tick` is the S2C_EntityProps tick they last changed in: a
	// snapshot removal only drops properties older than itself, since the two
	// travel on different lanes (see apply_snapshot).
	struct ReceivedEntityProps {
		std::optional<protocol::EntityText> text;
		std::optional<std::string> clip;
		std::optional<protocol::EntityAttachment> attach;
		std::uint32_t server_tick = 0;
	};
	std::unordered_map<core::NetId, ReceivedEntityProps> entity_props_;
	world::DayNightCurve day_night_curve_; // empty = default_day_night_curve()
	std::optional<protocol::S2CFogParams> fog_override_;
	BlockDamageTracker block_damage_;

	physics::MoveState predicted_;
	physics::MoveParams move_params_;
	std::vector<protocol::InputCmd> history_; // unacked, ascending seq
	std::uint32_t last_acked_seq_ = 0;
	std::vector<PendingEdit> pending_edits_;
};

} // namespace vb::net
