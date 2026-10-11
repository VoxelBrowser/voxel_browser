#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"
#include "vb/protocol/handshake.hpp" // Decoded<>
#include "vb/protocol/message.hpp"

// World replication messages (spec §8.5), lane 1. The chunk payload is an
// opaque blob produced by vb::world::encode_chunk_payload — the protocol layer
// stays independent of the voxel data model.

namespace vb::protocol {

// --- block registry (spec §8.3 / §9) ------------------------------------
// Sent between C2S_Ready and S2C_JoinAccept (Phase 4.3) so the client can
// mirror the server's (possibly Lua-extended) block table. No model/
// collision-shape fields exist here yet.

struct BlockRegistryRecord {
	std::string name;
	bool solid = true;
	bool opaque = true;
	bool liquid = false;
	std::uint8_t light_emission = 0;
	// Real texture/atlas system: mirrors vb::world::BlockType::texture --
	// pack-relative path, empty = no texture. The client resolves it against
	// its own Asset Sync virtual FS (vb::assetsync::ClientAssetCache), same
	// posture as every other BlockType field mirrored here.
	std::string texture;
	// Phase 6.5 (spec §10.7): 0 = instant break, no shared damage pool.
	std::uint16_t max_damage = 0;
	// Phase 6.5 (spec §10.7/§5.2): mirrors vb::world::BlockType::crack_texture
	// -- pack-relative crack-stage spritesheet path, empty = the client's own
	// built-in generic crack overlay.
	std::string crack_texture;

	bool operator==(const BlockRegistryRecord &) const = default;
};

struct S2CBlockRegistry {
	static constexpr MessageType kType = MessageType::kS2CBlockRegistry;

	std::vector<BlockRegistryRecord> blocks; // index == BlockId

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CBlockRegistry> decode(std::span<const std::byte> in);
};

// --- physics parameters (spec §7.3, Phase 6.7) --------------------------
// vb::physics::MoveParams's own comment says "Engine defaults; a Lua pack
// overrides per entity kind" -- mirrored here flat (no dependency from
// protocol/ onto physics/, same posture as BlockRegistryRecord mirroring
// BlockType) so the client's prediction can use the exact tunables the
// server's authoritative simulation does, instead of silently drifting from
// vb::physics::MoveParams's own hardcoded defaults. Sent between C2S_Ready
// and S2C_JoinAccept alongside S2C_BlockRegistry/S2C_KeybindRegistry.
struct S2CMoveParams {
	static constexpr MessageType kType = MessageType::kS2CMoveParams;

	double half_width = 0.4;
	double height = 1.8;
	double eye_height = 1.62;
	double walk_speed = 4.5;
	double sprint_speed = 7.0;
	double accel = 45.0;
	double air_accel = 10.0;
	double friction = 12.0;
	double gravity = 28.0;
	double jump_speed = 8.9;
	double terminal_velocity = 60.0;
	double step_height = 1.05;
	double fly_speed = 12.0;
	bool fly = false;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CMoveParams> decode(std::span<const std::byte> in);
};

// --- day/night curve (spec §5.4, Phase 6.8) -----------------------------
// Mirrors vb::world::DayNightKeyframe flat, same posture as
// BlockRegistryRecord/S2CMoveParams (protocol/ never depends on world/).
// Sent between C2S_Ready and S2C_JoinAccept alongside S2C_BlockRegistry/
// S2C_MoveParams only when a pack overrides the default curve via
// vb.daynight.set_curve{...}; no frame at all keeps every client on
// vb::world::default_day_night_curve().
struct DayNightKeyframeRecord {
	std::uint32_t tick = 0;
	double brightness = 1.0;
	std::uint8_t r = 0;
	std::uint8_t g = 0;
	std::uint8_t b = 0;

	bool operator==(const DayNightKeyframeRecord &) const = default;
};

struct S2CDayNightCurve {
	static constexpr MessageType kType = MessageType::kS2CDayNightCurve;

