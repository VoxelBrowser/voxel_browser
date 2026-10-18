#include "vb/auth/config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <system_error>
#include <variant>

#include "vb/script/vm.hpp"
#if VB_WITH_LUA
#include "vb/script/vm_internal.hpp"
#endif

namespace vb::auth {

std::string_view provider_name(Provider p) {
	switch (p) {
		case Provider::kOidc:
			return "oidc";
		case Provider::kKeycloak:
			return "keycloak";
		case Provider::kFirebase:
			return "firebase";
	}
	return "unknown";
}

namespace {

constexpr std::size_t kMaxSourceBytes = 16 * 1024;

// What auth.lua's returned table is flattened to before validation. Only
// these three shapes are accepted from Lua, so a function/userdata/nested
// table anywhere is rejected at extraction with the key named.
using RawArray = std::vector<std::string>;
using RawValue = std::variant<std::string, std::int64_t, RawArray>;
using RawTable = std::map<std::string, RawValue>;

AuthLoad fail(std::string message) {
	AuthLoad out;
	out.present = true;
	out.error = "auth.lua: " + std::move(message);
	return out;
}

#if VB_WITH_LUA

// Returns an empty string on success, else the error.
std::string extract_table(std::string_view source, RawTable &out) {
	script::VmLimits limits;
	limits.memory_bytes = 8u * 1024u * 1024u;
	limits.instruction_budget = 200'000;
	limits.wall_clock_budget_ms = 100;
	script::Vm vm(limits);
	vm.begin_call_budget();
	sol::state &lua = vm.native_impl().lua;

	sol::load_result loaded = lua.load(source, "auth.lua", sol::load_mode::text);
	if (!loaded.valid()) {
		sol::error err = loaded;
		return err.what();
	}
	sol::protected_function fn = loaded;
	sol::protected_function_result ran = fn();
	if (!ran.valid()) {
		sol::error err = ran;
		const std::string msg = err.what();
		if (msg.find("budget-exceeded") != std::string::npos) {
			return "ran too long (auth.lua is declarative data: no loops or heavy work)";
		}
		return msg;
	}
	if (ran.return_count() < 1 || ran.get_type(0) != sol::type::table) {
		return "must `return { ... }` a table";
	}
	sol::table tbl = ran.get<sol::table>(0);

	// lua_next iteration only (no __index/__len metamethods consulted).
	for (const auto &kv : tbl) {
		if (kv.first.get_type() != sol::type::string) {
			return "keys must be strings (found a non-string key)";
		}
		const std::string key = kv.first.as<std::string>();
		const sol::object &val = kv.second;
		switch (val.get_type()) {
			case sol::type::string:
				out.emplace(key, val.as<std::string>());
				break;
			case sol::type::number: {
				const double d = val.as<double>();
				if (!std::isfinite(d) || d != std::floor(d) || std::fabs(d) > 1e12) {
					return "'" + key + "' must be a whole number";
				}
				out.emplace(key, static_cast<std::int64_t>(d));
				break;
			}
			case sol::type::table: {
				sol::table arr = val.as<sol::table>();
				std::map<std::int64_t, std::string> items;
				for (const auto &item : arr) {
					if (item.first.get_type() != sol::type::number ||
							item.second.get_type() != sol::type::string) {
						return "'" + key + "' must be a list of strings";
					}
					const double idx = item.first.as<double>();
					if (idx < 1 || idx != std::floor(idx) || idx > 1024) {
						return "'" + key + "' must be a plain list of strings";
					}
					items.emplace(static_cast<std::int64_t>(idx), item.second.as<std::string>());
				}
				RawArray list;
				std::int64_t expect = 1;
				for (auto &[idx, s] : items) {
					if (idx != expect++) {
						return "'" + key + "' must be a plain list of strings (no gaps)";
					}
					list.push_back(std::move(s));
				}
				out.emplace(key, std::move(list));
				break;
			}
			default:
				return "'" + key + "' has an unsupported value type (only strings, "
								   "whole numbers and lists of strings are allowed)";
		}
	}
	return {};
}

#else

std::string extract_table(std::string_view, RawTable &) {
	return "this build has no Lua scripting (VB_WITH_LUA), so auth.lua cannot be read";
}

#endif

bool printable(std::string_view s) {
	return std::all_of(s.begin(), s.end(), [](char c) {
		return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f;
	});
}

bool token_chars(std::string_view s) {
	return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
		const auto u = static_cast<unsigned char>(c);
		return u > 0x20 && u < 0x7f && c != '"' && c != '\\';
	});
}

