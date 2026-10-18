#include <doctest/doctest.h>

#include <ostream>

#include <filesystem>
#include <string>

#include "vb/net/loopback.hpp"
#include "vb/net/world_replicator.hpp"
#include "vb/script/pack_runtime.hpp"
#include "vb/world/block.hpp"
#include "vb/world/world.hpp"
#include "vb/worldgen/generator.hpp"
#include "vb/worldgen/worker_pool.hpp"

#if !VB_WITH_LUA

TEST_CASE("PackRuntime reports kDisabled when built without VB_WITH_LUA") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, "vb_disabled_storage.json");
	const auto r = rt.load_pack_file("return 1");
	CHECK_FALSE(r);
	CHECK(r.error == vb::core::ScriptError::kDisabled);
}

#else

namespace {

std::filesystem::path temp_storage(const char *name) {
	auto p = std::filesystem::temp_directory_path() /
			(std::string("vb_pack_runtime_test_") + name + ".json");
	std::filesystem::remove(p);
	return p;
}

// World + WorldReplicator fixture for the vb.world.* tests (attach_world()
// needs a real WorldReplicator).
struct Fixture {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt;
	vb::world::World world;
	vb::worldgen::WorldGenerator gen;
	vb::worldgen::WorldGenWorkerPool pool;
	vb::net::WorldReplicator replicator;

	explicit Fixture(const std::filesystem::path &storage_path) : rt(net.server(), registry, storage_path),
																  world(registry),
																  gen(vb::worldgen::WorldGenParams{}, registry),
																  pool(gen, vb::worldgen::WorldGenWorkerPool::kSynchronous),
																  replicator(world, pool, registry, /*view*/ 1, /*vview*/ 1) {
		rt.attach_world(replicator);
	}
};

} // namespace

TEST_CASE("vb.register_block is idempotent and rejected after freeze") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("register"));

	const auto r1 = rt.load_pack_file(R"(
		id1 = vb.register_block({ name = "test:glow", light = 15 })
		id2 = vb.register_block({ name = "test:glow" })
		assert(id1 == id2)
	)");
	REQUIRE(r1);
	CHECK(registry.find("test:glow") != vb::core::BlockId::kAir);
	CHECK(registry.light_emission(registry.find("test:glow")) == 15); // id2's call didn't reset it

	rt.freeze();

	const auto r2 = rt.load_pack_file(R"(vb.register_block({ name = "test:late" }))");
	CHECK_FALSE(r2);
}

TEST_CASE("vb.register_block{texture=...} re-declaring an existing block attaches the texture "
		  "without resetting fields it doesn't mention (real texture/atlas system)") {
	// Mirrors content/base/blocks/stone.lua's/water.lua's own pattern: a pack
	// file re-declares a block BlockRegistry::base() already hardcoded,
	// purely to attach `texture` -- BlockRegistry::add_or_get is a no-op on
	// an already-registered name (by design -- re-running init.lua must not
	// reset an earlier call's fields), so PackRuntime falls back to the
	// narrower BlockRegistry::set_texture() for this one field.
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("register_texture_existing"));

	REQUIRE(rt.load_pack_file(R"(
		vb.register_block({ name = "base:stone", solid = true, opaque = true,
			liquid = false, light = 0, texture = "textures/stone.png" })
	)"));

	const auto stone = registry.find("base:stone");
	CHECK(stone == vb::world::base_block::stone); // same id base() already assigned
	CHECK(registry.get(stone).texture == "textures/stone.png");
	CHECK(registry.is_solid(stone));
	CHECK(registry.is_opaque(stone));
	CHECK_FALSE(registry.is_liquid(stone));
}

TEST_CASE("vb.register_block{crack_texture=...} re-declaring an existing block attaches it "
		  "without resetting fields it doesn't mention (mirrors texture= exactly, REMAINING_TASKS 6.5)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("register_crack_texture_existing"));

	REQUIRE(rt.load_pack_file(R"(
		vb.register_block({ name = "base:stone", solid = true, opaque = true,
			liquid = false, light = 0, crack_texture = "textures/stone_crack.png" })
	)"));

	const auto stone = registry.find("base:stone");
	CHECK(stone == vb::world::base_block::stone);
	CHECK(registry.get(stone).crack_texture == "textures/stone_crack.png");
	CHECK(registry.is_solid(stone));
}

TEST_CASE("vb.register_block{crack_texture=...} is stored and defaults to empty") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("register_crack_texture"));

	REQUIRE(rt.load_pack_file(R"(
		vb.register_block({ name = "test:cracked", crack_texture = "textures/cracked_crack.png" })
		vb.register_block({ name = "test:plain" })
	)"));

	CHECK(registry.get(registry.find("test:cracked")).crack_texture == "textures/cracked_crack.png");
	CHECK(registry.get(registry.find("test:plain")).crack_texture.empty());
}

