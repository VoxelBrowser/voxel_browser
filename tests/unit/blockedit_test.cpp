#include <doctest/doctest.h>

#include <ostream>

#include <memory>
#include <optional>

#include "vb/net/loopback.hpp"
#include "vb/net/session.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/protocol/world.hpp"
#include "vb/world/block.hpp"
#include "vb/world/world.hpp"
#include "vb/worldgen/generator.hpp"
#include "vb/worldgen/worker_pool.hpp"

using namespace vb::net;
using vb::core::IVec3;
using vb::core::NetId;
using vb::core::Vec3d;
namespace wg = vb::worldgen;

namespace {

std::unique_ptr<WorldReplicator> make_rep(vb::world::World &w,
		wg::WorldGenWorkerPool &pool) {
	return std::make_unique<WorldReplicator>(w, pool,
			vb::world::BlockRegistry::base(), /*view*/ 1, /*vview*/ 2);
}

// First solid voxel scanning down a column.
IVec3 surface_voxel(const vb::world::BlockSolidQuery &q, int x, int z) {
	for (int y = 80; y > -16; --y) {
		if (q.solid_at({ x, y, z })) {
			return { x, y, z };
		}
	}
	return { x, 0, z };
}

} // namespace

TEST_CASE("C2SBlockEdit / S2CBlockEditResult round-trip") {
	vb::protocol::C2SBlockEdit e;
	e.predicted_seq = 42;
	e.action = vb::protocol::BlockEditAction::kPlace;
	e.pos = { -3, 70, 128 };
	e.block = static_cast<vb::core::BlockId>(6);
	std::vector<std::byte> b;
	e.encode(b);
	auto d = vb::protocol::C2SBlockEdit::decode({ b.data(), b.size() });
	REQUIRE(d);
	CHECK(d->predicted_seq == 42);
	CHECK(d->action == vb::protocol::BlockEditAction::kPlace);
	CHECK(d->pos == IVec3{ -3, 70, 128 });
	CHECK(d->block == static_cast<vb::core::BlockId>(6));

	vb::protocol::S2CBlockEditResult r;
	r.predicted_seq = 42;
	r.accepted = true;
	r.pos = e.pos;
	std::vector<std::byte> rb;
	r.encode(rb);
	auto rd = vb::protocol::S2CBlockEditResult::decode({ rb.data(), rb.size() });
	REQUIRE(rd);
	CHECK(rd->accepted);
	CHECK(rd->pos == IVec3{ -3, 70, 128 });
}

namespace {

struct EditWorld {
	LoopbackNetwork net;
	vb::world::World world{ vb::world::BlockRegistry::base() };
	wg::WorldGenWorkerPool pool{ wg::WorldGenerator(wg::WorldGenParams{},
										 vb::world::BlockRegistry::base()),
		wg::WorldGenWorkerPool::kSynchronous };
	std::optional<ServerSession> server;
	std::optional<ClientSession> a;
	std::optional<ClientSession> b;
	NetId a_id{ 0 };
	NetId b_id{ 0 };

	EditWorld() {
		HandshakeServerConfig cfg;
		cfg.world_seed = 7;
		server.emplace(net.server(), cfg);
		server->set_world_replicator(make_rep(world, pool));
		REQUIRE(net.server().listen(0));

		Transport &ta = net.create_client();
		auto ida = ta.connect("x", 0);
		REQUIRE(ida);
		a.emplace(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });
		Transport &tb = net.create_client();
		auto idb = tb.connect("x", 0);
		REQUIRE(idb);
		b.emplace(tb, *idb, HandshakeClientConfig{ "B", "", "v", 2 });

		pump(20);
		REQUIRE(a->joined());
		REQUIRE(b->joined());
		a_id = a->join_accept()->your_net_id;
		b_id = b->join_accept()->your_net_id;
	}

	void pump(int n) {
		for (int i = 0; i < n; ++i) {
			server->tick(0.05);
			a->tick(0.05);
			b->tick(0.05);
		}
	}
};

} // namespace

