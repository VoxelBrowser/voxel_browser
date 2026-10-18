#include "vb/cli/pack.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "vb/cli/commands.hpp"
#include "vb/cli/embedded.hpp"
#include "vb/cli/instance.hpp"
#include "vb/cli/process.hpp"
#include "vb/core/version_req.hpp"
#include "vb/script/pack_manifest.hpp"

namespace vb::cli {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

int fail(std::ostream &err, const std::string &msg, const std::string &hint = {}) {
	err << "error: " << msg << "\n";
	if (!hint.empty()) {
		err << "hint: " << hint << "\n";
	}
	return kExitFailure;
}

int usage(std::ostream &err, const std::string &msg) {
	err << "error: " << msg << "\nhint: run `vb help pack`\n";
	return kExitUsage;
}

std::string read_text(const fs::path &p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

Status write_file(const fs::path &p, const std::string &bytes) {
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << bytes;
	if (!out) {
		return { "cannot write " + p.string() };
	}
	return {};
}

std::string replace_all(std::string s, const std::string &from, const std::string &to) {
	for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) {
		s.replace(pos, from.size(), to);
	}
	return s;
}

bool is_text_path(const std::string &p) {
	return p.size() < 4 || p.substr(p.size() - 4) != ".png";
}

// Pack id: [a-z][a-z0-9_]{0,31}.
std::string sanitize_name(const std::string &raw) {
	std::string out;
	for (const char c : raw) {
		const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		out += ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_') ? l : '_';
	}
	if (out.empty() || !(out[0] >= 'a' && out[0] <= 'z')) {
		out = "pack_" + out;
	}
	return out.substr(0, 32);
}

bool valid_name(const std::string &n) {
	return std::regex_match(n, std::regex("[a-z][a-z0-9_]{0,31}"));
}

// <root>/sdk/lua/library, or beside a linked build dir (<root>/../sdk/...).
std::optional<fs::path> sdk_library_dir(const fs::path &root) {
	for (const fs::path &base : { root, root.parent_path() }) {
		std::error_code ec;
		if (fs::is_directory(base / "sdk" / "lua" / "library", ec)) {
			return base / "sdk" / "lua" / "library";
		}
	}
	return std::nullopt;
}

std::optional<core::SemVer> entry_semver(const Entry &e) {
	if (e.kind != EntryKind::Release) {
		return std::nullopt;
	}
	return core::parse_semver(e.name);
}

// Version a new pack should require: the selected installed release, else this vb's own.
core::SemVer selected_version(const Layout &layout, const std::string &requested) {
	if (const auto e = resolve_entry(layout, requested)) {
		if (const auto v = entry_semver(*e)) {
			return *v;
		}
	}
	return core::engine_version();
}

} // namespace

std::optional<Entry> pick_entry_for_pack(const Layout &layout, const fs::path &pack_dir,
		const std::string &explicit_version, std::string *why) {
	const script::PackManifest manifest = script::read_pack_manifest(pack_dir);
	std::optional<core::VersionReq> req;
	std::string req_text = "*";
	if (manifest.found && manifest.engine_version_req) {
		req_text = *manifest.engine_version_req;
		std::string err;
		req = core::VersionReq::parse(req_text, &err);
		if (!req) {
			if (why) {
				*why = "invalid engine_version_req \"" + req_text + "\" in " + (pack_dir / "pack.toml").string() + ": " + err;
			}
			return std::nullopt;
		}
	}
	const auto satisfies = [&](const Entry &e) {
		if (!req) {
			return true;
		}
		const auto v = entry_semver(e);
		return !v || req->matches(*v); // a link's version is unknown: allowed (dev builds)
	};
	if (!explicit_version.empty()) {
		std::string w;
		const auto e = resolve_entry(layout, explicit_version, &w);
		if (!e) {
			if (why) {
				*why = w;
			}
			return std::nullopt;
		}
		if (!satisfies(*e)) {
			if (why) {
				*why = e->name + " does not satisfy the pack's engine_version_req " + req_text;
			}
			return std::nullopt;
		}
		return e;
	}
	const auto entries = list_entries(layout);
	for (const Entry &e : entries) { // the default first when it fits
		if (e.is_default && e.root_exists && satisfies(e)) {
			return e;
		}
	}
	for (const Entry &e : entries) { // releases are newest-first
		if (e.root_exists && satisfies(e)) {
			return e;
		}
	}
	if (why) {
		std::string hint = "no installed version satisfies engine_version_req " + req_text;
		if (req) {
			if (const auto lb = req->lower_bound()) {
				hint += "; run `vb install v" + core::to_string(*lb) + "`";
			}
		}
		*why = hint;
	}
	return std::nullopt;
}

