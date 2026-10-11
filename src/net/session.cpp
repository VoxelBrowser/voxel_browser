#include "vb/net/session.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

#include "vb/core/log.hpp"
#include "vb/core/math.hpp"
#include "vb/protocol/compression.hpp"
#include "vb/protocol/message.hpp"
#include "vb/protocol/world.hpp"
#include "vb/world/block.hpp"
#include "vb/world/daynight.hpp"
#include "vb/world/raycast.hpp"

namespace vb::net {

namespace {

std::span<const std::byte> span_of(const std::vector<std::byte> &v) {
	return { v.data(), v.size() };
}

// Inverse of world::index_of (x fastest, then z, then y).
core::IVec3 world_voxel_of(core::ChunkCoord coord, std::size_t local_index) {
	const auto dim = static_cast<std::size_t>(world::kChunkDim);
	const core::IVec3 origin = core::chunk_origin(coord);
	return { origin.x + static_cast<int>(local_index % dim),
		origin.y + static_cast<int>(local_index / (dim * dim)),
		origin.z + static_cast<int>((local_index / dim) % dim) };
}

// Reverses frame_message()'s optional LZ4 framing (ARCHITECTURE_SPEC.md §18
// Q4) before a message's payload reaches its `decode()`. A no-op (returns
// true, `storage` untouched) when MessageFlag::kCompressed isn't set --
// `frame.payload` already points at the caller's own buffer in that case, and
// is left alone. When it is set, `storage` receives the decompressed bytes
// and `frame.payload` is repointed at it; `storage` must outlive every use of
// `frame.payload` after this call. The sender only ever sets the flag once it
// actually compressed a payload (frame_message()), so any failure here --
// corrupt bytes, or a compressed frame reaching a binary built without
// VB_WITH_COMPRESSION at all -- means something is genuinely wrong, not
// "nothing to do."
bool decompress_frame_payload(protocol::Frame &frame,
		[[maybe_unused]] std::vector<std::byte> &storage, const char *&reason) {
	if ((frame.header.flags &
				static_cast<std::uint16_t>(protocol::MessageFlag::kCompressed)) == 0) {
		return true;
	}
#if VB_WITH_COMPRESSION
	auto decoded = protocol::decompress_lz4(frame.payload);
	if (!decoded) {
		reason = "malformed compressed frame";
		return false;
	}
	storage = std::move(decoded.value());
	frame.payload = storage;
	return true;
#else
	reason = "received a compressed frame but built without VB_WITH_COMPRESSION";
	return false;
#endif
}

// Gameplay frames ClientSession holds between C2S_Ready and a late
// S2C_JoinAccept (see ClientSession::early_gameplay_). A few seconds of
// snapshots and the initial chunk burst fit easily; past this the server is
// misbehaving.
constexpr std::size_t kMaxEarlyGameplayFrames = 8192;

// Per-tick S2C_AssetData send budget (spec §9.3's pacing, simple per-tick
// cap rather than literal byte-in-flight windowing).
constexpr int kAssetSendBudgetPerTick = 4;

// How often S2C_TimeOfDay goes out to already-connected clients (spec §5.4).
// Coarser than snapshots/world state -- the clock only needs to look smooth,
// not be exact every tick.
constexpr double kTimeOfDayBroadcastIntervalSeconds = 1.0;

// Fallback voxel query when no world replicator is attached (entity-only
// sessions and early tests): everything is open air.
struct EmptyBlockQuery final : world::BlockSolidQuery {
	core::BlockId block_at(core::IVec3) const override {
		return core::BlockId::kAir;
	}
	bool solid_at(core::IVec3) const override { return false; }
};
const EmptyBlockQuery kEmptyBlockQuery;

physics::MoveInput to_move_input(const protocol::InputCmd &c) {
	physics::MoveInput mi;
	mi.dt = core::clamp(static_cast<double>(c.dt), 0.0, 0.1);
	mi.wish_dir = physics::wish_dir_from_local(c.move, c.yaw);
	mi.jump = (c.buttons & protocol::kInputJump) != 0;
	mi.sprint = (c.buttons & protocol::kInputSprint) != 0;
	mi.fly_up = (c.buttons & protocol::kInputFlyUp) != 0;
	mi.fly_down = (c.buttons & protocol::kInputFlyDown) != 0;
	return mi;
}

std::uint8_t pack_flags(bool on_ground) {
	return on_ground ? 1u : 0u;
}

void send_frames(Transport &t, ConnId conn,
		const std::vector<OutgoingFrame> &frames) {
	for (const auto &f : frames) {
		t.send(conn, f.lane, span_of(f.bytes));
	}
}

} // namespace

// ===========================================================================
// ServerSession
// ===========================================================================

ServerSession::ServerSession(Transport &transport, HandshakeServerConfig config,
		HandshakeServerHost host) : transport_(transport),
									config_(std::move(config)),
									host_(std::move(host)) {
	// Session owns the authoritative player count and net-id allocation; wrap
	// whatever the caller passed so their auth/spawn hooks still run.
	host_.current_player_count = [this] {
		return static_cast<std::uint32_t>(playing_);
	};
	// External auth: two different people may both be "alex"; the later one
	// becomes "alex#2" (auth.md §5.5). Same-subject sessions are skipped, since
	// the older one is about to be kicked (kick_duplicate_login).
	auto user_resolve = host_.resolve_name;
	host_.resolve_name = [this, user_resolve](std::string_view name,
								 const LoginData &login) {
		const std::string base = user_resolve ? user_resolve(name, login) : std::string(name);
		auto taken = [&](const std::string &candidate) {
			for (const auto &[c, other] : conns_) {
				(void)c;
				const auto &ol = other.handshake.login();
				if (ol && ol->issuer == login.issuer && ol->subject == login.subject) {
					continue;
				}
				if (!other.handshake.player_name().empty() &&
						other.handshake.player_name() == candidate) {
					return true;
				}
			}
			return false;
		};
		if (!taken(base)) {
			return base;
		}
		for (int n = 2; n < 10000; ++n) {
			const std::string suffix = "#" + std::to_string(n);
			std::string stem = base;
			if (stem.size() + suffix.size() > 32) {
				stem.resize(32 - suffix.size());
			}
			std::string candidate = stem + suffix;
			if (!taken(candidate)) {
				return candidate;
			}
		}
		return base;
	};
	// Keep the caller's grant logic, then fill in a net id / seed if it didn't.
	auto user_on_ready = host_.on_ready;
	host_.on_ready = [this, user_on_ready](std::string_view name) {
		JoinGrant grant = user_on_ready ? user_on_ready(name) : JoinGrant{};
		if (grant.net_id == core::NetId::kInvalid) {
			grant.net_id = static_cast<core::NetId>(next_net_id_++);
		}
		if (grant.world_seed == 0) {
			grant.world_seed = config_.world_seed;
		}
		grant.time_of_day = static_cast<std::uint32_t>(time_of_day_ticks_);
		return grant;
	};
}

void ServerSession::drop(ConnId conn, const std::string &reason) {
	auto it = conns_.find(conn);
	if (it == conns_.end()) {
		return;
	}
	transport_.close(conn, reason);
}

const world::BlockSolidQuery &ServerSession::world_query() const {
	if (replicator_) {
		return replicator_->world();
	}
	return kEmptyBlockQuery;
}

void ServerSession::handle_input_batch(Conn &conn,
		const protocol::C2SInputBatch &batch) {
	const world::BlockSolidQuery &world = world_query();
	auto &pos = registry_.get<ecs::Position>(conn.entity);
	auto &vel = registry_.get<ecs::Velocity>(conn.entity);
	auto &rot = registry_.get<ecs::Rotation>(conn.entity);
	auto &collider = registry_.get<ecs::Collider>(conn.entity);
	auto &input = registry_.get<ecs::PlayerInput>(conn.entity);

	physics::MoveState move{ pos.value, vel.value, collider.on_ground };
	for (const auto &cmd : batch.cmds) {
		if (cmd.seq <= input.last_seq) {
			continue; // already simulated (batches resend recent commands)
		}
		// Phase 6.3 (vb.on("player_input", handler)): a pack may veto this
		// cmd's effect on movement/rotation entirely, or replace the values
		// used below. The seq is still consumed either way (see the ack
		// comment above) -- a veto means "this happened, we chose to have it
		// do nothing," not "never received," so client reconciliation still
		// converges instead of replaying it forever.
		protocol::InputCmd effective = cmd;
		if (on_input_) {
			const ServerSession::InputHookResult hook = on_input_(conn.net_id, cmd);
			if (hook.veto) {
				input.last_seq = cmd.seq;
				continue;
			}
			if (hook.replacement) {
				effective.move = hook.replacement->move;
				effective.yaw = hook.replacement->yaw;
				effective.pitch = hook.replacement->pitch;
				effective.buttons = hook.replacement->buttons;
				effective.keybinds = hook.replacement->keybinds;
				effective.selected_slot = hook.replacement->selected_slot;
			}
		}
		// The spawn chunk may still be generating (async worldgen worker) --
		// simulating gravity against unloaded-as-air terrain lets the player
		// free-fall with no collision and end up embedded in the ground the
		// moment it finishes loading. Freeze position/velocity until the
		// column is actually there; still ack the seq so the client doesn't
		// pile up a backlog to replay once it unfreezes.
		if (replicator_ == nullptr ||
				physics::ground_area_loaded(move.position,
						[this](core::ChunkCoord c) {
							return replicator_->world().has_chunk(c);
						})) {
			const bool was_on_ground = move.on_ground;
			// Captured before step_movement integrates this tick's gravity --
			// the actual impact speed differs by at most one tick's worth of
			// gravity (a few hundredths of a second), which fall-damage policy
			// doesn't need exactly; avoids widening MoveState's public
			// contract just to smuggle this one value out.
			const double fall_speed_before = -move.velocity.y;
			move = physics::step_movement(move, to_move_input(effective),
					move_params_, world);
			if (on_landed_ && !was_on_ground && move.on_ground &&
					fall_speed_before > 0.0) {
				on_landed_(conn.net_id, fall_speed_before);
			}
		}
		input.last_seq = cmd.seq;
		input.selected_slot = effective.selected_slot;
		rot.yaw = effective.yaw;
		rot.pitch = effective.pitch;
	}
	pos.value = move.position;
	vel.value = move.velocity;
	collider.on_ground = move.on_ground;
	conn.input_driven = true;

	// Upserted immediately, not deferred to system_sync_interest()'s once-
	// per-tick pass: several other network_io-phase handlers this same tick
	// (handle_block_edit, handle_block_break_begin/stop, punch(), and
	// update_region_occupancy()/update_item_drops() later this tick) read
	// interest_ expecting this player's just-simulated position, not last
	// tick's. system_sync_interest() covers script entities, which have no
	// such synchronous readers.
	replication::EntityState s;
	if (const auto *e = interest_.get(conn.net_id)) {
		s = *e;
	}
	s.net_id = conn.net_id;
	s.pos = pos.value;
	s.rot = { rot.yaw, rot.pitch };
	s.vel = core::Vec3f{ static_cast<float>(vel.value.x),
		static_cast<float>(vel.value.y),
		static_cast<float>(vel.value.z) };
	s.flags = pack_flags(collider.on_ground);
	interest_.upsert(s);
}

void ServerSession::handle_block_edit(ConnId conn, Conn &state,
		const protocol::Frame &frame) {
	if (!replicator_) {
		return;
	}
	auto edit = protocol::C2SBlockEdit::decode(frame.payload);
	if (!edit) {
		return;
	}
	// Read straight from the registry, not interest_: interest_ is only
	// refreshed once per tick (system_sync_interest, after all of this
	// tick's messages are processed) -- a block edit arriving in the same
	// message batch as a fresh input move needs this tick's position, not
	// last tick's stale mirror.
	core::Vec3d eye = registry_.get<ecs::Position>(state.entity).value;
	eye.y += move_params_.eye_height;

	protocol::S2CBlockEditResult result;
	auto per_player =
			replicator_->apply_block_edit(state.net_id, eye, *edit, result);
	send_message(transport_, conn, result);

	for (auto &pf : per_player) {
		for (auto &[other_conn, other] : conns_) {
			if (other.playing && other.net_id == pf.id) {
				send_frames(transport_, other_conn, pf.frames);
				break;
			}
		}
	}
}

bool ServerSession::apply_script_block_edit(core::NetId editor,
		protocol::BlockEditAction action, core::IVec3 pos, core::BlockId block) {
	if (!replicator_) {
		return false;
	}
	core::Vec3d eye{};
	bool found = false;
	for (auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == editor) {
			eye = registry_.get<ecs::Position>(state.entity).value;
			found = true;
			break;
		}
	}
	if (!found) {
		// Not a live player conn -- may be a script entity id; interest_ is
		// still a fine fallback here since script entities don't move
		// mid-tick the way player input does.
		if (const replication::EntityState *e = interest_.get(editor)) {
			eye = e->pos;
		}
	}
	eye.y += move_params_.eye_height;

