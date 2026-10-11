#include <doctest/doctest.h>

#include <ostream>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "vb/core/version.hpp"
#include "vb/net/transport.hpp"
#include "vb/protocol/assetsync.hpp"
#include "vb/protocol/byte_buffer.hpp"
#include "vb/protocol/chat.hpp"
#include "vb/protocol/compression.hpp"
#include "vb/protocol/handshake.hpp"
#include "vb/protocol/input.hpp"
#include "vb/protocol/inventory.hpp"
#include "vb/protocol/message.hpp"
#include "vb/protocol/snapshot.hpp"
#include "vb/protocol/world.hpp"

using namespace vb::protocol;

namespace {

std::span<const std::byte> as_span(const std::vector<std::byte> &v) {
	return { v.data(), v.size() };
}

template <typename T>
T round_trip(const T &in) {
	std::vector<std::byte> bytes;
	in.encode(bytes);
	auto decoded = T::decode(as_span(bytes));
	REQUIRE(decoded);
	return std::move(*decoded);
}

} // namespace

TEST_CASE("varint / svarint round-trip across boundaries") {
	const std::uint64_t values[] = { 0, 1, 127, 128, 300, 16383, 16384,
		std::uint64_t(1) << 35, ~std::uint64_t(0) };
	for (std::uint64_t v : values) {
		std::vector<std::byte> b;
		ByteWriter(b).varint(v);
		ByteReader r(as_span(b));
		CHECK(r.varint() == v);
		CHECK_FALSE(r.failed());
		CHECK(r.at_end());
	}
	for (std::int64_t v : { std::int64_t(0), std::int64_t(-1), std::int64_t(1),
				 std::int64_t(-1000000), std::int64_t(1) << 40 }) {
		std::vector<std::byte> b;
		ByteWriter(b).svarint(v);
		ByteReader r(as_span(b));
		CHECK(r.svarint() == v);
		CHECK_FALSE(r.failed());
	}
}

TEST_CASE("ByteReader reports underrun instead of reading garbage") {
	std::vector<std::byte> b;
	ByteWriter(b).u32(0xDEADBEEF);
	ByteReader r({ b.data(), 2 }); // only 2 of 4 bytes
	CHECK(r.u32() == 0);
	CHECK(r.failed());
	CHECK(r.error() == vb::core::ProtocolError::kShortBuffer);
}

TEST_CASE("overlong varint is rejected") {
	std::vector<std::byte> b(11, std::byte{ 0x80 }); // never-terminating
	ByteReader r(as_span(b));
	r.varint();
	CHECK(r.error() == vb::core::ProtocolError::kOverlongVarint);
}

TEST_CASE("frame envelope round-trips and detects partial buffers") {
	std::vector<std::byte> payload;
	ByteWriter(payload).string("hi there");

	std::vector<std::byte> wire;
	write_frame(wire, MessageType::kC2SChat, as_span(payload));
	CHECK(wire.size() == kEnvelopeBytes + payload.size());

	std::size_t consumed = 0;
	auto frame = read_frame(as_span(wire), consumed);
	REQUIRE(frame);
	CHECK(frame->header.type == MessageType::kC2SChat);
	CHECK(consumed == wire.size());

	auto partial = read_frame({ wire.data(), wire.size() - 1 }, consumed);
	CHECK_FALSE(partial);
	CHECK(partial.error() == vb::core::ProtocolError::kShortBuffer);
	CHECK(consumed == 0);
}

TEST_CASE("lane assignment matches the spec") {
	CHECK(lane_for(MessageType::kC2SHello) == Lane::kControl);
	CHECK(lane_for(MessageType::kS2CChunkAdd) == Lane::kWorld);
	CHECK(lane_for(MessageType::kS2CEntitySnapshot) == Lane::kSnapshot);
	CHECK(lane_for(MessageType::kS2CAssetData) == Lane::kAssets);
	CHECK(lane_for(MessageType::kC2SInputBatch) == Lane::kInput);
	CHECK(lane_for(MessageType::kS2CKeybindRegistry) == Lane::kWorld);
	CHECK(lane_for(MessageType::kS2CEntityKindRegistry) == Lane::kWorld);
	CHECK(lane_for(MessageType::kS2CBlockDamage) == Lane::kFeedback);
	CHECK(lane_for(MessageType::kS2CEntityProps) == Lane::kFeedback);
}

TEST_CASE("transport lane policy: kFeedback is reliable, unbatched, on its own GNS lane") {
	using vb::net::gns_lane_index;
	using vb::net::lane_skips_nagle;
	using vb::net::send_mode_for_lane;
	using vb::net::SendMode;
	CHECK(send_mode_for_lane(Lane::kFeedback) == SendMode::kReliableOrdered);
	CHECK(gns_lane_index(Lane::kFeedback) == 1);
	CHECK(lane_skips_nagle(Lane::kFeedback));
	// Every other lane keeps sharing GNS lane 0 (control/world/assets stay
	// mutually ordered) and keeps Nagle batching.
	for (const Lane lane : { Lane::kControl, Lane::kWorld, Lane::kSnapshot, Lane::kAssets,
				 Lane::kInput }) {
		CHECK(gns_lane_index(lane) == 0);
		CHECK_FALSE(lane_skips_nagle(lane));
	}
	CHECK(gns_lane_index(Lane::kFeedback) < vb::net::kGnsLaneCount);
}