Status pack_write_types(const Layout &layout, const fs::path &dir, const std::string &version, bool overwrite_luarc,
		std::vector<std::string> *written) {
	std::vector<EmbeddedEntry> stubs;
	if (const auto e = resolve_entry(layout, version)) {
		if (const auto sdk = sdk_library_dir(e->root)) {
			std::error_code ec;
			for (const auto &f : fs::directory_iterator(*sdk, ec)) {
				if (f.path().extension() == ".lua") {
					stubs.push_back({ f.path().filename().string(), read_text(f.path()) });
				}
			}
		}
	}
	if (stubs.empty()) {
		stubs = embedded_under("sdk/lua/library/");
	}
	if (stubs.empty()) {
		return { "no Lua stubs available" };
	}
	std::error_code ec;
	fs::remove_all(dir / ".vb" / "lua", ec);
	for (const auto &s : stubs) {
		if (const Status st = write_file(dir / ".vb" / "lua" / s.path, s.bytes); !st) {
			return st;
		}
		if (written) {
			written->push_back(".vb/lua/" + s.path);
		}
	}
	if (overwrite_luarc || !fs::exists(dir / ".luarc.json", ec)) {
		const auto rc = embedded_under("sdk/lua/luarc.template.json");
		if (rc.empty()) {
			return { "no .luarc.json template available" };
		}
		if (const Status st = write_file(dir / ".luarc.json", rc[0].bytes); !st) {
			return st;
		}
		if (written) {
			written->push_back(".luarc.json");
		}
	}
	// ui/ runs in the client UI VM: its own config sees only the ui/client stubs, so an editor
	// that opens ui/ as a workspace folder flags `vb` there (architecture_spec/dev-experience.md §3.4.1).
	if (fs::is_directory(dir / "ui", ec) && (overwrite_luarc || !fs::exists(dir / "ui" / ".luarc.json", ec))) {
		const auto rc = embedded_under("sdk/lua/luarc.ui.template.json");
		if (rc.empty()) {
			return { "no ui/.luarc.json template available" };
		}
		if (const Status st = write_file(dir / "ui" / ".luarc.json", rc[0].bytes); !st) {
			return st;
		}
		if (written) {
			written->push_back("ui/.luarc.json");
		}
	}
	return {};
}