	protocol::C2SBlockEdit edit;
	edit.action = action;
	edit.pos = pos;
	edit.block = block;

	protocol::S2CBlockEditResult result;
	auto per_player = replicator_->apply_block_edit(editor, eye, edit, result);
	for (auto &[other_conn, other] : conns_) {
		if (other.playing && other.net_id == editor) {
			send_message(transport_, other_conn, result);
			break;
		}
	}
	for (auto &pf : per_player) {
		for (auto &[other_conn, other] : conns_) {
			if (other.playing && other.net_id == pf.id) {
				send_frames(transport_, other_conn, pf.frames);
				break;
			}
		}
	}
	return result.accepted;
}

ServerSession::PunchResult ServerSession::punch(
		core::NetId puncher, std::uint16_t block_damage) {
	PunchResult result;
	entt::entity puncher_entity{ entt::null };
	Conn *puncher_conn = nullptr;
	for (auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == puncher) {
			puncher_entity = state.entity;
			puncher_conn = &state;
			break;
		}
	}
	if (puncher_entity == entt::null) {
		return result;
	}
	if (punch_params_.punch_cooldown_seconds > 0.0 &&
			puncher_conn->punch_cooldown_remaining > 0.0) {
		return result; // still on cooldown -- silent no-op, same as no target
	}
	if (punch_params_.punch_cooldown_seconds > 0.0) {
		// Armed up front, before target resolution: a whiff still costs a
		// swing, matching a real attack-rate cap rather than only throttling
		// punches that happen to land.
		puncher_conn->punch_cooldown_remaining = punch_params_.punch_cooldown_seconds;
	}
	// Read straight from the registry, not interest_ (only refreshed once
	// per tick, after all of this tick's messages -- including this punch,
	// almost always called mid-network_io from a vb.on("player_input")
	// handler -- are processed).
	core::Vec3d eye = registry_.get<ecs::Position>(puncher_entity).value;
	eye.y += move_params_.eye_height;
	const ecs::Rotation &puncher_rot = registry_.get<ecs::Rotation>(puncher_entity);
	const core::Vec3d dir = core::forward_from_yaw_pitch(
			static_cast<double>(puncher_rot.yaw), static_cast<double>(puncher_rot.pitch));
	// Phase 6.21: read the same shared reach WorldReplicator's block-edit path
	// enforces, so punch reach and mining reach are always one value, not two
	// independently pack-overridable numbers. Falls back to ActionParams's own
	// built-in default if somehow called with no world attached (untested
	// configuration -- punch() already no-ops without a replicator below).
	const double reach = replicator_ ? replicator_->reach() : ActionParams{}.reach;

	// Nearest other player modeled as a vertical cylinder (feet at their
	// tracked position, top at move_params_.height above it, radius
	// hit_radius) that the puncher's look-ray passes through, capped at
	// `reach`. A cylinder rather than a single torso point so a punch lands
	// regardless of exact pitch -- aiming anywhere between another player's
	// feet and head at reasonable range should register, the same forgiving
	// hitbox any FPS gives a standing target, not a single point a puncher's
	// eye line has to intersect exactly.
	double best_player_t = reach;
	core::NetId best_player = core::NetId::kInvalid;
	for (auto &[conn, other] : conns_) {
		(void)conn;
		if (!other.playing || other.net_id == puncher) {
			continue;
		}
		const core::Vec3d other_pos = registry_.get<ecs::Position>(other.entity).value;
		const double denom = dir.x * dir.x + dir.z * dir.z;
		double t = 0.0;
		if (denom > 1e-9) {
			t = ((other_pos.x - eye.x) * dir.x + (other_pos.z - eye.z) * dir.z) / denom;
		}
		t = core::clamp(t, 0.0, reach);
		const double px = eye.x + dir.x * t;
		const double py = eye.y + dir.y * t;
		const double pz = eye.z + dir.z * t;
		const double dx = px - other_pos.x;
		const double dz = pz - other_pos.z;
		const double horiz_dist = std::sqrt(dx * dx + dz * dz);
		const double hr = static_cast<double>(punch_params_.hit_radius);
		const bool vertical_ok = py >= other_pos.y - hr &&
				py <= other_pos.y + move_params_.height + hr;
		if (horiz_dist <= hr && vertical_ok && t < best_player_t) {
			best_player_t = t;
			best_player = other.net_id;
		}
	}

	world::VoxelRayHit block_hit;
	double block_t = reach;
	if (replicator_) {
		block_hit =
				world::raycast_voxel(world_query(), eye, dir, reach);
		if (block_hit.hit) {
			const core::Vec3d center{ static_cast<double>(block_hit.voxel.x) + 0.5,
				static_cast<double>(block_hit.voxel.y) + 0.5,
				static_cast<double>(block_hit.voxel.z) + 0.5 };
			block_t = (center - eye).length();
		}
	}

	// Whichever is closer along the ray wins -- a player standing in front
	// of a wall gets hit instead of the wall behind them, and vice versa.
	if (best_player != core::NetId::kInvalid &&
			(!block_hit.hit || best_player_t <= block_t)) {
		result.hit_player = true;
		result.target = best_player;
		damage_player(best_player, punch_params_.player_damage, "pvp");
		return result;
	}

	if (block_hit.hit && replicator_) {
		result.hit_block = true;
		result.block_pos = block_hit.voxel;
		const core::BlockId existing = replicator_->world().get_block(block_hit.voxel);
		const world::BlockRegistry &reg = replicator_->world().registry();
		std::uint16_t max_damage = 0;
		if (reg.contains(existing)) {
			max_damage = reg.get(existing).max_damage;
		}
		if (max_damage == 0) {
			// Unset max_damage always meant "instant break" pre-6.18 (the
			// continuous-hold BlockDamageSystem skipped it outright); any
			// punch keeps that meaning regardless of `block_damage` -- there's
			// no "partial" instant break.
			result.block_punches = block_damage;
			result.block_broken = apply_script_block_edit(
					puncher, protocol::BlockEditAction::kBreak, block_hit.voxel);
		} else {
			PunchDamageState &state = block_punch_counts_[block_hit.voxel];
			state.punches = static_cast<std::uint16_t>(state.punches + block_damage);
			state.idle_seconds = 0.0; // a landed punch resets the heal clock
			state.heal_progress = 0.0;
			result.block_punches = state.punches;
			if (state.punches >= max_damage) {
				if (apply_script_block_edit(puncher,
							protocol::BlockEditAction::kBreak, block_hit.voxel)) {
					block_punch_counts_.erase(block_hit.voxel);
					result.block_broken = true;
					broadcast_block_damage(block_hit.voxel, 0);
				} else {
					// A veto (apply_script_block_edit returned false) leaves the
					// count at max_damage -- the very next punch retries the
					// break rather than needing max_damage+1 hits.
					broadcast_block_damage(block_hit.voxel, state.punches);
				}
			} else {
				broadcast_block_damage(block_hit.voxel, state.punches);
			}
		}
	}
	return result;
}

// Phase 6.18: idle-based self-heal for punch()'s block_punch_counts_ --
// distinct from update_block_damage() above, which drives the unrelated
// continuous-hold BlockDamageSystem (6.5). A block that hasn't been punched
// in PunchParams::heal_after_seconds loses one punch every heal_interval_
// seconds until it's back to full health (erased from the map) or hit
// again (both timers reset in punch() itself). A negative heal_after_
// seconds disables this entirely -- every entry just idles forever.
void ServerSession::broadcast_block_damage(core::IVec3 pos, std::uint16_t punches) {
	if (!replicator_) {
		return;
	}
	const core::ChunkCoord cc = core::chunk_of(pos);
	// The chunk's current revision versions this update against block
	// changes, which travel on another lane (see BlockDamageTracker).
	const world::Chunk *chunk = replicator_->world().find_chunk(cc);
	const protocol::S2CBlockDamage msg{ pos, punches, chunk != nullptr ? chunk->revision() : 0 };
	for (auto &[conn, state] : conns_) {
		if (state.playing && replicator_->player_has_chunk(state.net_id, cc)) {
			send_message(transport_, conn, msg);
		}
	}
}

void ServerSession::update_block_punch_healing(double dt_seconds) {
	if (punch_params_.heal_after_seconds < 0.0) {
		return;
	}
	for (auto it = block_punch_counts_.begin(); it != block_punch_counts_.end();) {
		PunchDamageState &state = it->second;
		const std::uint16_t before = state.punches;
		state.idle_seconds += dt_seconds;
		if (state.idle_seconds >= punch_params_.heal_after_seconds) {
			state.heal_progress += dt_seconds;
			while (state.heal_progress >= punch_params_.heal_interval_seconds &&
					state.punches > 0) {
				state.heal_progress -= punch_params_.heal_interval_seconds;
				--state.punches;
			}
		}
		if (state.punches == 0) {
			broadcast_block_damage(it->first, 0);
			it = block_punch_counts_.erase(it);
		} else {
			if (state.punches != before) {
				broadcast_block_damage(it->first, state.punches);
			}
			++it;
		}
	}
}

// Phase 7.3: per-tick region occupancy, keyed off each playing player's own
// position (same interest_ source update_item_drops reads for "where is this
// net id right now") -- checks the voxel the position itself falls inside,
// not a full AABB (REMAINING_TASKS' open "exact shape" note; a point check is
// enough for water's own collision box to already match visually). Fires
// enter/exit exactly on the crossing by diffing against region_occupancy_,
// never on every tick spent inside one.
void ServerSession::update_region_occupancy() {
	if (!replicator_ || (!region_hooks_.enter && !region_hooks_.exit)) {
		return;
	}
	const world::World &world = replicator_->world();
	const world::BlockRegistry &reg = world.registry();
	for (auto &[conn, state] : conns_) {
		if (!state.playing) {
			continue;
		}
		(void)conn;
		const replication::EntityState *e = interest_.get(state.net_id);
		if (!e) {
			continue;
		}
		const core::IVec3 voxel{ static_cast<int>(std::floor(e->pos.x)),
			static_cast<int>(std::floor(e->pos.y)),
			static_cast<int>(std::floor(e->pos.z)) };
		const core::BlockId block = world.get_block(voxel);
		const bool in_region = reg.contains(block) && reg.is_region(block);

		auto it = region_occupancy_.find(state.net_id);
		const bool was_in_region = it != region_occupancy_.end();
		if (in_region && !was_in_region) {
			region_occupancy_[state.net_id] = { voxel, block };
			if (region_hooks_.enter) {
				region_hooks_.enter(state.net_id, voxel, block);
			}
		} else if (!in_region && was_in_region) {
			const auto [last_voxel, last_block] = it->second;
			region_occupancy_.erase(it);
			if (region_hooks_.exit) {
				region_hooks_.exit(state.net_id, last_voxel, last_block);
			}
		} else if (in_region && was_in_region) {
			it->second = { voxel, block }; // stays current for a later exit
		}
	}
}

void ServerSession::handle_block_break_begin(ConnId conn, Conn &state,
		const protocol::Frame &frame) {
	(void)conn;
	if (!replicator_) {
		return;
	}
	auto msg = protocol::C2SBlockBreakBegin::decode(frame.payload);
	if (!msg) {
		return;
	}
	const replication::EntityState *e = interest_.get(state.net_id);
	core::Vec3d eye = e ? e->pos : core::Vec3d{};
	eye.y += move_params_.eye_height;
	if (!replicator_->in_reach(eye, msg->pos)) {
		return;
	}
	const core::BlockId block = replicator_->world().get_block(msg->pos);
	const world::BlockRegistry &reg = replicator_->world().registry();
	if (block == core::BlockId::kAir || !reg.contains(block)) {
		return;
	}
	const std::uint16_t max_damage = reg.get(block).max_damage;
	if (max_damage == 0) {
		return; // instant-break blocks never use this system (spec §10.7)
	}
	// No pack attached (or no vb.on("block_break_begin", ...) registered) --
	// zero policy, so nothing can ever start accruing damage. Matches
	// set_chat_handler/set_input_handler's "no handler, no side effect"
	// posture elsewhere in this class.
	if (!block_break_hooks_.begin || !block_break_hooks_.begin(state.net_id, msg->pos, block)) {
		return;
	}
	block_damage_.begin(msg->pos, state.net_id, max_damage, server_tick_);
}