TEST_CASE("vb.register_block{texture=...} is stored and defaults to empty (real texture/atlas system)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("register_texture"));

	REQUIRE(rt.load_pack_file(R"(
		vb.register_block({ name = "test:textured", texture = "textures/textured.png" })
		vb.register_block({ name = "test:untextured" })
	)"));

	const auto textured = registry.find("test:textured");
	const auto untextured = registry.find("test:untextured");
	CHECK(registry.get(textured).texture == "textures/textured.png");
	CHECK(registry.get(untextured).texture.empty());
}

TEST_CASE("vb.register_block{region=...}: liquid defaults to opting in, "
		  "non-liquid defaults out, both are explicitly overridable (Phase 7.3)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("register_region"));

	REQUIRE(rt.load_pack_file(R"(
		vb.register_block({ name = "test:lava", liquid = true })
		vb.register_block({ name = "test:dry_liquid", liquid = true, region = false })
		vb.register_block({ name = "test:poison_cloud", region = true })
		vb.register_block({ name = "test:plain" })
	)"));

	CHECK(registry.is_region(registry.find("test:lava")));
	CHECK_FALSE(registry.is_region(registry.find("test:dry_liquid")));
	CHECK(registry.is_region(registry.find("test:poison_cloud")));
	CHECK_FALSE(registry.is_region(registry.find("test:plain")));
}

TEST_CASE("vb.register_entity captures its callback table without dispatching") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity"));

	const auto r = rt.load_pack_file(R"(
		id = vb.register_entity({ name = "test:slime",
			on_spawn = function() end, on_tick = function() end,
			on_hit = function() end, on_death = function() end })
		assert(id == 1)
		id2 = vb.register_entity({ name = "test:slime" })
		assert(id == id2)
	)");
	REQUIRE(r);
}

TEST_CASE("vb.register_entity{health=} rejects a non-positive value") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_health_validation"));

	const auto r = rt.load_pack_file(R"(
		vb.register_entity({ name = "test:zombie", health = 0 })
	)");
	CHECK_FALSE(r);

	const auto r2 = rt.load_pack_file(R"(
		vb.register_entity({ name = "test:zombie2", health = -5 })
	)");
	CHECK_FALSE(r2);
}

TEST_CASE("vb.register_entity{width=, height=} reaches joining clients as "
		  "S2C_EntityKindRegistry, defaulting to the placeholder's own "
		  "0.8/1.8 when omitted (entity-management follow-up to Phase 6.1)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_kind_registry"));

	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "test:slime", width = 0.6, height = 0.6 })
		vb.register_entity({ name = "test:golem" })
	)"));
	rt.freeze();

	vb::net::HandshakeServerHost host;
	rt.install_entity_kind_registry(host);
	REQUIRE(host.entity_kind_registry);
	const auto kinds = host.entity_kind_registry();
	REQUIRE(kinds.has_value());
	REQUIRE(kinds->size() == 2);
	CHECK((*kinds)[0].name == "test:slime");
	CHECK((*kinds)[0].width == doctest::Approx(0.6f));
	CHECK((*kinds)[0].height == doctest::Approx(0.6f));
	CHECK((*kinds)[1].name == "test:golem");
	CHECK((*kinds)[1].width == doctest::Approx(0.8f));
	CHECK((*kinds)[1].height == doctest::Approx(1.8f));
}

TEST_CASE("vb.register_entity{visual=} reaches joining clients as "
		  "S2C_EntityKindRegistry.visual, absent for a kind that never sets "
		  "one (entity-management follow-up to the width/height item)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_visual"));

	REQUIRE(rt.load_pack_file(R"(
		vb.register_entity({ name = "test:slime", visual = {
			variant = "tall",
			texture = "textures/entities/slime.png",
			facings = 4,
			origin = { x = 0.5, y = 1.0 },
			clips = {
				{ clip = "idle", frames = 4, fps = 6 },
				{ clip = "walk", frames = 6, fps = 10 },
			},
		} })
		vb.register_entity({ name = "test:golem" })
	)"));
	rt.freeze();

	vb::net::HandshakeServerHost host;
	rt.install_entity_kind_registry(host);
	const auto kinds = host.entity_kind_registry();
	REQUIRE(kinds.has_value());
	REQUIRE(kinds->size() == 2);

	REQUIRE((*kinds)[0].visual.has_value());
	const auto &visual = *(*kinds)[0].visual;
	CHECK(visual.texture == "textures/entities/slime.png");
	CHECK(visual.frame_width == 128); // "tall" variant
	CHECK(visual.frame_height == 256);
	CHECK(visual.facings == 4);
	CHECK(visual.origin_x == doctest::Approx(0.5f));
	CHECK(visual.origin_y == doctest::Approx(1.0f));
	REQUIRE(visual.clips.size() == 2);
	CHECK(visual.clips[0].clip == "idle");
	CHECK(visual.clips[0].frames == 4);
	CHECK(visual.clips[1].clip == "walk");
	CHECK(visual.clips[1].fps == doctest::Approx(10.0f));

	CHECK_FALSE((*kinds)[1].visual.has_value());
}

TEST_CASE("vb.register_entity{visual=} rejects a bad shape") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	auto rejects = [&](const char *lua) {
		vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_visual_rejects"));
		CHECK_FALSE(rt.load_pack_file(lua));
	};

	rejects(R"(vb.register_entity({ name = "test:a", visual = {
		variant = "huge", texture = "t.png", clips = { { clip = "idle", frames = 1, fps = 1 } } } }))");
	rejects(R"(vb.register_entity({ name = "test:b", visual = {
		variant = "small", texture = "", clips = { { clip = "idle", frames = 1, fps = 1 } } } }))");
	rejects(R"(vb.register_entity({ name = "test:c", visual = {
		variant = "small", texture = "t.png", facings = 6,
		clips = { { clip = "idle", frames = 1, fps = 1 } } } }))");
	rejects(R"(vb.register_entity({ name = "test:d", visual = {
		variant = "small", texture = "t.png", origin = { x = 1.5, y = 1.0 },
		clips = { { clip = "idle", frames = 1, fps = 1 } } } }))");
	rejects(R"(vb.register_entity({ name = "test:e", visual = {
		variant = "small", texture = "t.png", clips = {} } }))");
	rejects(R"(vb.register_entity({ name = "test:f", visual = {
		variant = "small", texture = "t.png",
		clips = { { clip = "", frames = 1, fps = 1 } } } }))");
	rejects(R"(vb.register_entity({ name = "test:g", visual = {
		variant = "small", texture = "t.png",
		clips = { { clip = "idle", frames = 0, fps = 1 } } } }))");
	rejects(R"(vb.register_entity({ name = "test:h", visual = {
		variant = "small", texture = "t.png",
		clips = { { clip = "idle", frames = 1, fps = 0 } } } }))");
}