TEST_CASE("a break edit applies on the server and fans out to both clients") {
	EditWorld w;
	// Both players stand near the origin so the target is in range + streamed.
	w.server->set_player_state(w.a_id, Vec3d{ 4, 40, 4 });
	w.server->set_player_state(w.b_id, Vec3d{ 6, 40, 4 });
	w.pump(6);

	const IVec3 target =
			surface_voxel(w.server->world_replicator()->world(), 4, 4);
	// Put A's eye right above the target, well within reach.
	w.server->set_player_state(
			w.a_id, Vec3d{ target.x + 0.5, target.y + 2.0, target.z + 0.5 });
	w.pump(3);

	REQUIRE(w.a->chunk_store().solid_at(target));

	vb::protocol::C2SBlockEdit e;
	e.predicted_seq = 1;
	e.action = vb::protocol::BlockEditAction::kBreak;
	e.pos = target;
	w.a->push_block_edit(e);
	CHECK(w.a->chunk_store().block_at(target) == vb::core::BlockId::kAir); // optimistic

	w.pump(6);

	CHECK_FALSE(w.server->world_replicator()->world().solid_at(target));
	CHECK(w.a->chunk_store().block_at(target) == vb::core::BlockId::kAir);
	CHECK(w.b->chunk_store().block_at(target) == vb::core::BlockId::kAir);
	CHECK(w.a->pending_edit_count() == 0);
}

TEST_CASE("an out-of-reach edit is rejected and rolled back on the client") {
	EditWorld w;
	w.server->set_player_state(w.a_id, Vec3d{ 4, 40, 4 });
	w.pump(6);

	const IVec3 target =
			surface_voxel(w.server->world_replicator()->world(), 4, 4);
	// A is far above -> outside the 5.5 block reach.
	w.server->set_player_state(w.a_id, Vec3d{ 4, 90, 4 });
	w.pump(3);

	const vb::core::BlockId before = w.a->chunk_store().block_at(target);
	REQUIRE(before != vb::core::BlockId::kAir);

	vb::protocol::C2SBlockEdit e;
	e.predicted_seq = 7;
	e.action = vb::protocol::BlockEditAction::kBreak;
	e.pos = target;
	w.a->push_block_edit(e);
	CHECK(w.a->chunk_store().block_at(target) == vb::core::BlockId::kAir);
	CHECK(w.a->pending_edit_count() == 1);

	w.pump(6);

	CHECK(w.server->world_replicator()->world().solid_at(target));
	CHECK(w.a->chunk_store().block_at(target) == before); // rolled back
	CHECK(w.a->pending_edit_count() == 0);
}

// Phase 6.18 (Growtopia-style combat): ServerSession::punch() is the engine
// primitive a pack's player_input handler calls on a rising attack-key edge
// (content/base/mechanics.lua) -- these test the engine's own target
// resolution directly, with no Lua/PackRuntime involved at all, mirroring
// this file's existing plain-ServerSession style.

TEST_CASE("punch() breaks a max_damage == 0 block on the very first hit") {
	EditWorld w;
	w.server->set_player_state(w.a_id, Vec3d{ 4, 40, 4 });
	w.pump(6);

	const IVec3 target =
			surface_voxel(w.server->world_replicator()->world(), 4, 4);
	// Directly above the target, looking straight down.
	w.server->set_player_state(w.a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	w.pump(3);
	REQUIRE(w.server->world_replicator()->world().solid_at(target));

	const auto result = w.server->punch(w.a_id);

	CHECK(result.hit_block);
	CHECK(result.block_pos == target);
	CHECK(result.block_punches == 1);
	CHECK(result.block_broken);
	CHECK_FALSE(result.hit_player);
	CHECK_FALSE(w.server->world_replicator()->world().solid_at(target));
}

TEST_CASE("punch() accumulates hits on a max_damage > 0 block and only "
		  "breaks it once enough punches land") {
	// Needs its own registry (a custom block, not just base()) built before
	// the World/WorldReplicator that read it -- EditWorld's fields are fixed
	// to BlockRegistry::base(), so this doesn't reuse that fixture.
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	const vb::core::BlockId tough = registry.add_or_get(
			"test:tough_stone", vb::world::BlockType{
										.name = "test:tough_stone",
										.solid = true,
										.opaque = true,
										.texture = "",
										.max_damage = 3,
										.crack_texture = "",
								});
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	server.set_world_replicator(make_rep(world, pool));
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
		}
	};
	pump(20);
	REQUIRE(a.joined());
	const NetId a_id = a.join_accept()->your_net_id;

	server.set_player_state(a_id, Vec3d{ 4, 40, 4 });
	pump(6);
	const IVec3 target = surface_voxel(world, 4, 4);
	world.set_block(target, tough); // overwrite whatever naturally generated
	server.set_player_state(a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	pump(3);
	REQUIRE(world.get_block(target) == tough);

	const auto r1 = server.punch(a_id);
	CHECK(r1.hit_block);
	CHECK(r1.block_punches == 1);
	CHECK_FALSE(r1.block_broken);
	CHECK(world.get_block(target) == tough); // still standing

	const auto r2 = server.punch(a_id);
	CHECK(r2.block_punches == 2);
	CHECK_FALSE(r2.block_broken);

	const auto r3 = server.punch(a_id);
	CHECK(r3.block_punches == 3);
	CHECK(r3.block_broken);
	CHECK(world.get_block(target) == vb::core::BlockId::kAir);
}

