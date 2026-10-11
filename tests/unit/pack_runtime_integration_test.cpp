#include <doctest/doctest.h>

#include <ostream>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "vb/net/loopback.hpp"
#include "vb/net/session.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/protocol/world.hpp"
#include "vb/script/pack_runtime.hpp"
#include "vb/script/ui_runtime.hpp"
#include "vb/world/block.hpp"
#include "vb/world/world.hpp"
#include "vb/worldgen/generator.hpp"
#include "vb/worldgen/worker_pool.hpp"

#if VB_WITH_LUA

// End-to-end proof that Phase 4.2's block-edit veto/callback seam and
// player_join veto actually reach a real ServerSession/WorldReplicator over
// a LoopbackTransport, not just the bindings in isolation (pack_runtime_test.cpp).

using namespace vb::net;
using vb::core::IVec3;
using vb::core::NetId;
using vb::core::Vec3d;
namespace wg = vb::worldgen;

namespace {

std::filesystem::path temp_storage(const char *name) {
	auto p = std::filesystem::temp_directory_path() /
			(std::string("vb_pack_runtime_integration_") + name + ".json");
	std::filesystem::remove(p);
	return p;
}

IVec3 surface_voxel(const vb::world::BlockSolidQuery &q, int x, int z) {
	for (int y = 80; y > -16; --y) {
		if (q.solid_at({ x, y, z })) {
			return { x, y, z };
		}
	}
	return { x, 0, z };
}

} // namespace

TEST_CASE("pack script vetoes a block break and observes on_break") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("veto"));
	REQUIRE(rt.load_pack_file(R"(
		on_break_calls = 0
		local function count() on_break_calls = on_break_calls + 1 end
		-- The scanned surface voxel is base:grass or base:sand (beach)
		-- depending on the noise at that column; attach to both.
		vb.register_block({ name = "base:grass", on_break = count })
		vb.register_block({ name = "base:sand", on_break = count })
		vb.on("block_break", function(player, pos) return pos.y > 0 end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry,
			/*view*/ 1, /*vview*/ 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	// A surface voxel (y > 0): the veto only rejects y <= 0.
	const IVec3 target = surface_voxel(world, 4, 4);
	REQUIRE(target.y > 0);
	server.set_player_state(
			a_id, Vec3d{ target.x + 0.5, target.y + 2.0, target.z + 0.5 });
	pump(6);
	REQUIRE(client.chunk_store().solid_at(target));

	vb::protocol::C2SBlockEdit e;
	e.predicted_seq = 1;
	e.action = vb::protocol::BlockEditAction::kBreak;
	e.pos = target;
	client.push_block_edit(e);
	pump(6);

	CHECK_FALSE(world.solid_at(target)); // accepted
	CHECK_FALSE(client.chunk_store().solid_at(target));
	REQUIRE(rt.load_pack_file("assert(on_break_calls == 1)"));

	// A deep target: same reach setup but the veto (pos.y > 0) should now
	// reject it, so the block stays intact.
	const IVec3 deep{ 20, -5, 20 };
	server.set_player_state(
			a_id, Vec3d{ deep.x + 0.5, deep.y + 2.0, deep.z + 0.5 });
	pump(6);
	REQUIRE(world.solid_at(deep)); // generator fills solid stone this deep

	vb::protocol::C2SBlockEdit e2;
	e2.predicted_seq = 2;
	e2.action = vb::protocol::BlockEditAction::kBreak;
	e2.pos = deep;
	client.push_block_edit(e2);
	pump(6);

	CHECK(world.solid_at(deep)); // vetoed: unchanged
}

// Phase 6.17: block breaking is no longer an engine default (the old
// hardcoded client-side hold-to-break timer is gone). The client only ever
// reports raw input (InputCmd::buttons's kInputPrimary bit, "LMB held");
// this proves (a) holding it does nothing at all without any pack policy,
// and (b) a minimal vb.on("player_input", ...) handler calling the new
// player:break_block() is enough for a pack to implement breaking itself,
// through the exact same validated pipeline a real C2S_BlockEdit uses.
TEST_CASE("block breaking is opt-in content, not an engine default: "
		  "buttons.primary alone does nothing until a pack calls "
		  "player:break_block()") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("break_block"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry,
			/*view*/ 1, /*vview*/ 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	const IVec3 target = surface_voxel(world, 4, 4);
	REQUIRE(target.y > 0);
	pump(6);
	REQUIRE(world.solid_at(target));

	// Holding "primary" every cmd, with no pack handler registered at all,
	// must not break anything -- breaking has to be something a pack opts
	// into, not a side effect the engine produces on its own. Note this
	// deliberately does *not* call server.set_player_state() to line the
	// player up with `target` first: sending a real InputCmd (unlike
	// pack_runtime_integration_test.cpp's veto test above, which only ever
	// sends a single C2S_BlockEdit) marks the connection input-driven, and
	// handle_input_batch's post-loop interest_.upsert() then overwrites the
	// interest-grid position with the ECS-authoritative one every tick
	// regardless -- there'd be nothing left to assert about reach.
	vb::protocol::InputCmd held;
	held.buttons = vb::protocol::kInputPrimary;
	for (std::uint32_t i = 1; i <= 5; ++i) {
		held.seq = i;
		client.push_input(held);
		pump(1);
	}
	CHECK(world.solid_at(target)); // untouched: no pack policy at all

	// Now install a minimal player_input handler that breaks the exact
	// target the instant it sees buttons.primary held. ServerSession only
	// wires its input handler at attach_session() time, gated on whether a
	// "player_input" handler was registered *by then* (Phase 6.3's "packs
	// that never use this channel pay zero extra cost" posture) -- since
	// this test registers one only now, well after the first attach_session()
	// call, it must call attach_session() again to actually pick it up.
	REQUIRE(rt.load_pack_file(
			"vb.on('player_input', function(player, input) "
			"if input.buttons.primary then player:break_block(" +
			std::to_string(target.x) + ", " + std::to_string(target.y) + ", " +
			std::to_string(target.z) + ") end end)"));
	rt.attach_session(server);

	// set_player_state() right before the triggering cmd, not earlier: the
	// player_input hook fires *inside* handle_input_batch's per-cmd loop,
	// before that same call's post-loop interest_.upsert() overwrite (see
	// the comment above) -- so this position is exactly what the hook (and
	// therefore break_block's reach check) sees for this one cmd.
	server.set_player_state(
			a_id, Vec3d{ target.x + 0.5, target.y + 2.0, target.z + 0.5 });
	held.seq = 100;
	client.push_input(held);
	pump(3);

	CHECK_FALSE(world.solid_at(target));
	CHECK_FALSE(client.chunk_store().solid_at(target));
}

TEST_CASE("shared block-damage breaking: begin -> tick -> completes the break "
		  "(Phase 6.5)") {
	// Registry must be frozen (pack loaded) *before* World copies it (matches
	// src/server/main.cpp's real construction order) -- otherwise the new
	// "test:crumbly" block and its max_damage never reach World's own
	// BlockRegistry copy, which is what apply_block_edit/handle_block_break_
	// begin actually query.
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("block_damage"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_block({ name = "test:crumbly", max_damage = 3 })
		vb.on("block_break_tick", function(player, pos) return 1 end)
	)"));
	rt.freeze();

	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry,
			/*view*/ 1, /*vview*/ 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	const IVec3 target = surface_voxel(world, 4, 4);
	REQUIRE(target.y > 0);
	const vb::core::BlockId crumbly = registry.find("test:crumbly");
	REQUIRE(crumbly != vb::core::BlockId::kAir);
	world.set_block(target, crumbly);
	REQUIRE(world.solid_at(target));

	server.set_player_state(
			a_id, Vec3d{ target.x + 0.5, target.y + 2.0, target.z + 0.5 });
	pump(1);

	client.send_block_break_begin(target, { 0, 1, 0 });
	pump(1);
	CHECK(world.solid_at(target)); // one tick of damage (1/3), not broken yet

	pump(3); // two more ticks of damage_tick_fn -> 3 == max_damage
	// completed: the existing BlockEdit pipeline actually broke it
	CHECK_FALSE(world.solid_at(target));
}

TEST_CASE("a target with max_damage == 0 never reaches the damage system") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("block_damage_zero"));
	// Registers the hook, but the target block (base:stone) keeps its default
	// max_damage == 0 -- begin must be rejected outright regardless.
	REQUIRE(rt.load_pack_file(R"(
		vb.on("block_break_tick", function(player, pos) return 100 end)
	)"));
	rt.freeze();

	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry, 1, 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	const IVec3 deep{ 20, -5, 20 };
	server.set_player_state(
			a_id, Vec3d{ deep.x + 0.5, deep.y + 2.0, deep.z + 0.5 });
	pump(6); // let the chunk around `deep` actually load
	REQUIRE(world.solid_at(deep)); // generator fills solid stone this deep

	client.send_block_break_begin(deep, { 0, 1, 0 });
	pump(20); // even a huge per-tick delta never accrues -- begin was rejected
	CHECK(world.solid_at(deep));
}

// REMAINING_TASKS.md's "vary break time by block/tool" gap: player:punch()'s
// optional block_damage argument, exercised end-to-end through a real
// vb.on("player_input") handler (the same shape content/base/mechanics.lua
// itself uses), not just the direct C++ call blockedit_test.cpp's own
// punch() cases exercise.
TEST_CASE("player:punch(block_damage) lets a pack break a tough block in "
		  "fewer, heavier swings") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	const vb::core::BlockId tough = registry.add_or_get(
			"test:tough_ore", vb::world::BlockType{
									  .name = "test:tough_ore",
									  .solid = true,
									  .opaque = true,
									  .texture = "",
									  .max_damage = 3,
									  .crack_texture = "",
							  });

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("punch_block_damage"));
	// A "pickaxe" that hits for 2 instead of the default 1 -- a pack decides
	// this however it likes (here: unconditionally, standing in for a real
	// held-item lookup); the engine only ever sees the resulting number.
	REQUIRE(rt.load_pack_file(
			"vb.on('player_input', function(player, input) "
			"if input.buttons.primary then player:punch(2) end end)"));
	rt.freeze();

	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry, 1, 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	const IVec3 target = surface_voxel(world, 4, 4);
	pump(6);
	world.set_block(target, tough);
	// Directly above the target, looking straight down -- same posture
	// blockedit_test.cpp's own punch() tests use.
	server.set_player_state(a_id,
			Vec3d{ target.x + 0.5, target.y + 3.0, target.z + 0.5 },
			vb::core::Vec2f{ 0.0f, -90.0f });
	pump(3);
	REQUIRE(world.get_block(target) == tough);

	vb::protocol::InputCmd hit;
	hit.buttons = vb::protocol::kInputPrimary;
	hit.pitch = -90.0f;

	hit.seq = 1;
	client.push_input(hit);
	pump(1);
	CHECK(world.get_block(target) == tough); // 2 of 3 -- not broken yet

	hit.seq = 2;
	client.push_input(hit);
	pump(1);
	CHECK(world.get_block(target) == vb::core::BlockId::kAir); // 4 >= 3
}