TEST_CASE("vb.register_entity{represents=} accepts 'player'/'item_drop', "
		  "rejects an unknown value, and rejects a role claimed twice") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();

	{
		vb::script::PackRuntime rt(net.server(), registry, temp_storage("represents_ok"));
		REQUIRE(rt.load_pack_file(R"(
			vb.register_entity({ name = "test:hero", represents = "player" })
			vb.register_entity({ name = "test:coin", represents = "item_drop" })
		)"));
	}
	{
		vb::script::PackRuntime rt(net.server(), registry, temp_storage("represents_unknown"));
		CHECK_FALSE(rt.load_pack_file(
				R"(vb.register_entity({ name = "test:a", represents = "monster" }))"));
	}
	{
		vb::script::PackRuntime rt(net.server(), registry, temp_storage("represents_dup_player"));
		CHECK_FALSE(rt.load_pack_file(R"(
			vb.register_entity({ name = "test:a", represents = "player" })
			vb.register_entity({ name = "test:b", represents = "player" })
		)"));
	}
	{
		vb::script::PackRuntime rt(net.server(), registry, temp_storage("represents_dup_item_drop"));
		CHECK_FALSE(rt.load_pack_file(R"(
			vb.register_entity({ name = "test:a", represents = "item_drop" })
			vb.register_entity({ name = "test:b", represents = "item_drop" })
		)"));
	}
}

TEST_CASE("install_entity_kind_registry leaves the hook at nullopt when no "
		  "pack ever called vb.register_entity") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("entity_kind_registry_empty"));
	rt.freeze();

	vb::net::HandshakeServerHost host;
	rt.install_entity_kind_registry(host);
	CHECK_FALSE(host.entity_kind_registry());
}

TEST_CASE("vb.register_keybind is idempotent, capped, and rejected after freeze") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("keybind"));

	const auto r1 = rt.load_pack_file(R"(
		id1 = vb.register_keybind("dash")
		id2 = vb.register_keybind("dash")
		assert(id1 == id2)
		id3 = vb.register_keybind("interact")
		assert(id3 == id1 + 1)
	)");
	REQUIRE(r1);

	// 8 engine-default names (Phase 6.19, pre-registered before this pack
	// script ever ran) + 2 registered above (dash, interact) + 22 here = 32
	// (the cap); the 33rd registration attempt must be rejected.
	const auto r_cap = rt.load_pack_file(R"(
		for i = 1, 22 do
			vb.register_keybind("bind_" .. i)
		end
		local ok, err = pcall(vb.register_keybind, "one_too_many")
		assert(not ok)
	)");
	REQUIRE(r_cap);

	rt.freeze();

	const auto r2 = rt.load_pack_file(R"(vb.register_keybind("too_late"))");
	CHECK_FALSE(r2);
}

TEST_CASE("vb.physics.set_params overrides only the fields it sets, rejected "
		  "after freeze (Phase 6.7)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("physics"));

	const auto r = rt.load_pack_file(R"(
		vb.physics.set_params({ gravity = 3.5, jump_speed = 4.0 })
	)");
	REQUIRE(r);
	rt.freeze();

	vb::physics::MoveParams base;
	base.gravity = 24.0; // e.g. ServerConfig::gravity already folded in
	const vb::physics::MoveParams effective = rt.effective_move_params(base);
	CHECK(effective.gravity == doctest::Approx(3.5)); // pack override wins
	CHECK(effective.jump_speed == doctest::Approx(4.0));
	CHECK(effective.walk_speed == doctest::Approx(base.walk_speed)); // untouched

	const auto r2 = rt.load_pack_file(R"(vb.physics.set_params({ gravity = 1 }))");
	CHECK_FALSE(r2);
}

TEST_CASE("without a vb.physics.set_params call, effective_move_params "
		  "returns base unchanged (Phase 6.7)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("physics_default"));
	rt.freeze();

	vb::physics::MoveParams base;
	base.gravity = 24.0;
	const vb::physics::MoveParams effective = rt.effective_move_params(base);
	CHECK(effective.gravity == doctest::Approx(24.0));
	CHECK(effective.walk_speed == doctest::Approx(base.walk_speed));
	CHECK(effective.jump_speed == doctest::Approx(base.jump_speed));
	CHECK(effective.fly == base.fly);
}

TEST_CASE("vb.action.set_params overrides reach, rejected after freeze "
		  "(Phase 6.21)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("action"));

	const auto r = rt.load_pack_file(R"(
		vb.action.set_params({ reach = 12.0 })
	)");
	REQUIRE(r);
	rt.freeze();

	const vb::net::ActionParams effective =
			rt.effective_action_params(vb::net::ActionParams{});
	CHECK(effective.reach == doctest::Approx(12.0));

	const auto r2 = rt.load_pack_file(R"(vb.action.set_params({ reach = 1 }))");
	CHECK_FALSE(r2);
}

TEST_CASE("without a vb.action.set_params call, effective_action_params "
		  "returns the built-in default (Phase 6.21)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(
			net.server(), registry, temp_storage("action_default"));
	rt.freeze();

	const vb::net::ActionParams effective =
			rt.effective_action_params(vb::net::ActionParams{});
	CHECK(effective.reach == doctest::Approx(vb::net::ActionParams{}.reach));
}

TEST_CASE("vb.physics.get_params()/vb.action.get_params() round-trip the "
		  "effective (post-override) values, and report engine defaults with "
		  "no override (Phase 6.21)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(
			net.server(), registry, temp_storage("get_params"));

	const auto r = rt.load_pack_file(R"(
		local defaults_physics = vb.physics.get_params()
		local defaults_action = vb.action.get_params()
		assert(defaults_action.reach == 5.5)

		vb.physics.set_params({ eye_height = 1.5 })
		vb.action.set_params({ reach = 9.0 })

		local p = vb.physics.get_params()
		local a = vb.action.get_params()
		assert(p.eye_height == 1.5)
		assert(a.reach == 9.0)
		-- an untouched field still reports the same value get_params reported
		-- before any override -- set_params only replaced the fields it set.
		assert(p.walk_speed == defaults_physics.walk_speed)
	)");
	REQUIRE(r);
}