TEST_CASE("handshake structs round-trip") {
	C2SHello hello{ vb::kEngineProtocolVersion, 0x1122334455667788ull, "vb-test/0" };
	auto h2 = round_trip(hello);
	CHECK(h2.engine_protocol_version == vb::kEngineProtocolVersion);
	CHECK(h2.client_nonce == 0x1122334455667788ull);
	CHECK(h2.client_version == "vb-test/0");
	CHECK(h2.client_flags == 0);

	C2SHello flagged{ vb::kEngineProtocolVersion, 1, "vb-test/0",
		kClientFlagAutomation };
	CHECK(round_trip(flagged).client_flags == kClientFlagAutomation);

	S2CServerInfo info{ "base", "1.0.0", vb::kEngineProtocolVersion, 20, 6, "hello!",
		AuthMode::kNone, ">=0.6.0, <0.7.0" };
	auto i2 = round_trip(info);
	CHECK(i2.engine_version_req == ">=0.6.0, <0.7.0");
	CHECK(i2.pack_name == "base");
	CHECK(i2.tick_rate == 20);
	CHECK(i2.view_distance == 6);
	CHECK(i2.auth_mode == AuthMode::kNone);

	S2CJoinAccept accept{ vb::core::NetId{ 7 }, { 1.5, 64.25, -3.0 }, 0xABCDEF, 1200 };
	auto a2 = round_trip(accept);
	CHECK(a2.your_net_id == vb::core::NetId{ 7 });
	CHECK(a2.spawn_pos.y == doctest::Approx(64.25));
	CHECK(a2.world_seed == 0xABCDEF);
	CHECK(a2.time_of_day == 1200);

	auto d2 = round_trip(S2CDisconnect{ DisconnectReason::kServerFull, "full" });
	CHECK(d2.reason == DisconnectReason::kServerFull);
	CHECK(d2.message == "full");

	round_trip(C2SReady{});
}

TEST_CASE("entity snapshot round-trips") {
	S2CEntitySnapshot s;
	s.server_tick = 4242;
	s.last_acked_input_seq = 17;
	s.entered.push_back({ vb::core::NetId{ 3 }, vb::core::EntityKindId{ 1 },
			{ 1.0, 2.0, 3.0 }, { 45.0f, -10.0f }, { 0.1f, 0.0f, -0.2f }, 0, std::nullopt, std::nullopt });
	s.updated.push_back({ vb::core::NetId{ 4 }, vb::core::EntityKindId{ 0 },
			{ -8.0, 64.0, 0.0 }, {}, {}, 2, std::nullopt, std::nullopt });
	s.removed.push_back(vb::core::NetId{ 9 });

	auto s2 = round_trip(s);
	CHECK(s2.server_tick == 4242);
	CHECK(s2.last_acked_input_seq == 17);
	REQUIRE(s2.entered.size() == 1);
	CHECK(s2.entered[0] == s.entered[0]);
	REQUIRE(s2.updated.size() == 1);
	CHECK(s2.updated[0].pos.y == doctest::Approx(64.0));
	REQUIRE(s2.removed.size() == 1);
	CHECK(s2.removed[0] == vb::core::NetId{ 9 });
}

TEST_CASE("entity snapshot round-trips a per-instance visual override") {
	S2CEntitySnapshot s;
	s.server_tick = 1;
	s.last_acked_input_seq = 0;

	EntityRecord entered{ vb::core::NetId{ 3 }, vb::core::EntityKindId{ 1 },
		{ 1.0, 2.0, 3.0 }, { 45.0f, -10.0f }, { 0.1f, 0.0f, -0.2f }, 0, std::nullopt, std::nullopt };
	EntityVisualOverride ov;
	ov.texture = "textures/entities/skins/player_red.png";
	ov.facings = 4;
	ov.clips = std::vector<EntityClipDef>{ { "idle", 4, 6.0f } };
	entered.visual_override = ov;
	s.entered.push_back(entered);

	// A `stayed`/updated record never carries an override (see
	// net::ServerSession::to_record) -- nullopt round-trips as absent, not a
	// spurious default-constructed EntityVisualOverride.
	s.updated.push_back({ vb::core::NetId{ 4 }, vb::core::EntityKindId{ 1 },
			{ 0.0, 0.0, 0.0 }, {}, {}, 0, std::nullopt, std::nullopt });

	auto s2 = round_trip(s);
	REQUIRE(s2.entered.size() == 1);
	REQUIRE(s2.entered[0].visual_override.has_value());
	CHECK(s2.entered[0].visual_override->texture == "textures/entities/skins/player_red.png");
	CHECK_FALSE(s2.entered[0].visual_override->frame_width.has_value());
	REQUIRE(s2.entered[0].visual_override->facings.has_value());
	CHECK(*s2.entered[0].visual_override->facings == 4);
	REQUIRE(s2.entered[0].visual_override->clips.has_value());
	REQUIRE(s2.entered[0].visual_override->clips->size() == 1);
	CHECK((*s2.entered[0].visual_override->clips)[0].clip == "idle");
	CHECK(s2.entered[0] == entered);

	REQUIRE(s2.updated.size() == 1);
	CHECK_FALSE(s2.updated[0].visual_override.has_value());
}