TEST_CASE("pack script vetoes a specific player's join") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("join_veto"));
	REQUIRE(rt.load_pack_file(
			R"(vb.on("player_join", function(name) return name ~= "Blocked" end))"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	HandshakeServerHost host;
	rt.install_join_veto(host);
	ServerSession server(net.server(), cfg, host);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession good(ta, *ida, HandshakeClientConfig{ "Alice", "", "v", 1 });

	Transport &tb = net.create_client();
	auto idb = tb.connect("x", 0);
	REQUIRE(idb);
	ClientSession blocked(tb, *idb, HandshakeClientConfig{ "Blocked", "", "v", 2 });

	for (int i = 0; i < 20; ++i) {
		server.tick(0.05);
		good.tick(0.05);
		blocked.tick(0.05);
	}

	CHECK(good.joined());
	CHECK(blocked.failed());
}

TEST_CASE("pack script vetoes and replaces player input via a handler chain") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("input"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_keybind("dash")
		seen_dash = nil
		second_saw_move_x = nil
		second_saw_move_z = nil
		vb.on("player_input", function(player, input)
			seen_dash = input.keybinds["dash"]
			if input.buttons.secondary then
				return false -- veto
			end
			if input.keybinds["dash"] then
				-- Only override x; y/z (and every other field) must pass
				-- through unchanged to the next handler in the chain.
				return { move = { x = 42.0 } }
			end
		end)
		vb.on("player_input", function(player, input)
			second_saw_move_x = input.move.x
			second_saw_move_z = input.move.z
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	HandshakeServerHost host;
	rt.install_keybind_registry(host);
	ServerSession server(net.server(), cfg, host);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry,
			/*view*/ 1, /*vview*/ 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	// The registered keybind reached the client as S2C_KeybindRegistry,
	// alongside the engine's 8 pre-registered move/action names (Phase
	// 6.19) that every PackRuntime now seeds before any pack script runs.
	REQUIRE(client.registered_keybinds().size() == 9);
	const auto &names = client.registered_keybinds();
	const auto dash_it = std::find(names.begin(), names.end(), "dash");
	REQUIRE(dash_it != names.end());
	const std::size_t dash_bit =
			static_cast<std::size_t>(dash_it - names.begin());

	const Vec3d pos_at_join = server.player_move_state(a_id)->position;

	// Cmd 1: secondary held -> vetoed. Its effect on movement is dropped
	// entirely (not even gravity), so the authoritative position must be
	// bit-for-bit unchanged.
	vb::protocol::InputCmd veto_cmd;
	veto_cmd.seq = 1;
	veto_cmd.dt = 0.05f;
	veto_cmd.buttons = vb::protocol::kInputSecondary;
	client.push_input(veto_cmd);
	pump(4);

	REQUIRE(rt.load_pack_file("assert(seen_dash == false)"));
	const Vec3d pos_after_veto = server.player_move_state(a_id)->position;
	CHECK(pos_after_veto.x == doctest::Approx(pos_at_join.x));
	CHECK(pos_after_veto.y == doctest::Approx(pos_at_join.y));
	CHECK(pos_after_veto.z == doctest::Approx(pos_at_join.z));

	// Cmd 2: dash held, not vetoed -> the first handler's { move = { x = 42
	// } } reaches the second handler with x replaced but z untouched, and
	// movement actually integrates this time (position changes).
	vb::protocol::InputCmd dash_cmd;
	dash_cmd.seq = 2;
	dash_cmd.dt = 0.05f;
	dash_cmd.move = { 1.0f, 0.0f, 7.0f };
	dash_cmd.keybinds = 1u << dash_bit;
	client.push_input(dash_cmd);
	pump(4);

	REQUIRE(rt.load_pack_file(R"(
		assert(seen_dash == true)
		assert(second_saw_move_x == 42.0)
		assert(second_saw_move_z == 7.0)
	)"));
	const Vec3d pos_after_dash = server.player_move_state(a_id)->position;
	CHECK((pos_after_dash.x != pos_after_veto.x ||
			pos_after_dash.y != pos_after_veto.y ||
			pos_after_dash.z != pos_after_veto.z));
}

TEST_CASE("player:get_selected_slot()/get_held_item() track a real client's "
		  "InputCmd::selected_slot") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("held_item_live"));
	REQUIRE(rt.load_pack_file(R"(
		last_selected_slot = nil
		last_held_item = "unset"
		vb.on("chat", function(player, text)
			if text == "give" then
				player:give({ item = 5, count = 9 })
				return false
			elseif text == "check" then
				last_selected_slot = player:get_selected_slot()
				local held = player:get_held_item()
				last_held_item = held and held.item or nil
				return false
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());

	// Before any InputCmd carries a selected_slot, the default (slot 1,
	// index 0) applies.
	client.send_chat("check");
	pump(4);
	REQUIRE(rt.load_pack_file("assert(last_selected_slot == 1)"));

	// give() puts one stack of item 5 into slot 1 -- slot 1 is still what's
	// "held" until the client selects something else.
	client.send_chat("give");
	pump(4);
	client.send_chat("check");
	pump(4);
	REQUIRE(rt.load_pack_file("assert(last_held_item == 5)"));

	// Now select slot index 4 (5th slot, empty) via a real InputCmd -- the
	// wire's 0-based selected_slot should surface as Lua's 1-based slot 5,
	// with nothing held there.
	vb::protocol::InputCmd cmd;
	cmd.seq = 1;
	cmd.dt = 0.05f;
	cmd.selected_slot = 4;
	client.push_input(cmd);
	pump(4);
	client.send_chat("check");
	pump(4);
	REQUIRE(rt.load_pack_file(R"(
		assert(last_selected_slot == 5)
		assert(last_held_item == nil)
	)"));
}

TEST_CASE("player_leave dispatch fires with the right net id") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("leave"));
	REQUIRE(rt.load_pack_file(R"(
		left_fired = false
		left_name = nil
		left_pos = nil
		vb.on("player_leave", function(p)
			left_fired = true
			left_name = p:get_name()
			left_pos = p:get_pos() -- the last known position, not "entity is gone"
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry,
			/*view*/ 0, /*vview*/ 0);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "Leaver", "", "v", 1 });

	for (int i = 0; i < 20; ++i) {
		server.tick(0.05);
		client.tick(0.05);
	}
	REQUIRE(client.joined());
	const NetId expected_id = client.join_accept()->your_net_id;

	ta.close(*ida, "left"); // triggers the server's kDisconnected event

	NetId captured_id = NetId::kInvalid;
	for (int i = 0; i < 10; ++i) {
		server.tick(0.05);
		for (auto &l : server.take_leaves()) {
			captured_id = l.net_id;
			rt.dispatch_player_leave(l);
		}
	}

	CHECK(captured_id == expected_id);
	const auto check = rt.load_pack_file(R"(
		assert(left_fired == true)
		assert(left_name == "Leaver", "player_leave get_name(): [" .. tostring(left_name) .. "]")
		assert(type(left_pos) == "table" and type(left_pos.y) == "number")
	)");
	REQUIRE(check);
}

TEST_CASE("client UI round trip: server open_ui -> click -> server ui_event") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("ui"));
	REQUIRE(rt.load_pack_file(R"(
		seen_ui_name = nil
		seen_widget_id = nil
		seen_kind = nil
		seen_value = nil
		vb.on("ui_event", function(player, ui_name, widget_id, kind, value)
			seen_ui_name = ui_name
			seen_widget_id = widget_id
			seen_kind = kind
			seen_value = value
		end)
		-- Piggyback on block_break: it's the first event that hands the
		-- script a PlayerHandle for an online player (player_join fires
		-- before a net id/session exist).
		vb.on("block_break", function(player, pos)
			player:open_ui("test_ui", { greeting = "hi" })
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry,
			/*view*/ 1, /*vview*/ 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	vb::script::UiRuntime ui_runtime;
	ui_runtime.attach_session(client);
	REQUIRE(ui_runtime.load_pack_file(R"(
		ui.define("test_ui", function(state)
			return {
				widgets = {
					{ id = "close_btn", type = "button", x = 0, y = 0, w = 10, h = 10,
					  text = "Close",
					  on_click = function() ui.send_event("clicked", true) end },
				}
			}
		end)
	)"));

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
			if (auto opened = client.take_open_ui()) {
				ui_runtime.open(opened->ui_name, opened->ctx_json);
			}
			if (ui_runtime.is_open()) {
				ui_runtime.render_frame();
			}
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	const IVec3 target = surface_voxel(world, 4, 4);
	server.set_player_state(
			a_id, Vec3d{ target.x + 0.5, target.y + 2.0, target.z + 0.5 });
	pump(6);
	REQUIRE(client.chunk_store().solid_at(target));

	vb::protocol::C2SBlockEdit e;
	e.predicted_seq = 1;
	e.action = vb::protocol::BlockEditAction::kBreak;
	e.pos = target;
	client.push_block_edit(e);
	pump(6);

	REQUIRE(ui_runtime.is_open());
	CHECK(ui_runtime.current_name() == "test_ui");
	REQUIRE(ui_runtime.widgets().size() == 1);

	ui_runtime.report_click("close_btn");
	pump(4);

	REQUIRE(rt.load_pack_file(R"(
		assert(seen_ui_name == "test_ui")
		assert(seen_widget_id == "close_btn")
		assert(seen_kind == "clicked")
		assert(seen_value == true)
	)"));
}

TEST_CASE("client HUD round trip: a HUD widget's on_click -> ui.send_event "
		  "reaches the server with ui_name = \"hud\" (REMAINING_TASKS' 'HUD "
		  "widgets aren't wired to report_click/report_change' gap)") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("hud_ui"));
	REQUIRE(rt.load_pack_file(R"(
		seen_ui_name = nil
		seen_widget_id = nil
		vb.on("ui_event", function(player, ui_name, widget_id, kind, value)
			seen_ui_name = ui_name
			seen_widget_id = widget_id
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry,
			/*view*/ 1, /*vview*/ 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	vb::script::UiRuntime ui_runtime;
	ui_runtime.attach_session(client);
	REQUIRE(ui_runtime.load_pack_file(R"(
		ui.define_hud(function(state)
			return {
				widgets = {
					{ id = "hud_btn", type = "button", x = 0, y = 0, w = 10, h = 10,
					  text = "Poke", on_click = function() ui.send_event("poked", true) end },
				}
			}
		end)
	)"));

	for (int i = 0; i < 20; ++i) {
		server.tick(0.05);
		client.tick(0.05);
		ui_runtime.render_hud();
	}
	REQUIRE(client.joined());

	ui_runtime.report_hud_click("hud_btn");
	for (int i = 0; i < 4; ++i) {
		server.tick(0.05);
		client.tick(0.05);
	}

	REQUIRE(rt.load_pack_file(R"(
		assert(seen_ui_name == "hud")
		assert(seen_widget_id == "hud_btn")
	)"));
}

TEST_CASE("pack script vetoes chat from a specific player") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("chat_veto"));
	REQUIRE(rt.load_pack_file(
			R"(vb.on("chat", function(player, text) return player:get_name() ~= "Blocked" end))"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession allowed(ta, *ida, HandshakeClientConfig{ "Allowed", "", "v", 1 });

	Transport &tb = net.create_client();
	auto idb = tb.connect("x", 0);
	REQUIRE(idb);
	ClientSession blocked(tb, *idb, HandshakeClientConfig{ "Blocked", "", "v", 2 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			allowed.tick(0.05);
			blocked.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(allowed.joined());
	REQUIRE(blocked.joined());
	// Drain the join system line(s) both clients may have picked up while
	// joining near-simultaneously -- not what this test asserts on.
	allowed.take_chat_messages();
	blocked.take_chat_messages();

	allowed.send_chat("hi everyone");
	blocked.send_chat("i should not be heard");
	pump(4);

	const auto seen = allowed.take_chat_messages();
	REQUIRE(seen.size() == 1);
	CHECK(seen[0] == "Allowed: hi everyone");
	CHECK(blocked.take_chat_messages() == seen); // same broadcast, both see it
}

TEST_CASE("pack script rewrites chat text before it broadcasts (Phase 6.10)") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("chat_rewrite"));
	// Two handlers chained: the first uppercases, the second appends a tag --
	// proves each handler sees the prior one's replacement, not the original
	// C2S_Chat text, same chaining contract as run_player_input.
	REQUIRE(rt.load_pack_file(R"(
		vb.on("chat", function(player, text) return text:upper() end)
		vb.on("chat", function(player, text) return text .. " [mod]" end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	client.take_chat_messages(); // drain join system line(s)

	client.send_chat("hi everyone");
	pump(4);

	const auto seen = client.take_chat_messages();
	REQUIRE(seen.size() == 1);
	CHECK(seen[0] == "A: HI EVERYONE [mod]");
}

TEST_CASE("player:give() pushes a live S2C_Inventory to the client") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("inventory"));
	// No player-handle-bearing event fires at join time (spec §4.2's own gap
	// note: player_join only hands a name), so drive give() from the chat
	// veto seam instead -- it already hands a real PlayerHandle.
	REQUIRE(rt.load_pack_file(R"(
		vb.on("chat", function(player, text)
			player:give({ item = 2, count = 5 })
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	CHECK(client.inventory().empty()); // nothing given yet

	client.send_chat("give me stone");
	pump(6);

	const auto &inv = client.inventory();
	REQUIRE(inv.size() == 1);
	CHECK(static_cast<int>(inv[0].item) == 2);
	CHECK(inv[0].count == 5);
}

TEST_CASE("vb.world.spawn_item_drop replicates to a client and is picked up on approach") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("item_drop"));
	REQUIRE(rt.load_pack_file(R"(
		vb.on("chat", function(player, text)
			vb.world.spawn_item_drop({ x = 5, y = 5, z = 5 }, 3, 2)
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	// Far from the drop's spawn position: no interest yet.
	server.set_player_state(a_id, Vec3d{ 100, 100, 100 });
	client.send_chat("drop it");
	pump(6);
	CHECK(client.remote_entities().empty());
	CHECK(client.inventory().empty());

	// Move within interest range (default radius) and the drop should now
	// replicate as a remote entity -- but not be picked up yet (still 2m
	// away from its exact position).
	server.set_player_state(a_id, Vec3d{ 5, 5, 7 });
	pump(6);
	CHECK_FALSE(client.remote_entities().empty());
	CHECK(client.inventory().empty());

	// Walk directly onto it: picked up, credited to inventory, and removed
	// from replication.
	server.set_player_state(a_id, Vec3d{ 5, 5, 5 });
	pump(6);
	CHECK(client.remote_entities().empty());
	const auto &inv = client.inventory();
	REQUIRE(inv.size() == 1);
	CHECK(static_cast<int>(inv[0].item) == 3);
	CHECK(inv[0].count == 2);
}

TEST_CASE("vb.register_entity{represents=\"item_drop\"} tags real drops with "
		  "that kind's id instead of the reserved sentinel (entity-management "
		  "follow-up)") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("item_drop_represents"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "test:coin", represents = "item_drop",
			width = 0.3, height = 0.3 })
		vb.on("chat", function(player, text)
			vb.world.spawn_item_drop({ x = 5, y = 5, z = 5 }, 3, 2)
			return true
		end)
	)"));
	rt.freeze();

	// install_entity_kind_registry() must run before ServerSession's
	// constructor copies `host` (same ordering src/server/main.cpp and
	// src/client/main.cpp's --singleplayer path both use) -- otherwise no
	// S2C_EntityKindRegistry frame ever reaches a joining client and
	// entity_kind() below stays empty regardless of what represents= claimed.
	HandshakeServerHost host;
	rt.install_entity_kind_registry(host);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg, host);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());

	// Spawn the drop while far away (matches "vb.world.spawn_item_drop
	// replicates to a client..." above), then move close on a later tick --
	// two separate steps, not one, so the interest diff has a real position
	// change to react to.
	const NetId player_id = client.join_accept()->your_net_id;
	server.set_player_state(player_id, Vec3d{ 100, 100, 100 });
	client.send_chat("drop it");
	pump(6);

	// 2m away: within interest range but outside the default pickup radius,
	// so the drop stays replicated instead of being immediately collected
	// (same distance the "spawn_item_drop replicates..." test above uses).
	server.set_player_state(player_id, Vec3d{ 5, 5, 7 });
	pump(6);

	REQUIRE_FALSE(client.remote_entities().empty());
	const auto kind = client.remote_entities().begin()->second.kind;
	const auto *record = client.entity_kind(kind);
	REQUIRE(record != nullptr);
	CHECK(record->name == "test:coin");
	CHECK(record->width == doctest::Approx(0.3f));
}

TEST_CASE("vb.register_entity{represents=\"player\"} tags a joining player's "
		  "replicated kind, seen by another client (entity-management "
		  "follow-up)") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("player_represents"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "test:hero", represents = "player",
			width = 0.6, height = 1.9 })
	)"));
	rt.freeze();

	// See the item_drop test above for why this must run before
	// ServerSession's constructor copies `host`.
	HandshakeServerHost host;
	rt.install_entity_kind_registry(host);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg, host);
	rt.attach_session(server);
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
	pump(16);
	REQUIRE(a.joined());
	REQUIRE(b.joined());

	// Force both within interest range of each other (same pattern as
	// replication_test.cpp's TwoClientWorld -- default spawn positions aren't
	// guaranteed close enough on their own).
	const NetId a_id = a.join_accept()->your_net_id;
	const NetId b_id = b.join_accept()->your_net_id;
	server.set_player_state(a_id, Vec3d{ 0, 64, 0 });
	server.set_player_state(b_id, Vec3d{ 8, 64, 0 });
	pump(4);

	REQUIRE(a.remote_entities().count(b_id) == 1); // sees B
	const auto kind = a.remote_entities().at(b_id).kind;
	const auto *record = a.entity_kind(kind);
	REQUIRE(record != nullptr);
	CHECK(record->name == "test:hero");
	CHECK(record->width == doctest::Approx(0.6f));
}

TEST_CASE("vb.register_block{pickup_radius=...} widens a dropped item's "
		  "pickup range beyond the engine default") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("item_drop_radius"));
	REQUIRE(rt.load_pack_file(R"(
		magnet_id = vb.register_block({ name = "test:magnet", pickup_radius = 10 })
		vb.on("chat", function(player, text)
			vb.world.spawn_item_drop({ x = 5, y = 5, z = 5 }, magnet_id, 1)
			return true
		end)
	)"));
	rt.freeze();

	// Constructed after freeze() so its registry copy includes "test:magnet"
	// (see STATE.md's construction-order note -- World copies BlockRegistry
	// by value at construction time).
	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry, 1, 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	client.send_chat("drop it");
	pump(6);

	// 8m away: well outside ItemDropSystem's 1.5m engine default, but inside
	// this block's own pickup_radius = 10 override.
	server.set_player_state(a_id, Vec3d{ 13, 5, 5 });
	pump(6);
	const auto &inv = client.inventory();
	REQUIRE(inv.size() == 1);
	CHECK(inv[0].count == 1);
}

TEST_CASE(
		"player:damage() + vb.on('player_death') drives a custom respawn "
		"(heal/pos/message/drop_inventory)") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("death"));
	REQUIRE(rt.load_pack_file(R"(
		seen_cause = nil
		seen_health_before = nil
		vb.on("player_death", function(player, cause, health_before)
			seen_cause = cause
			seen_health_before = health_before
			return { heal = 7, pos = { x = 1, y = 2, z = 3 },
				message = "* custom respawn", drop_inventory = true }
		end)
		vb.on("chat", function(player, text)
			player:give({ item = 2, count = 5 })
			player:damage(100, "test")
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;
	client.take_chat_messages(); // drain join-system lines

	client.send_chat("hit me");
	pump(6);

	// The handler's chosen respawn: custom heal, custom position, custom
	// message, and the pre-existing inventory got dropped instead of kept.
	const auto srv = server.player_move_state(a_id);
	REQUIRE(srv.has_value());
	CHECK(srv->position.x == doctest::Approx(1.0));
	CHECK(srv->position.y == doctest::Approx(2.0));
	CHECK(srv->position.z == doctest::Approx(3.0));
	CHECK(client.inventory().empty()); // given 5 stone, then dropped on death

	bool saw_custom_msg = false;
	for (const auto &m : client.take_chat_messages()) {
		if (m == "* custom respawn") {
			saw_custom_msg = true;
		}
	}
	CHECK(saw_custom_msg);

	REQUIRE(rt.load_pack_file(R"(
		assert(seen_cause == "test")
		assert(seen_health_before == 20.0)
	)"));
}

// REMAINING_TASKS.md's hunger gap: player:get_hunger()/add_hunger() through
// a real session, plus vb.hunger.set_params driving real decay to a real
// starvation death (mirrors the fall-damage/PvP precedent of proving a
// primitive end-to-end, not just its Lua binding shape).
TEST_CASE("player:get_hunger()/add_hunger() read and spend hunger, and "
		  "configured decay leads to a real starvation death") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("hunger_e2e"));
	REQUIRE(rt.load_pack_file(R"(
		vb.hunger.set_params({ decay_per_second = 2000, starvation_damage_per_second = 400 })
		seen_hunger = nil
		death_cause = nil
		vb.on("player_death", function(player, cause, health_before)
			death_cause = cause
			return { heal = 20, pos = { x = 0, y = 64, z = 0 }, message = "" }
		end)
		vb.on("chat", function(player, text)
			if text == "spend" then
				player:add_hunger(-30)
				seen_hunger = player:get_hunger()
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	server.set_hunger_params(rt.effective_hunger_params(ServerSession::HungerParams{}));
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());

	client.send_chat("spend");
	pump(1);
	// Already decayed some (decay_per_second=2000 -> 100/tick) plus the
	// explicit -30 spend from add_hunger() -- get_hunger() must report a
	// real, already-lower value, not a stale/stub one. Clamped at 0 either
	// way, so this also holds if decay alone already zeroed it out.
	REQUIRE(rt.load_pack_file(R"(assert(seen_hunger <= 100 - 30))"));

	// Let decay + starvation damage keep running to a real death.
	pump(3);
	REQUIRE(rt.load_pack_file(R"(assert(death_cause == "hunger"))"));
}

TEST_CASE("player:get_health() reports {current, max} and drops after damage()") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("get_health"));
	REQUIRE(rt.load_pack_file(R"(
		before, after = nil, nil
		vb.on("chat", function(player, text)
			if text == "hurt" then
				before = player:get_health()
				player:damage(6, "test")
				after = player:get_health()
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });
	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());

	client.send_chat("hurt");
	pump(2);
	REQUIRE(rt.load_pack_file(R"(
		assert(before.current == 20 and before.max == 20)
		assert(after.current == 14 and after.max == 20)
	)"));
}

// player:set_pos() teleports through ServerSession::teleport_player (no
// longer automation-only): the server position moves at once, velocity is
// zeroed, and the owning client's prediction reconciles to the new spot.
TEST_CASE("player:set_pos() teleports a player and the client follows") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("set_pos"));
	REQUIRE(rt.load_pack_file(R"(
		after, spawn, bad_ok = nil, nil, nil
		vb.on("chat", function(player, text)
			if text == "tp" then
				player:set_pos(100.5, 80, -20.5)
				after = player:get_pos()
				spawn = player:get_spawn_pos()
				bad_ok = pcall(function() player:set_pos(0 / 0, 1, 2) end)
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	// Fly mode: no gravity, so the teleported position holds still while the
	// client catches up.
	vb::physics::MoveParams fly;
	fly.fly = true;
	server.set_move_params(fly);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });
	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());

	client.send_chat("tp");
	pump(2);
	REQUIRE(rt.load_pack_file(R"(
		assert(after.x == 100.5 and after.y == 80 and after.z == -20.5)
		assert(spawn.x == 0 and spawn.y == 64 and spawn.z == 0)
		assert(bad_ok == false)
	)"));
	const auto st = server.player_move_state(client.join_accept()->your_net_id);
	REQUIRE(st);
	CHECK(st->position.x == doctest::Approx(100.5));
	CHECK(st->position.y == doctest::Approx(80.0));
	CHECK(st->position.z == doctest::Approx(-20.5));

	// A real client is always sending input; idle cmds are enough for the
	// server to include its authoritative state in snapshots to reconcile to.
	vb::protocol::InputCmd idle;
	for (std::uint32_t i = 1; i <= 10; ++i) {
		idle.seq = i;
		client.push_input(idle);
		pump(1);
	}
	const auto feet = client.predicted_feet();
	CHECK(feet.x == doctest::Approx(100.5).epsilon(0.01));
	CHECK(feet.y == doctest::Approx(80.0).epsilon(0.01));
	CHECK(feet.z == doctest::Approx(-20.5).epsilon(0.01));
}

TEST_CASE("vb.register_entity + vb.world.spawn: self persists across on_tick, "
		  "on_hit/on_death fire, and the instance replicates to a client") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_kind"));
	REQUIRE(rt.load_pack_file(R"(
		spawn_count = 0
		hit_amount = nil
		hit_cause = nil
		death_cause = nil
		instance = nil

		vb.register_entity({
			name = "test:slime",
			on_spawn = function(self)
				self.hp = 10
				spawn_count = spawn_count + 1
			end,
			on_tick = function(self, dt)
				self.hp = self.hp + dt
			end,
			on_hit = function(self, amount, cause)
				hit_amount = amount
				hit_cause = cause
			end,
			on_death = function(self, cause)
				death_cause = cause
			end,
		})

		vb.on("chat", function(player, text)
			if text == "spawn" then
				instance = vb.world.spawn("test:slime", { x = 5, y = 5, z = 5 })
			elseif text == "hit" then
				instance:damage(3, "punch")
			elseif text == "kill" then
				instance:remove("script")
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	server.set_player_state(client.join_accept()->your_net_id, Vec3d{ 5, 5, 7 });

	client.send_chat("spawn");
	pump(6);
	REQUIRE(rt.load_pack_file(R"(
		assert(spawn_count == 1)
		assert(instance ~= nil)
		-- on_tick has already fired a few times by now (spawn happened partway
		-- through this pump), so hp is > 10, not exactly 10 -- remember it as
		-- the baseline for the persistence check below.
		assert(instance.hp > 10)
		hp_after_spawn = instance.hp
	)"));

	// Every subsequent pumped tick calls on_tick(self, dt) and self.hp keeps
	// accumulating past its own baseline -- proves `self` is the *same*
	// persistent table across calls, not a fresh one rebuilt each dispatch
	// (unlike PlayerHandle).
	pump(10);
	REQUIRE(rt.load_pack_file(R"( assert(instance.hp > hp_after_spawn) )"));

	// Replicated to the client through the same interest-grid path as a
	// dropped item -- no dedicated wire message.
	CHECK_FALSE(client.remote_entities().empty());

	client.send_chat("hit");
	pump(6);
	REQUIRE(rt.load_pack_file(R"(
		assert(hit_amount == 3)
		assert(hit_cause == "punch")
	)"));

	client.send_chat("kill");
	pump(6);
	REQUIRE(rt.load_pack_file(R"( assert(death_cause == "script") )"));
	CHECK(client.remote_entities().empty());
}

TEST_CASE("vb.register_entity{health=}: damage() auto-despawns at 0 without "
		  "an explicit :remove() call, and leaves kinds that never opted in "
		  "untouched") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_health"));
	REQUIRE(rt.load_pack_file(R"(
		death_cause = nil
		zombie = nil
		slime = nil -- opts out of health tracking entirely

		vb.register_entity({
			name = "test:zombie",
			health = 5,
			on_death = function(self, cause) death_cause = cause end,
		})
		vb.register_entity({ name = "test:slime" })

		vb.on("chat", function(player, text)
			if text == "spawn" then
				zombie = vb.world.spawn("test:zombie", { x = 5, y = 5, z = 5 })
				slime = vb.world.spawn("test:slime", { x = 6, y = 5, z = 5 })
			elseif text == "hit" then
				zombie:damage(3, "punch") -- 5 -> 2, still alive
			elseif text == "kill" then
				zombie:damage(3, "punch") -- 2 -> -1 clamped to 0, auto-despawns
			elseif text == "hit_untracked" then
				slime:damage(1000, "punch") -- no health= set, notification only
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	server.set_player_state(client.join_accept()->your_net_id, Vec3d{ 5, 5, 7 });

	client.send_chat("spawn");
	pump(6);
	REQUIRE(rt.load_pack_file(R"(
		local hp = zombie:get_health()
		assert(hp ~= nil)
		assert(hp.current == 5 and hp.max == 5)
		assert(slime:get_health() == nil) -- never opted into health tracking
	)"));

	client.send_chat("hit");
	pump(6);
	REQUIRE(rt.load_pack_file(R"(
		local hp = zombie:get_health()
		assert(hp.current == 2 and hp.max == 5)
		assert(death_cause == nil) -- not dead yet
	)"));
	CHECK_FALSE(client.remote_entities().empty());

	client.send_chat("kill");
	pump(6);
	REQUIRE(rt.load_pack_file(R"( assert(death_cause == "punch") )"));
	CHECK(client.remote_entities().size() == 1); // zombie gone, slime remains

	// Untracked kind: damage() is still notification-only, never despawns.
	client.send_chat("hit_untracked");
	pump(6);
	REQUIRE(rt.load_pack_file(R"( assert(slime:get_health() == nil) )"));
	CHECK(client.remote_entities().size() == 1); // slime still alive
}

TEST_CASE("vb.world.spawn(kind, pos, {visual_override=}) reaches a joining "
		  "client on the entered EntityRecord, overriding just texture while "
		  "the kind's own facings/clips still apply") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_visual_override"));
	REQUIRE(rt.load_pack_file(R"(
		instance = nil
		vb.register_entity({
			name = "test:hero",
			visual = {
				variant = "tall",
				texture = "textures/entities/hero.png",
				facings = 4,
				clips = { { clip = "idle", frames = 2, fps = 4 } },
			},
		})
		vb.on("chat", function(player, text)
			if text == "spawn" then
				instance = vb.world.spawn("test:hero", { x = 5, y = 5, z = 5 },
						{ visual_override = { texture = "textures/entities/skins/hero_red.png" } })
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());
	server.set_player_state(client.join_accept()->your_net_id, Vec3d{ 5, 5, 7 });

	client.send_chat("spawn");
	pump(6);
	CHECK_FALSE(client.remote_entities().empty());

	// Find the spawned kind's own NetId (the only remote entity here).
	REQUIRE(client.remote_entities().size() == 1);
	const auto net_id = client.remote_entities().begin()->first;

	const auto *ov = client.entity_visual_override(net_id);
	REQUIRE(ov != nullptr);
	REQUIRE(ov->texture.has_value());
	CHECK(*ov->texture == "textures/entities/skins/hero_red.png");
	// Only texture was overridden -- facings/frame size/clips were never set
	// on the override table, so they stay nullopt (render::
	// merge_visual_override inherits the kind's own visual for those).
	CHECK_FALSE(ov->facings.has_value());
	CHECK_FALSE(ov->frame_width.has_value());
	CHECK_FALSE(ov->clips.has_value());
}

TEST_CASE("vb.world.spawn rejects a malformed visual_override without "
		  "spawning anything") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_visual_override_bad"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "test:hero" })
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);

	const auto r = rt.load_pack_file(R"(
		vb.world.spawn("test:hero", { x = 0, y = 0, z = 0 },
				{ visual_override = { facings = 5 } })
	)");
	CHECK_FALSE(r);
}

TEST_CASE("entity text labels: spawn with text, set_text replicates as a "
		  "delta without respawning, a late joiner sees the current text, "
		  "nil removes it") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_text"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({
			name = "test:label",
			visual = false,
			text = { size = 0.4, background = { 40, 40, 40, 200 } },
		})
	)"));
	rt.freeze();

	HandshakeServerHost host; // carries the kind registry (hidden flag)
	rt.install_entity_kind_registry(host);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg, host);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });
	std::unique_ptr<ClientSession> b;

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
			if (b) {
				b->tick(0.05);
			}
		}
	};
	pump(16);
	REQUIRE(a.joined());
	server.set_player_state(a.join_accept()->your_net_id, Vec3d{ 5, 5, 7 });

	REQUIRE(rt.load_pack_file(R"(
		label = vb.world.spawn("test:label", { x = 5, y = 5, z = 5 },
				{ text = { value = "27s", color = { 255, 255, 255 }, offset_y = 0.2 } })
		assert(label:get_text() == "27s")
	)"));
	pump(4);
	REQUIRE(a.remote_entities().size() == 1);
	const NetId id = a.remote_entities().begin()->first;
	const vb::protocol::EntityKindRegistryRecord *kind =
			a.entity_kind(a.remote_entities().begin()->second.kind);
	REQUIRE(kind != nullptr);
	CHECK(kind->hidden);
	{
		const auto *t = a.entity_text(id);
		REQUIRE(t != nullptr);
		CHECK(t->value == "27s");
		CHECK(t->size == doctest::Approx(0.4)); // kind default
		CHECK(t->offset_y == doctest::Approx(0.2)); // per-spawn field
		REQUIRE(t->background.has_value());
		CHECK((*t->background)[3] == 200);
	}

	// A string keeps the style, a table re-applies the kind style with its
	// own fields on top; neither respawns the entity.
	REQUIRE(rt.load_pack_file(R"( label:set_text("26s") )"));
	pump(2);
	REQUIRE(a.remote_entities().size() == 1);
	CHECK(a.remote_entities().begin()->first == id);
	REQUIRE(a.entity_text(id) != nullptr);
	CHECK(a.entity_text(id)->value == "26s");
	CHECK(a.entity_text(id)->offset_y == doctest::Approx(0.2));

	REQUIRE(rt.load_pack_file(R"( label:set_text({ value = "Ripe!", color = { 120, 235, 110 } }) )"));
	pump(2);
	REQUIRE(a.entity_text(id) != nullptr);
	CHECK(a.entity_text(id)->value == "Ripe!");
	CHECK(a.entity_text(id)->color[1] == 235);
	CHECK(a.entity_text(id)->color[3] == 255);
	CHECK(a.entity_text(id)->offset_y == doctest::Approx(0.0)); // hidden kind's default

	// A client that joins later sees the current text, not the first one.
	Transport &tb = net.create_client();
	auto idb = tb.connect("x", 0);
	REQUIRE(idb);
	b = std::make_unique<ClientSession>(tb, *idb, HandshakeClientConfig{ "B", "", "v", 1 });
	pump(16);
	REQUIRE(b->joined());
	server.set_player_state(b->join_accept()->your_net_id, Vec3d{ 5, 5, 3 });
	pump(4);
	REQUIRE(b->entity_text(id) != nullptr);
	CHECK(b->entity_text(id)->value == "Ripe!");

	REQUIRE(rt.load_pack_file(R"(
		label:set_text(nil)
		assert(label:get_text() == nil)
	)"));
	pump(2);
	CHECK(a.entity_text(id) == nullptr);
	CHECK(b->entity_text(id) == nullptr);
	CHECK(a.remote_entities().size() == 2); // entity itself still there (+ B)

	// Moving out of interest drops the label client-side; coming back
	// re-sends it.
	REQUIRE(rt.load_pack_file(R"( label:set_text("back") )"));
	pump(2);
	server.set_player_state(a.join_accept()->your_net_id, Vec3d{ 5000, 5, 7 });
	pump(3);
	CHECK(a.entity_text(id) == nullptr);
	server.set_player_state(a.join_accept()->your_net_id, Vec3d{ 5, 5, 7 });
	pump(3);
	REQUIRE(a.entity_text(id) != nullptr);
	CHECK(a.entity_text(id)->value == "back");
}

TEST_CASE("entity text labels: over-long text and bad colours are Lua errors") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_text_bad"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "test:label" })
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);

	const auto expect_error = [&](const char *code, const char *needle) {
		const auto r = rt.load_pack_file(code);
		CHECK_FALSE(r);
		CHECK_MESSAGE(r.message.find(needle) != std::string::npos, r.message);
	};
	expect_error(R"( vb.world.spawn("test:label", { x = 0, y = 0, z = 0 },
			{ text = string.rep("x", 65) }) )",
			"the limit is 64");
	expect_error(R"( vb.world.spawn("test:label", { x = 0, y = 0, z = 0 },
			{ text = { value = "hi", color = { 300, 0, 0 } } }) )",
			"text.color components must be integers in [0, 255]");
	expect_error(R"( vb.world.spawn("test:label", { x = 0, y = 0, z = 0 },
			{ text = { value = "hi", background = { 1, 2 } } }) )",
			"text.background must have 3 or 4 components");
	expect_error(R"( vb.world.spawn("test:label", { x = 0, y = 0, z = 0 },
			{ text = "\xff\xfe" }) )",
			"not valid UTF-8");
	REQUIRE(rt.load_pack_file(R"(
		e = vb.world.spawn("test:label", { x = 0, y = 0, z = 0 }, { text = "ok" })
	)"));
	expect_error(R"( e:set_text(string.rep("y", 100)) )", "entity:set_text(): text is 100 bytes");
	expect_error(R"( e:set_text({ value = "a", size = -1 }) )", "text.size must be in");
	expect_error(R"( e:set_text(42) )", "expected a string, a table or nil");
	REQUIRE(rt.load_pack_file(R"( assert(e:get_text() == "ok") )"));
}