void ServerSession::handle_block_break_stop(
		Conn &state, const protocol::Frame &frame) {
	auto msg = protocol::C2SBlockBreakStop::decode(frame.payload);
	if (!msg) {
		return;
	}
	block_damage_.stop(msg->pos, state.net_id);
}

void ServerSession::handle_chat(Conn &state, const protocol::Frame &frame) {
	auto msg = protocol::C2SChat::decode(frame.payload);
	if (!msg) {
		return;
	}
	if (msg->text.empty()) {
		return;
	}
	std::string text = msg->text;
	if (on_chat_) {
		const ChatHookResult result = on_chat_(state.net_id, text);
		if (result.veto) {
			return; // vetoed by the pack (vb.on("chat"))
		}
		if (result.replacement_text) {
			text = *result.replacement_text;
		}
	}
	const protocol::S2CChat out{
		registry_.get<ecs::PlayerTag>(state.entity).name + ": " + text
	};
	for (auto &[other_conn, other] : conns_) {
		if (other.playing) {
			send_message(transport_, other_conn, out);
		}
	}
}

std::optional<physics::MoveState> ServerSession::player_move_state(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			return physics::MoveState{
				registry_.get<ecs::Position>(state.entity).value,
				registry_.get<ecs::Velocity>(state.entity).value,
				registry_.get<ecs::Collider>(state.entity).on_ground
			};
		}
	}
	return std::nullopt;
}

std::optional<std::pair<float, float>> ServerSession::player_health(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			const auto &h = registry_.get<ecs::Health>(state.entity);
			return std::make_pair(h.current, h.max);
		}
	}
	return std::nullopt;
}

bool ServerSession::teleport_player(core::NetId id, core::Vec3d pos) {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			const auto rot = registry_.get<ecs::Rotation>(state.entity);
			set_player_state(id, pos, { rot.yaw, rot.pitch }, {});
			// Same as a respawn: the next movement step re-derives grounding
			// at the new spot instead of trusting the old one's.
			registry_.get<ecs::Collider>(state.entity).on_ground = false;
			return true;
		}
	}
	return false;
}

#if defined(VB_WITH_AUTOMATION)
bool ServerSession::set_player_health(core::NetId id, float value) {
	for (auto &[conn, state] : conns_) {
		(void)conn;
		if (!state.playing || state.net_id != id) {
			continue;
		}
		auto &h = registry_.get<ecs::Health>(state.entity);
		const float target = std::clamp(value, 0.0f, h.max);
		if (target < h.current) {
			apply_damage(state, h.current - target, "automation");
		} else {
			h.current = target;
		}
		return true;
	}
	return false;
}

void ServerSession::set_time_of_day(std::uint32_t ticks) {
	time_of_day_ticks_ = static_cast<double>(ticks);
	broadcast_time_of_day();
}

bool ServerSession::kick_player(core::NetId id, std::string_view reason) {
	const ConnId conn = conn_for_player(id);
	if (conn == ConnId::kInvalid) {
		return false;
	}
	drop(conn, std::string(reason));
	return true;
}

bool ServerSession::advance_reauth(core::NetId id, double seconds) {
	const ConnId conn = conn_for_player(id);
	if (conn == ConnId::kInvalid || seconds < 0.0) {
		return false;
	}
	auto &c = conns_.at(conn);
	if (!c.login) {
		return false;
	}
	// Only the countdown in effect moves: a pending request's grace, else the timer.
	// The reply still has to arrive in real time, so a slow IdP is not "fast-forwarded".
	(c.reauth.pending ? c.reauth.remaining : c.reauth.timer) -= seconds;
	return true;
}
#endif

std::uint8_t ServerSession::selected_slot(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			return registry_.get<ecs::PlayerInput>(state.entity).selected_slot;
		}
	}
	return 0;
}

void ServerSession::set_player_velocity(core::NetId id, core::Vec3d vel) {
	for (auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			registry_.get<ecs::Velocity>(state.entity).value = vel;
			return;
		}
	}
}

ConnId ServerSession::conn_for_player(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		if (state.playing && state.net_id == id) {
			return conn;
		}
	}
	return ConnId::kInvalid;
}

std::shared_ptr<const LoginData> ServerSession::player_login(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			return state.login ? state.login : state.handshake.login();
		}
	}
	return nullptr;
}

std::string_view ServerSession::player_name(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			return registry_.get<ecs::PlayerTag>(state.entity).name;
		}
	}
	return {};
}

void ServerSession::system_network_io(double dt_seconds) {
	scratch_.clear();
	transport_.poll(scratch_);

	if (max_messages_per_second_ > 0.0) {
		for (auto &[c, state] : conns_) {
			(void)c;
			state.msg_tokens = std::min(max_messages_per_second_,
					state.msg_tokens + max_messages_per_second_ * dt_seconds);
		}
	}

	if (punch_params_.punch_cooldown_seconds > 0.0) {
		for (auto &[c, state] : conns_) {
			(void)c;
			if (state.punch_cooldown_remaining > 0.0) {
				state.punch_cooldown_remaining =
						std::max(0.0, state.punch_cooldown_remaining - dt_seconds);
			}
		}
	}

	for (auto &ev : scratch_) {
		switch (ev.kind) {
			case TransportEvent::Kind::kConnected: {
				const std::optional<std::string> addr = transport_.remote_address(ev.conn);
				if (max_connections_per_ip_ > 0 && addr) {
					int count = 0;
					for (const auto &[c, state] : conns_) {
						if (state.remote_address == addr) {
							++count;
						}
					}
					if (count >= max_connections_per_ip_) {
						VB_INFO("net", "rejecting connection from ", *addr,
								": per-IP cap (", max_connections_per_ip_,
								") already reached");
						transport_.close(ev.conn, "too many connections from this address");
						break;
					}
				}
				auto it = conns_.try_emplace(ev.conn, ServerHandshake(config_, host_)).first;
				it->second.remote_address = addr;
				it->second.msg_tokens = max_messages_per_second_; // full burst allowance up front
				VB_DEBUG("net", "connection ", static_cast<std::uint64_t>(ev.conn),
						" opened");
				break;
			}
			case TransportEvent::Kind::kMessage: {
				auto it = conns_.find(ev.conn);
				if (it == conns_.end()) {
					break;
				}
				std::size_t consumed = 0;
				auto frame = protocol::read_frame(span_of(ev.frame), consumed);
				if (!frame) {
					drop(ev.conn, "malformed frame");
					break;
				}
				std::vector<std::byte> decompressed;
				const char *decompress_err = nullptr;
				if (!decompress_frame_payload(*frame, decompressed, decompress_err)) {
					drop(ev.conn, decompress_err);
					break;
				}
				if (it->second.playing) {
					// Flood guard (set_max_messages_per_second): one token per
					// message, regardless of type -- dropped, not disconnected,
					// if the bucket is empty (see that setter's own comment for
					// why this is deliberately forgiving rather than punitive).
					if (max_messages_per_second_ > 0.0) {
						if (it->second.msg_tokens < 1.0) {
							break;
						}
						it->second.msg_tokens -= 1.0;
					}
					if (frame->header.type == protocol::MessageType::kC2SInputBatch) {
						if (auto b = protocol::C2SInputBatch::decode(frame->payload)) {
							handle_input_batch(it->second, *b);
						}
						break;
					}
					if (frame->header.type ==
							protocol::MessageType::kC2SBlockEdit) {
						handle_block_edit(ev.conn, it->second, *frame);
						break;
					}
					if (frame->header.type == protocol::MessageType::kC2SUiEvent) {
						if (auto e = protocol::C2SUiEvent::decode(frame->payload)) {
							if (on_ui_event_) {
								on_ui_event_(it->second.net_id, *e);
							}
						}
						break;
					}
					if (frame->header.type == protocol::MessageType::kC2SChat) {
						handle_chat(it->second, *frame);
						break;
					}
					if (frame->header.type == protocol::MessageType::kC2SReauth) {
						// Only an answer to an outstanding request counts; an
						// unsolicited or repeated one is ignored (no free
						// verification work for a client).
						auto &r = it->second.reauth;
						if (r.pending && !r.ticket && host_.begin_authenticate) {
							if (auto m = protocol::C2SReauth::decode(frame->payload)) {
								r.ticket = host_.begin_authenticate(m->token, r.nonce);
							}
						}
						break;
					}
					if (frame->header.type ==
							protocol::MessageType::kC2SBlockBreakBegin) {
						handle_block_break_begin(ev.conn, it->second, *frame);
						break;
					}
					if (frame->header.type ==
							protocol::MessageType::kC2SBlockBreakStop) {
						handle_block_break_stop(it->second, *frame);
						break;
					}
					// Other post-join C2S messages land in later phases;
					// ignore unknown types rather than dropping.
					break;
				}
				if (max_auth_attempts_per_minute_ > 0 && it->second.remote_address &&
						config_.auth_mode == protocol::AuthMode::kExternal &&
						it->second.handshake.state() == ServerHandshakeState::kAwaitingAuth &&
						frame->header.type == protocol::MessageType::kC2SAuth) {
					auto &attempts = auth_attempts_[*it->second.remote_address];
					while (!attempts.empty() && attempts.front() < uptime_seconds_ - 60.0) {
						attempts.pop_front();
					}
					if (static_cast<int>(attempts.size()) >= max_auth_attempts_per_minute_) {
						VB_WARN("auth", "too many sign-in attempts from ",
								*it->second.remote_address, "; dropping");
						kick_with_message(ev.conn, protocol::DisconnectReason::kAuthFailed,
								"too many sign-in attempts, try again later");
						break;
					}
					attempts.push_back(uptime_seconds_);
				}
				const ServerHandshakeState prev_state = it->second.handshake.state();
				auto step = it->second.handshake.on_frame(*frame);
				send_frames(transport_, ev.conn, step.send);
				// The (long) external sign-in window is over: the remaining
				// states get a fresh handshake-timeout budget.
				if (prev_state == ServerHandshakeState::kAwaitingAuth &&
						it->second.handshake.state() ==
								ServerHandshakeState::kAwaitingAssetManifestRequest) {
					it->second.age = 0.0;
					kick_duplicate_login(ev.conn);
				}
				if (step.completed) {
					it->second.playing = true;
					++playing_;
					const JoinGrant &g = it->second.handshake.grant();
					it->second.net_id = g.net_id;
					it->second.spawn_pos = g.spawn_pos;
					it->second.login = it->second.handshake.login();
					it->second.reauth.timer = reauth_interval_for(g.net_id);
					it->second.entity = registry_.create();
					registry_.emplace<ecs::Position>(it->second.entity, g.spawn_pos);
					registry_.emplace<ecs::Velocity>(it->second.entity);
					registry_.emplace<ecs::Rotation>(it->second.entity);
					registry_.emplace<ecs::Collider>(
							it->second.entity, move_params_, /*on_ground=*/false);
					registry_.emplace<ecs::PlayerInput>(it->second.entity);
					registry_.emplace<ecs::Health>(it->second.entity, 20.0f, 20.0f);
					registry_.emplace<ecs::Hunger>(it->second.entity, 100.0f, 100.0f);
					registry_.emplace<ecs::PlayerTag>(it->second.entity, step.player_name);
					registry_.emplace<ecs::NetReplicated>(it->second.entity, g.net_id);
					// Entity-management follow-up: player_visual_kind_ lets a
					// pack's vb.register_entity{represents="player"} kind
					// supply width/height/visual for players (see
					// set_player_visual_kind()); unset means kInvalid, exact
					// pre-existing behavior.
					interest_.upsert(replication::EntityState{
							g.net_id,
							player_visual_kind_.value_or(core::EntityKindId::kInvalid),
							g.spawn_pos, {}, {} });
					joins_.push_back({ ev.conn, g.net_id, step.player_name,
							it->second.handshake.login() });
					VB_INFO("net", "player '", step.player_name, "' joined as net id ",
							static_cast<std::uint32_t>(g.net_id));

					// Phase 5.4: tell the newcomer who's already here, and
					// tell everyone already here that they joined.
					protocol::S2CPlayerList list_msg;
					for (auto &[other_conn, other] : conns_) {
						if (other.playing && other_conn != ev.conn) {
							list_msg.players.push_back({ other.net_id,
									registry_.get<ecs::PlayerTag>(other.entity).name });
						}
					}
					send_message(transport_, ev.conn, list_msg);
					const protocol::S2CPlayerJoin join_msg{ g.net_id,
						step.player_name };
					for (auto &[other_conn, other] : conns_) {
						if (other.playing && other_conn != ev.conn) {
							send_message(transport_, other_conn, join_msg);
						}
					}
				}
				if (step.disconnect) {
					drop(ev.conn, "handshake rejected");
				}
				break;
			}
			case TransportEvent::Kind::kDisconnected: {
				auto it = conns_.find(ev.conn);
				if (it == conns_.end()) {
					break;
				}
				if (it->second.playing) {
					--playing_;
					interest_.remove(it->second.net_id);
					script_entity_props_.erase(it->second.net_id);
					dirty_entity_props_.erase(it->second.net_id);
					block_damage_.remove_player(it->second.net_id);
					region_occupancy_.erase(it->second.net_id);
					if (replicator_) {
						replicator_->forget_player(it->second.net_id);
					}
					leaves_.push_back({ ev.conn, it->second.net_id, ev.reason,
							registry_.get<ecs::PlayerTag>(it->second.entity).name,
							physics::MoveState{
									registry_.get<ecs::Position>(it->second.entity).value,
									registry_.get<ecs::Velocity>(it->second.entity).value,
									registry_.get<ecs::Collider>(it->second.entity).on_ground } });
					const protocol::S2CPlayerLeave leave_msg{ it->second.net_id };
					for (auto &[other_conn, other] : conns_) {
						if (other_conn != ev.conn && other.playing) {
							send_message(transport_, other_conn, leave_msg);
						}
					}
					registry_.destroy(it->second.entity);
				}
				conns_.erase(it);
				break;
			}
		}
	}
}