// Splits "scheme://host[:port][/path]". Returns false on anything the
// server shouldn't trust as an issuer (userinfo, query, fragment, spaces).
bool parse_issuer(std::string_view url, std::string &scheme, std::string &host) {
	const auto sep = url.find("://");
	if (sep == std::string_view::npos) {
		return false;
	}
	scheme = std::string(url.substr(0, sep));
	std::string_view rest = url.substr(sep + 3);
	if (!token_chars(url) || url.find_first_of("?#@") != std::string_view::npos) {
		return false;
	}
	const auto slash = rest.find('/');
	std::string_view authority = rest.substr(0, slash);
	const auto colon = authority.find(':');
	host = std::string(authority.substr(0, colon));
	if (colon != std::string_view::npos) {
		const std::string_view port = authority.substr(colon + 1);
		if (port.empty() || port.size() > 5 ||
				!std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
			return false;
		}
	}
	return !host.empty();
}

std::string check_issuer(const std::string &issuer, std::string &host) {
	std::string scheme;
	if (!parse_issuer(issuer, scheme, host)) {
		return "'issuer' is not a valid URL (expected https://host[/path], no "
			   "credentials, query or fragment)";
	}
	if (scheme == "https") {
		return {};
	}
	if (scheme == "http" && (host == "127.0.0.1" || host == "localhost")) {
		return {}; // development only: a local IdP
	}
	return "'issuer' must use https (http is only allowed for 127.0.0.1/localhost)";
}

class Reader {
public:
	explicit Reader(RawTable &table) :
			table_(table) {}

	// Optional string; nullopt when absent. Sets error_ on a type mismatch.
	std::optional<std::string> str(const std::string &key, std::size_t max_len) {
		const auto it = table_.find(key);
		if (it == table_.end()) {
			return std::nullopt;
		}
		const auto *s = std::get_if<std::string>(&it->second);
		if (s == nullptr) {
			error_ = "'" + key + "' must be a string";
			return std::nullopt;
		}
		if (s->empty() || s->size() > max_len || !printable(*s)) {
			error_ = "'" + key + "' must be 1.." + std::to_string(max_len) +
					" printable characters";
			return std::nullopt;
		}
		consumed_.push_back(key);
		return *s;
	}

	std::optional<std::vector<std::string>> list(const std::string &key, std::size_t max_items,
			std::size_t max_len) {
		const auto it = table_.find(key);
		if (it == table_.end()) {
			return std::nullopt;
		}
		const auto *a = std::get_if<RawArray>(&it->second);
		if (a == nullptr) {
			error_ = "'" + key + "' must be a list of strings";
			return std::nullopt;
		}
		if (a->empty() || a->size() > max_items) {
			error_ = "'" + key + "' must have 1.." + std::to_string(max_items) + " entries";
			return std::nullopt;
		}
		for (const auto &s : *a) {
			if (s.size() > max_len || !token_chars(s)) {
				error_ = "'" + key + "' has an invalid entry";
				return std::nullopt;
			}
		}
		std::vector<std::string> sorted = *a;
		std::sort(sorted.begin(), sorted.end());
		if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
			error_ = "'" + key + "' has a duplicate entry";
			return std::nullopt;
		}
		consumed_.push_back(key);
		return *a;
	}

	std::optional<std::uint32_t> integer(const std::string &key, std::int64_t lo, std::int64_t hi,
			bool allow_zero = false) {
		const auto it = table_.find(key);
		if (it == table_.end()) {
			return std::nullopt;
		}
		const auto *n = std::get_if<std::int64_t>(&it->second);
		if (n == nullptr) {
			error_ = "'" + key + "' must be a whole number";
			return std::nullopt;
		}
		if (!((allow_zero && *n == 0) || (*n >= lo && *n <= hi))) {
			error_ = "'" + key + "' must be " + (allow_zero ? "0 or " : "") + "between " +
					std::to_string(lo) + " and " + std::to_string(hi);
			return std::nullopt;
		}
		consumed_.push_back(key);
		return static_cast<std::uint32_t>(*n);
	}

	bool failed() const { return !error_.empty(); }
	const std::string &error() const { return error_; }

	// Any key never consumed is unknown for this provider.
	std::string first_unknown() const {
		for (const auto &[k, v] : table_) {
			(void)v;
			if (std::find(consumed_.begin(), consumed_.end(), k) == consumed_.end()) {
				return k;
			}
		}
		return {};
	}

