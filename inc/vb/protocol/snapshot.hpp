#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vb/core/ids.hpp"
#include "vb/core/math.hpp"
#include "vb/protocol/handshake.hpp" // Decoded<>
#include "vb/protocol/message.hpp"
#include "vb/protocol/world.hpp" // EntityVisualOverride

// S2C_EntitySnapshot (spec §8.4) — lane 2, unreliable, one per server tick.
// Carries newly-visible entities in full (`entered`), position/rotation deltas
// for still-visible ones (`updated`), and ids that left the interest set
// (`removed`). See docs/replication.md.

namespace vb::protocol {

struct EntityRecord {
	core::NetId net_id = core::NetId::kInvalid;
	core::EntityKindId kind = core::EntityKindId::kInvalid;
	core::Vec3d pos{};
	core::Vec2f rot{}; // yaw, pitch (degrees)
	core::Vec3f vel{};
	std::uint8_t flags = 0;
	// Entity-management follow-up (spec architecture_spec/rendering.md
	// §11.3's "Per-instance override"): a script entity's `vb.world.spawn`
	// `visual_override` option, learned once and cached client-side
	// (ClientSession's own entity_visual_overrides_ map) rather than resent
	// every tick. Only ever populated on an `entered` record -- ServerSession
	// only attaches it there (see net::ServerSession::to_record); `updated`/
	// `local` records always leave this nullopt, which means "unchanged", not
	// "cleared" (there is no clear path yet -- the override is fixed for the
	// entity's whole replicated lifetime, same as `kind`). Absent entirely
	// (the common case) costs one bool on the wire.
	std::optional<EntityVisualOverride> visual_override;
	// The block id a dropped-item entity represents, so the client can draw
	// it as that block. Same "entered records only, nullopt = unchanged"
	// contract as `visual_override` above.
	std::optional<std::uint16_t> item;

	bool operator==(const EntityRecord &) const = default;
};

struct S2CEntitySnapshot {
	static constexpr MessageType kType = MessageType::kS2CEntitySnapshot;

	std::uint32_t server_tick = 0;
	std::uint32_t last_acked_input_seq = 0; // highest InputCmd seq simulated
	std::vector<EntityRecord> entered;
	std::vector<EntityRecord> updated;
	std::vector<core::NetId> removed;

	// The recipient's own authoritative state (interest culling excludes self,
	// so it is carried separately for client-side reconciliation, spec §8.4).
	bool has_local = false;
	EntityRecord local{};

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CEntitySnapshot> decode(std::span<const std::byte> in);
};

// A world-space text label on a script entity (protocol v32;
// `vb.world.spawn(kind, pos, {text = ...})` / `entity:set_text(...)`). Always
// fully resolved server-side (kind default + per-call fields merged by
// PackRuntime), so the client never has to know a kind's text defaults.
inline constexpr std::size_t kMaxEntityTextBytes = 64;

struct EntityText {
	std::string value; // UTF-8, <= kMaxEntityTextBytes, '\n' starts a new line
	std::array<std::uint8_t, 4> color{ 255, 255, 255, 255 }; // RGBA
	// Rounded panel drawn behind the text; nullopt draws a dark outline
	// around the glyphs instead, so the label stays legible either way.
	std::optional<std::array<std::uint8_t, 4>> background;
	float size = 0.3f; // height of one text line, metres
	float offset_y = 2.05f; // metres above the entity's position (its anchor)
	float max_distance = 0.0f; // hide beyond this many metres; 0 = no limit
	bool through_walls = false; // skip the depth test (name tags)

	bool operator==(const EntityText &) const = default;
};

// `entity:attach_to(parent, {offset=, face_offset=, layer=})`: the client
// draws the entity at its parent's interpolated position plus `offset`, so
// the two move together without the follower lagging a tick behind; the
// server moves it the same way each tick so interest culling and
// entity:get_pos() agree. With `face_offset`, `offset` is in the parent's
// frame (x = right, z = forward at yaw 0 is -Z) and turns with its yaw.
struct EntityAttachment {
	core::NetId parent = core::NetId::kInvalid;
	core::Vec3f offset{};
	bool face_offset = false;
	std::optional<std::int8_t> layer; // overrides the visual's layer while attached

	bool operator==(const EntityAttachment &) const = default;
};

// World-space offset of an attachment for a parent facing `parent_yaw_deg`
// (same yaw convention as physics::wish_dir_from_local: yaw 0 faces -Z).
inline core::Vec3d attachment_world_offset(const EntityAttachment &a, float parent_yaw_deg) {
	const double x = a.offset.x;
	const double y = a.offset.y;
	const double z = a.offset.z;
	if (!a.face_offset) {
		return { x, y, z };
	}
	const double yaw = static_cast<double>(parent_yaw_deg) * 3.14159265358979323846 / 180.0;
	const double sy = std::sin(yaw);
	const double cy = std::cos(yaw);
	return { cy * x - sy * z, y, sy * x + cy * z };
}

// Bit per field in EntityPropsUpdate (and on the wire).
enum EntityPropField : std::uint8_t {
	kEntityPropText = 1u << 0,
	kEntityPropClip = 1u << 1,
	kEntityPropAttach = 1u << 2,
	kEntityPropAll = kEntityPropText | kEntityPropClip | kEntityPropAttach,
};

inline constexpr std::size_t kMaxEntityClipNameBytes = 64;

// One entity's changed properties. An outer nullopt means "unchanged"; an
// engaged outer holding an empty inner optional means "cleared".
struct EntityPropsUpdate {
	core::NetId net_id = core::NetId::kInvalid;
	std::optional<std::optional<EntityText>> text;
	std::optional<std::optional<std::string>> clip; // entity:set_clip(name)
	std::optional<std::optional<EntityAttachment>> attach;

	bool operator==(const EntityPropsUpdate &) const = default;
};

// S2C_EntityProps (protocol v32) -- lane kFeedback (reliable ordered), at
// most one per player per server tick. Carries the pack-set, changeable
// properties of script entities that don't belong in the per-tick snapshot:
// every property of an entity that entered the recipient's interest set this
// tick (all fields present, set or cleared), plus each property that changed
// on an entity already in it. The properties' lifetime on the client is the
// entity's: a snapshot `removed` entry drops them. Because this is reliable
// and snapshots are not, the two can arrive in either order; `server_tick`
// lets the client keep properties sent at or after the tick of a (late)
// removal, see ClientSession::apply_snapshot.
struct S2CEntityProps {
	static constexpr MessageType kType = MessageType::kS2CEntityProps;

	std::uint32_t server_tick = 0;
	std::vector<EntityPropsUpdate> updates;

	void encode(std::vector<std::byte> &out) const;
	static Decoded<S2CEntityProps> decode(std::span<const std::byte> in);
};

} // namespace vb::protocol