TEST_CASE("entity snapshot round-trips a dropped item's block id") {
	S2CEntitySnapshot s;
	EntityRecord drop{ vb::core::NetId{ 0x80000001u }, vb::core::EntityKindId{ 2 },
		{ 1.0, 2.0, 3.0 }, {}, {}, 0, std::nullopt, std::nullopt };
	drop.item = 7;
	s.entered.push_back(drop);
	s.updated.push_back({ vb::core::NetId{ 5 }, vb::core::EntityKindId{ 1 },
			{ 0.0, 0.0, 0.0 }, {}, {}, 0, std::nullopt, std::nullopt });

	auto s2 = round_trip(s);
	REQUIRE(s2.entered.size() == 1);
	REQUIRE(s2.entered[0].item.has_value());
	CHECK(*s2.entered[0].item == 7);
	CHECK(s2.entered[0] == drop);
	CHECK_FALSE(s2.updated[0].item.has_value());
}

TEST_CASE("chat / open_ui round-trip") {
	auto c1 = round_trip(C2SChat{ "hi there" });
	CHECK(c1.text == "hi there");

	auto c2 = round_trip(S2CChat{ "hello world" });
	CHECK(c2.text == "hello world");

	auto u2 = round_trip(S2COpenUi{ "inventory", R"({"slots":3})" });
	CHECK(u2.ui_name == "inventory");
	CHECK(u2.ctx_json == R"({"slots":3})");

	// Empty ctx_json (no ctx table passed) round-trips too.
	auto u3 = round_trip(S2COpenUi{ "pause", "" });
	CHECK(u3.ui_name == "pause");
	CHECK(u3.ctx_json.empty());

	auto e2 = round_trip(C2SUiEvent{ "inventory", "close_btn", "click", "null" });
	CHECK(e2.ui_name == "inventory");
	CHECK(e2.widget_id == "close_btn");
	CHECK(e2.event_kind == "click");
	CHECK(e2.value_json == "null");
}

TEST_CASE("player join / leave / list round-trip") {
	auto j = round_trip(S2CPlayerJoin{ vb::core::NetId{ 3 }, "Alice" });
	CHECK(j.net_id == vb::core::NetId{ 3 });
	CHECK(j.name == "Alice");

	auto l = round_trip(S2CPlayerLeave{ vb::core::NetId{ 3 } });
	CHECK(l.net_id == vb::core::NetId{ 3 });

	auto empty = round_trip(S2CPlayerList{});
	CHECK(empty.players.empty());

	S2CPlayerList list;
	list.players.push_back({ vb::core::NetId{ 1 }, "Alice" });
	list.players.push_back({ vb::core::NetId{ 2 }, "Bob" });
	auto list2 = round_trip(list);
	REQUIRE(list2.players.size() == 2);
	CHECK(list2.players[0].net_id == vb::core::NetId{ 1 });
	CHECK(list2.players[0].name == "Alice");
	CHECK(list2.players[1].net_id == vb::core::NetId{ 2 });
	CHECK(list2.players[1].name == "Bob");
}

TEST_CASE("inventory round-trips, including an empty snapshot") {
	auto empty = round_trip(S2CInventory{});
	CHECK(empty.slots.empty());

	S2CInventory inv;
	inv.slots.push_back({ vb::core::BlockId{ 3 }, 5 });
	inv.slots.push_back({ vb::core::BlockId{ 7 }, 64 });
	auto inv2 = round_trip(inv);
	REQUIRE(inv2.slots.size() == 2);
	CHECK(inv2.slots[0].item == vb::core::BlockId{ 3 });
	CHECK(inv2.slots[0].count == 5);
	CHECK(inv2.slots[1].item == vb::core::BlockId{ 7 });
	CHECK(inv2.slots[1].count == 64);
}

TEST_CASE("player status round-trips, and rejects a truncated payload") {
	S2CPlayerStatus st;
	st.health = 13.5f;
	st.max_health = 20.0f;
	st.hunger = 42.0f;
	st.max_hunger = 100.0f;
	auto st2 = round_trip(st);
	CHECK(st2.health == 13.5f);
	CHECK(st2.max_health == 20.0f);
	CHECK(st2.hunger == 42.0f);
	CHECK(st2.max_hunger == 100.0f);

	std::vector<std::byte> bytes;
	st.encode(bytes);
	bytes.pop_back();
	CHECK_FALSE(S2CPlayerStatus::decode(bytes));
}