private:
	RawTable &table_;
	std::string error_;
	std::vector<std::string> consumed_;
};

AuthLoad validate(RawTable &raw, const AuthOverrides &ov) {
	AuthConfig cfg;

	{
		const auto it = raw.find("provider");
		if (it == raw.end()) {
			return fail("missing required key 'provider' (\"oidc\", \"keycloak\" or \"firebase\")");
		}
		const auto *name = std::get_if<std::string>(&it->second);
		if (name == nullptr) {
			return fail("'provider' must be a string");
		}
		if (*name == "oidc") {
			cfg.provider = Provider::kOidc;
		} else if (*name == "keycloak") {
			cfg.provider = Provider::kKeycloak;
		} else if (*name == "firebase") {
			cfg.provider = Provider::kFirebase;
		} else {
			return fail("unknown provider '" + *name + "' (expected oidc, keycloak or firebase)");
		}
	}
	const bool firebase = cfg.provider == Provider::kFirebase;

	// server.toml [auth] overrides replace deployment-specific values only.
	const auto apply = [&](const std::string &key, const std::string &value,
							   bool applies) -> std::string {
		if (value.empty()) {
			return {};
		}
		if (!applies) {
			return "server.toml [auth] '" + key + "' does not apply to provider '" +
					std::string(provider_name(cfg.provider)) + "'";
		}
		raw[key] = value;
		return {};
	};
	for (const auto &err : { apply("issuer", ov.issuer, !firebase),
				 apply("client_id", ov.client_id, !firebase),
				 apply("project_id", ov.project_id, firebase),
				 apply("api_key", ov.api_key, firebase) }) {
		if (!err.empty()) {
			return fail(err);
		}
	}

	Reader r(raw);
	(void)r.str("provider", 32); // consumed above; marks the key as known
	const auto require = [&](const std::optional<std::string> &v,
								 const char *key) -> std::string {
		if (r.failed()) {
			return r.error();
		}
		if (!v) {
			return std::string("missing required key '") + key + "'";
		}
		return {};
	};

	std::string host_for_default;
	if (firebase) {
		const auto project = r.str("project_id", 128);
		const auto api_key = r.str("api_key", 256);
		if (auto e = require(project, "project_id"); !e.empty()) {
			return fail(e);
		}
		if (auto e = require(api_key, "api_key"); !e.empty()) {
			return fail(e);
		}
		const bool id_ok = std::all_of(project->begin(), project->end(), [](char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' ||
					c == '.';
		});
		if (!id_ok || !token_chars(*api_key)) {
			return fail("'project_id' or 'api_key' contains invalid characters");
		}
		cfg.project_id = *project;
		cfg.api_key = *api_key;
		cfg.issuer = "https://securetoken.google.com/" + cfg.project_id;
		cfg.client_id = cfg.project_id;
		host_for_default = cfg.project_id;
		cfg.name_claim = "name";
		cfg.sign_in = { FirebaseSignIn::kPassword };
		if (const auto methods = r.list("sign_in", 2, 16)) {
			cfg.sign_in.clear();
			for (const auto &m : *methods) {
				if (m == "password") {
					cfg.sign_in.push_back(FirebaseSignIn::kPassword);
				} else if (m == "google") {
					cfg.sign_in.push_back(FirebaseSignIn::kGoogle);
				} else {
					return fail("unknown sign_in method '" + m + "' (expected password or google)");
				}
			}
		}
		const auto google_id = r.str("google_client_id", 256);
		const auto google_secret = r.str("google_client_secret", 256);
		if (r.failed()) {
			return fail(r.error());
		}
		const bool google = std::find(cfg.sign_in.begin(), cfg.sign_in.end(), FirebaseSignIn::kGoogle) != cfg.sign_in.end();
		if (google && !google_id) {
			return fail("sign_in includes \"google\" but 'google_client_id' is missing (the client id of a "
						"Google Cloud OAuth client of type \"Desktop app\")");
		}
		if ((google_id && !token_chars(*google_id)) || (google_secret && !token_chars(*google_secret))) {
			return fail("'google_client_id' or 'google_client_secret' contains invalid characters");
		}
		if (google_secret && !google_id) {
			return fail("'google_client_secret' needs 'google_client_id'");
		}
		cfg.google_client_id = google_id.value_or(std::string());
		cfg.google_client_secret = google_secret.value_or(std::string());
	} else {
		const auto issuer = r.str("issuer", 256);
		const auto client_id = r.str("client_id", 256);
		if (auto e = require(issuer, "issuer"); !e.empty()) {
			return fail(e);
		}
		if (auto e = require(client_id, "client_id"); !e.empty()) {
			return fail(e);
		}
		if (auto e = check_issuer(*issuer, host_for_default); !e.empty()) {
			return fail(e);
		}
		if (!token_chars(*client_id)) {
			return fail("'client_id' contains invalid characters");
		}
		cfg.issuer = *issuer;
		cfg.client_id = *client_id;
		cfg.name_claim = "preferred_username";
		cfg.scopes = { "openid", "profile" };
		if (auto scopes = r.list("scopes", 16, 64)) {
			cfg.scopes = std::move(*scopes);
			if (std::find(cfg.scopes.begin(), cfg.scopes.end(), "openid") == cfg.scopes.end()) {
				return fail("'scopes' must include \"openid\"");
			}
		}
	}

	cfg.display_name = host_for_default;
	if (auto v = r.str("display_name", 64)) {
		cfg.display_name = *v;
	}
	if (auto v = r.str("name_claim", 64)) {
		if (!token_chars(*v)) {
			return fail("'name_claim' contains invalid characters");
		}
		cfg.name_claim = *v;
	}
	if (auto v = r.list("claims", 32, 64)) {
		cfg.claims = std::move(*v);
	}
	if (auto v = r.integer("max_token_age_seconds", 30, 86400)) {
		cfg.max_token_age_seconds = *v;
	}
	if (auto v = r.integer("reauth_interval_seconds", 60, 86400, /*allow_zero=*/true)) {
		cfg.reauth_interval_seconds = *v;
	}
	if (auto v = r.integer("reauth_grace_seconds", 10, 3600)) {
		cfg.reauth_grace_seconds = *v;
	}
	if (r.failed()) {
		return fail(r.error());
	}
	if (const std::string unknown = r.first_unknown(); !unknown.empty()) {
		return fail("unknown key '" + unknown + "' for provider '" +
				std::string(provider_name(cfg.provider)) + "'");
	}

	AuthLoad out;
	out.present = true;
	out.config = std::move(cfg);
	return out;
}

} // namespace