PackInitResult pack_init(const Layout &layout, const PackInitOptions &opts) {
	PackInitResult r;
	std::error_code ec;
	const fs::path dir = fs::absolute(opts.dir, ec).lexically_normal();

	r.name = opts.name.empty() ? sanitize_name(dir.filename().empty() ? dir.parent_path().filename().string() : dir.filename().string())
							   : opts.name;
	if (!valid_name(r.name)) {
		r.status = { "invalid pack name '" + r.name + "' (use lowercase letters, digits and '_', starting with a letter, max 32)" };
		return r;
	}
	const core::SemVer selected = selected_version(layout, opts.version);
	r.engine_req = opts.engine_req.empty() ? ">=" + core::to_string(selected) : opts.engine_req;
	if (std::string perr; !core::VersionReq::parse(r.engine_req, &perr)) {
		r.status = { "invalid --engine-req \"" + r.engine_req + "\": " + perr };
		return r;
	}

	const std::set<std::string> known = { "minimal", "ui", "worldgen", "base" };
	if (known.count(opts.tmpl) == 0) {
		r.status = { "unknown template '" + opts.tmpl + "' (minimal, ui, worldgen, base)" };
		return r;
	}

	if (fs::is_directory(dir, ec) && !opts.force) {
		std::vector<std::string> existing;
		for (const auto &f : fs::directory_iterator(dir, ec)) {
			if (f.path().filename() != ".git") {
				existing.push_back(f.path().filename().string());
			}
		}
		if (!existing.empty()) {
			r.status = { dir.string() + " is not empty (" + existing.front() + (existing.size() > 1 ? ", ..." : "") +
				"); use --force to write into it" };
			return r;
		}
	}

	// Collect {relative path, bytes}.
	std::map<std::string, std::string> files;
	const auto subst = [&](const std::string &path, std::string text) {
		if (!is_text_path(path)) {
			return text;
		}
		text = replace_all(text, "{{name}}", r.name);
		text = replace_all(text, "{{version}}", "0.1.0");
		return replace_all(text, "{{engine_req}}", r.engine_req);
	};
	if (opts.tmpl == "base") {
		const auto entry = resolve_entry(layout, opts.version);
		const auto content = entry ? resolve_pack("builtin:base", entry->root) : std::nullopt;
		if (!content) {
			r.status = { "--template base needs an installed version with content/base (run `vb install`)" };
			return r;
		}
		for (auto it = fs::recursive_directory_iterator(*content, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
			const fs::path rel = fs::relative(it->path(), *content, ec);
			const std::string first = rel.begin()->string();
			if (first == "storage.json" || first == "db" || first == "world" || first.rfind('.', 0) == 0) {
				if (it->is_directory(ec)) {
					it.disable_recursion_pending();
				}
				continue;
			}
			if (it->is_regular_file(ec)) {
				files[rel.generic_string()] = read_text(it->path());
			}
		}
		std::string toml = files["pack.toml"];
		toml = std::regex_replace(toml, std::regex("(^|\n)name\\s*=\\s*\"[^\"]*\""), "$1name = \"" + r.name + "\"");
		toml = std::regex_replace(toml, std::regex("(^|\n)engine_version_req\\s*=\\s*\"[^\"]*\""),
				"$1engine_version_req = \"" + r.engine_req + "\"");
		files["pack.toml"] = toml;
		for (const auto &f : embedded_under("templates/pack/_common/")) {
			if (f.path != "pack.toml" && files.count(f.path) == 0) {
				files[f.path] = subst(f.path, f.bytes);
			}
		}
	} else {
		for (const char *group : { "_common", opts.tmpl.c_str() }) {
			for (const auto &f : embedded_under(std::string("templates/pack/") + group + "/")) {
				files[f.path] = subst(f.path, f.bytes);
			}
		}
	}

	for (const auto &[rel, bytes] : files) {
		if (opts.force && fs::exists(dir / rel, ec) && rel == "pack.toml") {
			// --force still rewrites pack.toml: the caller asked for this name/requirement.
		}
		if (const Status st = write_file(dir / rel, bytes); !st) {
			r.status = st;
			return r;
		}
		r.created.push_back(rel);
	}
	if (const Status st = pack_write_types(layout, dir, opts.version, true, &r.created); !st) {
		r.status = st;
		return r;
	}
	std::sort(r.created.begin(), r.created.end());
	return r;
}

namespace {

struct Args {
	std::vector<std::string> positional;
	std::map<std::string, std::string> values;
	std::set<std::string> flags;
	std::string error;
};

Args parse(const std::vector<std::string> &raw, const std::set<std::string> &value_opts, const std::set<std::string> &flag_opts) {
	Args a;
	for (std::size_t i = 0; i < raw.size(); ++i) {
		const std::string &t = raw[i];
		const std::size_t eq = t.find('=');
		const std::string key = (t.rfind("--", 0) == 0 && eq != std::string::npos) ? t.substr(0, eq) : t;
		if (value_opts.count(key) != 0) {
			if (key.size() != t.size()) {
				a.values[key] = t.substr(eq + 1);
			} else if (i + 1 < raw.size()) {
				a.values[key] = raw[++i];
			} else {
				a.error = key + " needs a value";
				return a;
			}
		} else if (flag_opts.count(t) != 0) {
			a.flags.insert(t);
		} else if (!t.empty() && t[0] == '-') {
			a.error = "unknown option '" + t + "'";
			return a;
		} else {
			a.positional.push_back(t);
		}
	}
	if (a.positional.size() > 1) {
		a.error = "unexpected argument '" + a.positional[1] + "'";
	}
	return a;
}

std::string value_or(const Args &a, const std::string &k, const std::string &d = {}) {
	const auto it = a.values.find(k);
	return it == a.values.end() ? d : it->second;
}

fs::path dir_arg(const Args &a) {
	return a.positional.empty() ? fs::path(".") : fs::path(a.positional[0]);
}

int cmd_init(const Layout &layout, std::ostream &out, std::ostream &err, const std::vector<std::string> &raw) {
	const Args a = parse(raw, { "--template", "--name", "--engine-req", "--version" }, { "--force", "--json" });
	if (!a.error.empty()) {
		return usage(err, a.error);
	}
	PackInitOptions o;
	o.dir = dir_arg(a);
	o.tmpl = value_or(a, "--template", "minimal");
	o.name = value_or(a, "--name");
	o.engine_req = value_or(a, "--engine-req");
	o.version = value_or(a, "--version");
	o.force = a.flags.count("--force") != 0;
	const PackInitResult r = pack_init(layout, o);
	if (!r.status) {
		return fail(err, r.status.error);
	}
	std::error_code ec;
	const fs::path abs = fs::absolute(o.dir, ec).lexically_normal();
	if (a.flags.count("--json") != 0) {
		out << json{ { "dir", abs.generic_string() }, { "name", r.name }, { "template", o.tmpl },
			{ "engine_version_req", r.engine_req }, { "created", r.created } }
						.dump(2)
			<< "\n";
		return kExitOk;
	}
	out << "created pack '" << r.name << "' (" << o.tmpl << ") in " << abs.string() << "\n";
	for (const auto &f : r.created) {
		out << "  " << f << "\n";
	}
	const std::string cd = a.positional.empty() ? "" : "cd " + a.positional[0] + " && ";
	out << "\nnext:\n  " << cd << "vb pack check\n  " << cd << "vb pack dev\n";
	return kExitOk;
}

int cmd_types(const Layout &layout, std::ostream &out, std::ostream &err, const std::vector<std::string> &raw) {
	const Args a = parse(raw, { "--version" }, { "--force", "--json" });
	if (!a.error.empty()) {
		return usage(err, a.error);
	}
	const fs::path dir = dir_arg(a);
	if (!fs::exists(dir / "pack.toml")) {
		return fail(err, "no pack.toml in " + dir.string(), "run `vb pack init` first");
	}
	std::vector<std::string> written;
	if (const Status st = pack_write_types(layout, dir, value_or(a, "--version"), a.flags.count("--force") != 0, &written); !st) {
		return fail(err, st.error);
	}
	if (a.flags.count("--json") != 0) {
		out << json{ { "written", written } }.dump(2) << "\n";
	} else {
		out << "wrote " << written.size() << " file(s) under " << dir.string() << "\n";
	}
	return kExitOk;
}

int cmd_info(const Layout &layout, std::ostream &out, std::ostream &err, const std::vector<std::string> &raw) {
	const Args a = parse(raw, {}, { "--json" });
	if (!a.error.empty()) {
		return usage(err, a.error);
	}
	const fs::path dir = dir_arg(a);
	const script::PackManifest m = script::read_pack_manifest(dir);
	if (!m.found) {
		return fail(err, "no pack.toml in " + dir.string(), "run `vb pack init` to create a pack");
	}
	if (!m.error.empty()) {
		return fail(err, m.error);
	}
	std::map<std::string, int> counts;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
		const fs::path rel = fs::relative(it->path(), dir, ec);
		if (rel.begin()->string().rfind('.', 0) == 0) {
			if (it->is_directory(ec)) {
				it.disable_recursion_pending();
			}
			continue;
		}
		if (!it->is_regular_file(ec)) {
			continue;
		}
		const std::string top = std::next(rel.begin()) == rel.end() ? "(root)" : rel.begin()->string();
		const std::string ext = it->path().extension().string();
		if (ext == ".lua") {
			++counts["lua:" + top];
		} else if (ext == ".png") {
			++counts["textures"];
		}
	}
	std::vector<std::string> satisfied;
	std::string req_error;
	std::optional<core::VersionReq> req;
	if (m.engine_version_req) {
		req = core::VersionReq::parse(*m.engine_version_req, &req_error);
	}
	for (const Entry &e : list_entries(layout)) {
		if (const auto v = entry_semver(e); v && (!req || req->matches(*v))) {
			satisfied.push_back(e.name);
		}
	}
	json files = json::object();
	for (const auto &[k, v] : counts) {
		files[k] = v;
	}
	if (a.flags.count("--json") != 0) {
		out << json{ { "name", m.name }, { "version", m.version }, { "entry", m.entry },
			{ "engine_version_req", m.engine_version_req ? json(*m.engine_version_req) : json(nullptr) },
			{ "engine_version_req_valid", !m.engine_version_req || req.has_value() },
			{ "files", files }, { "satisfying_versions", satisfied } }
						.dump(2)
			<< "\n";
		return kExitOk;
	}
	out << "name:                " << m.name << "\nversion:             " << m.version << "\nengine_version_req:  "
		<< (m.engine_version_req ? *m.engine_version_req : "(none: treated as \"*\")") << (req || !m.engine_version_req ? "" : "  [INVALID: " + req_error + "]")
		<< "\ninstalled versions that satisfy it:";
	if (satisfied.empty()) {
		out << " none";
	}
	for (const auto &s : satisfied) {
		out << " " << s;
	}
	out << "\nfiles:\n";
	for (const auto &[k, v] : counts) {
		out << "  " << k << ": " << v << "\n";
	}
	return kExitOk;
}

int cmd_check(const Layout &layout, std::ostream &out, std::ostream &err, const std::vector<std::string> &raw) {
	const Args a = parse(raw, { "--version" }, { "--json", "--strict" });
	if (!a.error.empty()) {
		return usage(err, a.error);
	}
	const fs::path dir = dir_arg(a);
	std::string why;
	const auto entry = pick_entry_for_pack(layout, dir, value_or(a, "--version"), &why);
	if (!entry) {
		return fail(err, why);
	}
	const auto server = find_binary(*entry, Binary::Server);
	if (!server) {
		return fail(err, entry->name + " has no " + binary_file_name(Binary::Server));
	}
	std::vector<std::string> args = { "--check-pack", dir.string() };
	if (a.flags.count("--json") != 0) {
		args.push_back("--json");
	}
	if (a.flags.count("--strict") != 0) {
		args.push_back("--strict");
	}
	out << std::flush;
	const RunResult r = run_foreground(*server, args);
	if (!r.error.empty()) {
		return fail(err, r.error);
	}
	return r.exit_code == 0 ? kExitOk : kExitFailure;
}

int cmd_dev(const Layout &layout, std::ostream &out, std::ostream &err, const std::vector<std::string> &raw,
		const PackHooks &hooks) {
	const Args a = parse(raw, { "--version", "--port" }, { "--no-client" });
	if (!a.error.empty()) {
		return usage(err, a.error);
	}
	const fs::path dir = dir_arg(a);
	if (!fs::exists(dir / "pack.toml")) {
		return fail(err, "no pack.toml in " + dir.string(), "run `vb pack init` first");
	}
	std::string why;
	const auto entry = pick_entry_for_pack(layout, dir, value_or(a, "--version"), &why);
	if (!entry) {
		return fail(err, why);
	}
	if (!hooks.host) {
		return fail(err, "hosting is not available here");
	}
	const std::string port = value_or(a, "--port", "7777");
	std::error_code ec;
	const std::string pack = fs::absolute(dir, ec).lexically_normal().string();
	std::thread client;
	if (a.flags.count("--no-client") == 0 && hooks.launch) {
		client = std::thread([&] {
			std::this_thread::sleep_for(std::chrono::seconds(3)); // let the server come up
			hooks.launch({ "--version", entry->name, "--connect", "localhost:" + port });
		});
		client.detach();
	}
	out << "hosting " << pack << " on " << entry->name << " (port " << port << ", restarts on save; Ctrl+C stops)\n"
		<< std::flush;
	return hooks.host({ "--watch", "--version", entry->name, "--pack", pack, "--port", port });
}

} // namespace

int run_pack_command(const Layout &layout, std::ostream &out, std::ostream &err, const std::vector<std::string> &args,
		const PackHooks &hooks) {
	if (args.empty()) {
		return usage(err, "usage: vb pack <init|check|dev|types|info> ...");
	}
	const std::vector<std::string> rest(args.begin() + 1, args.end());
	if (args[0] == "init") {
		return cmd_init(layout, out, err, rest);
	}
	if (args[0] == "check") {
		return cmd_check(layout, out, err, rest);
	}
	if (args[0] == "dev") {
		return cmd_dev(layout, out, err, rest, hooks);
	}
	if (args[0] == "types") {
		return cmd_types(layout, out, err, rest);
	}
	if (args[0] == "info") {
		return cmd_info(layout, out, err, rest);
	}
	return usage(err, "unknown pack command '" + args[0] + "'");
}

} // namespace vb::cli