TEST_CASE("block registry round-trips, including an empty list") {
	auto empty = round_trip(S2CBlockRegistry{});
	CHECK(empty.blocks.empty());

	S2CBlockRegistry reg;
	reg.blocks.push_back({ .name = "base:air", .solid = false, .opaque = false, .liquid = false, .texture = "", .crack_texture = "" });
	reg.blocks.push_back({ .name = "base:stone", .solid = true, .opaque = true, .liquid = false, .texture = "", .crack_texture = "" });
	reg.blocks.push_back({ .name = "test:glow", .solid = true, .opaque = true, .liquid = false, .light_emission = 15, .texture = "", .crack_texture = "" });
	reg.blocks.push_back({ .name = "test:crumbly", .solid = true, .opaque = true, .liquid = false, .texture = "", .max_damage = 5, .crack_texture = "" });
	// Real texture/atlas system: a pack-relative texture path round-trips too.
	reg.blocks.push_back({ .name = "test:stone", .solid = true, .opaque = true, .liquid = false, .texture = "textures/stone.png", .crack_texture = "" });
	// crack_texture (6.5's last piece): also round-trips.
	reg.blocks.push_back({ .name = "test:ore", .solid = true, .opaque = true, .liquid = false, .texture = "", .max_damage = 6, .crack_texture = "textures/ore_crack.png" });
	auto r2 = round_trip(reg);
	REQUIRE(r2.blocks.size() == 6);
	CHECK(r2.blocks[0].name == "base:air");
	CHECK_FALSE(r2.blocks[0].solid);
	CHECK(r2.blocks[2].name == "test:glow");
	CHECK(r2.blocks[2].light_emission == 15);
	CHECK(r2.blocks[3].max_damage == 5); // Phase 6.5
	CHECK(r2.blocks[4].texture == "textures/stone.png");
	CHECK(r2.blocks[4].crack_texture.empty());
	CHECK(r2.blocks[5].crack_texture == "textures/ore_crack.png");
	CHECK(r2.blocks == reg.blocks);
}

TEST_CASE("block-break begin/stop round-trip (Phase 6.5)") {
	auto begin = round_trip(
			C2SBlockBreakBegin{ { 1, 2, 3 }, { 0, 1, 0 } });
	CHECK(begin.pos == vb::core::IVec3{ 1, 2, 3 });
	CHECK(begin.face == vb::core::IVec3{ 0, 1, 0 });

	auto begin_neg = round_trip(
			C2SBlockBreakBegin{ { -5, -100, 7 }, { -1, 0, 0 } });
	CHECK(begin_neg.pos == vb::core::IVec3{ -5, -100, 7 });
	CHECK(begin_neg.face == vb::core::IVec3{ -1, 0, 0 });

	auto stop = round_trip(C2SBlockBreakStop{ { 1, 2, 3 } });
	CHECK(stop.pos == vb::core::IVec3{ 1, 2, 3 });
}

TEST_CASE("move params round-trip (Phase 6.7)") {
	auto defaults = round_trip(S2CMoveParams{});
	CHECK(defaults.gravity == doctest::Approx(28.0));
	CHECK(defaults.fly == false);

	S2CMoveParams p;
	p.half_width = 0.35;
	p.height = 1.9;
	p.eye_height = 1.7;
	p.walk_speed = 5.0;
	p.sprint_speed = 9.0;
	p.accel = 40.0;
	p.air_accel = 8.0;
	p.friction = 10.0;
	p.gravity = 12.5;
	p.jump_speed = 6.0;
	p.terminal_velocity = 50.0;
	p.step_height = 1.1;
	p.fly_speed = 20.0;
	p.fly = true;
	auto p2 = round_trip(p);
	CHECK(p2.half_width == doctest::Approx(0.35));
	CHECK(p2.height == doctest::Approx(1.9));
	CHECK(p2.eye_height == doctest::Approx(1.7));
	CHECK(p2.walk_speed == doctest::Approx(5.0));
	CHECK(p2.sprint_speed == doctest::Approx(9.0));
	CHECK(p2.accel == doctest::Approx(40.0));
	CHECK(p2.air_accel == doctest::Approx(8.0));
	CHECK(p2.friction == doctest::Approx(10.0));
	CHECK(p2.gravity == doctest::Approx(12.5));
	CHECK(p2.jump_speed == doctest::Approx(6.0));
	CHECK(p2.terminal_velocity == doctest::Approx(50.0));
	CHECK(p2.step_height == doctest::Approx(1.1));
	CHECK(p2.fly_speed == doctest::Approx(20.0));
	CHECK(p2.fly == true);
}

TEST_CASE("day/night curve round-trips, including an empty list (Phase 6.8)") {
	auto empty = round_trip(S2CDayNightCurve{});
	CHECK(empty.keyframes.empty());

	S2CDayNightCurve curve;
	curve.keyframes = {
		{ 0, 0.1, 10, 20, 30 },
		{ 12000, 0.9, 200, 210, 220 },
	};
	auto r2 = round_trip(curve);
	REQUIRE(r2.keyframes.size() == 2);
	CHECK(r2.keyframes[0].tick == 0);
	CHECK(r2.keyframes[0].brightness == doctest::Approx(0.1));
	CHECK(r2.keyframes[0].r == 10);
	CHECK(r2.keyframes[1].tick == 12000);
	CHECK(r2.keyframes[1].brightness == doctest::Approx(0.9));
	CHECK(r2.keyframes[1].b == 220);
	CHECK(r2.keyframes == curve.keyframes);
}