AuthLoad parse_auth_lua(std::string_view source, const AuthOverrides &overrides) {
	if (source.size() > kMaxSourceBytes) {
		return fail("file is larger than " + std::to_string(kMaxSourceBytes) + " bytes");
	}
	RawTable raw;
	if (const std::string err = extract_table(source, raw); !err.empty()) {
		return fail(err);
	}
	return validate(raw, overrides);
}

AuthLoad load_auth_lua(const std::filesystem::path &pack_dir, const AuthOverrides &overrides) {
	const std::filesystem::path path = pack_dir / std::string(kAuthLuaFilename);
	std::error_code ec;
	if (!std::filesystem::exists(path, ec) || ec) {
		return {}; // no auth.lua => authentication off
	}
	// Anything at that path that isn't a readable regular file is a broken
	// declaration, not an absent one: fail closed.
	if (!std::filesystem::is_regular_file(path, ec) || ec) {
		return fail("'" + path.string() + "' exists but is not a regular file");
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return fail("could not read '" + path.string() + "'");
	}
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return parse_auth_lua(text, overrides);
}

std::string describe(const AuthConfig &c) {
	const auto join = [](const std::vector<std::string> &v) {
		std::string s;
		for (const auto &e : v) {
			s += (s.empty() ? "" : ",") + e;
		}
		return s;
	};
	std::ostringstream o;
	o << "provider=" << provider_name(c.provider) << " display_name=\"" << c.display_name
	  << "\" issuer=" << c.issuer << " client_id=" << c.client_id;
	if (c.provider == Provider::kFirebase) {
		o << " api_key=<redacted> sign_in=";
		for (std::size_t i = 0; i < c.sign_in.size(); ++i) {
			o << (i ? "," : "") << (c.sign_in[i] == FirebaseSignIn::kPassword ? "password" : "google");
		}
		if (!c.google_client_id.empty()) {
			o << " google_client_id=" << c.google_client_id;
		}
	} else {
		o << " scopes=" << join(c.scopes);
	}
	o << " name_claim=" << c.name_claim << " claims=" << (c.claims.empty() ? "-" : join(c.claims))
	  << " max_token_age=" << c.max_token_age_seconds << "s reauth_interval="
	  << c.reauth_interval_seconds << "s reauth_grace=" << c.reauth_grace_seconds << 's';
	return o.str();
}

} // namespace vb::auth
