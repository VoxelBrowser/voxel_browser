#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "check_pack.hpp"
#include "vb/script/global_scan.hpp"

// `voxel_browser_server --check-pack` (architecture_spec/dev-experience.md §3.4): headless pack
// validation with file:line diagnostics, plus the static per-environment global check (§3.4.1).

#if VB_WITH_LUA

namespace fs = std::filesystem;
using vb::server::CheckOptions;
using vb::server::CheckReport;
using vb::server::PackDiagnostic;

namespace {

struct TempPack {
	fs::path dir;
	explicit TempPack(const char *name) {
		dir = fs::temp_directory_path() / (std::string("vb_check_pack_test_") + name);
		fs::remove_all(dir);
		fs::create_directories(dir);
		write("pack.toml", "name = \"t\"\nversion = \"0.1.0\"\nengine_version_req = \"*\"\nentry = \"init.lua\"\n");
	}
	~TempPack() { fs::remove_all(dir); }
	void write(const std::string &rel, const std::string &text) const {
		fs::create_directories((dir / rel).parent_path());
		std::ofstream(dir / rel, std::ios::binary) << text;
	}
};

const PackDiagnostic *find(const CheckReport &r, const std::string &file, int line = -1) {
	for (const auto &d : r.diagnostics) {
		if (d.file == file && (line < 0 || d.line == line)) {
			return &d;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("scan_globals lists reads and writes with lines, including nested functions") {
	const auto scan = vb::script::scan_globals("local x = 1\nfoo = bar\nlocal function f()\n  return baz.qux\nend\n", "t.lua");
	REQUIRE(scan.ok);
	bool foo_w = false, bar_r = false, baz_r = false;
	for (const auto &a : scan.accesses) {
		foo_w = foo_w || (a.name == "foo" && a.is_write && a.line == 2);
		bar_r = bar_r || (a.name == "bar" && !a.is_write && a.line == 2);
		baz_r = baz_r || (a.name == "baz" && !a.is_write && a.line == 4);
	}
	CHECK(foo_w);
	CHECK(bar_r);
	CHECK(baz_r);

	const auto bad = vb::script::scan_globals("x = = 1", "bad.lua");
	CHECK_FALSE(bad.ok);
	CHECK(bad.error.rfind("bad.lua:1:", 0) == 0);
}

TEST_CASE("check_pack: a clean pack passes") {
	TempPack p("clean");
	p.write("init.lua", "vb.on('chat', function(player, text) end)\n");
	p.write("blocks/a.lua", "vb.register_block{ name = 'mypack:a', texture = 'textures/a.png' }\n");
	p.write("textures/a.png", "png");
	p.write("ui/hud.lua", "ui.define_hud(function(state) return { widgets = {} } end)\n");
	const CheckReport r = vb::server::check_pack(p.dir);
	INFO(vb::server::format_human(r, false));
	CHECK(r.errors() == 0);
	CHECK(r.ok(true));
}

TEST_CASE("check_pack: syntax errors carry file:line") {
	TempPack p("syntax");
	p.write("init.lua", "local x = 1\nlocal = 2\n");
	const CheckReport r = vb::server::check_pack(p.dir);
	REQUIRE(r.errors() >= 1);
	const auto *d = find(r, "init.lua", 2);
	REQUIRE(d != nullptr);
	CHECK(d->severity == PackDiagnostic::Severity::kError);
}

TEST_CASE("check_pack: runtime and registration errors name the pack file") {
	TempPack p("runtime");
	p.write("init.lua", "\nlocal x = 1\nerror('boom')\n");
	CheckReport r = vb::server::check_pack(p.dir);
	const auto *d = find(r, "init.lua", 3);
	REQUIRE(d != nullptr);
	CHECK(d->message.find("boom") != std::string::npos);

	TempPack q("registration");
	q.write("blocks/bad.lua", "-- comment\nvb.register_block{ solid = true }\n");
	r = vb::server::check_pack(q.dir);
	const auto *e = find(r, "blocks/bad.lua", 2);
	REQUIRE(e != nullptr);
	CHECK(e->message.find("'name' is required") != std::string::npos);
}

TEST_CASE("check_pack: invalid worldgen data and bad ui scripts are reported") {
	TempPack p("worldgen");
	p.write("init.lua",
			"vb.register_biome{ name = 'mypack:b', decoration = { { structure = 'mypack:no_such_tree', spawn_rate = 1 } } }\n"
			"vb.worldgen.set_pipeline{ height = vb.noise.constant(64) }\n");
	CheckReport r = vb::server::check_pack(p.dir);
	INFO(vb::server::format_human(r, false));
	CHECK(r.errors() >= 1);

	TempPack q("ui_runtime");
	q.write("ui/a.lua", "\n\nerror('ui boom')\n");
	r = vb::server::check_pack(q.dir);
	CHECK(find(r, "ui/a.lua", 3) != nullptr);
}

TEST_CASE("check_pack: environment globals are enforced statically") {
	TempPack p("env");
	p.write("init.lua", "ui.define('x', function() end)\nclient.time()\nlocal t = os.time()\n");
	p.write("ui/hud.lua", "\nlocal w = vb.world.get_block(0,0,0)\nui.define_hud(function() end)\n");
	CheckReport r = vb::server::check_pack(p.dir);
	INFO(vb::server::format_human(r, false));
	const auto *a = find(r, "init.lua", 1);
	REQUIRE(a != nullptr);
	CHECK(a->message.find("'ui' is not available in the server pack VM") != std::string::npos);
	CHECK(find(r, "init.lua", 2) != nullptr);
	const auto *b = find(r, "init.lua", 3);
	REQUIRE(b != nullptr);
	CHECK(b->message.find("sandbox") != std::string::npos);
	const auto *c = find(r, "ui/hud.lua", 2);
	REQUIRE(c != nullptr);
	CHECK(c->message.find("'vb' is not available in the client UI VM") != std::string::npos);
	CHECK(r.errors() >= 4);
}

TEST_CASE("check_pack: globals shared inside one environment are fine, across environments are not") {
	TempPack p("shared");
	p.write("ui/_style.lua", "base_ui = { pad = 4 }\n");
	p.write("ui/hud.lua", "local p = base_ui.pad\n");
	p.write("init.lua", "local p = base_ui.pad\n");
	const CheckReport r = vb::server::check_pack(p.dir);
	CHECK(find(r, "ui/hud.lua") == nullptr);
	// base_ui exists only in the client UI VM: on the server it is nil, so the load fails there.
	const auto *d = find(r, "init.lua", 1);
	REQUIRE(d != nullptr);
	CHECK(d->severity == PackDiagnostic::Severity::kError);
	CHECK(d->message.find("base_ui") != std::string::npos);
}

TEST_CASE("check_pack: unknown globals warn, and --strict fails on them") {
	TempPack p("strict");
	p.write("init.lua", "\nprint(undefined_thing)\n");
	const CheckReport r = vb::server::check_pack(p.dir);
	CHECK(r.errors() == 0);
	CHECK(r.warnings() == 1);
	CHECK(r.ok(false));
	CHECK_FALSE(r.ok(true));
	CHECK(find(r, "init.lua", 2) != nullptr);
}

TEST_CASE("check_pack: engine_version_req is enforced and reported on its pack.toml line") {
	TempPack p("req");
	p.write("pack.toml", "name = \"t\"\nversion = \"0.1.0\"\nengine_version_req = \">=9.0.0\"\n");
	p.write("init.lua", "-- ok\n");
	CheckOptions options;
	options.engine = { 0, 5, 2 };
	CheckReport r = vb::server::check_pack(p.dir, options);
	const auto *d = find(r, "pack.toml", 3);
	REQUIRE(d != nullptr);
	CHECK(d->message.find("requires engine >=9.0.0") != std::string::npos);
	CHECK(d->message.find("this build is 0.5.2") != std::string::npos);

	options.ignore_engine_req = true;
	CHECK(vb::server::check_pack(p.dir, options).errors() == 0);

	p.write("pack.toml", "name = \"t\"\nengine_version_req = \"nonsense\"\n");
	options.ignore_engine_req = false;
	r = vb::server::check_pack(p.dir, options);
	CHECK(find(r, "pack.toml", 2) != nullptr);

	p.write("pack.toml", "name = \"t\"\n"); // missing field: warning, not an error
	r = vb::server::check_pack(p.dir, options);
	CHECK(r.errors() == 0);
	CHECK(r.warnings() == 1);
}

TEST_CASE("check_pack: a malformed auth.lua and a missing pack.toml are errors") {
	TempPack p("auth");
	p.write("auth.lua", "return { provider = 'nope' }\n");
	CHECK(find(vb::server::check_pack(p.dir), "auth.lua") != nullptr);

	TempPack q("nomanifest");
	fs::remove(q.dir / "pack.toml");
	CHECK(find(vb::server::check_pack(q.dir), "pack.toml") != nullptr);
}

TEST_CASE("check_pack: --json output has the documented shape") {
	TempPack p("json");
	p.write("init.lua", "\nerror('x')\n");
	const CheckReport r = vb::server::check_pack(p.dir);
	const auto j = nlohmann::json::parse(vb::server::format_json(r, false));
	CHECK(j["ok"] == false);
	CHECK(j["errors"] == 1);
	CHECK(j["pack"]["name"] == "t");
	REQUIRE(j["diagnostics"].size() == 1);
	CHECK(j["diagnostics"][0]["severity"] == "error");
	CHECK(j["diagnostics"][0]["file"] == "init.lua");
	CHECK(j["diagnostics"][0]["line"] == 2);
	CHECK(j["diagnostics"][0]["message"].get<std::string>().find("x") != std::string::npos);
}

TEST_CASE("check_pack: unknown keys in definition tables warn at the registering line") {
	TempPack p("keys");
	p.write("textures/a.png", "png");
	p.write("blocks/a.lua",
			"-- a block\n"
			"vb.register_block{ name = 'mypack:a', texture = 'textures/a.png', max_damge = 3 }\n");
	p.write("init.lua", "\nvb.register_entity{ name = 'mypack:e', helth = 5, on_tick = function(self, dt) end }\n");
	const CheckReport r = vb::server::check_pack(p.dir);
	INFO(vb::server::format_human(r, false));
	CHECK(r.errors() == 0);
	const auto *block = find(r, "blocks/a.lua", 2);
	REQUIRE(block != nullptr);
	CHECK(block->severity == PackDiagnostic::Severity::kWarning);
	CHECK(block->message.find("unknown key 'max_damge'") != std::string::npos);
	CHECK(block->message.find("BlockDef") != std::string::npos);
	const auto *entity = find(r, "init.lua", 2);
	REQUIRE(entity != nullptr);
	CHECK(entity->message.find("unknown key 'helth'") != std::string::npos);
	CHECK(r.warnings() == 2); // the valid keys (on_tick, texture, ...) are not reported
}

TEST_CASE("check_pack: a block without a texture, or with one the pack lacks, warns") {
	TempPack p("textures");
	p.write("blocks/a.lua",
			"vb.register_block{ name = 'mypack:plain' }\n"
			"vb.register_block{ name = 'mypack:lost', texture = 'textures/nope.png', crack_texture = 'cracks.png' }\n");
	const CheckReport r = vb::server::check_pack(p.dir);
	INFO(vb::server::format_human(r, false));
	CHECK(r.errors() == 0);
	REQUIRE(r.warnings() == 3);
	const auto *plain = find(r, "blocks/a.lua", 1);
	REQUIRE(plain != nullptr);
	CHECK(plain->message.find("'mypack:plain' has no texture") != std::string::npos);
	int lost = 0;
	for (const auto &d : r.diagnostics) {
		if (d.line == 2 && (d.message.find("texture 'textures/nope.png' is not in the pack") != std::string::npos ||
								   d.message.find("crack_texture 'cracks.png' is not in the pack") != std::string::npos)) {
			++lost;
		}
	}
	CHECK(lost == 2);
}

TEST_CASE("check_pack: using an API newer than engine_version_req's lower bound warns") {
	TempPack p("since");
	p.write("pack.toml", "name = \"t\"\nengine_version_req = \">=0.1.0\"\n");
	p.write("init.lua",
			"-- vb.render.set_third_person(false) in a comment does not count\n"
			"local s = 'vb.render.set_third_person'\n"
			"vb.render.set_third_person(false)\n"
			"vb.render.set_third_person(true) -- reported once per file\n");
	p.write("ui/hud.lua", "ui.define_hud(function(state)\n  client.capture_mouse(true)\n  return { widgets = {} }\nend)\n");
	CheckOptions options;
	options.engine = { 9, 0, 0 };
	CheckReport r = vb::server::check_pack(p.dir, options);
	INFO(vb::server::format_human(r, false));
	CHECK(r.errors() == 0);
	CHECK(r.warnings() == 2);
	const auto *server = find(r, "init.lua", 3);
	REQUIRE(server != nullptr);
	CHECK(server->message.find("vb.render.set_third_person is new in 0.1.7") != std::string::npos);
	CHECK(server->message.find(">=0.1.7") != std::string::npos);
	CHECK(find(r, "ui/hud.lua", 2) != nullptr); // client.capture_mouse, since 0.1.5

	// A requirement that already admits only new enough engines, or none at all: quiet.
	p.write("pack.toml", "name = \"t\"\nengine_version_req = \">=0.1.7\"\n");
	CHECK(vb::server::check_pack(p.dir, options).warnings() == 0);
	p.write("pack.toml", "name = \"t\"\nengine_version_req = \"*\"\n");
	CHECK(vb::server::check_pack(p.dir, options).warnings() == 0);
}

TEST_CASE("check_pack: the bundled packs are clean") {
	for (const char *rel : { "content/base", "content/examples/kitchen_sink" }) {
		const CheckReport r = vb::server::check_pack(fs::path(VB_PROJECT_SOURCE_DIR) / rel);
		INFO(rel << "\n" << vb::server::format_human(r, true));
		CHECK(r.errors() == 0);
		CHECK(r.warnings() == 0);
	}
}

#endif