void ServerSession::system_handshake_timeouts(double dt_seconds) {
	uptime_seconds_ += dt_seconds;
	// Forget IPs whose last attempt is over a minute old (bounded memory).
	if (!auth_attempts_.empty()) {
		for (auto it = auth_attempts_.begin(); it != auth_attempts_.end();) {
			if (it->second.empty() || it->second.back() < uptime_seconds_ - 60.0) {
				it = auth_attempts_.erase(it);
			} else {
				++it;
			}
		}
	}
	// Handshake timeouts + asset-stream pacing.
	std::vector<std::pair<ConnId, std::string>> to_drop;
	std::vector<ConnId> verified; // external auth just succeeded (async path)
	for (auto &[conn, state] : conns_) {
		if (state.playing) {
			continue;
		}
		if (state.handshake.state() == ServerHandshakeState::kStreamingAssets) {
			auto step = state.handshake.pump_assets(kAssetSendBudgetPerTick);
			send_frames(transport_, conn, step.send);
			if (step.disconnect) {
				to_drop.emplace_back(conn, "asset streaming protocol error");
				continue;
			}
		}
		if (state.handshake.state() == ServerHandshakeState::kVerifyingAuth) {
			auto step = state.handshake.poll_auth();
			send_frames(transport_, conn, step.send);
			if (step.disconnect) {
				to_drop.emplace_back(conn, "sign-in rejected");
				continue;
			}
			if (state.handshake.state() ==
					ServerHandshakeState::kAwaitingAssetManifestRequest) {
				state.age = 0.0;
				verified.push_back(conn);
			}
		}
		state.age += dt_seconds;
		if (state.age > state.handshake.timeout_seconds()) {
			auto step = state.handshake.on_timeout();
			send_frames(transport_, conn, step.send);
			to_drop.emplace_back(conn, "handshake timeout");
		}
	}
	for (const ConnId conn : verified) {
		kick_duplicate_login(conn);
	}
	for (const auto &[conn, reason] : to_drop) {
		drop(conn, reason);
	}
}

void ServerSession::kick_duplicate_login(ConnId newcomer) {
	const auto nit = conns_.find(newcomer);
	if (nit == conns_.end() || !nit->second.handshake.login()) {
		return;
	}
	const auto &login = nit->second.handshake.login();
	// The newcomer is never refused: a ghost session left by a crash or a
	// dropped link must not lock its owner out (auth.md §5.5).
	for (auto &[conn, other] : conns_) {
		if (conn == newcomer) {
			continue;
		}
		const auto &ol = other.handshake.login();
		if (ol && ol->issuer == login->issuer && ol->subject == login->subject) {
			VB_INFO("auth", "kicking older session of '", other.handshake.player_name(),
					"': signed in elsewhere");
			drop(conn, "signed in elsewhere");
		}
	}
}

void ServerSession::system_advance_time_of_day(double dt_seconds) {
	time_of_day_ticks_ = world::advance_time_of_day(
			time_of_day_ticks_, dt_seconds, day_length_seconds_);
	time_of_day_broadcast_accum_ += dt_seconds;
	if (time_of_day_broadcast_accum_ >= kTimeOfDayBroadcastIntervalSeconds) {
		time_of_day_broadcast_accum_ = 0.0;
		broadcast_time_of_day();
	}
}

// Spec §6's InterestManagementSystem/ReplicationSystem, scoped to script
// entities: spawn_script_entity()/set_script_entity_state() write Position/
// Rotation/Velocity components instead of upserting interest_ directly (see
// their comments), and this generic pass is what actually pushes them into
// the interest grid, once per tick. Players are excluded (PlayerTag) and
// keep their existing immediate-upsert path (handle_input_batch,
// check_respawns, set_player_state, join) -- several other message handlers
// this same tick (handle_block_edit, punch(), block-break, and later
// same-tick systems like update_region_occupancy/update_item_drops) read
// interest_ expecting a player's *just-simulated* position, which a single
// end-of-tick pass can't provide without those call sites reading the
// registry directly instead (done for the reach-sensitive ones; not worth
// doing everywhere just to make this loop fully generic). Item drops keep
// their own path too (update_item_drops) -- they aren't registry entities.
void ServerSession::system_sync_interest() {
	for (auto entity : registry_.view<ecs::Position, ecs::NetReplicated>(
				 entt::exclude<ecs::PlayerTag>)) {
		const auto &pos = registry_.get<ecs::Position>(entity);
		const auto &net = registry_.get<ecs::NetReplicated>(entity);
		core::EntityKindId kind = core::EntityKindId::kInvalid;
		if (const auto *k = registry_.try_get<ecs::EntityKind>(entity)) {
			kind = k->id;
		}
		core::Vec2f rot{};
		if (const auto *r = registry_.try_get<ecs::Rotation>(entity)) {
			rot = { r->yaw, r->pitch };
		}
		core::Vec3f vel{};
		if (const auto *v = registry_.try_get<ecs::Velocity>(entity)) {
			vel = core::Vec3f{ static_cast<float>(v->value.x),
				static_cast<float>(v->value.y),
				static_cast<float>(v->value.z) };
		}
		// No Collider means the pack positions this entity itself: there is
		// no notion of ground, so report it grounded and let the client pick
		// idle/walk/run from its velocity.
		std::uint8_t flags = pack_flags(true);
		if (const auto *c = registry_.try_get<ecs::Collider>(entity)) {
			flags = pack_flags(c->on_ground);
		}
		interest_.upsert(replication::EntityState{
				net.net_id, kind, pos.value, rot, vel, flags });
	}
}

void ServerSession::tick(double dt_seconds) {
	if (systems_.names().empty()) {
		build_systems();
	}
	systems_.run(registry_, ecs::TickContext{ dt_seconds, server_tick_ });
}

void ServerSession::build_systems() {
	systems_.add("network_io", [this](entt::registry &, const ecs::TickContext &ctx) {
		system_network_io(ctx.dt_seconds);
	});
	systems_.add("handshake_timeouts",
			[this](entt::registry &, const ecs::TickContext &ctx) {
				system_handshake_timeouts(ctx.dt_seconds);
			});
	systems_.add("reauth", [this](entt::registry &, const ecs::TickContext &ctx) {
		system_reauth(ctx.dt_seconds);
	});
	systems_.add("advance_time_of_day",
			[this](entt::registry &, const ecs::TickContext &ctx) {
				system_advance_time_of_day(ctx.dt_seconds);
			});
	// Ahead of check_respawns so same-tick starvation damage (cause
	// "hunger") is seen by that same phase's health<=0 check, matching
	// void-kill's own single-tick "damage then check" shape.
	systems_.add("update_hunger", [this](entt::registry &, const ecs::TickContext &ctx) {
		update_hunger(ctx.dt_seconds);
	});
	systems_.add("check_respawns", [this](entt::registry &, const ecs::TickContext &) {
		check_respawns();
	});
	// Formalizes the "EntityKind tick/spawn/hit/death callbacks wired into
	// real systems" gap: runs before sync_interest/broadcast_snapshots below
	// so a script-driven entity move (self:set_pos() from on_tick) reaches
	// this same tick's snapshot instead of next tick's, matching the spec's
	// ScriptPreTickSystem placement ahead of interest/replication. A no-op
	// when no PackRuntime is attached (on_script_tick_ unset).
	systems_.add("script_tick", [this](entt::registry &, const ecs::TickContext &ctx) {
		if (on_script_tick_) {
			on_script_tick_(ctx.dt_seconds);
		}
	});
	systems_.add("update_item_drops",
			[this](entt::registry &, const ecs::TickContext &ctx) {
				update_item_drops(ctx.dt_seconds);
			});
	systems_.add("update_block_damage",
			[this](entt::registry &, const ecs::TickContext &) { update_block_damage(); });
	systems_.add("update_block_punch_healing",
			[this](entt::registry &, const ecs::TickContext &ctx) {
				update_block_punch_healing(ctx.dt_seconds);
			});
	systems_.add("update_region_occupancy",
			[this](entt::registry &, const ecs::TickContext &) {
				update_region_occupancy();
			});
	systems_.add("sync_player_status", [this](entt::registry &, const ecs::TickContext &) {
		sync_player_status();
	});
	systems_.add("update_attachments", [this](entt::registry &, const ecs::TickContext &) {
		update_attachments();
	});
	systems_.add("sync_interest", [this](entt::registry &, const ecs::TickContext &) {
		system_sync_interest();
	});
	// Matches the pre-SystemRunner order: the tick counter advances before
	// broadcast_snapshots() so the outgoing snapshot carries the new tick.
	systems_.add("advance_server_tick",
			[this](entt::registry &, const ecs::TickContext &) { ++server_tick_; });
	systems_.add("broadcast_snapshots",
			[this](entt::registry &, const ecs::TickContext &) { broadcast_snapshots(); });
	systems_.add("broadcast_world", [this](entt::registry &, const ecs::TickContext &) {
		broadcast_world();
	});
}

void ServerSession::update_item_drops(double dt_seconds) {
	// Reads positions from the interest grid rather than Conn::move.position
	// directly: it's the same single source of truth broadcast_snapshots()
	// already uses for "where is this net id right now", kept current by
	// both real input-driven movement (handle_input_batch) and the
	// test/script-facing set_player_state() -- picking up an item works the
	// same way regardless of which path moved the player.
	std::vector<std::pair<core::NetId, core::Vec3d>> players;
	for (auto &[conn, state] : conns_) {
		if (!state.playing) {
			continue;
		}
		if (const auto *e = interest_.get(state.net_id)) {
			players.emplace_back(state.net_id, e->pos);
		}
	}
	const world::ItemDropTickResult result = item_drops_.tick(dt_seconds, players,
			move_params_.height, replicator_ != nullptr ? &world_query() : nullptr);
	for (core::NetId id : result.moved) {
		const auto drop = item_drops_.drops().find(id);
		const auto *e = interest_.get(id);
		if (drop != item_drops_.drops().end() && e != nullptr) {
			replication::EntityState moved = *e;
			moved.pos = drop->second.pos;
			interest_.upsert(moved);
		}
	}
	for (core::NetId id : result.removed) {
		interest_.remove(id);
	}
	for (const world::ItemPickup &p : result.pickups) {
		if (on_item_pickup_) {
			on_item_pickup_(p.player, p.item, p.count);
		}
	}
}