// REMAINING_TASKS.md's hunger gap.
TEST_CASE("vb.hunger.set_params overrides decay/starvation rates, disabled "
		  "(0/0) with no override, rejected after freeze") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("hunger"));

	{
		const vb::net::ServerSession::HungerParams effective =
				rt.effective_hunger_params(vb::net::ServerSession::HungerParams{});
		CHECK(effective.decay_per_second == doctest::Approx(0.0f));
		CHECK(effective.starvation_damage_per_second == doctest::Approx(0.0f));
	}

	const auto r = rt.load_pack_file(R"(
		local defaults = vb.hunger.get_params()
		assert(defaults.decay_per_second == 0)
		assert(defaults.starvation_damage_per_second == 0)

		vb.hunger.set_params({ decay_per_second = 2.0, starvation_damage_per_second = 1.0 })

		local p = vb.hunger.get_params()
		assert(p.decay_per_second == 2.0)
		assert(p.starvation_damage_per_second == 1.0)
	)");
	REQUIRE(r);
	rt.freeze();

	const vb::net::ServerSession::HungerParams effective =
			rt.effective_hunger_params(vb::net::ServerSession::HungerParams{});
	CHECK(effective.decay_per_second == doctest::Approx(2.0f));
	CHECK(effective.starvation_damage_per_second == doctest::Approx(1.0f));

	const auto r2 = rt.load_pack_file(
			R"(vb.hunger.set_params({ decay_per_second = 5.0 }))");
	CHECK_FALSE(r2);
}

TEST_CASE("vb.config.get exposes the operator's ServerConfig read-only "
		  "(Phase 6.13)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("config"));

	vb::core::ServerConfig cfg;
	cfg.tick_rate = 30;
	cfg.max_players = 42;
	cfg.view_distance = 12;
	cfg.void_kill_y = -128.0;
	cfg.motd = "hello";
	cfg.max_connections_per_ip = 3;
	cfg.max_messages_per_second = 25.0;
	rt.set_server_config(cfg);

	const auto r = rt.load_pack_file(R"(
		assert(vb.config.get("tick_rate") == 30)
		assert(vb.config.get("max_players") == 42)
		assert(vb.config.get("view_distance") == 12)
		assert(vb.config.get("void_kill_y") == -128.0)
		assert(vb.config.get("motd") == "hello")
		assert(vb.config.get("max_connections_per_ip") == 3)
		assert(vb.config.get("max_messages_per_second") == 25.0)
		assert(vb.config.get("no_such_key") == nil)
	)");
	REQUIRE(r);
}

TEST_CASE("vb.config.get returns nil for every key when set_server_config "
		  "was never called (Phase 6.13)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("config_unset"));

	const auto r = rt.load_pack_file(R"(
		assert(vb.config.get("tick_rate") == nil)
		assert(vb.config.get("max_players") == nil)
	)");
	REQUIRE(r);
}

TEST_CASE("vb.worldgen.set_pipeline requires 'height', rejected after "
		  "freeze (Phase 6.14)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("worldgen_missing_height"));

	const auto r = rt.load_pack_file(R"(vb.worldgen.set_pipeline({}))");
	CHECK_FALSE(r);

	vb::script::PackRuntime rt2(net.server(), registry, temp_storage("worldgen_frozen"));
	const auto r2 = rt2.load_pack_file(R"(vb.worldgen.set_pipeline({ height = vb.noise.value() }))");
	REQUIRE(r2);
	rt2.freeze();
	const auto r3 = rt2.load_pack_file(
			R"(vb.worldgen.set_pipeline({ height = vb.noise.value() }))");
	CHECK_FALSE(r3);
}

TEST_CASE("vb.noise.value/cellular take a frequency number or { frequency = n }") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("noise_args"));
	REQUIRE(rt.load_pack_file(R"(
		assert(vb.noise.value().frequency == 1)
		assert(vb.noise.value(0.25).frequency == 0.25)
		assert(vb.noise.value({ frequency = 0.5 }).frequency == 0.5) -- used to be ignored
		assert(vb.noise.value({}).frequency == 1)
		assert(vb.noise.cellular({ frequency = 1 / 24 }).frequency == 1 / 24)
		assert(vb.noise.cellular(2).frequency == 2)
		assert(not pcall(vb.noise.value, "fast"))
		assert(not pcall(vb.noise.cellular, { frequency = "high" }))
	)"));
}

TEST_CASE("without a vb.worldgen.set_pipeline call, build_worldgen_pipeline "
		  "returns nullptr (Phase 6.14)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("worldgen_unset"));
	rt.freeze();

	vb::worldgen::WorldGenParams base;
	base.seed = 123;
	CHECK(rt.build_worldgen_pipeline(base) == nullptr);
}