TEST_CASE("script entities replicate as grounded; set_clip is sticky, "
		  "replicated and seen by a late joiner") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_clip"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "t:probe" })
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession a(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });
	std::unique_ptr<ClientSession> b;
	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			a.tick(0.05);
			if (b) {
				b->tick(0.05);
			}
		}
	};
	pump(16);
	REQUIRE(a.joined());
	server.set_player_state(a.join_accept()->your_net_id, Vec3d{ 0.5, 65, 2 });

	REQUIRE(rt.load_pack_file(R"(
		probe = vb.world.spawn("t:probe", { x = 0.5, y = 65, z = 0.5 })
	)"));
	pump(3);
	REQUIRE(a.remote_entities().size() == 1);
	const NetId id = a.remote_entities().begin()->first;
	// No physics: on_ground (bit 0) is set, so a stationary probe plays idle.
	CHECK((a.remote_entities().begin()->second.flags & 1u) != 0);
	CHECK(a.entity_clip(id) == nullptr);

	REQUIRE(rt.load_pack_file(R"( probe:set_clip("jump") )"));
	pump(2);
	REQUIRE(a.entity_clip(id) != nullptr);
	CHECK(*a.entity_clip(id) == "jump");

	Transport &tb = net.create_client();
	auto idb = tb.connect("x", 0);
	REQUIRE(idb);
	b = std::make_unique<ClientSession>(tb, *idb, HandshakeClientConfig{ "B", "", "v", 1 });
	pump(16);
	REQUIRE(b->joined());
	server.set_player_state(b->join_accept()->your_net_id, Vec3d{ 2, 65, 0.5 });
	pump(3);
	REQUIRE(b->entity_clip(id) != nullptr);
	CHECK(*b->entity_clip(id) == "jump");

	REQUIRE(rt.load_pack_file(R"( probe:set_clip(nil) )"));
	pump(2);
	CHECK(a.entity_clip(id) == nullptr);
	CHECK(b->entity_clip(id) == nullptr);

	const auto bad = rt.load_pack_file(R"( probe:set_clip("") )");
	CHECK_FALSE(bad);
	CHECK(bad.message.find("clip name must be") != std::string::npos);
}