// REMAINING_TASKS.md's "vary break time by block/tool" gap: punch()'s
// optional block_damage parameter (default 1, exercised by every test
// above) is the one knob the engine exposes for it -- a pack decides how
// much a given swing counts for (e.g. from the puncher's held item),
// without the engine needing any tool/hardness concept of its own.
TEST_CASE("punch()'s block_damage parameter breaks a max_damage block in "
		  "fewer, heavier hits") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	const vb::core::BlockId tough = registry.add_or_get(
			"test:tough_stone", vb::world::BlockType{
										.name = "test:tough_stone",
										.solid = true,
										.opaque = true,
										.texture = "",
										.max_damage = 3,
										.crack_texture = "",
								});
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	server.set_world_replicator(make_rep(world, pool));
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
		}
	};
	pump(20);
	REQUIRE(a.joined());
	const NetId a_id = a.join_accept()->your_net_id;

	server.set_player_state(a_id, Vec3d{ 4, 40, 4 });
	pump(6);
	const IVec3 target = surface_voxel(world, 4, 4);
	world.set_block(target, tough);
	server.set_player_state(a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	pump(3);
	REQUIRE(world.get_block(target) == tough);

	// A "better tool": 2 damage per swing against a max_damage=3 block --
	// broken on the second swing (2 + 2 = 4 >= 3), not the third.
	const auto r1 = server.punch(a_id, 2);
	CHECK(r1.block_punches == 2);
	CHECK_FALSE(r1.block_broken);

	const auto r2 = server.punch(a_id, 2);
	CHECK(r2.block_punches == 4);
	CHECK(r2.block_broken);
	CHECK(world.get_block(target) == vb::core::BlockId::kAir);
}

TEST_CASE("punch() replicates live block damage (S2C_BlockDamage) to every "
		  "nearby player watching the chunk, not just the puncher") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	const vb::core::BlockId tough = registry.add_or_get(
			"test:tough_stone3", vb::world::BlockType{
										 .name = "test:tough_stone3",
										 .solid = true,
										 .opaque = true,
										 .texture = "",
										 .max_damage = 3,
										 .crack_texture = "",
								 });
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	server.set_world_replicator(make_rep(world, pool));
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });
	Transport &tb = net.create_client();
	auto idb = tb.connect("x", 0);
	REQUIRE(idb);
	ClientSession b(tb, *idb, HandshakeClientConfig{ "B", "", "v", 2 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
			b.tick(0.05);
		}
	};
	pump(20);
	REQUIRE(a.joined());
	REQUIRE(b.joined());
	const NetId a_id = a.join_accept()->your_net_id;

	server.set_player_state(a_id, Vec3d{ 4, 40, 4 });
	server.set_player_state(b.join_accept()->your_net_id, Vec3d{ 6, 40, 4 });
	pump(6);
	const IVec3 target = surface_voxel(world, 4, 4);
	world.set_block(target, tough);
	server.set_player_state(a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	pump(3);

	// B never punches anything -- only A does -- but is standing close enough
	// to mirror the same chunk, so it should still see the live damage value.
	CHECK(b.block_damage().find(target) == b.block_damage().end());

	CHECK(server.punch(a_id).block_punches == 1);
	pump(2);
	REQUIRE(b.block_damage().find(target) != b.block_damage().end());
	CHECK(b.block_damage().at(target) == 1);
	// A itself is also a watcher of its own targeted chunk, same broadcast.
	REQUIRE(a.block_damage().find(target) != a.block_damage().end());
	CHECK(a.block_damage().at(target) == 1);

	CHECK(server.punch(a_id).block_punches == 2);
	pump(2);
	CHECK(b.block_damage().at(target) == 2);

	// The third punch breaks it -- both clients' replicated damage entries
	// for `target` are dropped rather than left stuck at a stale value.
	CHECK(server.punch(a_id).block_broken);
	pump(2);
	CHECK(b.block_damage().find(target) == b.block_damage().end());
	CHECK(a.block_damage().find(target) == a.block_damage().end());
}