void ServerSession::update_block_damage() {
	if (!replicator_) {
		return;
	}
	auto damage_fn = [this](core::IVec3 pos, core::NetId player,
							 std::uint16_t max_damage) -> float {
		if (!block_break_hooks_.tick_damage) {
			return 0.0f;
		}
		const core::BlockId block = replicator_->world().get_block(pos);
		return block_break_hooks_.tick_damage(player, pos, block, max_damage);
	};
	auto health_fn = [this](core::IVec3 pos, float damage,
							 std::uint16_t max_damage,
							 std::uint64_t idle) -> std::optional<float> {
		if (!block_break_hooks_.health_tick) {
			return std::nullopt;
		}
		const core::BlockId block = replicator_->world().get_block(pos);
		return block_break_hooks_.health_tick(pos, block, damage, max_damage, idle);
	};
	const world::BlockDamageTickResult result =
			block_damage_.tick(server_tick_, damage_fn, health_fn);

	for (const world::CompletedBreak &c : result.completed) {
		core::Vec3d eye{};
		if (const auto *e = interest_.get(c.contributor)) {
			eye = e->pos;
			eye.y += move_params_.eye_height;
		}
		protocol::C2SBlockEdit edit;
		edit.action = protocol::BlockEditAction::kBreak;
		edit.pos = c.pos;
		protocol::S2CBlockEditResult unused_result;
		auto per_player =
				replicator_->apply_block_edit(c.contributor, eye, edit, unused_result);
		for (auto &pf : per_player) {
			for (auto &[other_conn, other] : conns_) {
				if (other.playing && other.net_id == pf.id) {
					send_frames(transport_, other_conn, pf.frames);
					break;
				}
			}
		}
	}
	// result.changed/cleared: worth re-replicating once a wire message
	// consumes them (no client renders cracks yet, see REMAINING_TASKS.md
	// 6.5's texture-atlas dependency note) -- nothing to do here for now.
}

void ServerSession::apply_damage(Conn &state, float amount, std::string_view cause) {
	auto &health = registry_.get<ecs::Health>(state.entity);
	if (health.current <= 0.0f) {
		return; // already at 0, awaiting this tick's respawn
	}
	const float before = health.current;
	health.current = std::max(0.0f, health.current - amount);
	if (health.current <= 0.0f) {
		state.death_cause = std::string(cause);
		state.death_health_before = before;
	}
}

void ServerSession::damage_player(core::NetId id, float amount, std::string_view cause) {
	for (auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			apply_damage(state, amount, cause);
			return;
		}
	}
}

core::Vec3d ServerSession::spawn_point(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			return state.spawn_pos;
		}
	}
	return {};
}

std::optional<float> ServerSession::player_hunger(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			return registry_.get<ecs::Hunger>(state.entity).current;
		}
	}
	return std::nullopt;
}

void ServerSession::add_player_hunger(core::NetId id, float amount) {
	for (auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			auto &hunger = registry_.get<ecs::Hunger>(state.entity);
			hunger.current = core::clamp(hunger.current + amount, 0.0f, hunger.max);
			return;
		}
	}
}

void ServerSession::update_hunger(double dt_seconds) {
	if (hunger_params_.decay_per_second <= 0.0f) {
		return; // disabled -- see HungerParams::decay_per_second's own comment
	}
	const float dt = static_cast<float>(dt_seconds);
	for (auto &[conn, state] : conns_) {
		if (!state.playing) {
			continue;
		}
		auto &hunger = registry_.get<ecs::Hunger>(state.entity);
		hunger.current = std::max(0.0f, hunger.current - hunger_params_.decay_per_second * dt);
		if (hunger.current <= 0.0f && hunger_params_.starvation_damage_per_second > 0.0f) {
			apply_damage(state, hunger_params_.starvation_damage_per_second * dt, "hunger");
		}
	}
}

void ServerSession::check_respawns() {
	for (auto &[conn, state] : conns_) {
		if (!state.playing) {
			continue;
		}
		auto &pos = registry_.get<ecs::Position>(state.entity);
		auto &health = registry_.get<ecs::Health>(state.entity);
		if (pos.value.y < void_kill_y_ && health.current > 0.0f) {
			apply_damage(state, health.current, "void");
		}
		if (health.current > 0.0f) {
			continue;
		}
		RespawnDecision decision{ health.max, state.spawn_pos,
			"* you died and respawned" };
		if (on_respawn_) {
			decision = on_respawn_(
					state.net_id, state.death_cause, state.death_health_before);
		}
		health.current = decision.heal_to;
		// A death starved to 0 hunger shouldn't leave the respawned player
		// still starving and dying again next tick -- respawn is a full
		// reset, hunger included, same as health above (RespawnDecision has
		// no separate hunger override; a pack wanting a harsher respawn can
		// still call add_player_hunger() itself from vb.on("player_death")).
		auto &hunger = registry_.get<ecs::Hunger>(state.entity);
		hunger.current = hunger.max;
		pos.value = decision.pos;
		registry_.get<ecs::Velocity>(state.entity).value = {};
		registry_.get<ecs::Collider>(state.entity).on_ground = false;
		// Upserted immediately -- later phases this same tick (item drops,
		// block damage, region occupancy) read interest_ expecting the
		// post-respawn position, not last tick's pre-respawn one.
		replication::EntityState s;
		if (const auto *e = interest_.get(state.net_id)) {
			s = *e;
		}
		s.net_id = state.net_id;
		s.pos = decision.pos;
		s.vel = {};
		interest_.upsert(s);
		if (!decision.message.empty()) {
			send_message(transport_, conn, protocol::S2CChat{ decision.message });
		}
		state.death_cause.clear();
		state.death_health_before = 0.0f;
	}
}

void ServerSession::sync_player_status() {
	for (auto &[conn, state] : conns_) {
		if (!state.playing) {
			continue;
		}
		const auto &health = registry_.get<ecs::Health>(state.entity);
		const auto &hunger = registry_.get<ecs::Hunger>(state.entity);
		protocol::S2CPlayerStatus msg;
		msg.health = health.current;
		msg.max_health = health.max;
		msg.hunger = hunger.current;
		msg.max_hunger = hunger.max;
		const auto &last = state.last_status;
		if (last && last->health == msg.health && last->max_health == msg.max_health &&
				last->hunger == msg.hunger && last->max_hunger == msg.max_hunger) {
			continue;
		}
		send_message(transport_, conn, msg);
		state.last_status = msg;
	}
}

core::NetId ServerSession::spawn_item_drop(
		core::Vec3d pos, core::BlockId item, std::uint16_t count) {
	// Phase 6.11: a registered block can override the engine-default pickup
	// radius / despawn lifetime for its own dropped instances. No `replicator_`
	// (a test harness with no world attached) or an id the registry doesn't
	// know about both fall through to ItemDropSystem's own defaults.
	std::optional<double> pickup_radius;
	std::optional<double> lifetime_seconds;
	if (replicator_ != nullptr) {
		const world::BlockRegistry &registry = replicator_->world().registry();
		if (registry.contains(item)) {
			const world::BlockType &type = registry.get(item);
			if (type.pickup_radius >= 0.0) {
				pickup_radius = type.pickup_radius;
			}
			if (type.drop_lifetime_seconds >= 0.0) {
				lifetime_seconds = type.drop_lifetime_seconds;
			}
		}
	}
	const core::NetId id =
			item_drops_.spawn(pos, item, count, pickup_radius, lifetime_seconds);
	// Entity-management follow-up: item_drop_visual_kind_ lets a pack's
	// vb.register_entity{represents="item_drop"} kind supply width/height/
	// visual for drops (see set_item_drop_visual_kind()); unset falls back to
	// the reserved world::kItemDropKind sentinel, exact pre-existing behavior.
	interest_.upsert(replication::EntityState{
			id, item_drop_visual_kind_.value_or(world::kItemDropKind), pos, {}, {},
			pack_flags(true) });
	return id;
}

core::NetId ServerSession::spawn_script_entity(
		core::EntityKindId kind, core::Vec3d pos) {
	const auto id = static_cast<core::NetId>(next_script_entity_id_++);
	const entt::entity entity = registry_.create();
	registry_.emplace<ecs::Position>(entity, pos);
	registry_.emplace<ecs::EntityKind>(entity, kind);
	registry_.emplace<ecs::NetReplicated>(entity, id);
	script_entities_[id] = entity;
	// interest_ picks this entity up on the next system_sync_interest() pass
	// this same tick -- no need to upsert it here too.
	return id;
}

void ServerSession::set_script_entity_state(
		core::NetId id, core::Vec3d pos, core::Vec2f rot, core::Vec3f vel) {
	const auto it = script_entities_.find(id);
	if (it == script_entities_.end()) {
		return;
	}
	const entt::entity entity = it->second;
	registry_.get<ecs::Position>(entity).value = pos;
	if (auto *r = registry_.try_get<ecs::Rotation>(entity)) {
		*r = { rot.x, rot.y };
	} else {
		registry_.emplace<ecs::Rotation>(entity, rot.x, rot.y);
	}
	const core::Vec3d vel_d{ static_cast<double>(vel.x),
		static_cast<double>(vel.y), static_cast<double>(vel.z) };
	if (auto *v = registry_.try_get<ecs::Velocity>(entity)) {
		v->value = vel_d;
	} else {
		registry_.emplace<ecs::Velocity>(entity, vel_d);
	}
	// interest_ is refreshed from the registry every tick by
	// system_sync_interest(); no need to upsert it here too.
}

std::optional<core::Vec3d> ServerSession::script_entity_pos(core::NetId id) const {
	const auto it = script_entities_.find(id);
	if (it == script_entities_.end()) {
		return std::nullopt;
	}
	return registry_.get<ecs::Position>(it->second).value;
}

void ServerSession::remove_script_entity(core::NetId id) {
	const auto it = script_entities_.find(id);
	if (it != script_entities_.end()) {
		registry_.destroy(it->second);
		script_entities_.erase(it);
	}
	interest_.remove(id);
	script_entity_props_.erase(id);
	dirty_entity_props_.erase(id);
}

template <typename T>
void ServerSession::set_script_entity_prop(core::NetId id,
		std::optional<T> ScriptEntityProps::*field, std::optional<T> value,
		std::uint8_t bit) {
	if (script_entities_.find(id) == script_entities_.end() && !is_playing(id)) {
		return;
	}
	const auto it = script_entity_props_.find(id);
	if (it == script_entity_props_.end()) {
		if (!value) {
			return;
		}
		script_entity_props_[id].*field = std::move(value);
	} else {
		if (it->second.*field == value) {
			return; // unchanged: nothing to resend
		}
		it->second.*field = std::move(value);
		if (it->second.empty()) {
			script_entity_props_.erase(it);
		}
	}
	dirty_entity_props_[id] |= bit;
}

void ServerSession::set_script_entity_text(
		core::NetId id, std::optional<protocol::EntityText> text) {
	set_script_entity_prop(id, &ScriptEntityProps::text, std::move(text),
			protocol::kEntityPropText);
}

void ServerSession::set_script_entity_visual_override(
		core::NetId id, std::optional<protocol::EntityVisualOverride> override_def) {
	set_script_entity_prop(id, &ScriptEntityProps::visual, std::move(override_def),
			protocol::kEntityPropVisual);
}

bool ServerSession::is_playing(core::NetId id) const {
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			return true;
		}
	}
	return false;
}

void ServerSession::set_script_entity_clip(core::NetId id, std::optional<std::string> clip) {
	set_script_entity_prop(id, &ScriptEntityProps::clip, std::move(clip),
			protocol::kEntityPropClip);
}

void ServerSession::set_script_entity_attachment(
		core::NetId id, std::optional<protocol::EntityAttachment> attachment) {
	set_script_entity_prop(id, &ScriptEntityProps::attach, std::move(attachment),
			protocol::kEntityPropAttach);
}

// Runs after script_tick and before sync_interest: every attached script
// entity takes its parent's position (+ offset), facing and velocity, so
// interest culling, entity:get_pos() and the walk/run clip all follow the
// parent. A parent that left the interest grid (despawned, disconnected)
// orphans the entity.
void ServerSession::update_attachments() {
	std::vector<core::NetId> orphans;
	for (const auto &[id, props] : script_entity_props_) {
		if (!props.attach) {
			continue;
		}
		const auto ent = script_entities_.find(id);
		if (ent == script_entities_.end()) {
			continue;
		}
		// A script-entity parent is read from the registry (it may have been
		// spawned this tick, before sync_interest put it in interest_); a
		// player from interest_, which its input handling keeps current.
		core::Vec3d ppos{};
		core::Vec2f prot{};
		core::Vec3d pvel{};
		if (const auto pent = script_entities_.find(props.attach->parent);
				pent != script_entities_.end()) {
			ppos = registry_.get<ecs::Position>(pent->second).value;
			if (const auto *r = registry_.try_get<ecs::Rotation>(pent->second)) {
				prot = { r->yaw, r->pitch };
			}
			if (const auto *v = registry_.try_get<ecs::Velocity>(pent->second)) {
				pvel = v->value;
			}
		} else if (const replication::EntityState *parent = interest_.get(props.attach->parent)) {
			ppos = parent->pos;
			prot = parent->rot;
			pvel = { parent->vel.x, parent->vel.y, parent->vel.z };
		} else {
			orphans.push_back(id);
			continue;
		}
		const core::Vec3d off = protocol::attachment_world_offset(*props.attach, prot.x);
		registry_.get<ecs::Position>(ent->second).value =
				core::Vec3d{ ppos.x + off.x, ppos.y + off.y, ppos.z + off.z };
		registry_.emplace_or_replace<ecs::Rotation>(ent->second, prot.x, prot.y);
		registry_.emplace_or_replace<ecs::Velocity>(ent->second, pvel);
	}
	for (const core::NetId id : orphans) {
		if (on_script_entity_orphaned_) {
			on_script_entity_orphaned_(id);
		}
		// The handler normally despawns it; make sure it's gone either way.
		if (script_entities_.find(id) != script_entities_.end()) {
			remove_script_entity(id);
		}
	}
}