TEST_CASE("fog params round-trip (Phase 7.2)") {
	auto defaults = round_trip(S2CFogParams{});
	CHECK(defaults.fog_start == doctest::Approx(0.0f));
	CHECK(defaults.fog_end == doctest::Approx(0.0f));

	auto p = round_trip(S2CFogParams{ 96.0f, 160.0f });
	CHECK(p.fog_start == doctest::Approx(96.0f));
	CHECK(p.fog_end == doctest::Approx(160.0f));
	CHECK_FALSE(p.has_underwater_tint);
}

TEST_CASE("fog params round-trip with an underwater tint override (Phase 7.5)") {
	S2CFogParams src;
	src.fog_start = 2.0f;
	src.fog_end = 8.0f;
	src.has_underwater_tint = true;
	src.underwater_tint_r = 12;
	src.underwater_tint_g = 34;
	src.underwater_tint_b = 200;

	auto p = round_trip(src);
	CHECK(p.fog_start == doctest::Approx(2.0f));
	CHECK(p.fog_end == doctest::Approx(8.0f));
	CHECK(p.has_underwater_tint);
	CHECK(p.underwater_tint_r == 12);
	CHECK(p.underwater_tint_g == 34);
	CHECK(p.underwater_tint_b == 200);
}

TEST_CASE("block damage round-trips (Phase 6.5's deferred replication half)") {
	auto zero = round_trip(S2CBlockDamage{});
	CHECK(zero.pos == vb::core::IVec3{});
	CHECK(zero.punches == 0);

	S2CBlockDamage msg;
	msg.pos = { 10, -3, 42 };
	msg.punches = 5;
	msg.revision = 0x0123456789abcdefull;
	auto r = round_trip(msg);
	CHECK(r.pos == vb::core::IVec3{ 10, -3, 42 });
	CHECK(r.punches == 5);
	CHECK(r.revision == 0x0123456789abcdefull);
}

TEST_CASE("keybind registry round-trips, including an empty list") {
	auto empty = round_trip(S2CKeybindRegistry{});
	CHECK(empty.names.empty());

	S2CKeybindRegistry reg;
	reg.names = { "dash", "interact", "toggle_map" };
	auto r2 = round_trip(reg);
	REQUIRE(r2.names.size() == 3);
	CHECK(r2.names[0] == "dash");
	CHECK(r2.names[2] == "toggle_map");
	CHECK(r2.names == reg.names);
}

TEST_CASE("keybind registry decode rejects more than kMaxKeybinds names") {
	S2CKeybindRegistry reg;
	for (std::size_t i = 0; i <= S2CKeybindRegistry::kMaxKeybinds; ++i) {
		reg.names.push_back("bind_" + std::to_string(i));
	}
	std::vector<std::byte> bytes;
	reg.encode(bytes);
	auto decoded = S2CKeybindRegistry::decode(as_span(bytes));
	CHECK_FALSE(decoded);
	CHECK(decoded.error() == vb::core::ProtocolError::kLengthExceeded);
}

TEST_CASE("entity kind registry round-trips, including an empty list") {
	auto empty = round_trip(S2CEntityKindRegistry{});
	CHECK(empty.kinds.empty());

	S2CEntityKindRegistry reg;
	reg.kinds.push_back({ .name = "test:slime", .width = 0.6f, .height = 0.6f, .visual = std::nullopt });
	reg.kinds.push_back({ .name = "test:golem", .visual = std::nullopt }); // default width/height
	auto r2 = round_trip(reg);
	REQUIRE(r2.kinds.size() == 2);
	CHECK(r2.kinds[0].name == "test:slime");
	CHECK(r2.kinds[0].width == doctest::Approx(0.6f));
	CHECK(r2.kinds[0].height == doctest::Approx(0.6f));
	CHECK(r2.kinds[1].name == "test:golem");
	CHECK(r2.kinds[1].width == doctest::Approx(0.8f));
	CHECK(r2.kinds[1].height == doctest::Approx(1.8f));
	CHECK(r2.kinds == reg.kinds);
}

TEST_CASE("entity kind registry round-trips the hidden (text-only) flag") {
	S2CEntityKindRegistry reg;
	reg.kinds.push_back({ .name = "test:label", .visual = std::nullopt, .hidden = true });
	reg.kinds.push_back({ .name = "test:golem", .visual = std::nullopt });
	auto r2 = round_trip(reg);
	REQUIRE(r2.kinds.size() == 2);
	CHECK(r2.kinds[0].hidden);
	CHECK_FALSE(r2.kinds[1].hidden);
	CHECK(r2.kinds == reg.kinds);
}