TEST_CASE("punch() self-heals an idle block's punch count back to 0 over "
		  "time, and never breaks it along the way") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	const vb::core::BlockId tough = registry.add_or_get(
			"test:tough_stone2", vb::world::BlockType{
										 .name = "test:tough_stone2",
										 .solid = true,
										 .opaque = true,
										 .texture = "",
										 .max_damage = 5,
										 .crack_texture = "",
								 });
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	// Fast heal timings so the test doesn't need hundreds of ticks to prove
	// the mechanism -- the timing values themselves are exactly what
	// vb.combat.set_params overrides in real content.
	ServerSession::PunchParams pp;
	pp.heal_after_seconds = 0.1;
	pp.heal_interval_seconds = 0.1;
	server.set_punch_params(pp);
	server.set_world_replicator(make_rep(world, pool));
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
		}
	};
	pump(20);
	REQUIRE(a.joined());
	const NetId a_id = a.join_accept()->your_net_id;

	server.set_player_state(a_id, Vec3d{ 4, 40, 4 });
	pump(6);
	const IVec3 target = surface_voxel(world, 4, 4);
	world.set_block(target, tough);
	server.set_player_state(a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	pump(3);

	CHECK(server.punch(a_id).block_punches == 1);
	CHECK(server.punch(a_id).block_punches == 2);
	pump(2);
	REQUIRE(a.block_damage().find(target) != a.block_damage().end());
	CHECK(a.block_damage().at(target) == 2);

	// Idle well past heal_after_seconds + enough heal_interval_seconds
	// steps to fully repair 2 punches (0.1 + 2*0.1 = 0.3s; 1.0s of idle is
	// plenty) -- no more punches land in between.
	pump(20);

	// Healing back to 0 broadcasts a clearing S2C_BlockDamage exactly like a
	// break does -- the client never keeps a stale "2 punches" entry around
	// once the server has quietly forgiven them all.
	CHECK(a.block_damage().find(target) == a.block_damage().end());

	const auto after_heal = server.punch(a_id);
	CHECK(after_heal.block_punches == 1); // healed back to 0, this is fresh
	CHECK_FALSE(after_heal.block_broken);
	CHECK(world.get_block(target) == tough);
}

TEST_CASE("punch() landing again resets the heal clock instead of stacking "
		  "idle time from before it") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	const vb::core::BlockId tough = registry.add_or_get(
			"test:tough_stone3", vb::world::BlockType{
										 .name = "test:tough_stone3",
										 .solid = true,
										 .opaque = true,
										 .texture = "",
										 .max_damage = 5,
										 .crack_texture = "",
								 });
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	ServerSession::PunchParams pp;
	pp.heal_after_seconds = 0.5;
	pp.heal_interval_seconds = 0.5;
	server.set_punch_params(pp);
	server.set_world_replicator(make_rep(world, pool));
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
		}
	};
	pump(20);
	REQUIRE(a.joined());
	const NetId a_id = a.join_accept()->your_net_id;

	server.set_player_state(a_id, Vec3d{ 4, 40, 4 });
	pump(6);
	const IVec3 target = surface_voxel(world, 4, 4);
	world.set_block(target, tough);
	server.set_player_state(a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	pump(3);

	CHECK(server.punch(a_id).block_punches == 1);
	pump(6); // 0.3s idle -- under heal_after_seconds (0.5s), no heal yet
	const auto r = server.punch(a_id); // resets the idle clock again
	CHECK(r.block_punches == 2); // NOT healed back to 1 first
	pump(6); // another 0.3s -- still under 0.5s since THIS punch
	CHECK(world.get_block(target) == tough);
	CHECK(server.punch(a_id).block_punches == 3); // still no heal happened
}

TEST_CASE("punch() prefers a closer player over a block further along the "
		  "same ray") {
	EditWorld w;
	// Both float at y=100, well above any generated terrain (no block along
	// the ray to compete with the player hit).
	w.server->set_player_state(
			w.a_id, Vec3d{ 0, 100, 0 }, vb::core::Vec2f{ 180.0f, 0.0f });
	w.server->set_player_state(w.b_id, Vec3d{ 0, 100, 3 });
	w.pump(3);

	const auto result = w.server->punch(w.a_id);

	CHECK(result.hit_player);
	CHECK(result.target == w.b_id);
	CHECK_FALSE(result.hit_block);
}