	std::vector<DayNightKeyframeRecord> keyframes;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CDayNightCurve> decode(std::span<const std::byte> in);
};

// --- distance fog (spec §7.2, Phase 7.2) --------------------------------
// A pack's `vb.render.set_fog{start=, end=}` override, sent the same
// opt-in way as S2C_DayNightCurve/S2C_MoveParams above: `nullopt` from
// HandshakeServerHost::fog_params sends no frame at all, and the client
// falls back to its own engine default (fog distance matching its own
// view_distance config) -- there IS no server-side universal default to
// send unprompted, since the server doesn't know each client's
// view_distance. No color field -- fog always reads as "distance to the
// current sky color" (vb::world::sky_color_for_time()), never an
// independently drifting tint (decided 2026-09-19). That decision was later
// scoped down to *above-water* fog only (Phase 7.5, decided 2026-09-23):
// underwater fog tints from the submerged liquid block's own texture
// average by default, and `underwater_tint` below lets a pack override that
// one specific color -- `has_underwater_tint = false` means "no override,
// client picks its own texture-average/placeholder default", not "black".
struct S2CFogParams {
	static constexpr MessageType kType = MessageType::kS2CFogParams;

	float fog_start = 0.0f;
	float fog_end = 0.0f;
	bool has_underwater_tint = false;
	std::uint8_t underwater_tint_r = 0;
	std::uint8_t underwater_tint_g = 0;
	std::uint8_t underwater_tint_b = 0;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CFogParams> decode(std::span<const std::byte> in);
};

// --- entity kind registry (spec §11.3, entity management follow-up) -----
// Sent between C2S_Ready and S2C_JoinAccept alongside S2C_BlockRegistry/
// S2C_KeybindRegistry so the client can render script entities distinctly
// per kind instead of one flat placeholder for every EntityRecord::kind.
// `kinds[i]` describes `core::EntityKindId` value `i + 1` -- index-to-id
// mapping matches PackRuntime::Impl::entity_kinds' own assignment
// (`id = entity_kinds.size() + 1` at registration time), so no id needs to
// be sent per record. A player's EntityRecord::kind is always kInvalid (0),
// never indexes into this list.
// One animation clip's slice of a kind's spritesheet -- a column run within
// a pose row. `clip` is matched by name client-side against
// render::anim_clip_name(AnimClip) (entity-management follow-up to Phase
// 3.5/4.2, spec architecture_spec/rendering.md §11.3's 2026-09-17 schema).
struct EntityClipDef {
	std::string clip;
	std::uint16_t frames = 1;
	float fps = 1.0f;

	bool operator==(const EntityClipDef &) const = default;
};

// One extra sheet drawn in the same billboard as a visual's base texture
// (protocol v32; paper-doll clothing). It shares the base sheet's layout --
// frame size, facings, mirror, clips -- and is drawn with exactly the base
// frame's source rectangle, so body and clothes can never drift apart. The
// client skips (with a warning) a layer whose sheet size differs from the
// base sheet's.
struct EntityVisualLayer {
	std::string texture; // pack-relative path
	bool below = false; // draw before (behind) the base sheet
	// Bit i set = only drawn on pose row i (rows are the sheet's facing
	// rows, 0 = front); 0 = every row. Lets one cape sheet sit behind the
	// body on front views and over it on back views.
	std::uint8_t rows = 0;
	std::array<std::uint8_t, 4> tint{ 255, 255, 255, 255 }; // multiplies the sheet

	bool operator==(const EntityVisualLayer &) const = default;
};

inline constexpr std::size_t kMaxEntityVisualLayers = 16;

// Kind-level default spritesheet, `vb.register_entity{visual = {...}}`
// (rendering.md §11.3). `frame_width`/`frame_height` come from the pack's
// chosen named variant (resolved server-side at registration, see
// PackRuntime's register_entity binding) -- only the resolved pixel size
// travels the wire, not the variant name itself. `facings` is 4 or 8,
// validated at registration. `mirror` (default true, matching the original
// schema before this field existed) selects the pose-row count: `true`
// derives it as `facings/2 + 1` and the engine mirrors the missing half
// horizontally at render time (one authored "side" pose serves both left and
// right); `false` requires a fully authored row per facing (`rows ==
// facings`, e.g. 4 distinct rows for facings=4: front/right/back/left) and
// the engine never flips anything. `origin_x`/`origin_y` are the normalized
// feet/anchor point within a frame. The required sheet size
// (`frame_width * sum(clip frames)` by `frame_height * rows`) is validated
// against the real decoded PNG client-side (render::build_entity_visual_
// layout), not here -- this struct only carries the pack's declared intent.
struct EntityVisualDef {
	std::string texture; // pack-relative path, synced like any other asset
	std::uint16_t frame_width = 0;
	std::uint16_t frame_height = 0;
	std::uint8_t facings = 8;
	float origin_x = 0.5f;
	float origin_y = 1.0f;
	bool mirror = true;
	std::vector<EntityClipDef> clips;
	// Protocol v32. Sprites that overlap draw in `layer` order: each step
	// nudges the billboard 0.02 blocks toward the camera, so a layer 1 sprite
	// at a player's exact position always wins the depth test against it
	// (layer -1 always loses). Range [-8, 8], validated by the binding.
	std::int8_t layer = 0;
	// Skip the depth test so terrain never hides the sprite (markers).
	bool through_walls = false;
	// Extra sheets drawn in this billboard, in order (protocol v32).
	std::vector<EntityVisualLayer> layers;

