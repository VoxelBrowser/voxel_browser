#include "vb/protocol/world.hpp"

#include "vb/protocol/byte_buffer.hpp"

namespace vb::protocol {

using core::Err;

namespace {

inline constexpr std::uint64_t kMaxChunkBytes = 8u * 1024u * 1024u;
inline constexpr std::uint64_t kMaxBlockRegistryRecords = 4096u;
inline constexpr std::uint64_t kMaxDayNightKeyframes = 256u;
inline constexpr std::uint64_t kMaxEntityKindRegistryRecords = 4096u;
inline constexpr std::uint64_t kMaxEntityClips = 64u;

constexpr std::uint64_t chunk_volume() {
	return static_cast<std::uint64_t>(core::kChunkDim) *
			static_cast<std::uint64_t>(core::kChunkDim) *
			static_cast<std::uint64_t>(core::kChunkDim);
}

void write_coord(ByteWriter &w, core::ChunkCoord c) {
	w.i32(c.x);
	w.i32(c.y);
	w.i32(c.z);
}

core::ChunkCoord read_coord(ByteReader &r) {
	core::ChunkCoord c;
	c.x = r.i32();
	c.y = r.i32();
	c.z = r.i32();
	return c;
}

template <typename T>
Decoded<T> finish(ByteReader &r, T value) {
	r.expect_consumed();
	if (r.failed()) {
		return Err{ r.error() };
	}
	return value;
}

} // namespace

// --- S2CBlockRegistry ------------------------------------------------------
void S2CBlockRegistry::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.varint(blocks.size());
	for (const auto &b : blocks) {
		w.string(b.name);
		w.boolean(b.solid);
		w.boolean(b.opaque);
		w.boolean(b.liquid);
		w.u8(b.light_emission);
		w.string(b.texture);
		w.u16(b.max_damage);
		w.string(b.crack_texture);
	}
}

Decoded<S2CBlockRegistry> S2CBlockRegistry::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CBlockRegistry m;
	const std::uint64_t n = r.varint();
	if (n > kMaxBlockRegistryRecords) {
		return Err{ core::ProtocolError::kLengthExceeded };
	}
	m.blocks.reserve(static_cast<std::size_t>(n));
	for (std::uint64_t i = 0; i < n && !r.failed(); ++i) {
		BlockRegistryRecord b;
		b.name = r.string();
		b.solid = r.boolean();
		b.opaque = r.boolean();
		b.liquid = r.boolean();
		b.light_emission = r.u8();
		b.texture = r.string();
		b.max_damage = r.u16();
		b.crack_texture = r.string();
		m.blocks.push_back(std::move(b));
	}
	return finish(r, std::move(m));
}

// --- S2CMoveParams ----------------------------------------------------------
void S2CMoveParams::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.f64(half_width);
	w.f64(height);
	w.f64(eye_height);
	w.f64(walk_speed);
	w.f64(sprint_speed);
	w.f64(accel);
	w.f64(air_accel);
	w.f64(friction);
	w.f64(gravity);
	w.f64(jump_speed);
	w.f64(terminal_velocity);
	w.f64(step_height);
	w.f64(fly_speed);
	w.boolean(fly);
}

Decoded<S2CMoveParams> S2CMoveParams::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CMoveParams m;
	m.half_width = r.f64();
	m.height = r.f64();
	m.eye_height = r.f64();
	m.walk_speed = r.f64();
	m.sprint_speed = r.f64();
	m.accel = r.f64();
	m.air_accel = r.f64();
	m.friction = r.f64();
	m.gravity = r.f64();
	m.jump_speed = r.f64();
	m.terminal_velocity = r.f64();
	m.step_height = r.f64();
	m.fly_speed = r.f64();
	m.fly = r.boolean();
	return finish(r, std::move(m));
}

// --- S2CDayNightCurve -------------------------------------------------------
void S2CDayNightCurve::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.varint(keyframes.size());
	for (const auto &k : keyframes) {
		w.u32(k.tick);
		w.f64(k.brightness);
		w.u8(k.r);
		w.u8(k.g);
		w.u8(k.b);
	}
}

Decoded<S2CDayNightCurve> S2CDayNightCurve::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CDayNightCurve m;
	const std::uint64_t n = r.varint();
	if (n > kMaxDayNightKeyframes) {
		return Err{ core::ProtocolError::kLengthExceeded };
	}
	m.keyframes.reserve(static_cast<std::size_t>(n));
	for (std::uint64_t i = 0; i < n && !r.failed(); ++i) {
		DayNightKeyframeRecord k;
		k.tick = r.u32();
		k.brightness = r.f64();
		k.r = r.u8();
		k.g = r.u8();
		k.b = r.u8();
		m.keyframes.push_back(k);
	}
	return finish(r, std::move(m));
}