TEST_CASE("punch() hits nothing when no block or player is within reach") {
	EditWorld w;
	w.server->set_player_state(
			w.a_id, Vec3d{ 0, 100, 0 }, vb::core::Vec2f{ 0.0f, 0.0f });
	w.server->set_player_state(w.b_id, Vec3d{ 500, 100, 500 }); // far away
	w.pump(3);

	const auto result = w.server->punch(w.a_id);

	CHECK_FALSE(result.hit_player);
	CHECK_FALSE(result.hit_block);
}

// Phase 6 leftovers: PvP armor, knockback and the swing clip.
TEST_CASE("punch() scales PvP damage by the victim's armor") {
	EditWorld w;
	w.server->set_player_state(
			w.a_id, Vec3d{ 0, 100, 0 }, vb::core::Vec2f{ 180.0f, 0.0f });
	w.server->set_player_state(w.b_id, Vec3d{ 0, 100, 3 });
	w.pump(3);
	const float before = w.server->player_health(w.b_id)->first;

	w.server->set_player_armor(w.b_id, 0.5f);
	REQUIRE(w.server->punch(w.a_id).hit_player);
	const float after = w.server->player_health(w.b_id)->first;
	CHECK(before - after == doctest::Approx(0.5f));

	// Out-of-range fractions clamp rather than making the victim immune/heal.
	w.server->set_player_armor(w.b_id, 5.0f);
	CHECK(*w.server->player_armor(w.b_id) == doctest::Approx(0.95f));
	w.server->set_player_armor(w.b_id, -1.0f);
	CHECK(*w.server->player_armor(w.b_id) == doctest::Approx(0.0f));
}

TEST_CASE("punch() knocks a hit player back only when configured") {
	EditWorld w;
	w.server->set_player_state(
			w.a_id, Vec3d{ 0, 100, 0 }, vb::core::Vec2f{ 180.0f, 0.0f });
	w.server->set_player_state(w.b_id, Vec3d{ 0, 100, 3 });
	w.pump(3);
	w.server->set_player_velocity(w.b_id, Vec3d{});

	REQUIRE(w.server->punch(w.a_id).hit_player);
	const auto untouched = *w.server->player_velocity(w.b_id);
	CHECK(untouched.x == doctest::Approx(0.0));
	CHECK(untouched.z == doctest::Approx(0.0));

	vb::net::ServerSession::PunchParams params;
	params.knockback_speed = 6.0f;
	params.knockback_lift = 3.0f;
	w.server->set_punch_params(params);
	REQUIRE(w.server->punch(w.a_id).hit_player);
	const auto pushed = *w.server->player_velocity(w.b_id);
	// Yaw 180 looks along +z in this fixture (the victim stands at z = 3).
	CHECK(pushed.z > 5.0);
	CHECK(pushed.y == doctest::Approx(3.0));
}

TEST_CASE("punch() plays and then clears the configured swing clip") {
	EditWorld w;
	vb::net::ServerSession::PunchParams params;
	params.swing_clip = "swing";
	params.swing_clip_seconds = 0.2;
	w.server->set_punch_params(params);
	w.server->set_player_state(
			w.a_id, Vec3d{ 0, 100, 0 }, vb::core::Vec2f{ 0.0f, 0.0f });
	w.pump(3);
	CHECK_FALSE(w.server->script_entity_clip(w.a_id).has_value());

	w.server->punch(w.a_id); // whiffs: the swing animates anyway
	CHECK(w.server->script_entity_clip(w.a_id) == std::optional<std::string>("swing"));

	w.pump(8); // 8 * 0.05s = 0.4s > swing_clip_seconds
	CHECK_FALSE(w.server->script_entity_clip(w.a_id).has_value());

	// A pack's own clip set after the swing is left alone when it expires.
	w.server->punch(w.a_id);
	w.server->set_script_entity_clip(w.a_id, "dance");
	w.pump(8);
	CHECK(w.server->script_entity_clip(w.a_id) == std::optional<std::string>("dance"));
}