	bool operator==(const EntityVisualDef &) const = default;
};

// Per-instance override merged over a script entity's kind-level
// EntityVisualDef (architecture_spec/rendering.md §11.3's "Per-instance
// override" -- e.g. a player-skin pack overriding just `texture` while
// `facings`/`origin`/`clips` inherit the kind's own `visual` unchanged).
// Every field is independently optional; an unset field inherits the kind
// default. `frame_width`/`frame_height` travel together (both set or
// neither) since a pack always resolves them from one `variant` name
// server-side, same as `EntityVisualDef` itself -- there's no sensible way to
// override just one half of that pair; `origin_x`/`origin_y` are coupled the
// same way. See render::merge_visual_override (vb/render/
// entity_visual_layout.hpp) for how this is applied client-side.
struct EntityVisualOverride {
	std::optional<std::string> texture;
	std::optional<std::uint16_t> frame_width;
	std::optional<std::uint16_t> frame_height;
	std::optional<std::uint8_t> facings;
	std::optional<float> origin_x;
	std::optional<float> origin_y;
	std::optional<bool> mirror;
	std::optional<std::vector<EntityClipDef>> clips;
	std::optional<std::int8_t> layer; // v32
	std::optional<bool> through_walls; // v32
	std::optional<std::vector<EntityVisualLayer>> layers; // v32; replaces the kind's list

	bool operator==(const EntityVisualOverride &) const = default;
};

struct EntityKindRegistryRecord {
	std::string name;
	// Billboard footprint, metres -- mirrors render::EntityRenderer's
	// placeholder quad dimensions (width x height) until real per-kind
	// sprite art (`visual` below) is set; used for a kind that never sets one.
	float width = 0.8f;
	float height = 1.8f;
	// Real per-kind spritesheet -- absent (nullopt) for a kind that never set
	// `vb.register_entity{visual = {...}}` (e.g. kitchen_sink:sentry, which
	// only sets width/height), same "missing = default" posture as every
	// other opt-in registry field in this codebase.
	std::optional<EntityVisualDef> visual;
	// `vb.register_entity{visual = false}` (protocol v32): draw no sprite and
	// no placeholder quad at all -- for a text-only entity whose whole look
	// is its S2C_EntityProps label.
	bool hidden = false;

	bool operator==(const EntityKindRegistryRecord &) const = default;
};

struct S2CEntityKindRegistry {
	static constexpr MessageType kType = MessageType::kS2CEntityKindRegistry;

	std::vector<EntityKindRegistryRecord> kinds; // index == EntityKindId - 1

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CEntityKindRegistry> decode(std::span<const std::byte> in);
};

struct S2CChunkAdd {
	static constexpr MessageType kType = MessageType::kS2CChunkAdd;

	core::ChunkCoord coord{};
	std::uint64_t revision = 0;
	std::vector<std::byte> payload; // world::encode_chunk_payload output

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CChunkAdd> decode(std::span<const std::byte> in);
};

struct BlockChange {
	std::uint32_t local_index = 0; // 0 .. CHUNK_VOLUME-1
	core::BlockId block = core::BlockId::kAir;

	bool operator==(const BlockChange &) const = default;
};

struct LightChange {
	std::uint32_t local_index = 0;
	std::uint8_t packed = 0;

	bool operator==(const LightChange &) const = default;
};

struct S2CChunkDelta {
	static constexpr MessageType kType = MessageType::kS2CChunkDelta;

	core::ChunkCoord coord{};
	std::uint64_t base_revision = 0;
	std::uint64_t new_revision = 0;
	std::vector<BlockChange> blocks;
	std::vector<LightChange> light;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CChunkDelta> decode(std::span<const std::byte> in);
};

struct S2CChunkRemove {
	static constexpr MessageType kType = MessageType::kS2CChunkRemove;