// --- S2CFogParams -----------------------------------------------------------
void S2CFogParams::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.f32(fog_start);
	w.f32(fog_end);
	w.boolean(has_underwater_tint);
	if (has_underwater_tint) {
		w.u8(underwater_tint_r);
		w.u8(underwater_tint_g);
		w.u8(underwater_tint_b);
	}
}

Decoded<S2CFogParams> S2CFogParams::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CFogParams m;
	m.fog_start = r.f32();
	m.fog_end = r.f32();
	m.has_underwater_tint = r.boolean();
	if (m.has_underwater_tint) {
		m.underwater_tint_r = r.u8();
		m.underwater_tint_g = r.u8();
		m.underwater_tint_b = r.u8();
	}
	return finish(r, std::move(m));
}

// --- S2CEntityKindRegistry ---------------------------------------------
namespace {

void write_entity_visual(ByteWriter &w, const std::optional<EntityVisualDef> &visual) {
	w.boolean(visual.has_value());
	if (!visual) {
		return;
	}
	w.string(visual->texture);
	w.u16(visual->frame_width);
	w.u16(visual->frame_height);
	w.u8(visual->facings);
	w.f32(visual->origin_x);
	w.f32(visual->origin_y);
	w.boolean(visual->mirror);
	w.varint(visual->clips.size());
	for (const auto &c : visual->clips) {
		w.string(c.clip);
		w.u16(c.frames);
		w.f32(c.fps);
	}
	w.i8(visual->layer);
	w.boolean(visual->through_walls);
}

std::optional<EntityVisualDef> read_entity_visual(ByteReader &r) {
	if (!r.boolean()) {
		return std::nullopt;
	}
	EntityVisualDef v;
	v.texture = r.string();
	v.frame_width = r.u16();
	v.frame_height = r.u16();
	v.facings = r.u8();
	v.origin_x = r.f32();
	v.origin_y = r.f32();
	v.mirror = r.boolean();
	const std::uint64_t n = r.varint();
	if (n > kMaxEntityClips) {
		r.fail(core::ProtocolError::kLengthExceeded);
		return v;
	}
	v.clips.reserve(static_cast<std::size_t>(n));
	for (std::uint64_t i = 0; i < n && !r.failed(); ++i) {
		EntityClipDef c;
		c.clip = r.string();
		c.frames = r.u16();
		c.fps = r.f32();
		v.clips.push_back(std::move(c));
	}
	v.layer = r.i8();
	v.through_walls = r.boolean();
	return v;
}

} // namespace

void S2CEntityKindRegistry::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.varint(kinds.size());
	for (const auto &k : kinds) {
		w.string(k.name);
		w.f32(k.width);
		w.f32(k.height);
		write_entity_visual(w, k.visual);
		w.boolean(k.hidden);
	}
}

Decoded<S2CEntityKindRegistry> S2CEntityKindRegistry::decode(
		std::span<const std::byte> in) {
	ByteReader r(in);
	S2CEntityKindRegistry m;
	const std::uint64_t n = r.varint();
	if (n > kMaxEntityKindRegistryRecords) {
		return Err{ core::ProtocolError::kLengthExceeded };
	}
	m.kinds.reserve(static_cast<std::size_t>(n));
	for (std::uint64_t i = 0; i < n && !r.failed(); ++i) {
		EntityKindRegistryRecord k;
		k.name = r.string();
		k.width = r.f32();
		k.height = r.f32();
		k.visual = read_entity_visual(r);
		k.hidden = r.boolean();
		m.kinds.push_back(std::move(k));
	}
	return finish(r, std::move(m));
}

// --- S2CChunkAdd ----------------------------------------------------------
void S2CChunkAdd::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	write_coord(w, coord);
	w.u64(revision);
	w.varint(payload.size());
	w.bytes({ payload.data(), payload.size() });
}

Decoded<S2CChunkAdd> S2CChunkAdd::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CChunkAdd m;
	m.coord = read_coord(r);
	m.revision = r.u64();
	const std::uint64_t len = r.varint();
	if (len > kMaxChunkBytes) {
		return Err{ core::ProtocolError::kLengthExceeded };
	}
	const auto bytes = r.bytes(static_cast<std::size_t>(len));
	if (!r.failed()) {
		m.payload.assign(bytes.begin(), bytes.end());
	}
	return finish(r, std::move(m));
}

// --- S2CChunkDelta ------------------------------------------------------
void S2CChunkDelta::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	write_coord(w, coord);
	w.u64(base_revision);
	w.u64(new_revision);
	w.varint(blocks.size());
	for (const BlockChange &c : blocks) {
		w.varint(c.local_index);
		w.u16(static_cast<std::uint16_t>(c.block));
	}
	w.varint(light.size());
	for (const LightChange &c : light) {
		w.varint(c.local_index);
		w.u8(c.packed);
	}
}

