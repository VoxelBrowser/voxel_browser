#include "check_pack.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "vb/auth/config.hpp"
#include "vb/net/loopback.hpp"
#include "vb/script/global_scan.hpp"
#include "vb/script/pack_loader.hpp"
#include "vb/script/pack_manifest.hpp"
#include "vb/script/pack_runtime.hpp"
#include "vb/script/ui_runtime.hpp"
#include "vb/world/block.hpp"
#include "vb/world/chunk.hpp"
#include "vb/worldgen/generator.hpp"

namespace vb::server {

namespace fs = std::filesystem;
using Severity = PackDiagnostic::Severity;

int CheckReport::errors() const {
	return static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(),
			[](const PackDiagnostic &d) { return d.severity == Severity::kError; }));
}

int CheckReport::warnings() const {
	return static_cast<int>(diagnostics.size()) - errors();
}

LuaEnv environment_of(const std::string &rel) {
	if (rel == "auth.lua") {
		return LuaEnv::kAuth;
	}
	if (rel.rfind("ui/", 0) == 0) {
		return LuaEnv::kUi;
	}
	if (rel.rfind("data/", 0) == 0) {
		return LuaEnv::kData;
	}
	return LuaEnv::kServer;
}

namespace {

// What the sandbox leaves in every pack Lua state (Vm::strip_sandbox + the reinstated `require`).
const std::set<std::string> kBuiltins = { "_G", "_ENV", "_VERSION", "assert", "error", "getmetatable", "ipairs",
	"next", "pairs", "pcall", "print", "rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable",
	"tonumber", "tostring", "type", "warn", "xpcall", "require", "string", "table", "math", "coroutine",
	"utf8", "debug" };

// Present in stock Lua, removed by the sandbox.
const std::set<std::string> kSandboxRemoved = { "os", "io", "package", "dofile", "loadfile", "load", "loadstring",
	"collectgarbage" };

const std::set<std::string> kServerGlobals = { "vb" };

struct ApiSince {
	const char *member;
	const char *since;
};
struct ClassKeys {
	const char *name;
	const char *keys; // space-separated
};
#include "check_pack_data.inc"

// Wraps the registration functions while the pack loads so the definition tables they receive can
// be checked afterwards: the keys (against the stub's ---@class), block textures, and where the call
// came from. __vb_check_dump() returns one record per call: fields separated by \31, records by \30.
constexpr const char *kRecordRegistrations = R"lua(
local regs = {}
local function wrap(tbl, fname, class, label)
	local orig = tbl and tbl[fname]
	if type(orig) ~= "function" then
		return
	end
	tbl[fname] = function(def, ...)
		if type(def) == "table" then
			local keys = {}
			for k in pairs(def) do
				if type(k) == "string" then
					keys[#keys + 1] = k
				end
			end
			table.sort(keys)
			local function str(v)
				return type(v) == "string" and v or ""
			end
			regs[#regs + 1] = table.concat({ label, class, str(def.name), table.concat(keys, ","),
				str(def.texture), str(def.crack_texture), debug.traceback("", 2) }, "\31")
		end
		return orig(def, ...)
	end
end
wrap(vb, "register_block", "BlockDef", "vb.register_block")
wrap(vb, "register_item", "ItemDef", "vb.register_item")
wrap(vb, "register_entity", "EntityDef", "vb.register_entity")
wrap(vb, "register_biome", "BiomeDef", "vb.register_biome")
wrap(vb, "register_structure", "StructureDef", "vb.register_structure")
wrap(vb, "register_craft", "CraftDef", "vb.register_craft")
wrap(vb.worldgen, "set_pipeline", "PipelineDef", "vb.worldgen.set_pipeline")
function __vb_check_dump()
	return table.concat(regs, "\30")
end
)lua";
const std::set<std::string> kUiGlobals = { "ui", "client" };

const char *env_name(LuaEnv e) {
	switch (e) {
		case LuaEnv::kServer:
			return "the server pack VM";
		case LuaEnv::kUi:
			return "the client UI VM";
		case LuaEnv::kData:
			return "a data script";
		case LuaEnv::kAuth:
			return "auth.lua";
	}
	return "";
}