TEST_CASE("vb.worldgen.set_pipeline + vb.register_biome produce a working "
		  "pack-driven pipeline distinct from the fixed default (Phase 6.14)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("worldgen_pipeline"));

	const auto r = rt.load_pack_file(R"(
		vb.register_biome({
			name = "test:plains",
			surface = "base:grass",
			filler = "base:dirt",
			stone = "base:stone",
			probability = 5.0,
			adjacency = { ["test:desert"] = 1.0 },
		})
		vb.register_biome({
			name = "test:desert",
			surface = "base:sand",
			filler = "base:sand",
			stone = "base:stone",
			probability = 1.0,
			adjacency = { ["test:plains"] = 0.02 },
		})
		vb.worldgen.set_pipeline({
			height = vb.noise.fbm({
				source = vb.noise.value(1),
				octaves = 4,
				frequency = 1 / 64,
			}),
			base_height = 64,
			amplitude = 20,
			sea_level = 62,
			cell_size = 96,
			veins = {
				{ block = "base:sand", target_rock = "base:stone",
				  height_min = 0, height_max = 60, vein_size = 5, spawn_rate = 1.0 },
			},
		})
	)");
	REQUIRE(r);
	rt.freeze();

	vb::worldgen::WorldGenParams base;
	base.seed = 0xC0FFEEULL;
	const auto pipeline = rt.build_worldgen_pipeline(base);
	REQUIRE(pipeline != nullptr);
	CHECK_FALSE(pipeline->biomes.empty());
	CHECK(pipeline->biomes.biome_count() == 2);
	CHECK(pipeline->veins.size() == 1);

	const vb::worldgen::WorldGenerator pack_gen(base, registry, pipeline);
	const vb::worldgen::WorldGenerator default_gen(base, registry);

	vb::world::Chunk pack_chunk({ 0, 1, 0 });
	pack_gen.generate(pack_chunk);
	vb::world::Chunk default_chunk({ 0, 1, 0 });
	default_gen.generate(default_chunk);

	bool any_block_differs = false;
	for (std::size_t i = 0; i < vb::world::kChunkVolume; ++i) {
		if (pack_chunk.blocks().get(i) != default_chunk.blocks().get(i)) {
			any_block_differs = true;
			break;
		}
	}
	CHECK(any_block_differs);

	// Deterministic: regenerating with the same pipeline gives the same
	// blocks.
	vb::world::Chunk pack_chunk2({ 0, 1, 0 });
	pack_gen.generate(pack_chunk2);
	for (std::size_t i = 0; i < vb::world::kChunkVolume; ++i) {
		CHECK(pack_chunk.blocks().get(i) == pack_chunk2.blocks().get(i));
	}
}

TEST_CASE("vb.daynight.set_curve overrides the default gradient, rejected "
		  "after freeze (Phase 6.8)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("daynight"));

	const auto r = rt.load_pack_file(R"(
		vb.daynight.set_curve({
			keyframes = {
				{ tick = 0, brightness = 0.1, color = { 10, 20, 30 } },
				{ tick = 12000, brightness = 0.9, color = { 200, 210, 220 } },
			},
		})
	)");
	REQUIRE(r);
	rt.freeze();

	const auto curve = rt.effective_day_night_curve();
	REQUIRE(curve.has_value());
	REQUIRE(curve->keyframes.size() == 2);
	CHECK(curve->keyframes[0].tick == 0);
	CHECK(curve->keyframes[0].brightness == doctest::Approx(0.1));
	CHECK(curve->keyframes[0].color.r == 10);
	CHECK(curve->keyframes[1].tick == 12000);
	CHECK(curve->keyframes[1].color.b == 220);

	const auto r2 = rt.load_pack_file(
			R"(vb.daynight.set_curve({ keyframes = { { tick = 0 } } }))");
	CHECK_FALSE(r2);
}

TEST_CASE("vb.daynight.set_curve rejects an empty/missing keyframes table "
		  "(Phase 6.8)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("daynight_empty"));

	CHECK_FALSE(rt.load_pack_file(R"(vb.daynight.set_curve({}))"));
	CHECK_FALSE(rt.load_pack_file(R"(vb.daynight.set_curve({ keyframes = {} }))"));
}

TEST_CASE("without a vb.daynight.set_curve call, effective_day_night_curve "
		  "returns nullopt (Phase 6.8)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("daynight_default"));
	rt.freeze();

	CHECK_FALSE(rt.effective_day_night_curve().has_value());
}

TEST_CASE("vb.daynight.set_day_length overrides the base day length, "
		  "rejected for non-positive values (Phase 6.8)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("day_length"));

	const auto r = rt.load_pack_file(R"(vb.daynight.set_day_length(600))");
	REQUIRE(r);
	rt.freeze();

	CHECK(rt.effective_day_length_seconds(1200.0) == doctest::Approx(600.0));

	vb::script::PackRuntime rt2(net.server(), registry, temp_storage("day_length_default"));
	rt2.freeze();
	CHECK(rt2.effective_day_length_seconds(1200.0) == doctest::Approx(1200.0));

	vb::script::PackRuntime rt3(net.server(), registry, temp_storage("day_length_bad"));
	CHECK_FALSE(rt3.load_pack_file(R"(vb.daynight.set_day_length(0))"));
	CHECK_FALSE(rt3.load_pack_file(R"(vb.daynight.set_day_length(-5))"));
}

TEST_CASE("vb.render.set_fog overrides the default fog distance, rejected "
		  "after freeze (Phase 7.2)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("fog"));

	const auto r = rt.load_pack_file(R"(
		vb.render.set_fog({ start = 96, ["end"] = 160 })
	)");
	REQUIRE(r);
	rt.freeze();

	const auto fog = rt.effective_fog_params();
	REQUIRE(fog.has_value());
	CHECK(fog->fog_start == doctest::Approx(96.0f));
	CHECK(fog->fog_end == doctest::Approx(160.0f));

	const auto r2 = rt.load_pack_file(R"(vb.render.set_fog({ start = 1, ["end"] = 2 }))");
	CHECK_FALSE(r2);
}

TEST_CASE("vb.render.set_fog rejects a missing start/end or end <= start "
		  "(Phase 7.2)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("fog_bad"));

	CHECK_FALSE(rt.load_pack_file(R"(vb.render.set_fog({}))"));
	CHECK_FALSE(rt.load_pack_file(R"(vb.render.set_fog({ start = 10 }))"));
	CHECK_FALSE(rt.load_pack_file(R"(vb.render.set_fog({ ["end"] = 10 }))"));
	CHECK_FALSE(
			rt.load_pack_file(R"(vb.render.set_fog({ start = 100, ["end"] = 50 }))"));
}

TEST_CASE("without a vb.render.set_fog call, effective_fog_params returns "
		  "nullopt (Phase 7.2)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("fog_default"));
	rt.freeze();

	CHECK_FALSE(rt.effective_fog_params().has_value());
}