Decoded<S2CChunkDelta> S2CChunkDelta::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CChunkDelta m;
	m.coord = read_coord(r);
	m.base_revision = r.u64();
	m.new_revision = r.u64();

	const std::uint64_t nblocks = r.varint();
	if (nblocks > chunk_volume()) {
		return Err{ core::ProtocolError::kLengthExceeded };
	}
	for (std::uint64_t i = 0; i < nblocks && !r.failed(); ++i) {
		BlockChange c;
		c.local_index = static_cast<std::uint32_t>(r.varint());
		c.block = static_cast<core::BlockId>(r.u16());
		if (c.local_index >= chunk_volume()) {
			return Err{ core::ProtocolError::kMalformed };
		}
		m.blocks.push_back(c);
	}

	const std::uint64_t nlight = r.varint();
	if (nlight > chunk_volume()) {
		return Err{ core::ProtocolError::kLengthExceeded };
	}
	for (std::uint64_t i = 0; i < nlight && !r.failed(); ++i) {
		LightChange c;
		c.local_index = static_cast<std::uint32_t>(r.varint());
		c.packed = r.u8();
		if (c.local_index >= chunk_volume()) {
			return Err{ core::ProtocolError::kMalformed };
		}
		m.light.push_back(c);
	}

	return finish(r, std::move(m));
}

// --- S2CChunkRemove --------------------------------------------------
void S2CChunkRemove::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	write_coord(w, coord);
}

Decoded<S2CChunkRemove> S2CChunkRemove::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CChunkRemove m;
	m.coord = read_coord(r);
	return finish(r, std::move(m));
}

namespace {

void write_ivec3(ByteWriter &w, core::IVec3 v) {
	w.svarint(v.x);
	w.svarint(v.y);
	w.svarint(v.z);
}

core::IVec3 read_ivec3(ByteReader &r) {
	core::IVec3 v;
	v.x = static_cast<std::int32_t>(r.svarint());
	v.y = static_cast<std::int32_t>(r.svarint());
	v.z = static_cast<std::int32_t>(r.svarint());
	return v;
}

} // namespace

// --- C2SBlockEdit --------------------------------------------------------
void C2SBlockEdit::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.u32(predicted_seq);
	w.u8(static_cast<std::uint8_t>(action));
	write_ivec3(w, pos);
	w.u16(static_cast<std::uint16_t>(block));
}

Decoded<C2SBlockEdit> C2SBlockEdit::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	C2SBlockEdit m;
	m.predicted_seq = r.u32();
	const auto action = static_cast<BlockEditAction>(r.u8());
	if (!valid(action)) {
		return Err{ core::ProtocolError::kBadEnum };
	}
	m.action = action;
	m.pos = read_ivec3(r);
	m.block = static_cast<core::BlockId>(r.u16());
	return finish(r, std::move(m));
}

// --- S2CBlockEditResult -------------------------------------------------
void S2CBlockEditResult::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.u32(predicted_seq);
	w.boolean(accepted);
	write_ivec3(w, pos);
}

Decoded<S2CBlockEditResult> S2CBlockEditResult::decode(
		std::span<const std::byte> in) {
	ByteReader r(in);
	S2CBlockEditResult m;
	m.predicted_seq = r.u32();
	m.accepted = r.boolean();
	m.pos = read_ivec3(r);
	return finish(r, std::move(m));
}

// --- S2CBlockDamage --------------------------------------------------------
void S2CBlockDamage::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	write_ivec3(w, pos);
	w.u16(punches);
	w.u64(revision);
}

Decoded<S2CBlockDamage> S2CBlockDamage::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CBlockDamage m;
	m.pos = read_ivec3(r);
	m.punches = r.u16();
	m.revision = r.u64();
	return finish(r, std::move(m));
}

// --- C2SBlockBreakBegin / C2SBlockBreakStop -------------------------------
void C2SBlockBreakBegin::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	write_ivec3(w, pos);
	write_ivec3(w, face);
}

Decoded<C2SBlockBreakBegin> C2SBlockBreakBegin::decode(
		std::span<const std::byte> in) {
	ByteReader r(in);
	C2SBlockBreakBegin m;
	m.pos = read_ivec3(r);
	m.face = read_ivec3(r);
	return finish(r, std::move(m));
}

void C2SBlockBreakStop::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	write_ivec3(w, pos);
}

Decoded<C2SBlockBreakStop> C2SBlockBreakStop::decode(
		std::span<const std::byte> in) {
	ByteReader r(in);
	C2SBlockBreakStop m;
	m.pos = read_ivec3(r);
	return finish(r, std::move(m));
}

// --- S2CTimeOfDay ---------------------------------------------------------
void S2CTimeOfDay::encode(std::vector<std::byte> &out) const {
	ByteWriter w(out);
	w.u32(time_of_day);
}

Decoded<S2CTimeOfDay> S2CTimeOfDay::decode(std::span<const std::byte> in) {
	ByteReader r(in);
	S2CTimeOfDay m;
	m.time_of_day = r.u32();
	return finish(r, std::move(m));
}

} // namespace vb::protocol