void ServerSession::broadcast_time_of_day() {
	const protocol::S2CTimeOfDay msg{ static_cast<std::uint32_t>(time_of_day_ticks_) };
	for (auto &[conn, state] : conns_) {
		if (state.playing) {
			send_message(transport_, conn, msg);
		}
	}
}

void ServerSession::broadcast_world() {
	if (!replicator_) {
		return;
	}
	std::vector<std::pair<core::NetId, core::Vec3d>> players;
	for (const auto &[conn, state] : conns_) {
		(void)conn;
		if (!state.playing) {
			continue;
		}
		const replication::EntityState *e = interest_.get(state.net_id);
		players.emplace_back(state.net_id, e ? e->pos : core::Vec3d{});
	}

	for (auto &pf : replicator_->tick(players)) {
		for (auto &[conn, state] : conns_) {
			if (state.playing && state.net_id == pf.id) {
				send_frames(transport_, conn, pf.frames);
				break;
			}
		}
	}
}

namespace {

// `override_def` is only ever passed for an `entered` record (see
// broadcast_snapshots) -- `stayed`/`local` records call this with the
// default nullptr, leaving EntityRecord::visual_override unset ("unchanged",
// not "cleared"; see that field's own comment in snapshot.hpp).
protocol::EntityRecord to_record(const replication::EntityState &s,
		const protocol::EntityVisualOverride *override_def = nullptr,
		std::optional<std::uint16_t> item = std::nullopt) {
	protocol::EntityRecord r;
	r.net_id = s.net_id;
	r.kind = s.kind;
	r.pos = s.pos;
	r.rot = s.rot;
	r.vel = s.vel;
	r.flags = s.flags;
	if (override_def != nullptr) {
		r.visual_override = *override_def;
	}
	r.item = item;
	return r;
}

} // namespace

void ServerSession::broadcast_snapshots() {
	for (auto &[conn, state] : conns_) {
		if (!state.playing) {
			continue;
		}
		const replication::EntityState *self = interest_.get(state.net_id);
		const core::Vec3d eye = self ? self->pos : core::Vec3d{};

		std::vector<core::NetId> visible = interest_.visible_from(
				eye, interest_radius_cells_, state.net_id);
		const replication::InterestDiff d =
				replication::diff_interest(state.last_visible, visible);

		protocol::S2CEntitySnapshot snap;
		snap.server_tick = server_tick_;
		for (core::NetId id : d.entered) {
			if (const auto *e = interest_.get(id)) {
				// A visual override travels in S2C_EntityProps (reliable, and
				// changeable at runtime), not on this record.
				const auto drop_it = item_drops_.drops().find(id);
				std::optional<std::uint16_t> item;
				if (drop_it != item_drops_.drops().end()) {
					item = static_cast<std::uint16_t>(drop_it->second.item);
				}
				snap.entered.push_back(to_record(*e, nullptr, item));
			}
		}
		for (core::NetId id : d.stayed) {
			if (const auto *e = interest_.get(id)) {
				snap.updated.push_back(to_record(*e));
			}
		}
		snap.removed = d.left;

		// Text, clip and attachment ride their own reliable message
		// (S2C_EntityProps): everything about what just entered, plus each
		// property that changed on something this player already sees.
		protocol::S2CEntityProps props_msg;
		props_msg.server_tick = server_tick_;
		const auto add_props = [&](core::NetId id, std::uint8_t mask) {
			const auto it = script_entity_props_.find(id);
			const ScriptEntityProps none;
			const ScriptEntityProps &p = it != script_entity_props_.end() ? it->second : none;
			protocol::EntityPropsUpdate u;
			u.net_id = id;
			if (mask & protocol::kEntityPropText) {
				u.text = p.text;
			}
			if (mask & protocol::kEntityPropClip) {
				u.clip = p.clip;
			}
			if (mask & protocol::kEntityPropAttach) {
				u.attach = p.attach;
			}
			if (mask & protocol::kEntityPropVisual) {
				u.visual = p.visual;
			}
			props_msg.updates.push_back(std::move(u));
		};
		if (!script_entity_props_.empty()) {
			for (core::NetId id : d.entered) {
				if (script_entity_props_.find(id) != script_entity_props_.end()) {
					add_props(id, protocol::kEntityPropAll);
				}
			}
		}
		for (const auto &[id, mask] : dirty_entity_props_) {
			// Not visible, or just entered (handled above). A player's own
			// appearance goes to them too (third-person camera, UI preview);
			// interest culling never lists yourself.
			if (id == state.net_id ||
					std::binary_search(d.stayed.begin(), d.stayed.end(), id)) {
				add_props(id, mask);
			}
		}
		if (!props_msg.updates.empty()) {
			send_message(transport_, conn, props_msg);
		}

		state.last_visible = std::move(visible);

		snap.last_acked_input_seq = registry_.get<ecs::PlayerInput>(state.entity).last_seq;
		if (state.input_driven) {
			snap.has_local = true;
			if (self) {
				snap.local = to_record(*self);
			}
			const auto &pos = registry_.get<ecs::Position>(state.entity);
			const auto &vel = registry_.get<ecs::Velocity>(state.entity);
			const auto &rot = registry_.get<ecs::Rotation>(state.entity);
			const auto &collider = registry_.get<ecs::Collider>(state.entity);
			snap.local.net_id = state.net_id;
			snap.local.pos = pos.value;
			snap.local.vel = core::Vec3f{
				static_cast<float>(vel.value.x),
				static_cast<float>(vel.value.y),
				static_cast<float>(vel.value.z)
			};
			snap.local.rot = { rot.yaw, rot.pitch };
			snap.local.flags = pack_flags(collider.on_ground);
		}

		if (!snap.has_local && snap.entered.empty() && snap.updated.empty() &&
				snap.removed.empty()) {
			continue; // nothing changed for this player this tick
		}
		send_message(transport_, conn, snap);
	}
	dirty_entity_props_.clear();
}

void ServerSession::set_player_state(core::NetId id, core::Vec3d pos,
		core::Vec2f rot, core::Vec3f vel) {
	for (auto &[conn, state] : conns_) {
		(void)conn;
		if (state.playing && state.net_id == id) {
			registry_.get<ecs::Position>(state.entity).value = pos;
			registry_.get<ecs::Rotation>(state.entity) = { rot.x, rot.y };
			registry_.get<ecs::Velocity>(state.entity).value = {
				static_cast<double>(vel.x), static_cast<double>(vel.y),
				static_cast<double>(vel.z)
			};
			// Upserted immediately (see handle_input_batch's comment) --
			// callers (tests, script teleports) expect this to be visible to
			// interest_-reading code the moment they call it, same tick.
			replication::EntityState s;
			if (const auto *e = interest_.get(id)) {
				s = *e;
			}
			s.net_id = id;
			s.pos = pos;
			s.rot = rot;
			s.vel = vel;
			interest_.upsert(s);
			return;
		}
	}
}

std::vector<SessionPlayerJoined> ServerSession::take_joins() {
	return std::exchange(joins_, {});
}

std::vector<SessionLoginChanged> ServerSession::take_login_changes() {
	std::vector<SessionLoginChanged> out;
	out.swap(login_changes_);
	return out;
}

double ServerSession::reauth_interval_for(core::NetId id) const {
	if (config_.reauth_interval_seconds == 0) {
		return 0.0;
	}
	// Per-player jitter of ±10% so a server full of players who joined together
	// does not re-prove all at once (auth.md §5.6).
	const std::uint32_t h = static_cast<std::uint32_t>(id) * 2654435761u;
	const double j = 0.9 + 0.2 * (static_cast<double>(h % 1000u) / 1000.0);
	return static_cast<double>(config_.reauth_interval_seconds) * j;
}

void ServerSession::kick_with_message(ConnId conn, protocol::DisconnectReason reason,
		const std::string &message) {
	protocol::S2CDisconnect msg;
	msg.reason = reason;
	msg.message = message;
	send_message(transport_, conn, msg);
	drop(conn, message);
}

void ServerSession::finish_reauth(ConnId conn, Conn &state, const AuthOutcome &outcome) {
	auto &r = state.reauth;
	const auto old_login = state.login;
	auto kick = [&](const char *why) {
		VB_INFO("auth", "re-auth failed for '", old_login ? old_login->name : std::string(),
				"': ", why);
		kick_with_message(conn, protocol::DisconnectReason::kAuthFailed,
				"sign-in expired or revoked");
	};
	if (!outcome.ok || !outcome.login || !old_login) {
		kick(outcome.ok ? "no identity" : "token rejected");
		return;
	}
	if (outcome.login->issuer != old_login->issuer ||
			outcome.login->subject != old_login->subject) {
		kick("a different account answered");
		return;
	}
	// A refresh-derived token carries no nonce, so freshness is the binding: it
	// must have been issued after this request went out (minus clock skew).
	if (outcome.login->issued_at < r.sent_at - 60) {
		kick("stale token");
		return;
	}
	auto fresh = std::make_shared<LoginData>(*outcome.login);
	fresh->name = old_login->name; // the in-game name is fixed for the session
	const bool changed = fresh->claims_json != old_login->claims_json;
	state.login = fresh;
	VB_INFO("auth", "re-auth ok for '", fresh->name, "'", changed ? " (claims changed)" : "");
	r.pending = false;
	r.ticket = nullptr;
	r.nonce.clear();
	r.timer = reauth_interval_for(state.net_id);
	if (changed) {
		login_changes_.push_back({ state.net_id, fresh });
	}
}

void ServerSession::system_reauth(double dt_seconds) {
	if (config_.reauth_interval_seconds == 0 ||
			config_.auth_mode != protocol::AuthMode::kExternal) {
		return;
	}
	struct Kick {
		ConnId conn;
	};
	std::vector<Kick> kicks;
	for (auto &[conn, c] : conns_) {
		if (!c.playing || !c.login) {
			continue;
		}
		auto &r = c.reauth;
		if (!r.pending) {
			r.timer -= dt_seconds;
			if (r.timer > 0.0) {
				continue;
			}
			const auto challenge = host_.auth_challenge();
			if (!challenge || challenge->nonce.empty()) {
				r.timer = 10.0; // cannot issue a nonce right now; try again soon
				continue;
			}
			r.nonce = challenge->nonce;
			r.pending = true;
			r.remaining = static_cast<double>(config_.reauth_grace_seconds);
			r.sent_at = host_.unix_time();
			r.ticket = nullptr;
			protocol::S2CReauthRequest req;
			req.nonce = r.nonce;
			req.grace_seconds = static_cast<std::uint16_t>(
					std::min<std::uint32_t>(config_.reauth_grace_seconds, 65535u));
			send_message(transport_, conn, req);
			continue;
		}
		r.remaining -= dt_seconds;
		if (r.ticket) {
			if (const auto outcome = r.ticket()) {
				const AuthOutcome result = *outcome;
				// May kick (and so touch `conns_` state only via transport
				// close, which defers erasure to the disconnect event).
				finish_reauth(conn, c, result);
				continue;
			}
		}
		if (r.remaining <= 0.0) {
			VB_INFO("auth", "re-auth deadline passed for '", c.login->name, "'");
			kicks.push_back({ conn });
		}
	}
	for (const Kick &k : kicks) {
		kick_with_message(k.conn, protocol::DisconnectReason::kAuthFailed,
				"sign-in expired or revoked");
	}
}

std::vector<SessionPlayerLeft> ServerSession::take_leaves() {
	return std::exchange(leaves_, {});
}

// ===========================================================================
// ClientSession
// ===========================================================================

namespace {

HandshakeClientHost make_asset_host(assetsync::ClientAssetCache *cache) {
	if (cache == nullptr) {
		return {};
	}
	HandshakeClientHost host;
	host.assets_missing = [cache](const protocol::S2CAssetManifest &manifest) {
		return cache->compute_missing(manifest.entries, manifest.manifest_hash);
	};
	host.on_asset_chunk = [cache](const protocol::S2CAssetData &chunk) {
		return cache->ingest_chunk(chunk);
	};
	host.assets_all_received = [cache] { return cache->all_received(); };
	return host;
}

} // namespace