TEST_CASE("vb.render.set_fog's underwater_tint overrides the default "
		  "underwater tint (Phase 7.5)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("fog_tint"));

	const auto r = rt.load_pack_file(R"(
		vb.render.set_fog({
			start = 2, ["end"] = 8,
			underwater_tint = { r = 10, g = 20, b = 30 },
		})
	)");
	REQUIRE(r);
	rt.freeze();

	const auto fog = rt.effective_fog_params();
	REQUIRE(fog.has_value());
	CHECK(fog->has_underwater_tint);
	CHECK(fog->underwater_tint_r == 10);
	CHECK(fog->underwater_tint_g == 20);
	CHECK(fog->underwater_tint_b == 30);
}

TEST_CASE("vb.render.set_fog without underwater_tint leaves it unset "
		  "(Phase 7.5)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("fog_no_tint"));

	REQUIRE(rt.load_pack_file(R"(vb.render.set_fog({ start = 1, ["end"] = 2 }))"));
	rt.freeze();

	const auto fog = rt.effective_fog_params();
	REQUIRE(fog.has_value());
	CHECK_FALSE(fog->has_underwater_tint);
}

TEST_CASE("vb.render.set_fog rejects a malformed underwater_tint "
		  "(Phase 7.5)") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("fog_tint_bad"));

	CHECK_FALSE(rt.load_pack_file(R"(vb.render.set_fog({
		start = 1, ["end"] = 2, underwater_tint = { r = 1, g = 2 },
	}))"));
	CHECK_FALSE(rt.load_pack_file(R"(vb.render.set_fog({
		start = 1, ["end"] = 2, underwater_tint = { r = 300, g = 0, b = 0 },
	}))"));
	CHECK_FALSE(rt.load_pack_file(R"(vb.render.set_fog({
		start = 1, ["end"] = 2, underwater_tint = { r = -1, g = 0, b = 0 },
	}))"));
}

TEST_CASE("vb.register_biome / vb.register_craft accept arbitrary def tables") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("biome"));

	const auto r = rt.load_pack_file(R"(
		vb.register_biome({ name = "test:plains", surface = "test:grass" })
		vb.register_craft({ inputs = { "test:wood" }, output = "test:planks" })
	)");
	REQUIRE(r);
}

TEST_CASE("player:take() removes items across slots, all-or-nothing") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("take"));

	// dispatch_chat() fires vb.on("chat", ...) directly, no ServerSession/
	// attach_session() needed -- give()/take()/get_inventory() only touch
	// PackRuntime::Impl's own inventory map, not the network layer (only
	// send_message/open_ui/get_name need a real session). The handler
	// returns the outcome directly (rather than `assert`ing internally and
	// always returning true) so a wrong result fails the CHECK below instead
	// of being swallowed as a warned Lua error by run_veto's error handling.
	REQUIRE(rt.load_pack_file(R"(
		vb.on("chat", function(player, text)
			if text == "setup" then
				-- Two separate slots holding the same item, like give()
				-- naturally produces when called more than once.
				player:give({ item = 5, count = 3 })
				player:give({ item = 5, count = 4 })
				return true
			elseif text == "take-too-many" then
				return player:take({ item = 5, count = 100 })
			elseif text == "take-other-item" then
				return player:take({ item = 9, count = 1 })
			elseif text == "take-some" then
				return player:take({ item = 5, count = 5 })
			elseif text == "check-remaining" then
				local total = 0
				for _, s in ipairs(player:get_inventory()) do
					if s.item == 5 then total = total + s.count end
				end
				return total == 2
			end
			return true
		end)
	)"));
	rt.freeze();

	const vb::core::NetId id{ 1 };
	CHECK_FALSE(rt.dispatch_chat(id, "setup").veto);
	CHECK(rt.dispatch_chat(id, "take-too-many").veto); // over-request: false, no change
	CHECK(rt.dispatch_chat(id, "take-other-item").veto); // wrong item: false, no change
	CHECK_FALSE(rt.dispatch_chat(id, "take-some").veto); // 5 of 7 total across 2 slots
	CHECK_FALSE(rt.dispatch_chat(id, "check-remaining").veto); // 2 left
}

TEST_CASE("player:get_held_item() resolves get_selected_slot() against the "
		  "real inventory") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("held_item"));

	// No ServerSession attached (same "inventory-only" seam the take() test
	// above uses) -- get_selected_slot() falls back to slot 1 (index 0) with
	// no session to ask, exactly like every other accessor here that no-ops
	// without one attached.
	REQUIRE(rt.load_pack_file(R"(
		vb.on("chat", function(player, text)
			if text == "check-default-slot" then
				return player:get_selected_slot() == 1
			elseif text == "check-empty-held" then
				return player:get_held_item() == nil
			elseif text == "give" then
				player:give({ item = 5, count = 3 })
				return true
			elseif text == "check-held" then
				local held = player:get_held_item()
				return held ~= nil and held.item == 5 and held.count == 3
			end
			return true
		end)
	)"));
	rt.freeze();

	const vb::core::NetId id{ 1 };
	CHECK_FALSE(rt.dispatch_chat(id, "check-default-slot").veto);
	CHECK_FALSE(rt.dispatch_chat(id, "check-empty-held").veto); // no session and empty inventory
	CHECK_FALSE(rt.dispatch_chat(id, "give").veto);
	CHECK_FALSE(rt.dispatch_chat(id, "check-held").veto); // slot 1 now holds the given item
}