// REMAINING_TASKS.md's "no punch-rate cooldown enforced engine-side" gap
// (PunchParams::punch_cooldown_seconds).
TEST_CASE("punch() enforces a configured cooldown between swings") {
	EditWorld w;
	vb::net::ServerSession::PunchParams params;
	params.punch_cooldown_seconds = 1.0;
	w.server->set_punch_params(params);

	w.server->set_player_state(w.a_id, Vec3d{ 4, 40, 4 });
	w.pump(6);
	const IVec3 target =
			surface_voxel(w.server->world_replicator()->world(), 4, 4);
	w.server->set_player_state(w.a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	w.pump(3);
	REQUIRE(w.server->world_replicator()->world().solid_at(target));

	// First swing lands and breaks the (max_damage == 0) block.
	CHECK(w.server->punch(w.a_id).hit_block);
	// A second swing immediately after, no time elapsed, is still on
	// cooldown -- silent no-op, not "nothing within reach" (there's a block
	// right there, it's just too soon to swing again).
	const auto still_cooling_down = w.server->punch(w.a_id);
	CHECK_FALSE(still_cooling_down.hit_block);
	CHECK_FALSE(still_cooling_down.hit_player);

	// Advancing real time past the cooldown (system_network_io() ticks it
	// down once per server tick) lets the next swing land again.
	w.pump(21); // 21 * 0.05s = 1.05s > punch_cooldown_seconds
	w.server->set_player_state(w.a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	w.pump(1);
	// Nothing solid left at `target` (broken above) -- punch a fresh
	// neighbor voxel instead to prove the *cooldown* cleared, not just that
	// the old block respawned.
	const IVec3 target2 =
			surface_voxel(w.server->world_replicator()->world(), 4, 5);
	w.server->set_player_state(w.a_id,
			Vec3d{ target2.x + 0.5, target2.y + 3.0, target2.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	w.pump(1);
	CHECK(w.server->punch(w.a_id).hit_block);
}

TEST_CASE("punch() cooldown is disabled by default -- back-to-back swings "
		  "both land") {
	EditWorld w;
	LoopbackNetwork net2;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	const vb::core::BlockId tough = registry.add_or_get(
			"test:tough_stone_cooldown", vb::world::BlockType{
												 .name = "test:tough_stone_cooldown",
												 .solid = true,
												 .opaque = true,
												 .texture = "",
												 .max_damage = 5,
												 .crack_texture = "",
										 });
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net2.server(), cfg);
	server.set_world_replicator(make_rep(world, pool));
	REQUIRE(net2.server().listen(0));

	Transport &ta = net2.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
		}
	};
	pump(20);
	REQUIRE(a.joined());
	const NetId a_id = a.join_accept()->your_net_id;

	server.set_player_state(a_id, Vec3d{ 4, 40, 4 });
	pump(6);
	const IVec3 target = surface_voxel(world, 4, 4);
	world.set_block(target, tough);
	server.set_player_state(a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	pump(3);

	CHECK(server.punch(a_id).block_punches == 1);
	CHECK(server.punch(a_id).block_punches == 2); // no cooldown -> lands too
}

// Phase 6.21: WorldReplicator::set_reach(...) is the one shared knob both
// block-edit reach (WorldReplicator::apply_block_edit/in_reach) and combat
// reach (ServerSession::punch(), which reads world_replicator()->reach())
// now read -- widening it should widen both at once, from the same call,
// with no separate PunchParams::reach left to independently override.
TEST_CASE("WorldReplicator::set_reach widens both block-edit reach and "
		  "punch() reach from the same one value") {
	EditWorld w;
	w.server->set_player_state(w.a_id, Vec3d{ 4, 40, 4 });
	w.pump(6);

	const IVec3 target =
			surface_voxel(w.server->world_replicator()->world(), 4, 4);
	// Far enough above to be outside the default 5.5 block reach, but well
	// within a widened one.
	const Vec3d far_above{
		target.x + 0.5, target.y + 20.0, target.z + 0.5
	};
	w.server->set_player_state(w.a_id, far_above, vb::core::Vec2f{ 0.0f, -90.0f });
	w.pump(3);

	// Default reach: too far for either check to accept.
	CHECK_FALSE(w.server->world_replicator()->in_reach(far_above, target));
	CHECK_FALSE(w.server->punch(w.a_id).hit_block);

	w.server->world_replicator()->set_reach(25.0);
	w.pump(1);

	CHECK(w.server->world_replicator()->in_reach(far_above, target));
	const auto result = w.server->punch(w.a_id);
	CHECK(result.hit_block);
	CHECK(result.block_pos == target);
}