	core::ChunkCoord coord{};

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CChunkRemove> decode(std::span<const std::byte> in);
};

// --- block editing (spec §8.5 / §5.2) -----------------------------------

enum class BlockEditAction : std::uint8_t {
	kBreak = 0, // remove the block at `pos`
	kPlace = 1, // fill the (empty) cell at `pos` with `block`
};
constexpr bool valid(BlockEditAction a) {
	return a == BlockEditAction::kBreak || a == BlockEditAction::kPlace;
}

struct C2SBlockEdit {
	static constexpr MessageType kType = MessageType::kC2SBlockEdit;

	std::uint32_t predicted_seq = 0; // client's optimistic-apply id
	BlockEditAction action = BlockEditAction::kBreak;
	core::IVec3 pos{}; // world voxel coordinate
	core::BlockId block = core::BlockId::kAir; // kPlace only

	void encode(std::vector<std::byte> &out) const;
	static Decoded<C2SBlockEdit> decode(std::span<const std::byte> in);
};

struct S2CBlockEditResult {
	static constexpr MessageType kType = MessageType::kS2CBlockEditResult;

	std::uint32_t predicted_seq = 0;
	bool accepted = false; // false -> client rolls its optimistic apply back
	core::IVec3 pos{};

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CBlockEditResult> decode(std::span<const std::byte> in);
};

// --- live block-damage replication (Phase 6.5's deferred half, closed by
// 6.18's punch system) ----------------------------------------------------
// Broadcast by ServerSession whenever a block's punch count
// (ServerSession::block_punch_counts_, driven by punch()) changes, to every
// player who currently mirrors the chunk containing `pos`
// (WorldReplicator::player_has_chunk) -- not just the puncher. `punches ==
// 0` means "no damage" (fully healed, or the block just broke): the
// receiving client drops any crack-overlay state for `pos` rather than
// treating 0 as a value to render. No max_damage field -- the receiving
// client already knows the block's registered BlockType::max_damage from
// its own chunk mirror + block registry, so this stays a pure delta.
//
// Travels on Lane::kFeedback, so it can arrive before or after the
// S2C_ChunkDelta/ChunkAdd for the same block. `revision` is the server's
// revision of the chunk containing `pos` when this was sent; the client
// drops it if the block at `pos` has changed at a later revision
// (net::BlockDamageTracker).
struct S2CBlockDamage {
	static constexpr MessageType kType = MessageType::kS2CBlockDamage;

	core::IVec3 pos{};
	std::uint16_t punches = 0;
	std::uint64_t revision = 0; // v31+

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CBlockDamage> decode(std::span<const std::byte> in);
};

// --- shared block-damage breaking (spec §10.7) --------------------------
// Brackets a player holding a target: begin starts (or refreshes) them as a
// contributor to that pos's damage pool, stop drops them. The damage state
// itself, and its completion into an actual break, ride the *existing*
// C2S_BlockEdit/BlockEditSystem pipeline server-side -- these two messages
// only mark who's currently holding, nothing more.

struct C2SBlockBreakBegin {
	static constexpr MessageType kType = MessageType::kC2SBlockBreakBegin;

	core::IVec3 pos{}; // world voxel coordinate of the target
	core::IVec3 face{}; // hit-face normal (from the client's own raycast)

	void encode(std::vector<std::byte> &out) const;
	static Decoded<C2SBlockBreakBegin> decode(std::span<const std::byte> in);
};

struct C2SBlockBreakStop {
	static constexpr MessageType kType = MessageType::kC2SBlockBreakStop;

	core::IVec3 pos{};

	void encode(std::vector<std::byte> &out) const;
	static Decoded<C2SBlockBreakStop> decode(std::span<const std::byte> in);
};

// --- day/night (spec §5.4) ------------------------------------------------
// Periodic update of the server's time_of_day clock (whose initial value is
// already carried by S2C_JoinAccept); keeps already-connected clients' sky
// in sync as the server's clock advances. See vb::world::daynight.hpp for
// the tick semantics (0 = sunrise, wraps at kTicksPerDay).
struct S2CTimeOfDay {
	static constexpr MessageType kType = MessageType::kS2CTimeOfDay;

	std::uint32_t time_of_day = 0;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CTimeOfDay> decode(std::span<const std::byte> in);
};

} // namespace vb::protocol