TEST_CASE("S2C_EntityProps round-trips set, cleared and unchanged fields") {
	S2CEntityProps m;
	m.server_tick = 1234;
	EntityText t;
	t.value = "Ripe!\nnow";
	t.color = { 120, 235, 110, 255 };
	t.background = std::array<std::uint8_t, 4>{ 40, 40, 40, 200 };
	t.size = 0.4f;
	t.offset_y = 0.2f;
	t.max_distance = 24.0f;
	t.through_walls = true;
	EntityPropsUpdate full;
	full.net_id = static_cast<vb::core::NetId>(0x4000'0001u);
	full.text = t;
	full.clip = std::string("open");
	EntityAttachment a;
	a.parent = static_cast<vb::core::NetId>(7);
	a.offset = { 0.0f, 1.9f, -0.25f };
	a.face_offset = true;
	a.layer = -2;
	full.attach = a;
	m.updates.push_back(full);
	EntityPropsUpdate cleared; // every field present but cleared
	cleared.net_id = static_cast<vb::core::NetId>(0x4000'0002u);
	cleared.text.emplace();
	cleared.clip.emplace();
	cleared.attach.emplace();
	m.updates.push_back(cleared);
	EntityPropsUpdate clip_only; // text/attach unchanged
	clip_only.net_id = static_cast<vb::core::NetId>(0x4000'0003u);
	clip_only.clip = std::string("idle");
	m.updates.push_back(clip_only);

	auto r = round_trip(m);
	CHECK(r.server_tick == 1234);
	CHECK(r.updates == m.updates);
	CHECK_FALSE(r.updates[2].text.has_value());
	CHECK_FALSE(r.updates[2].attach.has_value());
	REQUIRE(r.updates[1].text.has_value());
	CHECK_FALSE(r.updates[1].text->has_value());
}

TEST_CASE("S2C_EntityProps decode rejects over-long text and unknown field bits") {
	S2CEntityProps m;
	EntityPropsUpdate u;
	u.net_id = static_cast<vb::core::NetId>(1);
	EntityText t;
	t.value = std::string(kMaxEntityTextBytes + 1, 'x');
	u.text = t;
	m.updates.push_back(u);
	std::vector<std::byte> buf;
	m.encode(buf);
	CHECK_FALSE(S2CEntityProps::decode(buf));

	S2CEntityProps ok;
	ok.updates.push_back({ static_cast<vb::core::NetId>(1), std::nullopt, std::nullopt, std::nullopt });
	std::vector<std::byte> bytes;
	ok.encode(bytes);
	bytes.back() = std::byte{ 0x80 }; // the field mask: an undefined bit
	CHECK_FALSE(S2CEntityProps::decode(bytes));
}

TEST_CASE("visual layer / through_walls round-trip in the kind registry and the override") {
	S2CEntityKindRegistry reg;
	EntityKindRegistryRecord rec{ .name = "test:aura", .visual = std::nullopt };
	EntityVisualDef visual;
	visual.texture = "textures/aura.png";
	visual.frame_width = 64;
	visual.frame_height = 64;
	visual.clips.push_back({ "idle", 1, 1.0f });
	visual.layer = -3;
	visual.through_walls = true;
	rec.visual = visual;
	reg.kinds.push_back(rec);
	auto r = round_trip(reg);
	REQUIRE(r.kinds[0].visual.has_value());
	CHECK(r.kinds[0].visual->layer == -3);
	CHECK(r.kinds[0].visual->through_walls);

	S2CEntitySnapshot snap;
	EntityRecord er;
	er.net_id = static_cast<vb::core::NetId>(3);
	er.flags = 1;
	EntityVisualOverride ov;
	ov.layer = 2;
	ov.through_walls = false;
	er.visual_override = ov;
	snap.entered.push_back(er);
	auto s2 = round_trip(snap);
	REQUIRE(s2.entered.size() == 1);
	CHECK(s2.entered[0] == er);
}

TEST_CASE("entity kind registry round-trips a real visual def") {
	S2CEntityKindRegistry reg;
	EntityKindRegistryRecord rec{ .name = "test:slime", .width = 0.6f, .height = 0.6f, .visual = std::nullopt };
	EntityVisualDef visual;
	visual.texture = "textures/entities/slime.png";
	visual.frame_width = 128;
	visual.frame_height = 128;
	visual.facings = 4;
	visual.origin_x = 0.5f;
	visual.origin_y = 1.0f;
	visual.clips.push_back({ "idle", 4, 6.0f });
	visual.clips.push_back({ "walk", 6, 10.0f });
	rec.visual = visual;
	reg.kinds.push_back(rec);
	reg.kinds.push_back({ .name = "test:golem", .visual = std::nullopt }); // no visual at all

	auto r2 = round_trip(reg);
	REQUIRE(r2.kinds.size() == 2);
	REQUIRE(r2.kinds[0].visual.has_value());
	CHECK(r2.kinds[0].visual->texture == "textures/entities/slime.png");
	CHECK(r2.kinds[0].visual->frame_width == 128);
	CHECK(r2.kinds[0].visual->frame_height == 128);
	CHECK(r2.kinds[0].visual->facings == 4);
	REQUIRE(r2.kinds[0].visual->clips.size() == 2);
	CHECK(r2.kinds[0].visual->clips[0].clip == "idle");
	CHECK(r2.kinds[0].visual->clips[0].frames == 4);
	CHECK(r2.kinds[0].visual->clips[1].clip == "walk");
	CHECK(r2.kinds[0].visual->clips[1].fps == doctest::Approx(10.0f));
	CHECK_FALSE(r2.kinds[1].visual.has_value());
	CHECK(r2.kinds == reg.kinds);
}

TEST_CASE("entity kind registry decode rejects a visual with too many clips") {
	S2CEntityKindRegistry reg;
	EntityKindRegistryRecord rec{ .name = "test:overstuffed", .visual = std::nullopt };
	EntityVisualDef visual;
	visual.texture = "textures/entities/overstuffed.png";
	visual.frame_width = 128;
	visual.frame_height = 128;
	// Mirrors world.cpp's internal kMaxEntityClips = 64 -- one past the cap.
	for (int i = 0; i <= 64; ++i) {
		visual.clips.push_back({ "clip_" + std::to_string(i), 1, 1.0f });
	}
	rec.visual = visual;
	reg.kinds.push_back(rec);

	std::vector<std::byte> bytes;
	reg.encode(bytes);
	auto decoded = S2CEntityKindRegistry::decode(as_span(bytes));
	CHECK_FALSE(decoded);
	CHECK(decoded.error() == vb::core::ProtocolError::kLengthExceeded);
}

TEST_CASE("time of day round-trips") {
	auto t = round_trip(S2CTimeOfDay{ 12345 });
	CHECK(t.time_of_day == 12345);
}

TEST_CASE("asset sync messages round-trip") {
	const vb::core::AssetHash h1{ 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull };
	const vb::core::AssetHash h2{ 0xDEADBEEFDEADBEEFull, 0x1ull };

	auto req = round_trip(C2SAssetManifestRequest{ h1 });
	CHECK(req.known_manifest_hash.lo == h1.lo);
	CHECK(req.known_manifest_hash.hi == h1.hi);

	S2CAssetManifest manifest;
	manifest.manifest_hash = h1;
	manifest.total_bytes = 4096;
	manifest.entries.push_back({ "scripts/init.lua", h1, 128, AssetKind::kScript });
	manifest.entries.push_back({ "textures/stone.png", h2, 2048, AssetKind::kTexture });
	auto m2 = round_trip(manifest);
	CHECK(m2.manifest_hash.lo == h1.lo);
	CHECK(m2.total_bytes == 4096);
	REQUIRE(m2.entries.size() == 2);
	CHECK(m2.entries[0] == manifest.entries[0]);
	CHECK(m2.entries[1].kind == AssetKind::kTexture);

	auto empty_manifest = round_trip(S2CAssetManifest{ h1, 0, {} });
	CHECK(empty_manifest.entries.empty());

	auto empty_req = round_trip(C2SAssetRequest{});
	CHECK(empty_req.missing.empty());
	auto req2 = round_trip(C2SAssetRequest{ { h1, h2 } });
	REQUIRE(req2.missing.size() == 2);
	CHECK(req2.missing[1].lo == h2.lo);

	S2CAssetData data;
	data.hash = h2;
	data.seq = 3;
	data.total_chunks = 7;
	data.bytes = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
	auto d2 = round_trip(data);
	CHECK(d2.hash.hi == h2.hi);
	CHECK(d2.seq == 3);
	CHECK(d2.total_chunks == 7);
	REQUIRE(d2.bytes.size() == 3);
	CHECK(d2.bytes[2] == std::byte{ 3 });

	// Zero-length chunk (a 0-byte file's single terminating chunk).
	auto d3 = round_trip(S2CAssetData{ h1, 0, 1, {} });
	CHECK(d3.bytes.empty());
}

TEST_CASE("external-auth messages round-trip and enforce their bounds") {
	S2CAuthChallenge ch;
	ch.provider = "keycloak";
	ch.display_name = "Acme SSO";
	ch.issuer = "https://id.example/realms/vb";
	ch.client_id = "voxel";
	ch.scopes = { "openid", "profile" };
	ch.params = { { "sign_in", "browser" }, { "api_key", "k" } };
	ch.nonce = "bm9uY2U";
	auto c2 = round_trip(ch);
	CHECK(c2.provider == "keycloak");
	CHECK(c2.issuer == ch.issuer);
	CHECK(c2.scopes == ch.scopes);
	CHECK(c2.params == ch.params);
	CHECK(c2.nonce == "bm9uY2U");

	auto r2 = round_trip(S2CAuthResult{ true, "", "alice" });
	CHECK(r2.resolved_name == "alice");

	auto q2 = round_trip(S2CReauthRequest{ "n", 120 });
	CHECK(q2.nonce == "n");
	CHECK(q2.grace_seconds == 120);
	CHECK(round_trip(C2SReauth{ "jwt" }).token == "jwt");

	CHECK(valid(AuthMode::kExternal));
	CHECK_FALSE(valid(static_cast<AuthMode>(3)));

	// Token cap: exactly the limit decodes, one byte over is rejected.
	C2SAuth big{ "n", std::string(kMaxAuthTokenBytes, 'a') };
	CHECK(round_trip(big).token.size() == kMaxAuthTokenBytes);
	big.token.push_back('a');
	std::vector<std::byte> bytes;
	big.encode(bytes);
	auto over = C2SAuth::decode(as_span(bytes));
	CHECK_FALSE(over);
	CHECK(over.error() == vb::core::ProtocolError::kLengthExceeded);

	// Too many scopes.
	S2CAuthChallenge many = ch;
	many.scopes.assign(kMaxAuthListEntries + 1, "s");
	bytes.clear();
	many.encode(bytes);
	CHECK_FALSE(S2CAuthChallenge::decode(as_span(bytes)));

	// Truncated payloads never crash.
	bytes.clear();
	ch.encode(bytes);
	for (std::size_t n = 0; n < bytes.size(); ++n) {
		CHECK_FALSE(S2CAuthChallenge::decode(
				std::span<const std::byte>(bytes.data(), n)));
	}
}

TEST_CASE("S2CServerInfo engine_version_req is length-capped and survives truncation") {
	S2CServerInfo info{ "p", "v", 1, 20, 8, "m", AuthMode::kNone, std::string(vb::protocol::kMaxEngineVersionReqBytes + 1, '>') };
	std::vector<std::byte> bytes;
	info.encode(bytes);
	auto over = S2CServerInfo::decode(as_span(bytes));
	CHECK_FALSE(over);
	CHECK(over.error() == vb::core::ProtocolError::kLengthExceeded);

	info.engine_version_req = ">=0.5.0";
	bytes.clear();
	info.encode(bytes);
	for (std::size_t n = 0; n < bytes.size(); ++n) {
		CHECK_FALSE(S2CServerInfo::decode(std::span<const std::byte>(bytes.data(), n)));
	}
	CHECK(S2CServerInfo::decode(as_span(bytes)));
}

TEST_CASE("decode rejects a bad enum and trailing bytes") {
	std::vector<std::byte> bytes;
	S2CServerInfo{ "p", "v", 1, 20, 8, "m", AuthMode::kNone, "" }.encode(bytes);
	bytes[bytes.size() - 2] = std::byte{ 0x7F }; // clobber auth_mode (before the empty engine_version_req)
	auto bad = S2CServerInfo::decode(as_span(bytes));
	CHECK_FALSE(bad);
	CHECK(bad.error() == vb::core::ProtocolError::kBadEnum);

	std::vector<std::byte> extra;
	C2SReady{}.encode(extra);
	extra.push_back(std::byte{ 0 });
	auto trailing = C2SReady::decode(as_span(extra));
	CHECK_FALSE(trailing);
	CHECK(trailing.error() == vb::core::ProtocolError::kTrailingBytes);
}

#if VB_WITH_COMPRESSION

// ARCHITECTURE_SPEC.md §18 Q4 ("Chunk compression: LZ4 vs. zstd vs.
// palette-only") -- resolved LZ4; these cover compress_lz4()/decompress_lz4()
// directly, independent of frame_message()'s own size-threshold policy
// (net_test.cpp covers that end of it).
TEST_CASE("compress_lz4/decompress_lz4 round-trips compressible, "
		  "incompressible, and empty input") {
	// Highly compressible: one repeated byte, the same shape a homogeneous
	// chunk's own RLE'd payload has.
	const std::vector<std::byte> compressible(1000, std::byte{ 0x2A });
	const auto compressed = compress_lz4(as_span(compressible));
	CHECK(compressed.size() < compressible.size());
	auto decoded = decompress_lz4(as_span(compressed));
	REQUIRE(decoded);
	CHECK(*decoded == compressible);

	// Effectively random bytes: LZ4 may not shrink this at all, but the
	// round-trip must still be byte-exact.
	std::vector<std::byte> noisy;
	noisy.reserve(500);
	std::uint32_t x = 0x12345678u;
	for (int i = 0; i < 500; ++i) {
		x = x * 1664525u + 1013904223u; // LCG, deterministic "noise"
		noisy.push_back(static_cast<std::byte>(x & 0xFF));
	}
	const auto compressed_noisy = compress_lz4(as_span(noisy));
	auto decoded_noisy = decompress_lz4(as_span(compressed_noisy));
	REQUIRE(decoded_noisy);
	CHECK(*decoded_noisy == noisy);

	// Empty input: still a valid (4-byte, zero-length-prefix) frame.
	const auto compressed_empty = compress_lz4({});
	auto decoded_empty = decompress_lz4(as_span(compressed_empty));
	REQUIRE(decoded_empty);
	CHECK(decoded_empty->empty());
}

TEST_CASE("decompress_lz4 rejects a truncated buffer instead of "
		  "reading/writing out of bounds") {
	const std::vector<std::byte> original(1000, std::byte{ 0x2A });
	const auto compressed = compress_lz4(as_span(original));
	REQUIRE(compressed.size() > 8);

	// The original-size prefix still says 1000 bytes, but the compressed
	// bytes needed to actually produce them are cut short.
	const std::vector<std::byte> truncated(
			compressed.begin(), compressed.end() - 4);
	auto decoded = decompress_lz4(as_span(truncated));
	CHECK_FALSE(decoded);

	// Just the 4-byte size prefix, no compressed bytes at all.
	const std::vector<std::byte> prefix_only(
			compressed.begin(), compressed.begin() + 4);
	auto decoded2 = decompress_lz4(as_span(prefix_only));
	CHECK_FALSE(decoded2);
}

#endif // VB_WITH_COMPRESSION
