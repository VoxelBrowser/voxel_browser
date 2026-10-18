// Phase 10.E: `vb pack init|check|types|info` (architecture_spec/dev-experience.md §3.5).
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

#include "vb/cli/commands.hpp"
#include "vb/cli/layout.hpp"
#include "vb/cli/store.hpp"

namespace fs = std::filesystem;
using namespace vb::cli;

namespace {

struct TempDir {
	fs::path path;
	TempDir() {
		std::random_device rd;
		path = fs::temp_directory_path() / ("vb_pack_cli_test_" + std::to_string(rd()));
		fs::create_directories(path);
	}
	~TempDir() {
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

struct Run {
	int code;
	std::string out;
	std::string err;
};

Run run_vb(const Layout &layout, std::vector<std::string> args) {
	std::ostringstream out, err;
	const int code = run_cli(args, layout, out, err);
	return { code, out.str(), err.str() };
}

std::string read(const fs::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

} // namespace

TEST_CASE("vb pack init writes every template and the editor files") {
	TempDir t;
	const Layout layout(t.path / "data", t.path / "config", t.path / "cache");
	for (const char *tmpl : { "minimal", "ui", "worldgen" }) {
		const fs::path dir = t.path / (std::string("pack_") + tmpl);
		const Run r = run_vb(layout, { "pack", "init", dir.string(), "--template", tmpl });
		INFO(r.err);
		REQUIRE(r.code == 0);
		for (const char *f : { "pack.toml", "init.lua", "README.md", "AGENTS.md", ".luarc.json", ".gitignore",
					 "blocks/example.lua", "textures/example.png", ".vb/lua/vb.lua", ".vb/lua/ui.lua" }) {
			CHECK(fs::exists(dir / f));
		}
		const std::string toml = read(dir / "pack.toml");
		CHECK(toml.find(std::string("name = \"pack_") + tmpl + "\"") != std::string::npos);
		CHECK(toml.find("engine_version_req = \">=") != std::string::npos);
		CHECK(read(dir / "init.lua").find("{{") == std::string::npos);
		CHECK(read(dir / "textures/example.png").substr(1, 3) == "PNG");
	}
	CHECK(fs::exists(t.path / "pack_ui" / "ui" / "hello.lua"));
	// ui/ gets its own LuaLS config with only the UI-VM stubs.
	const std::string ui_rc = read(t.path / "pack_ui" / "ui" / ".luarc.json");
	CHECK(ui_rc.find("../.vb/lua/ui.lua") != std::string::npos);
	CHECK(ui_rc.find("vb.lua") == std::string::npos);
	CHECK_FALSE(fs::exists(t.path / "pack_minimal" / "ui" / ".luarc.json"));
	CHECK(fs::exists(t.path / "pack_worldgen" / "worldgen.lua"));
}

TEST_CASE("vb pack init refuses a non-empty directory unless --force, and validates input") {
	TempDir t;
	const Layout layout(t.path / "data", t.path / "config", t.path / "cache");
	const fs::path dir = t.path / "p";
	REQUIRE(run_vb(layout, { "pack", "init", dir.string() }).code == 0);
	const Run again = run_vb(layout, { "pack", "init", dir.string() });
	CHECK(again.code == 1);
	CHECK(again.err.find("error:") == 0);
	CHECK(again.err.find("--force") != std::string::npos);
	CHECK(run_vb(layout, { "pack", "init", dir.string(), "--force", "--name", "renamed" }).code == 0);
	CHECK(read(dir / "pack.toml").find("name = \"renamed\"") != std::string::npos);

	CHECK(run_vb(layout, { "pack", "init", (t.path / "x").string(), "--name", "Bad Name" }).code == 1);
	CHECK(run_vb(layout, { "pack", "init", (t.path / "y").string(), "--engine-req", "latest" }).code == 1);
	CHECK(run_vb(layout, { "pack", "init", (t.path / "z").string(), "--template", "nope" }).code == 1);
	CHECK(run_vb(layout, { "pack", "init", (t.path / "w").string(), "--engine-req", "^0.6.1" }).code == 0);
	CHECK(read(t.path / "w" / "pack.toml").find("engine_version_req = \"^0.6.1\"") != std::string::npos);
}

TEST_CASE("vb pack init --json lists what it created") {
	TempDir t;
	const Layout layout(t.path / "data", t.path / "config", t.path / "cache");
	const Run r = run_vb(layout, { "pack", "init", (t.path / "j").string(), "--json" });
	REQUIRE(r.code == 0);
	const auto j = nlohmann::json::parse(r.out);
	CHECK(j["name"] == "j");
	CHECK(j["template"] == "minimal");
	bool saw = false;
	for (const auto &f : j["created"]) {
		saw = saw || f == "pack.toml";
	}
	CHECK(saw);
}

TEST_CASE("vb pack types rewrites the stubs and keeps a customised .luarc.json") {
	TempDir t;
	const Layout layout(t.path / "data", t.path / "config", t.path / "cache");
	const fs::path dir = t.path / "p";
	REQUIRE(run_vb(layout, { "pack", "init", dir.string() }).code == 0);
	fs::remove(dir / ".vb" / "lua" / "vb.lua");
	{
		std::ofstream(dir / ".luarc.json") << "{\"custom\":true}";
	}
	REQUIRE(run_vb(layout, { "pack", "types", dir.string() }).code == 0);
	CHECK(fs::exists(dir / ".vb" / "lua" / "vb.lua"));
	CHECK(read(dir / ".luarc.json") == "{\"custom\":true}");
	REQUIRE(run_vb(layout, { "pack", "types", dir.string(), "--force" }).code == 0);
	CHECK(read(dir / ".luarc.json").find("Lua 5.4") != std::string::npos);
	CHECK(run_vb(layout, { "pack", "types", (t.path / "nothing").string() }).code == 1);
}

TEST_CASE("vb pack info reports the manifest and file counts") {
	TempDir t;
	const Layout layout(t.path / "data", t.path / "config", t.path / "cache");
	const fs::path dir = t.path / "p";
	REQUIRE(run_vb(layout, { "pack", "init", dir.string(), "--template", "ui" }).code == 0);
	const Run r = run_vb(layout, { "pack", "info", dir.string(), "--json" });
	REQUIRE(r.code == 0);
	const auto j = nlohmann::json::parse(r.out);
	CHECK(j["name"] == "p");
	CHECK(j["files"]["lua:ui"] == 1);
	CHECK(j["files"]["textures"] == 1);
	CHECK(j["engine_version_req_valid"] == true);
}

#if defined(VB_TEST_SERVER_EXE) && VB_WITH_LUA
TEST_CASE("vb pack check runs the installed server's --check-pack") {
	TempDir t;
	const Layout layout(t.path / "data", t.path / "config", t.path / "cache");
	REQUIRE(add_link(layout, "dev", fs::path(VB_TEST_SERVER_EXE).parent_path()));
	const fs::path dir = t.path / "p";
	REQUIRE(run_vb(layout, { "pack", "init", dir.string(), "--engine-req", "*" }).code == 0);

	Run ok = run_vb(layout, { "pack", "check", dir.string() });
	CHECK(ok.code == 0);

	{
		std::ofstream(dir / "init.lua") << "\nerror('boom')\n";
	}
	const Run bad = run_vb(layout, { "pack", "check", dir.string(), "--json" });
	CHECK(bad.code == 1);

	// A requirement no installed version can satisfy is reported with an install hint.
	{
		std::ofstream(dir / "pack.toml") << "name = \"p\"\nengine_version_req = \">=99.0.0\"\n";
	}
	const Layout empty(t.path / "d2", t.path / "c2", t.path / "k2");
	const Run none = run_vb(empty, { "pack", "check", dir.string() });
	CHECK(none.code == 1);
	CHECK(none.err.find("error: no installed version satisfies") == 0);
}
#endif