TEST_CASE("player:give() combines into existing slots up to max_stack, then starts new ones") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("give_stack"));

	// base:water (id 5) keeps the engine default max_stack (64, no
	// register_block override) -- two 40-count gives should merge into one
	// 64-slot plus an 16-count overflow slot, not three/four separate slots.
	REQUIRE(rt.load_pack_file(R"(
		vb.on("chat", function(player, text)
			if text == "give" then
				player:give({ item = 5, count = 40 })
				player:give({ item = 5, count = 40 })
				return true
			elseif text == "check" then
				local counts = {}
				for _, s in ipairs(player:get_inventory()) do
					table.insert(counts, s.count)
				end
				return #counts == 2 and counts[1] == 64 and counts[2] == 16
			end
			return true
		end)
	)"));
	rt.freeze();

	const vb::core::NetId id{ 1 };
	CHECK_FALSE(rt.dispatch_chat(id, "give").veto);
	CHECK_FALSE(rt.dispatch_chat(id, "check").veto);
}

TEST_CASE("register_block{max_stack=N} caps how many combine into one slot") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("give_stack_override"));

	REQUIRE(rt.load_pack_file(R"(
		local tool_id = vb.register_block({ name = "test:pickaxe", max_stack = 1 })
		vb.on("chat", function(player, text)
			if text == "give" then
				player:give({ item = tool_id, count = 1 })
				player:give({ item = tool_id, count = 1 })
				return true
			elseif text == "check" then
				local counts = {}
				for _, s in ipairs(player:get_inventory()) do
					table.insert(counts, s.count)
				end
				return #counts == 2 and counts[1] == 1 and counts[2] == 1
			end
			return true
		end)
	)"));
	rt.freeze();

	const vb::core::NetId id{ 1 };
	CHECK_FALSE(rt.dispatch_chat(id, "give").veto);
	CHECK_FALSE(rt.dispatch_chat(id, "check").veto);
}

TEST_CASE("vb.world.get_block/set_block operate on the attached world") {
	Fixture f(temp_storage("world_blocks"));

	const auto r = f.rt.load_pack_file(R"(
		assert(vb.world.get_block(0, 500, 0) == 0) -- unloaded -> air
		vb.world.set_block(0, 5, 0, 1) -- base:stone
		assert(vb.world.get_block(0, 5, 0) == 1)
	)");
	REQUIRE(r);

	const auto r2 = f.rt.load_pack_file("vb.world.set_block(0, 5, 0, 999)");
	CHECK_FALSE(r2);
}

TEST_CASE("vb.world.raycast finds the first solid voxel") {
	Fixture f(temp_storage("raycast"));
	f.world.set_block({ 0, 5, 0 }, vb::world::base_block::stone);

	const auto r = f.rt.load_pack_file(R"(
		local hit = vb.world.raycast({ x = 0.5, y = 10.5, z = 0.5 },
				{ x = 0, y = -1, z = 0 }, 20)
		assert(hit ~= nil)
		assert(hit.x == 0 and hit.y == 5 and hit.z == 0)
	)");
	REQUIRE(r);

	const auto miss = f.rt.load_pack_file(R"(
		local hit = vb.world.raycast({ x = 100.5, y = 10.5, z = 100.5 },
				{ x = 0, y = -1, z = 0 }, 20)
		assert(hit == nil)
	)");
	REQUIRE(miss);
}

TEST_CASE("vb.world.spawn on an unregistered kind raises a Lua error") {
	Fixture f(temp_storage("spawn"));

	// Phase 6.1: same "reject, don't silently no-op" convention as
	// vb.world.set_block's unknown-block-id check -- a typo'd kind name is a
	// bug, not something to swallow.
	const auto r = f.rt.load_pack_file(R"(
		vb.world.spawn("unknown_kind", { x = 0, y = 0, z = 0 })
	)");
	CHECK_FALSE(r);
}

TEST_CASE("vb.on('tick') fires with dt; vb.after/vb.every fire on schedule") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("timers"));

	const auto r = rt.load_pack_file(R"(
		ticks = 0
		vb.on("tick", function(dt) ticks = ticks + 1 end)
		after_fired = 0
		vb.after(1.0, function() after_fired = after_fired + 1 end)
		every_fired = 0
		vb.every(0.5, function() every_fired = every_fired + 1 end)
	)");
	REQUIRE(r);

	rt.dispatch_tick(0.4);
	rt.dispatch_tick(0.4);
	rt.dispatch_tick(0.4); // 1.2s elapsed

	const auto r2 = rt.load_pack_file(R"(
		assert(ticks == 3)
		assert(after_fired == 1)
		assert(every_fired == 2)
	)");
	REQUIRE(r2);
}

TEST_CASE("a timer handler that errors doesn't crash dispatch_tick") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("timer_error"));

	const auto r = rt.load_pack_file(
			R"(vb.after(0.1, function() error("boom") end))");
	REQUIRE(r);
	rt.dispatch_tick(0.2); // must not throw / hang
}

TEST_CASE("vb.storage persists across PackRuntime instances") {
	const auto storage = temp_storage("storage_persist");
	{
		vb::net::LoopbackNetwork net;
		vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
		vb::script::PackRuntime rt(net.server(), registry, storage);
		const auto r = rt.load_pack_file(
				R"(vb.storage.count = 42; vb.storage.name = "hello")");
		REQUIRE(r);
		CHECK(rt.storage_dirty());
		rt.flush_storage();
		CHECK_FALSE(rt.storage_dirty());
	}
	{
		vb::net::LoopbackNetwork net;
		vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
		vb::script::PackRuntime rt(net.server(), registry, storage);
		const auto r = rt.load_pack_file(R"(
			assert(vb.storage.count == 42)
			assert(vb.storage.name == "hello")
		)");
		REQUIRE(r);
	}
	std::filesystem::remove(storage);
}