ClientSession::ClientSession(Transport &transport, ConnId conn,
		HandshakeClientConfig config, assetsync::ClientAssetCache *cache) : transport_(transport),
																			conn_(conn),
																			handshake_(std::move(config), make_asset_host(cache)),
																			asset_cache_(cache) {}

const std::unordered_map<std::string, std::vector<std::byte>> &
ClientSession::virtual_pack_fs() const {
	static const std::unordered_map<std::string, std::vector<std::byte>> kEmpty;
	return asset_cache_ != nullptr ? asset_cache_->virtual_fs() : kEmpty;
}

void ClientSession::cancel_sign_in() {
	auto step = handshake_.cancel_sign_in();
	if (step.failed) {
		failure_reason_ = step.failure_reason;
	}
}

void ClientSession::tick(double dt_seconds) {
	// Keep the wall-clock server-time estimate progressing every tick, even
	// one with no snapshot in it -- see ServerTimeEstimator's own header
	// comment for why this is what fixes interpolated_pos()'s old
	// "frozen until the next packet" staircase.
	server_time_.advance(dt_seconds);
	block_damage_.advance(dt_seconds);

	// An asynchronous sign-in (browser, form, token file) resolves between
	// frames; the server sends nothing while it waits.
	if (handshake_.status() == ClientHandshakeStatus::kSigningIn) {
		auto step = handshake_.poll();
		send_frames(transport_, conn_, step.send);
		if (step.failed) {
			failure_reason_ = step.failure_reason;
			return;
		}
	}

	if (reauth_ticket_) {
		TokenPoll p = reauth_ticket_();
		if (p.done) {
			reauth_ticket_ = nullptr;
			if (p.error.empty() && !p.token.empty() &&
					p.token.size() <= protocol::kMaxAuthTokenBytes) {
				send_message(transport_, conn_, protocol::C2SReauth{ std::move(p.token) });
			}
		}
	}

	scratch_.clear();
	transport_.poll(scratch_);

	for (auto &ev : scratch_) {
		if (ev.conn != conn_) {
			continue;
		}
		switch (ev.kind) {
			case TransportEvent::Kind::kConnected: {
				if (!started_) {
					started_ = true;
					send_frames(transport_, conn_, handshake_.start().send);
				}
				break;
			}
			case TransportEvent::Kind::kMessage: {
				std::size_t consumed = 0;
				auto frame = protocol::read_frame(span_of(ev.frame), consumed);
				if (!frame) {
					failure_reason_ = "malformed frame from server";
					return;
				}
				std::vector<std::byte> decompressed;
				const char *decompress_err = nullptr;
				if (!decompress_frame_payload(*frame, decompressed, decompress_err)) {
					failure_reason_ = decompress_err;
					return;
				}
				// Handled unconditionally (not through the handshake FSM's
				// strict per-state type checks nor gated on kJoined): on a
				// real transport this travels on a different lane than
				// JoinAccept with no cross-lane ordering guarantee, so it may
				// arrive just before or just after it.
				if (frame->header.type == protocol::MessageType::kS2CBlockRegistry) {
					if (auto m = protocol::S2CBlockRegistry::decode(frame->payload)) {
						apply_block_registry(*m);
					} else {
						VB_ERROR("net", "malformed S2C_BlockRegistry: ",
								core::message(m.error()));
					}
					break;
				}
				if (frame->header.type == protocol::MessageType::kS2CKeybindRegistry) {
					if (auto m = protocol::S2CKeybindRegistry::decode(frame->payload)) {
						apply_keybind_registry(*m);
					} else {
						VB_ERROR("net", "malformed S2C_KeybindRegistry: ",
								core::message(m.error()));
					}
					break;
				}
				if (frame->header.type == protocol::MessageType::kS2CEntityKindRegistry) {
					if (auto m = protocol::S2CEntityKindRegistry::decode(frame->payload)) {
						apply_entity_kind_registry(*m);
					} else {
						VB_ERROR("net", "malformed S2C_EntityKindRegistry: ",
								core::message(m.error()));
					}
					break;
				}
				if (frame->header.type == protocol::MessageType::kS2CMoveParams) {
					if (auto m = protocol::S2CMoveParams::decode(frame->payload)) {
						apply_move_params(*m);
					} else {
						VB_ERROR("net", "malformed S2C_MoveParams: ",
								core::message(m.error()));
					}
					break;
				}
				if (frame->header.type == protocol::MessageType::kS2CDayNightCurve) {
					if (auto m = protocol::S2CDayNightCurve::decode(frame->payload)) {
						apply_day_night_curve(*m);
					} else {
						VB_ERROR("net", "malformed S2C_DayNightCurve: ",
								core::message(m.error()));
					}
					break;
				}
				if (frame->header.type == protocol::MessageType::kS2CFogParams) {
					if (auto m = protocol::S2CFogParams::decode(frame->payload)) {
						apply_fog_params(*m);
					} else {
						VB_ERROR("net", "malformed S2C_FogParams: ",
								core::message(m.error()));
					}
					break;
				}
				if (frame->header.type == protocol::MessageType::kS2CReauthRequest &&
						handshake_.status() == ClientHandshakeStatus::kJoined) {
					if (auto m = protocol::S2CReauthRequest::decode(frame->payload)) {
						if (reauth_provider_) {
							reauth_ticket_ = reauth_provider_(*m);
						}
					}
					break;
				}
				if (handshake_.status() == ClientHandshakeStatus::kJoined &&
						apply_gameplay_frame(*frame)) {
					break;
				}
				// The server is already playing once it sends JoinAccept, so its
				// first gameplay frames can overtake a lost-and-resent JoinAccept
				// (different lanes, no cross-lane ordering). Hold them until we
				// have joined instead of failing the join over them -- and don't
				// drop them: the server never resends a chunk it believes sent.
				if (handshake_.status() == ClientHandshakeStatus::kSyncing &&
						frame->header.type != protocol::MessageType::kS2CJoinAccept) {
					if (early_gameplay_.size() >= kMaxEarlyGameplayFrames) {
						failure_reason_ = "too many messages before JoinAccept";
						return;
					}
					early_gameplay_.emplace_back(frame->header,
							std::vector<std::byte>(frame->payload.begin(), frame->payload.end()));
					break;
				}
				auto step = handshake_.on_frame(*frame);
				send_frames(transport_, conn_, step.send);
				if (step.failed) {
					failure_reason_ = step.failure_reason;
				}
				if (handshake_.status() == ClientHandshakeStatus::kJoined &&
						!early_gameplay_.empty()) {
					for (const auto &[header, payload] : early_gameplay_) {
						const protocol::Frame early{ header, payload };
						if (!apply_gameplay_frame(early)) {
							VB_WARN("net", "ignored early message type ",
									static_cast<unsigned>(header.type), " before JoinAccept");
						}
					}
					early_gameplay_.clear();
					early_gameplay_.shrink_to_fit();
				}
				break;
			}
			case TransportEvent::Kind::kDisconnected: {
				if (handshake_.status() != ClientHandshakeStatus::kJoined &&
						failure_reason_.empty()) {
					failure_reason_ =
							ev.reason.empty() ? "connection closed" : ev.reason;
				}
				break;
			}
		}
	}
}