std::string read_file(const fs::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

std::vector<std::string> list_lua_files(const fs::path &dir) {
	std::vector<std::string> out;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator();
			it.increment(ec)) {
		if (ec) {
			break;
		}
		const fs::path rel = fs::relative(it->path(), dir, ec);
		if (!rel.empty() && rel.begin()->generic_string().rfind('.', 0) == 0) {
			if (it->is_directory(ec)) {
				it.disable_recursion_pending();
			}
			continue;
		}
		if (it->is_regular_file(ec) && it->path().extension() == ".lua") {
			out.push_back(rel.generic_string());
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

void add(CheckReport &r, Severity s, std::string file, int line, std::string message) {
	r.diagnostics.push_back({ s, std::move(file), line, std::move(message) });
}

// Turns a Lua error ("blocks/a.lua:5: boom\nstack traceback:\n\t[C]: in function 'vb.register_block'\n\t
// blocks/a.lua:5: in main chunk") into a diagnostic with the best file:line available. Errors raised
// from C++ bindings carry no position of their own, so the traceback's first pack-file frame is used.
void add_lua_error(CheckReport &r, const std::set<std::string> &known_files, const std::string &raw,
		const std::string &fallback_file) {
	// Chunks are named by their pack-relative path, which Lua prints as `[string "blocks/a.lua"]:5:`
	// (the loader passes a plain name) or `blocks/a.lua:5:` (compile errors of an `@name`).
	static const std::regex head(R"x(^(?:\[string "([^"]+)"\]|([^:\n]+\.lua)):(\d+): (.*)$)x");
	static const std::regex frame(R"x(\n\s+(?:\[string "([^"]+)"\]|([^:\s]+\.lua)):(\d+):)x");
	const std::string first = raw.substr(0, raw.find('\n'));
	std::smatch m;
	if (std::regex_match(first, m, head)) {
		const std::string file = m[1].matched ? m[1].str() : m[2].str();
		if (known_files.count(file) != 0) {
			add(r, Severity::kError, file, std::stoi(m[3].str()), m[4].str());
			return;
		}
	}
	// Errors raised from C++ bindings carry no position: use the first pack-file frame of the traceback.
	for (auto it = std::sregex_iterator(raw.begin(), raw.end(), frame); it != std::sregex_iterator(); ++it) {
		const std::string file = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
		if (known_files.count(file) != 0) {
			add(r, Severity::kError, file, std::stoi((*it)[3].str()), first);
			return;
		}
	}
	add(r, Severity::kError, fallback_file, 0, first);
}

std::vector<std::string> split(const std::string &s, char sep) {
	std::vector<std::string> out;
	std::size_t start = 0;
	for (;;) {
		const std::size_t at = s.find(sep, start);
		out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
		if (at == std::string::npos) {
			return out;
		}
		start = at + 1;
	}
}

// The first pack-file frame of a Lua traceback, as (file, line); ("", 0) if none.
std::pair<std::string, int> first_pack_frame(const std::string &traceback, const std::set<std::string> &known_files) {
	static const std::regex frame(R"x(\n\s+(?:\[string "([^"]+)"\]|([^:\s]+\.lua)):(\d+):)x");
	for (auto it = std::sregex_iterator(traceback.begin(), traceback.end(), frame); it != std::sregex_iterator(); ++it) {
		const std::string file = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
		if (known_files.count(file) != 0) {
			return { file, std::stoi((*it)[3].str()) };
		}
	}
	return { "", 0 };
}

// What the registration wrapper saw: unknown definition keys (typos the engine silently ignores)
// and block textures that are absent or missing from the pack.
void report_registrations(CheckReport &r, const fs::path &dir, const std::string &dump,
		const std::set<std::string> &known_files) {
	std::map<std::string, std::set<std::string>> class_keys;
	for (const ClassKeys &c : kClassKeys) {
		std::istringstream in(c.keys);
		for (std::string k; in >> k;) {
			class_keys[c.name].insert(k);
		}
	}
	std::error_code ec;
	for (const std::string &record : split(dump, '\x1e')) {
		const std::vector<std::string> f = split(record, '\x1f');
		if (f.size() < 7) {
			continue;
		}
		const std::string &fn = f[0], &cls = f[1], &name = f[2], &texture = f[4], &crack = f[5];
		const auto [file, line] = first_pack_frame(f[6], known_files);
		const std::string what = name.empty() ? fn : fn + " '" + name + "'";
		if (const auto it = class_keys.find(cls); it != class_keys.end() && !f[3].empty()) {
			for (const std::string &key : split(f[3], ',')) {
				if (it->second.count(key) == 0) {
					add(r, Severity::kWarning, file, line, what + ": unknown key '" + key + "' (not a " + cls + " field, so the engine ignores it; a typo?)");
				}
			}
		}
		if (fn != "vb.register_block") {
			continue;
		}
		if (texture.empty()) {
			add(r, Severity::kWarning, file, line, what + " has no texture: it is drawn as a flat placeholder colour");
		} else if (!fs::is_regular_file(dir / texture, ec)) {
			add(r, Severity::kWarning, file, line, what + ": texture '" + texture + "' is not in the pack");
		}
		if (!crack.empty() && !fs::is_regular_file(dir / crack, ec)) {
			add(r, Severity::kWarning, file, line, what + ": crack_texture '" + crack + "' is not in the pack");
		}
	}
}

// API members a file uses (`vb.world.raycast`, `ui.define`, ...) with the line they first appear on.
// A small Lua scanner: comments and strings are skipped, so only real code counts.
std::vector<std::pair<std::string, int>> api_chains(const std::string &src) {
	std::vector<std::pair<std::string, int>> out;
	const auto is_id0 = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; };
	const auto is_id = [&](char c) { return is_id0(c) || std::isdigit(static_cast<unsigned char>(c)) != 0; };
	// Length of the `[[` / `[==[` opener at i (its `=` count + 2), or 0.
	const auto long_open = [&](std::size_t i) -> std::size_t {
		if (i >= src.size() || src[i] != '[') {
			return 0;
		}
		std::size_t j = i + 1;
		while (j < src.size() && src[j] == '=') {
			++j;
		}
		return j < src.size() && src[j] == '[' ? j - i + 1 : 0;
	};
	int line = 1;
	const auto skip_long = [&](std::size_t &i, std::size_t open) {
		const std::string close = "]" + std::string(open - 2, '=') + "]";
		const std::size_t end = src.find(close, i + open);
		const std::size_t stop = end == std::string::npos ? src.size() : end + close.size();
		line += static_cast<int>(std::count(src.begin() + static_cast<std::ptrdiff_t>(i), src.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
		i = stop;
	};
	std::size_t i = 0;
	char prev = 0; // last significant character, to skip `x.vb` and `obj:vb`
	while (i < src.size()) {
		const char c = src[i];
		if (c == '\n') {
			++line;
			++i;
			continue;
		}
		if (c == '-' && i + 1 < src.size() && src[i + 1] == '-') {
			if (const std::size_t open = long_open(i + 2)) {
				i += 2;
				skip_long(i, open);
			} else {
				while (i < src.size() && src[i] != '\n') {
					++i;
				}
			}
			continue;
		}
		if (const std::size_t open = long_open(i)) {
			skip_long(i, open);
			prev = 's';
			continue;
		}
		if (c == '"' || c == '\'') {
			++i;
			while (i < src.size() && src[i] != c && src[i] != '\n') {
				i += src[i] == '\\' ? 2 : 1;
			}
			++i;
			prev = 's';
			continue;
		}
		if (is_id0(c)) {
			std::size_t j = i;
			while (j < src.size() && is_id(src[j])) {
				++j;
			}
			std::string chain = src.substr(i, j - i);
			const bool root = (chain == "vb" || chain == "ui" || chain == "client") && prev != '.' && prev != ':';
			while (root && j + 1 < src.size() && src[j] == '.' && is_id0(src[j + 1])) {
				std::size_t k = j + 1;
				while (k < src.size() && is_id(src[k])) {
					++k;
				}
				chain += src.substr(j, k - j);
				j = k;
			}
			if (root && chain.find('.') != std::string::npos) {
				out.emplace_back(chain, line);
			}
			i = j;
			prev = 'a';
			continue;
		}
		if (c != ' ' && c != '\t' && c != '\r') {
			prev = c;
		}
		++i;
	}
	return out;
}

// `---@vb since` newer than the lowest engine the pack admits: the pack would load on an engine
// that lacks the API and fail at the call.
void report_newer_apis(CheckReport &r, const fs::path &dir, const std::vector<std::string> &files,
		const core::SemVer &lowest, const std::string &req_text) {
	std::map<std::string, core::SemVer> since;
	for (const ApiSince &a : kApiSince) {
		if (const auto v = core::parse_semver(a.since)) {
			since[a.member] = *v;
		}
	}
	for (const std::string &rel : files) {
		const LuaEnv env = environment_of(rel);
		if (env != LuaEnv::kServer && env != LuaEnv::kUi) {
			continue;
		}
		std::set<std::string> reported;
		for (const auto &[chain, line] : api_chains(read_file(dir / rel))) {
			// The longest documented prefix: `vb.world.raycast(...).x` -> vb.world.raycast.
			std::string member = chain;
			auto it = since.find(member);
			while (it == since.end() && member.find('.') != std::string::npos) {
				member.erase(member.rfind('.'));
				it = since.find(member);
			}
			if (it == since.end() || !(lowest < it->second) || !reported.insert(member).second) {
				continue;
			}
			add(r, Severity::kWarning, rel, line, member + " is new in " + core::to_string(it->second) + ", but engine_version_req \"" + req_text + "\" also admits " + core::to_string(lowest) + " (raise it to >=" + core::to_string(it->second) + ")");
		}
	}
}

// §3.4.1: the static per-environment global check.
struct Scanned {
	std::string rel;
	LuaEnv env;
	std::vector<script::GlobalAccess> accesses;
};

// Compiles every file (nothing runs) and collects its global accesses. Syntax errors are reported
// here; returns true when any file failed to compile.
bool scan_files(CheckReport &r, const fs::path &dir, const std::vector<std::string> &files,
		std::vector<Scanned> &scanned, std::map<LuaEnv, std::set<std::string>> &assigned) {
	bool syntax_error = false;
	for (const std::string &rel : files) {
		const script::GlobalScan scan = script::scan_globals(read_file(dir / rel), rel);
		if (!scan.ok) {
			syntax_error = true;
			static const std::regex head(R"(^[^:\n]+:(\d+): (.*)$)");
			std::smatch m;
			const std::string first = scan.error.substr(0, scan.error.find('\n'));
			if (std::regex_match(first, m, head)) {
				add(r, Severity::kError, rel, std::stoi(m[1].str()), m[2].str());
			} else {
				add(r, Severity::kError, rel, 0, first);
			}
			continue;
		}
		const LuaEnv env = environment_of(rel);
		for (const auto &a : scan.accesses) {
			if (a.is_write) {
				assigned[env].insert(a.name);
			}
		}
		scanned.push_back({ rel, env, scan.accesses });
	}
	return syntax_error;
}

// §3.4.1: judge each read against the file's environment. `assigned` holds the globals defined by
// any file of an environment, statically (`x = 1`) and at run time (`_G[name] = ...`, as content/base's
// blocks/register.lua does). `judge_unknown` is false for an environment whose load aborted, where an
// "unknown global" would only be a consequence of the abort.
void report_globals(CheckReport &r, const std::vector<Scanned> &scanned,
		std::map<LuaEnv, std::set<std::string>> &assigned, const std::map<LuaEnv, bool> &judge_unknown) {
	for (const Scanned &s : scanned) {
		std::set<std::pair<int, std::string>> seen;
		for (const auto &a : s.accesses) {
			if (a.is_write || kBuiltins.count(a.name) != 0 || !seen.insert({ a.line, a.name }).second) {
				continue;
			}
			const std::set<std::string> &mine = s.env == LuaEnv::kServer ? kServerGlobals
					: s.env == LuaEnv::kUi								 ? kUiGlobals
																		 : std::set<std::string>{};
			if (mine.count(a.name) != 0) {
				continue;
			}
			if (s.env == LuaEnv::kData && a.name == "vb") {
				add(r, Severity::kError, s.rel, a.line,
						"'vb' is not usable in a data script (data scripts must only return data; register from pack code)");
				continue;
			}
			if (kSandboxRemoved.count(a.name) != 0) {
				add(r, Severity::kError, s.rel, a.line,
						"'" + a.name + "' is not available: the Lua sandbox removes it (see sdk/lua/library/sandbox.lua)");
				continue;
			}
			const bool other_server = s.env != LuaEnv::kServer && kServerGlobals.count(a.name) != 0;
			const bool other_ui = s.env != LuaEnv::kUi && kUiGlobals.count(a.name) != 0;
			if (other_server || other_ui) {
				add(r, Severity::kError, s.rel, a.line,
						"'" + a.name + "' is not available in " + env_name(s.env) + " (" +
								(other_server ? "server-only" : "client-UI-only") + ")");
				continue;
			}
			if (assigned[s.env].count(a.name) != 0) {
				continue; // a global some file of this environment defines (e.g. base_ui in ui/_style.lua)
			}
			if (!judge_unknown.at(s.env)) {
				continue;
			}
			add(r, Severity::kWarning, s.rel, a.line, "unknown global '" + a.name + "' (typo? it is never assigned in " + env_name(s.env) + ")");
		}
	}
}

// A static environment error ("'vb' is not available in the client UI VM") and the run-time error it
// causes ("attempt to index a nil value (global 'vb')") land on the same line; keep the static one.
void dedupe_errors(CheckReport &r) {
	const auto is_static = [](const PackDiagnostic &d) {
		return d.message.find("is not available") != std::string::npos ||
				d.message.find("is not usable") != std::string::npos;
	};
	std::vector<PackDiagnostic> out;
	for (const PackDiagnostic &d : r.diagnostics) {
		bool dropped = false;
		if (d.severity == Severity::kError && !d.file.empty() && d.line > 0) {
			for (PackDiagnostic &kept : out) {
				if (kept.severity == Severity::kError && kept.file == d.file && kept.line == d.line) {
					if (is_static(d) && !is_static(kept)) {
						kept = d;
					}
					dropped = true;
					break;
				}
			}
		}
		if (!dropped) {
			out.push_back(d);
		}
	}
	r.diagnostics = std::move(out);
}

} // namespace

CheckReport check_pack(const fs::path &dir, const CheckOptions &options) {
	CheckReport r;
	r.pack_dir = dir.generic_string();
	r.engine_version = core::to_string(options.engine);
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) {
		add(r, Severity::kError, "", 0, "not a directory: " + dir.string());
		return r;
	}

	// 1. pack.toml + engine_version_req.
	const script::PackManifest manifest = script::read_pack_manifest(dir);
	r.pack_name = manifest.name.empty() ? dir.filename().string() : manifest.name;
	if (!manifest.found) {
		add(r, Severity::kError, "pack.toml", 0, "no pack.toml in " + dir.string());
	} else if (!manifest.error.empty()) {
		add(r, Severity::kError, "pack.toml", 0, manifest.error);
	} else {
		r.engine_req = manifest.engine_version_req.value_or("");
		const core::EngineReqCheck req = script::check_pack_engine_req(manifest, options.engine);
		if (!req.ok && !options.ignore_engine_req) {
			add(r, Severity::kError, "pack.toml", manifest.engine_version_req_line, req.message);
		} else if (req.warning) {
			add(r, Severity::kWarning, "pack.toml", 0, req.message);
		}
	}

	const std::vector<std::string> files = list_lua_files(dir);
	const std::set<std::string> known(files.begin(), files.end());

	// 2. Syntax + static global-access check (nothing is executed). A syntax error stops the
	// run-it phase below: it would only repeat the same error.
	std::vector<Scanned> scanned;
	std::map<LuaEnv, std::set<std::string>> assigned;
	const bool has_syntax_error = scan_files(r, dir, files, scanned, assigned);

	// 3. auth.lua.
	const auth::AuthLoad auth_load = auth::load_auth_lua(dir);
	if (auth_load.present && !auth_load.error.empty()) {
		add(r, Severity::kError, "auth.lua", 0, auth_load.error);
	}

	if (has_syntax_error) {
		report_globals(r, scanned, assigned, { { LuaEnv::kServer, false }, { LuaEnv::kUi, false }, { LuaEnv::kData, false }, { LuaEnv::kAuth, false } });
		return r;
	}
	std::map<LuaEnv, bool> judge = { { LuaEnv::kServer, true }, { LuaEnv::kUi, true }, { LuaEnv::kData, true }, { LuaEnv::kAuth, true } };

	// 4. Load the server-side pack like a real start.
	const fs::path scratch = fs::temp_directory_path() / ("vb_check_pack_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(scratch, ec);
	{
		net::LoopbackNetwork net;
		world::BlockRegistry registry = world::BlockRegistry::base();
		script::PackRuntime rt(net.server(), registry, scratch / "storage.json");
		rt.set_pack_modules(script::collect_requirable_modules(dir));
		rt.set_auth_required(auth_load.present && auth_load.error.empty());

		std::vector<std::string> order;
		const auto in_dir = [&](const char *sub) {
			const std::string prefix = std::string(sub) + "/";
			for (const auto &f : files) {
				if (f.rfind(prefix, 0) == 0 && f.find('/', prefix.size()) == std::string::npos) {
					order.push_back(f);
				}
			}
		};
		for (const char *sub : { "blocks", "entities", "biomes" }) {
			in_dir(sub);
		}
		for (const auto &f : files) {
			if (f.find('/') == std::string::npos && f != "init.lua" && f != "auth.lua") {
				order.push_back(f);
			}
		}
		if (known.count("init.lua") != 0) {
			order.push_back("init.lua");
		}

		// Pack `print` output would otherwise corrupt --json on stdout.
		(void)rt.load_pack_file("print = function(...) end", "=check-pack");
		(void)rt.load_pack_file(kRecordRegistrations, "=check-pack");
		bool load_failed = false;
		for (const auto &rel : order) {
			const script::ScriptResult res = rt.load_pack_file(read_file(dir / rel), rel);
			if (!res.ok) {
				add_lua_error(r, known, res.message, rel);
				load_failed = true;
				break; // later files depend on this one; the cascade would be noise
			}
		}
		judge[LuaEnv::kServer] = !load_failed;
		for (const auto &name : rt.global_names()) {
			assigned[LuaEnv::kServer].insert(name);
		}
		if (const auto dump = rt.eval_string("return __vb_check_dump()")) {
			report_registrations(r, dir, *dump, known);
		}

		if (!load_failed) {
			rt.freeze();
			if (const auto worldgen = rt.validate_worldgen(); !worldgen) {
				add(r, Severity::kError, "", 0, "invalid worldgen data: " + worldgen.message);
			} else {
				try {
					worldgen::WorldGenParams params;
					params.seed = 1;
					const auto pipeline = rt.build_worldgen_pipeline(params);
					const worldgen::WorldGenerator generator(params, registry, pipeline);
					world::Chunk chunk(core::ChunkCoord{ 0, 0, 0 });
					generator.generate(chunk);
				} catch (const std::exception &e) {
					add(r, Severity::kError, "", 0, std::string("worldgen failed on the first chunk: ") + e.what());
				}
			}
		}
	}

	// 5. Client UI scripts, in the client's own (sorted) order.
	{
		script::UiRuntime ui;
		(void)ui.load_pack_file("print = function(...) end", "=check-pack");
		for (const auto &rel : files) {
			if (environment_of(rel) != LuaEnv::kUi) {
				continue;
			}
			const script::ScriptResult res = ui.load_pack_file(read_file(dir / rel), rel);
			if (!res.ok) {
				add_lua_error(r, known, res.message, rel);
				judge[LuaEnv::kUi] = false;
			}
		}
		for (const auto &name : ui.global_names()) {
			assigned[LuaEnv::kUi].insert(name);
		}
	}
	report_globals(r, scanned, assigned, judge);
	if (manifest.found && manifest.error.empty() && manifest.engine_version_req) {
		if (const auto req = core::VersionReq::parse(*manifest.engine_version_req)) {
			if (const auto lowest = req->lower_bound()) {
				report_newer_apis(r, dir, files, *lowest, *manifest.engine_version_req);
			}
		}
	}
	dedupe_errors(r);
	fs::remove_all(scratch, ec);
	return r;
}

std::string format_human(const CheckReport &r, bool strict) {
	std::ostringstream out;
	for (const auto &d : r.diagnostics) {
		if (!d.file.empty()) {
			out << d.file;
			if (d.line > 0) {
				out << ':' << d.line;
			}
			out << ": ";
		}
		out << (d.severity == Severity::kError ? "error: " : "warning: ") << d.message << '\n';
	}
	out << (r.ok(strict) ? "ok" : "FAILED") << ": " << r.pack_name << " -- " << r.errors() << " error(s), "
		<< r.warnings() << " warning(s)" << (strict && r.warnings() > 0 ? " (--strict)" : "") << '\n';
	return out.str();
}

std::string format_json(const CheckReport &r, bool strict) {
	nlohmann::json j;
	j["ok"] = r.ok(strict);
	j["pack"] = { { "dir", r.pack_dir }, { "name", r.pack_name }, { "engine_version_req", r.engine_req } };
	j["engine"] = r.engine_version;
	j["errors"] = r.errors();
	j["warnings"] = r.warnings();
	j["diagnostics"] = nlohmann::json::array();
	for (const auto &d : r.diagnostics) {
		j["diagnostics"].push_back({ { "severity", d.severity == Severity::kError ? "error" : "warning" },
				{ "file", d.file }, { "line", d.line }, { "message", d.message } });
	}
	return j.dump(2);
}

} // namespace vb::server