TEST_CASE("storage_revision() bumps on explicit flush_storage() and on "
		  "dispatch_tick()'s own implicit auto-flush, not on a no-op tick") {
	const auto storage = temp_storage("storage_revision");
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, storage);
	CHECK(rt.storage_revision() == 0);

	const auto r = rt.load_pack_file(R"(vb.storage.count = 1)");
	REQUIRE(r);
	CHECK(rt.storage_dirty());
	rt.flush_storage();
	CHECK(rt.storage_revision() == 1);

	// A tick with no dirty storage must not bump the revision -- otherwise
	// an embedder polling it (the dedicated server's manifest-refresh check)
	// would rebuild the whole asset manifest every tick for nothing.
	rt.dispatch_tick(0.1);
	CHECK(rt.storage_revision() == 1);

	const auto r2 = rt.load_pack_file(R"(vb.storage.count = 2)");
	REQUIRE(r2);
	rt.dispatch_tick(0.1); // dispatch_tick's own internal auto-flush
	CHECK_FALSE(rt.storage_dirty());
	CHECK(rt.storage_revision() == 2);

	std::filesystem::remove(storage);
}

TEST_CASE("vb.db persists across PackRuntime instances, unlike vb.storage "
		  "it's keyed per-script-chosen-string") {
	const auto storage = temp_storage("db_persist");
	{
		vb::net::LoopbackNetwork net;
		vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
		vb::script::PackRuntime rt(net.server(), registry, storage);
		const auto r = rt.load_pack_file(R"(
			assert(vb.db.get("pack_runtime_test:db_persist:user:alice") == nil)
			vb.db.set("pack_runtime_test:db_persist:user:alice", { level = 3 })
		)");
		REQUIRE(r);
	}
	{
		vb::net::LoopbackNetwork net;
		vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
		vb::script::PackRuntime rt(net.server(), registry, storage);
		const auto r = rt.load_pack_file(R"(
			local v = vb.db.get("pack_runtime_test:db_persist:user:alice")
			assert(v.level == 3)
			vb.db.delete("pack_runtime_test:db_persist:user:alice")
			assert(vb.db.get("pack_runtime_test:db_persist:user:alice") == nil)
		)");
		REQUIRE(r);
	}
	std::filesystem::remove(storage);
}

TEST_CASE("vb.crypto.hash is deterministic and content-sensitive") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("crypto_hash"));
	const auto r = rt.load_pack_file(R"(
		local a = vb.crypto.hash("password123")
		local b = vb.crypto.hash("password123")
		local c = vb.crypto.hash("password124")
		assert(a == b)
		assert(a ~= c)
		assert(#a == 64) -- sha256 hex digest
	)");
	REQUIRE(r);
}

TEST_CASE("player_leave dispatch without an attached session doesn't crash "
		  "a send_message call") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("leave_nosession"));

	const auto r = rt.load_pack_file(
			R"(vb.on("player_leave", function(p) p:send_message("bye") end))");
	REQUIRE(r);

	vb::net::SessionPlayerLeft left;
	left.net_id = vb::core::NetId{ 5 };
	rt.dispatch_player_leave(left); // session never attached -> graceful no-op
}

TEST_CASE("a busy-looping tick handler hits the instruction budget instead "
		  "of hanging") {
	vb::script::VmLimits limits;
	limits.instruction_budget = 200'000;
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry,
			temp_storage("tick_budget"), limits);

	const auto r = rt.load_pack_file(
			R"(vb.on("tick", function() while true do end end))");
	REQUIRE(r);
	rt.dispatch_tick(0.016); // returns instead of hanging
}

#endif // VB_WITH_LUA

TEST_CASE("vb.worldgen.set_pipeline{beach=} puts the beach block at and below sea level + 1") {
	vb::net::LoopbackNetwork net;
	vb::world::BlockRegistry registry = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt(net.server(), registry, temp_storage("worldgen_beach"));
	REQUIRE(rt.load_pack_file(R"(
		vb.register_biome({ name = "t:b", surface = "base:grass", filler = "base:dirt", stone = "base:stone" })
		vb.worldgen.set_pipeline({
			height = vb.noise.constant(0.5), base_height = 62, amplitude = 0, sea_level = 62, beach = "base:sand",
		})
	)"));
	rt.freeze();
	REQUIRE(rt.validate_worldgen());
	const auto pipeline = rt.build_worldgen_pipeline(vb::worldgen::WorldGenParams{});
	REQUIRE(pipeline);
	CHECK(pipeline->beach == registry.find("base:sand"));
	// A flat world exactly at sea level is all beach.
	const vb::worldgen::WorldGenerator gen(vb::worldgen::WorldGenParams{}, registry, pipeline);
	CHECK(gen.block_at_pregen(5, 62, 5) == registry.find("base:sand"));
	CHECK(gen.block_at_pregen(5, 60, 5) == registry.find("base:sand"));

	// Without it the biome's own surface is used.
	vb::world::BlockRegistry registry2 = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt2(net.server(), registry2, temp_storage("worldgen_nobeach"));
	REQUIRE(rt2.load_pack_file(R"(
		vb.register_biome({ name = "t:b", surface = "base:grass", filler = "base:dirt", stone = "base:stone" })
		vb.worldgen.set_pipeline({ height = vb.noise.constant(0.5), base_height = 62, amplitude = 0, sea_level = 62 })
	)"));
	rt2.freeze();
	const auto plain = rt2.build_worldgen_pipeline(vb::worldgen::WorldGenParams{});
	const vb::worldgen::WorldGenerator gen2(vb::worldgen::WorldGenParams{}, registry2, plain);
	CHECK(gen2.block_at_pregen(5, 62, 5) == registry2.find("base:grass"));

	// An unknown beach block is a validation error.
	vb::world::BlockRegistry registry3 = vb::world::BlockRegistry::base();
	vb::script::PackRuntime rt3(net.server(), registry3, temp_storage("worldgen_badbeach"));
	REQUIRE(rt3.load_pack_file(R"(vb.worldgen.set_pipeline({ height = vb.noise.constant(0.5), beach = "nope:sand" }))"));
	rt3.freeze();
	const auto bad = rt3.validate_worldgen();
	CHECK_FALSE(bad);
	CHECK(bad.message.find("nope:sand") != std::string::npos);
}