bool ClientSession::apply_gameplay_frame(const protocol::Frame &frame) {
	using protocol::MessageType;
	switch (frame.header.type) {
		case MessageType::kS2CEntitySnapshot: {
			if (auto snap = protocol::S2CEntitySnapshot::decode(frame.payload)) {
				apply_snapshot(*snap);
			}
			return true;
		}
		case MessageType::kS2CChunkAdd: {
			if (auto m = protocol::S2CChunkAdd::decode(frame.payload)) {
				// A failure here used to be silently swallowed: the chunk would
				// never render (mesher sees it as unloaded) yet the server still
				// thinks it was sent (last_sent_ includes it), so it's never
				// retried -- a permanent, invisible hole with no trace of why.
				// Log it loudly so a report like that has something to go on.
				// A chunk re-sent while already loaded (its revision moved
				// outside the per-edit delta path) may have changed blocks
				// that block damage refers to.
				std::vector<core::BlockId> before;
				if (const world::Chunk *old = chunks_.find(m->coord)) {
					before.resize(world::kChunkVolume);
					for (std::size_t i = 0; i < world::kChunkVolume; ++i) {
						before[i] = old->blocks().get(i);
					}
				}
				if (auto applied = chunks_.apply_add(*m); !applied) {
					VB_ERROR("net", "chunk (", m->coord.x, ",", m->coord.y, ",",
							m->coord.z, ") add rejected: ",
							core::message(applied.error()));
				} else if (!before.empty()) {
					const world::Chunk *now = chunks_.find(m->coord);
					for (std::size_t i = 0; now != nullptr && i < world::kChunkVolume; ++i) {
						if (now->blocks().get(i) != before[i]) {
							block_damage_.on_block_changed(
									world_voxel_of(m->coord, i), m->revision);
						}
					}
				}
			} else {
				VB_ERROR("net", "malformed S2C_ChunkAdd: ", core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CChunkDelta: {
			if (auto m = protocol::S2CChunkDelta::decode(frame.payload)) {
				if (auto applied = chunks_.apply_delta(*m); !applied) {
					VB_ERROR("net", "chunk (", m->coord.x, ",", m->coord.y, ",",
							m->coord.z, ") delta rejected: ",
							core::message(applied.error()));
				} else {
					for (const protocol::BlockChange &c : m->blocks) {
						block_damage_.on_block_changed(
								world_voxel_of(m->coord, c.local_index), m->new_revision);
					}
				}
				forget_pending_edits_for(m->coord); // authoritative wins
			} else {
				VB_ERROR("net", "malformed S2C_ChunkDelta: ", core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CBlockEditResult: {
			if (auto m = protocol::S2CBlockEditResult::decode(frame.payload)) {
				handle_block_edit_result(*m);
			}
			return true;
		}
		case MessageType::kS2CChunkRemove: {
			if (auto m = protocol::S2CChunkRemove::decode(frame.payload)) {
				chunks_.apply_remove(*m);
				// A chunk leaving this client's view means the server has
				// stopped broadcasting S2C_BlockDamage for it (broadcast_
				// block_damage() only reaches players who currently mirror
				// the chunk) -- drop any stale entries now so a re-entered
				// chunk never starts showing a leftover crack from before it
				// was last seen.
				block_damage_.on_chunk_removed(m->coord);
			}
			return true;
		}
		case MessageType::kS2COpenUi: {
			if (auto m = protocol::S2COpenUi::decode(frame.payload)) {
				pending_open_ui_ = std::move(*m);
			} else {
				VB_ERROR("net", "malformed S2C_OpenUi: ", core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CChat: {
			if (auto m = protocol::S2CChat::decode(frame.payload)) {
				pending_chat_.push_back(std::move(m->text));
			} else {
				VB_ERROR("net", "malformed S2C_Chat: ", core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CTimeOfDay: {
			if (auto m = protocol::S2CTimeOfDay::decode(frame.payload)) {
				time_of_day_override_ = m->time_of_day;
			} else {
				VB_ERROR("net", "malformed S2C_TimeOfDay: ",
						core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CPlayerList: {
			if (auto m = protocol::S2CPlayerList::decode(frame.payload)) {
				players_.clear();
				for (auto &p : m->players) {
					players_[p.net_id] = std::move(p.name);
				}
			} else {
				VB_ERROR("net", "malformed S2C_PlayerList: ",
						core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CPlayerJoin: {
			if (auto m = protocol::S2CPlayerJoin::decode(frame.payload)) {
				pending_chat_.push_back("* " + m->name + " joined the game");
				players_[m->net_id] = std::move(m->name);
			} else {
				VB_ERROR("net", "malformed S2C_PlayerJoin: ",
						core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CInventory: {
			if (auto m = protocol::S2CInventory::decode(frame.payload)) {
				inventory_ = std::move(m->slots);
			} else {
				VB_ERROR("net", "malformed S2C_Inventory: ",
						core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CPlayerStatus: {
			if (auto m = protocol::S2CPlayerStatus::decode(frame.payload)) {
				player_status_ = *m;
			} else {
				VB_ERROR("net", "malformed S2C_PlayerStatus: ",
						core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CEntityProps: {
			if (auto m = protocol::S2CEntityProps::decode(frame.payload)) {
				apply_entity_props(*m);
			} else {
				VB_ERROR("net", "malformed S2C_EntityProps: ",
						core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CBlockDamage: {
			if (auto m = protocol::S2CBlockDamage::decode(frame.payload)) {
				apply_block_damage(*m);
			} else {
				VB_ERROR("net", "malformed S2C_BlockDamage: ",
						core::message(m.error()));
			}
			return true;
		}
		case MessageType::kS2CPlayerLeave: {
			if (auto m = protocol::S2CPlayerLeave::decode(frame.payload)) {
				auto it = players_.find(m->net_id);
				const std::string name = it != players_.end() ? it->second : "player";
				if (it != players_.end()) {
					players_.erase(it);
				}
				pending_chat_.push_back("* " + name + " left the game");
			} else {
				VB_ERROR("net", "malformed S2C_PlayerLeave: ",
						core::message(m.error()));
			}
			return true;
		}
		default:
			return false;
	}
}

std::optional<protocol::S2COpenUi> ClientSession::take_open_ui() {
	std::optional<protocol::S2COpenUi> out = std::move(pending_open_ui_);
	pending_open_ui_.reset();
	return out;
}

std::vector<std::string> ClientSession::take_chat_messages() {
	std::vector<std::string> out = std::move(pending_chat_);
	pending_chat_.clear();
	return out;
}

void ClientSession::apply_block_registry(const protocol::S2CBlockRegistry &msg) {
	world::BlockRegistry reg;
	for (const auto &b : msg.blocks) {
		// Real bug fixed here (same class as src/server/main.cpp's and
		// src/client/main.cpp's own copy of it): this aggregate-init used to
		// list only the first 6 of BlockType's fields, so max_damage/
		// crack_texture never survived onto the client's own mirrored
		// registry even after S2CBlockRegistry itself started carrying them.
		reg.add({ b.name, b.solid, b.opaque, b.liquid, b.light_emission, b.texture,
				b.max_damage, b.crack_texture });
	}
	VB_INFO("net", "received block registry (", msg.blocks.size(), " blocks)");
	chunks_.set_registry(std::move(reg));
}

void ClientSession::apply_keybind_registry(const protocol::S2CKeybindRegistry &msg) {
	keybind_names_ = msg.names;
	VB_INFO("net", "received keybind registry (", msg.names.size(), " keybinds)");
}

void ClientSession::apply_entity_kind_registry(
		const protocol::S2CEntityKindRegistry &msg) {
	entity_kinds_ = msg.kinds;
	VB_INFO("net", "received entity kind registry (", msg.kinds.size(), " kinds)");
}

void ClientSession::apply_move_params(const protocol::S2CMoveParams &msg) {
	physics::MoveParams p;
	p.half_width = msg.half_width;
	p.height = msg.height;
	p.eye_height = msg.eye_height;
	p.walk_speed = msg.walk_speed;
	p.sprint_speed = msg.sprint_speed;
	p.accel = msg.accel;
	p.air_accel = msg.air_accel;
	p.friction = msg.friction;
	p.gravity = msg.gravity;
	p.jump_speed = msg.jump_speed;
	p.terminal_velocity = msg.terminal_velocity;
	p.step_height = msg.step_height;
	p.fly_speed = msg.fly_speed;
	p.fly = msg.fly;
	move_params_ = p;
	VB_INFO("net", "received move params (gravity=", p.gravity, ")");
}

void ClientSession::apply_day_night_curve(const protocol::S2CDayNightCurve &msg) {
	world::DayNightCurve curve;
	curve.keyframes.reserve(msg.keyframes.size());
	for (const auto &k : msg.keyframes) {
		curve.keyframes.push_back(
				{ k.tick, k.brightness, world::SkyColor{ k.r, k.g, k.b } });
	}
	VB_INFO("net", "received day/night curve (", curve.keyframes.size(),
			" keyframes)");
	day_night_curve_ = std::move(curve);
}

void ClientSession::apply_fog_params(const protocol::S2CFogParams &msg) {
	VB_INFO("net", "received fog params (start=", msg.fog_start,
			", end=", msg.fog_end, ")");
	fog_override_ = msg;
}

void ClientSession::apply_block_damage(const protocol::S2CBlockDamage &msg) {
	block_damage_.on_damage(msg.pos, msg.punches, msg.revision);
}

void ClientSession::apply_entity_props(const protocol::S2CEntityProps &msg) {
	for (const auto &u : msg.updates) {
		ReceivedEntityProps &p = entity_props_[u.net_id];
		p.server_tick = msg.server_tick;
		if (u.text) {
			p.text = *u.text;
		}
		if (u.clip) {
			p.clip = *u.clip;
		}
		if (u.attach) {
			p.attach = *u.attach;
		}
		if (u.visual) {
			p.visual = *u.visual;
		}
		if (!p.text && !p.clip && !p.attach && !p.visual) {
			entity_props_.erase(u.net_id);
		}
	}
}

void ClientSession::apply_snapshot(const protocol::S2CEntitySnapshot &snap) {
	last_server_tick_ = snap.server_tick;

	// Wall-clock server-time estimate (REMAINING_TASKS.md Phase 3): the
	// real server tick rate replicated at join (S2CServerInfo, default 20
	// if that hasn't arrived yet for some reason) converts this snapshot's
	// tick to seconds; half the transport's round-trip time (0 if unknown,
	// e.g. LoopbackTransport) estimates how long it's already been in
	// flight, so the implied "true" server time is as-of-now, not as-of-
	// when-it-was-sent.
	const double tick_rate =
			server_info() && server_info()->tick_rate > 0 ? server_info()->tick_rate : 20.0;
	const double one_way_latency =
			transport_.round_trip_time_seconds(conn_).value_or(0.0) / 2.0;
	server_time_.on_snapshot(
			static_cast<double>(snap.server_tick) / tick_rate, one_way_latency);

	const auto ingest = [&](const protocol::EntityRecord &r) {
		remote_[r.net_id] = r;
		if (r.visual_override) {
			entity_visual_overrides_[r.net_id] = *r.visual_override;
		}
		if (r.item) {
			entity_items_[r.net_id] = *r.item;
		}
		auto [it, inserted] = net_to_entity_.try_emplace(r.net_id, entt::null);
		if (inserted) {
			it->second = entity_registry_.create();
		}
		const entt::entity entity = it->second;
		entity_registry_.emplace_or_replace<ecs::EntityKind>(entity, r.kind);
		ecs::InterpBuffer &s =
				entity_registry_.get_or_emplace<ecs::InterpBuffer>(entity);
		if (s.cur_tick == 0) {
			s.prev_pos = r.pos;
			s.prev_yaw = r.rot.x;
			s.prev_tick = snap.server_tick;
		} else if (s.cur_tick != snap.server_tick) {
			s.prev_pos = s.cur_pos;
			s.prev_yaw = s.cur_yaw;
			s.prev_tick = s.cur_tick;
		}
		s.cur_pos = r.pos;
		s.cur_yaw = r.rot.x;
		s.cur_tick = snap.server_tick;
	};
	for (const auto &r : snap.entered) {
		ingest(r);
	}
	for (const auto &r : snap.updated) {
		ingest(r);
	}
	for (core::NetId id : snap.removed) {
		remote_.erase(id);
		entity_visual_overrides_.erase(id);
		entity_items_.erase(id);
		// Properties sent at or after this snapshot's tick belong to a later
		// re-entry that overtook this (unreliable) removal -- keep them.
		if (const auto it = entity_props_.find(id);
				it != entity_props_.end() && it->second.server_tick < snap.server_tick) {
			entity_props_.erase(it);
		}
		if (const auto it = net_to_entity_.find(id); it != net_to_entity_.end()) {
			entity_registry_.destroy(it->second);
			net_to_entity_.erase(it);
		}
	}

	if (snap.has_local) {
		local_kind_ = snap.local.kind;
		reconcile(snap.local, snap.last_acked_input_seq);
	}
}

void ClientSession::reconcile(const protocol::EntityRecord &authoritative,
		std::uint32_t acked_seq) {
	last_acked_seq_ = acked_seq;
	predicted_.position = authoritative.pos;
	predicted_.velocity = core::Vec3d{ static_cast<double>(authoritative.vel.x),
		static_cast<double>(authoritative.vel.y),
		static_cast<double>(authoritative.vel.z) };
	predicted_.on_ground = (authoritative.flags & 1u) != 0;

	history_.erase(std::remove_if(history_.begin(), history_.end(),
						   [acked_seq](const protocol::InputCmd &c) {
							   return c.seq <= acked_seq;
						   }),
			history_.end());

	for (const auto &c : history_) {
		predicted_ = physics::step_movement(predicted_, to_move_input(c),
				move_params_, chunks_);
	}
}

void ClientSession::push_input(const protocol::InputCmd &cmd) {
	if (!joined()) {
		return;
	}
	// Mirror the server's freeze while the local chunk mirror doesn't have the
	// spawn column yet -- otherwise the client predicts its own independent
	// fall into empty space and gets snapped back once a reconcile catches up,
	// which looks like falling through the world even when the server itself
	// never actually let the player move (see ServerSession::handle_input_batch).
	if (physics::ground_area_loaded(predicted_.position,
				[this](core::ChunkCoord c) { return chunks_.has(c); })) {
		predicted_ = physics::step_movement(predicted_, to_move_input(cmd),
				move_params_, chunks_);
	}
	history_.push_back(cmd);
	while (history_.size() > protocol::C2SInputBatch::kMaxCmds) {
		history_.erase(history_.begin());
	}
	protocol::C2SInputBatch batch;
	batch.cmds = history_;
	send_message(transport_, conn_, batch);
}

void ClientSession::push_block_edit(const protocol::C2SBlockEdit &edit) {
	if (!joined()) {
		return;
	}
	const core::BlockId applied = edit.action == protocol::BlockEditAction::kBreak
			? core::BlockId::kAir
			: edit.block;
	const core::BlockId prev = chunks_.edit_block(edit.pos, applied);
	pending_edits_.push_back(
			{ edit.predicted_seq, edit.pos, prev, core::chunk_of(edit.pos) });
	send_message(transport_, conn_, edit);
}

void ClientSession::handle_block_edit_result(
		const protocol::S2CBlockEditResult &res) {
	for (auto it = pending_edits_.begin(); it != pending_edits_.end(); ++it) {
		if (it->seq != res.predicted_seq) {
			continue;
		}
		if (!res.accepted) {
			// Roll the optimistic apply back; the authoritative delta (if any)
			// will still correct light on accept.
			chunks_.edit_block(it->pos, it->prev);
		}
		pending_edits_.erase(it);
		return;
	}
}

void ClientSession::forget_pending_edits_for(core::ChunkCoord coord) {
	std::erase_if(pending_edits_,
			[coord](const PendingEdit &e) { return e.coord == coord; });
}

core::Vec3d ClientSession::interpolated_pos(core::NetId id) const {
	const auto net_it = net_to_entity_.find(id);
	if (net_it == net_to_entity_.end() ||
			!entity_registry_.all_of<ecs::InterpBuffer>(net_it->second)) {
		const auto r = remote_.find(id);
		return r == remote_.end() ? core::Vec3d{} : r->second.pos;
	}
	const ecs::InterpBuffer &s = entity_registry_.get<ecs::InterpBuffer>(net_it->second);
	if (s.cur_tick <= s.prev_tick) {
		return s.cur_pos;
	}
	// Render ~1 tick behind the newest sample (spec §8.4 interpolation
	// delay), at a continuously-advancing wall-clock estimate of "now" in
	// server ticks rather than the last *received* tick number -- using
	// last_server_tick_ directly here used to freeze `a` solid between
	// snapshot arrivals (it only changes when a new packet lands), so a
	// remote entity only ever appeared to move on the frames a snapshot
	// happened to arrive, not every render frame. server_time_est_seconds()
	// (fed every tick() call, snapshot or not -- ServerTimeEstimator's own
	// header comment) keeps `a` progressing smoothly in between too.
	const double span = static_cast<double>(s.cur_tick - s.prev_tick);
	const double tick_rate =
			server_info() && server_info()->tick_rate > 0 ? server_info()->tick_rate : 20.0;
	const double now_ticks = server_time_.primed()
			? server_time_.estimate_seconds() * tick_rate
			: static_cast<double>(last_server_tick_);
	const double target = now_ticks - 1.0 - static_cast<double>(s.prev_tick);
	const double a = core::clamp(span > 0.0 ? target / span : 1.0, 0.0, 1.0);
	return s.prev_pos + (s.cur_pos - s.prev_pos) * a;
}

} // namespace vb::net