TEST_CASE("attach_to: the child follows its parent on the server and the "
		  "client learns the link; it is despawned when the parent goes") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_attach"));
	REQUIRE(rt.load_pack_file(R"(
		deaths = {}
		vb.register_entity({
			name = "t:hat",
			on_death = function(self, cause) deaths[#deaths + 1] = cause end,
		})
		vb.register_entity({ name = "t:npc" })
		vb.on("chat", function(player, text)
			if text == "hat" then
				hat = vb.world.spawn("t:hat", player:get_pos())
				hat:attach_to(player, { offset = { x = 0, y = 1.9, z = 0 }, layer = 1 })
			end
			return true
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
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
	pump(16);
	REQUIRE(a.joined());
	const NetId me = a.join_accept()->your_net_id;
	server.set_player_state(me, Vec3d{ 10, 65, 10 });

	a.send_chat("hat");
	pump(3);
	REQUIRE(a.remote_entities().size() == 1);
	const NetId hat = a.remote_entities().begin()->first;
	const auto *link = a.entity_attachment(hat);
	REQUIRE(link != nullptr);
	CHECK(link->parent == me);
	CHECK(link->offset.y == doctest::Approx(1.9f));
	REQUIRE(link->layer.has_value());
	CHECK(*link->layer == 1);

	// The player walks; the hat goes with it, server-side too.
	server.set_player_state(me, Vec3d{ 14, 66, 10 });
	pump(2);
	REQUIRE(rt.load_pack_file(R"(
		local p = hat:get_pos()
		assert(math.abs(p.x - 14) < 1e-6 and math.abs(p.y - 67.9) < 1e-6, p.x .. "," .. p.y)
	)"));
	CHECK(a.remote_entities().at(hat).pos.x == doctest::Approx(14.0));

	// Chain: an npc carries a second hat; removing the npc removes the hat.
	REQUIRE(rt.load_pack_file(R"(
		npc = vb.world.spawn("t:npc", { x = 12, y = 65, z = 10 })
		hat2 = vb.world.spawn("t:hat", { x = 12, y = 65, z = 10 })
		hat2:attach_to(npc, { offset = { y = 2 } })
		local ok, err = pcall(function() npc:attach_to(hat2) end)
		assert(not ok and tostring(err):find("cycle"), tostring(err))
		ok, err = pcall(function() npc:attach_to(npc) end)
		assert(not ok and tostring(err):find("itself"), tostring(err))
		ok, err = pcall(function() hat2:attach_to(npc, { layer = 9 }) end)
		assert(not ok and tostring(err):find("%[%-8, 8%]"), tostring(err))
	)"));
	pump(2);
	CHECK(a.remote_entities().size() == 3);
	REQUIRE(rt.load_pack_file(R"( npc:remove("test") )"));
	pump(3);
	CHECK(a.remote_entities().size() == 1);
	REQUIRE(rt.load_pack_file(R"(
		assert(#deaths == 1 and deaths[1] == "parent_removed", tostring(deaths[1]))
	)"));

	// detach: the hat stays where it is and stops following.
	REQUIRE(rt.load_pack_file(R"( hat:detach() )"));
	pump(2);
	CHECK(a.entity_attachment(hat) == nullptr);
	server.set_player_state(me, Vec3d{ 20, 66, 10 });
	pump(2);
	REQUIRE(rt.load_pack_file(R"(
		assert(math.abs(hat:get_pos().x - 14) < 1e-6)
	)"));
}

TEST_CASE("visual layer / through_walls are validated") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_layer_bad"));
	const auto r = rt.load_pack_file(R"(
		vb.register_entity({ name = "t:aura", visual = {
			variant = "small", texture = "t.png", layer = 1.5,
			clips = { { clip = "idle", frames = 1, fps = 1 } } } })
	)");
	CHECK_FALSE(r);
	CHECK(r.message.find("visual.layer must be an integer in [-8, 8]") != std::string::npos);
	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "t:ok", visual = {
			variant = "small", texture = "t.png", layer = -1, through_walls = true,
			clips = { { clip = "idle", frames = 1, fps = 1 } } } })
	)"));
}

TEST_CASE("player:set_visual_override replicates a paper-doll outfit to "
		  "other players, to the player themselves and to late joiners, and "
		  "can change or clear it at runtime") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("player_outfit"));
	REQUIRE(rt.load_pack_file(R"(
		vb.render.set_third_person(false)
		vb.on("chat", function(player, text)
			if text == "red" then
				player:set_visual_override({ layers = {
					{ texture = "textures/cape.png", below = true, rows = { 0 } },
					{ texture = "textures/shirt_red.png" },
					{ texture = "textures/hat_straw.png", tint = { 200, 60, 60 } },
				} })
			elseif text == "blue" then
				player:set_visual_override({ layers = { { texture = "textures/shirt_blue.png" } } })
			elseif text == "none" then
				player:set_visual_override(nil)
			end
			return true
		end)
	)"));
	rt.freeze();
	CHECK_FALSE(rt.third_person_allowed());
	CHECK_FALSE(rt.load_pack_file(R"( vb.render.set_third_person(true) )"));

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	cfg.third_person_allowed = rt.third_person_allowed();
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	std::vector<std::unique_ptr<ClientSession>> clients;
	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			for (auto &c : clients) {
				c->tick(0.05);
			}
		}
	};
	auto join = [&](const char *name, Vec3d pos) -> ClientSession & {
		Transport &t = net.create_client();
		auto id = t.connect("x", 0);
		REQUIRE(id);
		clients.push_back(std::make_unique<ClientSession>(t, *id, HandshakeClientConfig{ name, "", "v", 1 }));
		pump(16);
		REQUIRE(clients.back()->joined());
		server.set_player_state(clients.back()->join_accept()->your_net_id, pos);
		pump(2);
		return *clients.back();
	};
	ClientSession &a = join("A", Vec3d{ 0, 65, 0 });
	ClientSession &b = join("B", Vec3d{ 3, 65, 0 });
	REQUIRE(a.server_info());
	CHECK_FALSE(a.server_info()->third_person_allowed);
	const NetId a_id = a.join_accept()->your_net_id;
	REQUIRE(b.remote_entities().count(a_id) == 1);

	a.send_chat("red");
	pump(3);
	for (const auto *ov : { b.entity_visual_override(a_id), a.my_visual_override() }) {
		REQUIRE(ov != nullptr);
		REQUIRE(ov->layers.has_value());
		REQUIRE(ov->layers->size() == 3);
		CHECK((*ov->layers)[0].below);
		CHECK((*ov->layers)[0].rows == 0b0000'0001);
		CHECK((*ov->layers)[1].texture == "textures/shirt_red.png");
		CHECK((*ov->layers)[2].tint[1] == 60);
	}

	a.send_chat("blue");
	pump(3);
	REQUIRE(b.entity_visual_override(a_id) != nullptr);
	CHECK((*b.entity_visual_override(a_id)->layers)[0].texture == "textures/shirt_blue.png");
	REQUIRE(a.my_visual_override() != nullptr);
	CHECK(a.my_visual_override()->layers->size() == 1);

	ClientSession &c = join("C", Vec3d{ 0, 65, 3 });
	pump(2);
	REQUIRE(c.entity_visual_override(a_id) != nullptr);
	CHECK((*c.entity_visual_override(a_id)->layers)[0].texture == "textures/shirt_blue.png");
	CHECK(c.entity_visual_override(b.join_accept()->your_net_id) == nullptr); // B never dressed

	a.send_chat("none");
	pump(3);
	CHECK(b.entity_visual_override(a_id) == nullptr);
	CHECK(c.entity_visual_override(a_id) == nullptr);
	CHECK(a.my_visual_override() == nullptr);
}

TEST_CASE("entity:set_visual_override changes a script entity's look at "
		  "runtime; malformed layers are Lua errors") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_outfit"));
	REQUIRE(rt.load_pack_file(R"( vb.register_entity({ name = "t:npc" }) )"));
	rt.freeze();
	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
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
	pump(16);
	REQUIRE(a.joined());
	server.set_player_state(a.join_accept()->your_net_id, Vec3d{ 0, 65, 2 });

	REQUIRE(rt.load_pack_file(R"(
		npc = vb.world.spawn("t:npc", { x = 0, y = 65, z = 0 },
				{ visual_override = { texture = "textures/npc.png" } })
	)"));
	pump(3);
	REQUIRE(a.remote_entities().size() == 1);
	const NetId id = a.remote_entities().begin()->first;
	REQUIRE(a.entity_visual_override(id) != nullptr);
	CHECK(*a.entity_visual_override(id)->texture == "textures/npc.png");

	REQUIRE(rt.load_pack_file(R"(
		npc:set_visual_override({ texture = "textures/npc.png", layers = { { texture = "textures/apron.png" } } })
	)"));
	pump(2);
	REQUIRE(a.entity_visual_override(id)->layers.has_value());
	CHECK((*a.entity_visual_override(id)->layers)[0].texture == "textures/apron.png");
	CHECK(a.remote_entities().begin()->first == id); // same entity, no respawn

	REQUIRE(rt.load_pack_file(R"( npc:set_visual_override(nil) )"));
	pump(2);
	CHECK(a.entity_visual_override(id) == nullptr);

	const auto expect_error = [&](const char *code, const char *needle) {
		const auto r = rt.load_pack_file(code);
		CHECK_FALSE(r);
		CHECK_MESSAGE(r.message.find(needle) != std::string::npos, r.message);
	};
	expect_error(R"( npc:set_visual_override({ layers = { { below = true } } }) )",
			"visual_override: layers[1].texture is required");
	expect_error(R"( npc:set_visual_override({ layers = { { texture = "x.png", rows = { 9 } } } }) )",
			"rows entries must be integers in [0, 7]");
	expect_error(R"( npc:set_visual_override({ layers = { { texture = "x.png", tint = { 1, 2 } } } }) )",
			"layers[1].tint must have 3 or 4 components");
	expect_error(R"( npc:set_visual_override(5) )", "expected a table or nil");
}

TEST_CASE("region_enter/region_exit (Phase 7.3): fires once per crossing, "
		  "not per tick spent inside, and passes the block's registered name") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("region_hook"));
	// Only base:water opts into the region flag by default -- gate on the
	// block name so a wrong/garbled name silently leaves the inventory
	// empty instead of passing for the wrong reason.
	REQUIRE(rt.load_pack_file(R"(
		vb.on("region_enter", function(player, pos, block)
			if block == "base:water" then player:give({ item = 1, count = 1 }) end
		end)
		vb.on("region_exit", function(player, pos, block)
			if block == "base:water" then player:give({ item = 6, count = 1 }) end
		end)
	)"));
	rt.freeze();

	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry, 1, 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	const IVec3 water_voxel = surface_voxel(world, 4, 4) + IVec3{ 0, 5, 0 };
	world.set_block(water_voxel, vb::world::base_block::water);
	const Vec3d in_water{ water_voxel.x + 0.5, water_voxel.y + 0.5,
		water_voxel.z + 0.5 };
	const Vec3d dry{ water_voxel.x + 0.5, water_voxel.y + 5.0, water_voxel.z + 0.5 };

	server.set_player_state(a_id, dry);
	pump(3);
	CHECK(client.inventory().empty()); // never entered a region block

	server.set_player_state(a_id, in_water);
	pump(5); // several ticks *inside* the same region block
	{
		const auto &inv = client.inventory();
		REQUIRE(inv.size() == 1);
		CHECK(inv[0].item == vb::world::base_block::stone);
		CHECK(inv[0].count == 1); // one enter, not one per tick
	}

	server.set_player_state(a_id, dry);
	pump(3);
	{
		const auto &inv = client.inventory();
		REQUIRE(inv.size() == 2);
		CHECK(inv[1].item == vb::world::base_block::wood);
		CHECK(inv[1].count == 1); // one exit
	}

	// A second crossing fires again -- proves this is edge-triggered per
	// crossing, not a one-shot latch.
	server.set_player_state(a_id, in_water);
	pump(3);
	server.set_player_state(a_id, dry);
	pump(3);
	{
		const auto &inv = client.inventory();
		REQUIRE(inv.size() == 2);
		CHECK(inv[0].count == 2);
		CHECK(inv[1].count == 2);
	}
}

TEST_CASE("player_landed (Phase 6.22): fires exactly once on impact, with a "
		  "real gravity-driven fall, not while airborne or once already "
		  "resting on the ground") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(net.server(), registry, temp_storage("landed_hook"));
	REQUIRE(rt.load_pack_file(R"(
		vb.on("player_landed", function(player, impact_speed)
			player:give({ item = 1, count = math.floor(impact_speed) })
		end)
	)"));
	rt.freeze();

	vb::world::World world(registry);
	wg::WorldGenWorkerPool pool(
			wg::WorldGenerator(wg::WorldGenParams{}, registry),
			wg::WorldGenWorkerPool::kSynchronous);

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	auto replicator = std::make_unique<WorldReplicator>(world, pool, registry, 1, 2);
	rt.attach_world(*replicator);
	server.set_world_replicator(std::move(replicator));
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};

	pump(20);
	REQUIRE(client.joined());
	const NetId a_id = client.join_accept()->your_net_id;

	const IVec3 ground = surface_voxel(world, 4, 4);
	constexpr double kDropHeight = 5.0; // blocks -- well above the 8 m/s "safe" threshold
	server.set_player_state(a_id,
			Vec3d{ ground.x + 0.5, ground.y + 1.0 + kDropHeight, ground.z + 0.5 });

	vb::protocol::InputCmd falling; // move/jump all default-false: pure free-fall
	falling.dt = 0.05f; // must match pump()'s own per-tick dt below
	std::uint32_t seq = 1;
	// Enough real 0.05s ticks to fall kDropHeight blocks under this engine's
	// gravity (28 m/s^2) and land -- generous margin over the ~0.6s the
	// kinematics predict.
	for (int i = 0; i < 40 && client.inventory().empty(); ++i) {
		falling.seq = seq++;
		client.push_input(falling);
		pump(1);
	}

	const auto &inv = client.inventory();
	REQUIRE(inv.size() == 1); // fired exactly once, not once per airborne tick
	CHECK(inv[0].item == vb::world::base_block::stone);
	// sqrt(2 * 28 * 5) ~= 16.7 m/s -- loose bounds around the discretely
	// integrated value, well clear of the 0 that "fired while still airborne
	// or never fired at all" would produce.
	CHECK(inv[0].count >= 10);
	CHECK(inv[0].count <= 22);

	// Resting on the ground for many more ticks must not fire it again.
	for (int i = 0; i < 10; ++i) {
		falling.seq = seq++;
		client.push_input(falling);
		pump(1);
	}
	REQUIRE(client.inventory().size() == 1);
}

// Regression test for the "PlayerHandle stashed across ticks" engine bug
// (REMAINING_TASKS.md's Cross-Cutting item, found 2026-09-28, root-caused and
// fixed 2026-09-30 -- see remaining_tasks/cross_cutting.md's dated entry for
// the full story). A PlayerHandle received by a `chat` handler is stashed
// into a Lua global, then read back from *both* a vb.on("tick", ...) handler
// and a vb.every(...) timer -- the two call shapes that used to crash the
// whole vb_tests binary (a dangling reference to a destroyed C++ stack local,
// not a race) before the SOL_FUNCTION_CALL_VALUE_SEMANTICS=1 fix in
// cmake/Dependencies.cmake.
TEST_CASE("a PlayerHandle stashed from a chat handler survives being read "
		  "back from a later tick/timer handler") {
	LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	vb::script::PackRuntime rt(
			net.server(), registry, temp_storage("stashed_player_handle"));
	REQUIRE(rt.load_pack_file(R"(
		tick_name = "unset"
		timer_name = "unset"
		vb.on("chat", function(player, text)
			if text == "stash" then stashed = player end
		end)
		vb.on("tick", function(dt)
			if stashed ~= nil then
				tick_name = stashed:get_name()
			end
		end)
		vb.every(0.05, function()
			if stashed ~= nil then
				timer_name = stashed:get_name()
			end
		end)
	)"));
	rt.freeze();

	HandshakeServerConfig cfg;
	cfg.world_seed = 7;
	ServerSession server(net.server(), cfg);
	rt.attach_session(server);
	REQUIRE(net.server().listen(0));

	Transport &ta = net.create_client();
	auto ida = ta.connect("x", 0);
	REQUIRE(ida);
	ClientSession client(ta, *ida, HandshakeClientConfig{ "A", "", "v", 1 });

	auto pump = [&](int n) {
		for (int i = 0; i < n; ++i) {
			server.tick(0.05);
			client.tick(0.05);
		}
	};
	pump(16);
	REQUIRE(client.joined());

	client.send_chat("stash");
	pump(10); // several more ticks reading the stashed handle back

	REQUIRE(rt.load_pack_file(
			R"(assert(tick_name == "A", "tick_name was: " .. tostring(tick_name)))"));
	REQUIRE(rt.load_pack_file(
			R"(assert(timer_name == "A", "timer_name was: " .. tostring(timer_name)))"));
}

#endif // VB_WITH_LUA
