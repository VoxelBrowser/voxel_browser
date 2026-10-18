#include "vb/auth/session_store.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "vb/auth/crypto.hpp"
#include "vb/auth/jwt.hpp"
#include "vb/auth/secret_store.hpp"

namespace vb::auth {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

std::string hex(const std::array<std::uint8_t, 32> &h) {
	static const char *d = "0123456789abcdef";
	std::string out;
	for (const auto b : h) {
		out.push_back(d[b >> 4]);
		out.push_back(d[b & 0xF]);
	}
	return out;
}

std::string get(const json &o, const char *k) {
	auto it = o.find(k);
	return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Writes via a 0600 temp file + rename so a crash never leaves a half-written
// or world-readable credential.
bool write_private(const fs::path &path, const std::string &data) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	const fs::path tmp = path.string() + ".tmp";
	{
		std::ofstream create(tmp, std::ios::binary | std::ios::trunc);
		if (!create) {
			return false;
		}
	}
	fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
			fs::perm_options::replace, ec);
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) {
			return false;
		}
		out << data;
		if (!out.good()) {
			return false;
		}
	}
	fs::rename(tmp, path, ec);
	return !ec;
}

nlohmann::json read_json(const fs::path &p) {
	std::ifstream in(p, std::ios::binary);
	if (!in) {
		return nullptr;
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return json::parse(text, nullptr, false);
}

// The keychain entry of a session file: unique per file, so two config
// directories (two VB_HOMEs, a test's temp dir) never share one.
std::string account_for(const fs::path &p) {
	std::error_code ec;
	fs::path abs = fs::absolute(p, ec);
	if (ec) {
		abs = p;
	}
	return hex(sha256(abs.lexically_normal().string()));
}

// The marker a file carries when its token is in the keychain.
constexpr const char *kSecretInKeychain = "os";

bool is_session_file(const fs::path &p) {
	return p.extension() == ".json" && p.filename() != "trust.json";
}

} // namespace

SessionStore::SessionStore(fs::path dir, std::shared_ptr<SecretBackend> secrets) :
		dir_(std::move(dir)), secrets_(std::move(secrets)) {
	// Saved logins used to be per (issuer, client_id) and shared by every
	// server on that provider. Those files have no "server" field; remove them
	// so their refresh tokens don't linger. Tokens still in plaintext move to
	// the keychain when there is one.
	std::error_code ec;
	for (const auto &e : fs::directory_iterator(dir_, ec)) {
		if (!is_session_file(e.path())) {
			continue;
		}
		const json doc = read_json(e.path());
		if (!doc.is_object() || get(doc, "server").empty()) {
			fs::remove(e.path(), ec);
			continue;
		}
		if (secrets_ && !get(doc, "refresh_token").empty()) {
			if (auto s = read(e.path())) {
				save(*s);
			}
		}
	}
}

std::string_view SessionStore::secrets_name() const {
	return secrets_ ? secrets_->name() : std::string_view("files");
}

std::optional<StoredSession> SessionStore::read(const fs::path &p) const {
	const json doc = read_json(p);
	if (!doc.is_object()) {
		return std::nullopt;
	}
	StoredSession s;
	s.server = get(doc, "server");
	s.issuer = get(doc, "issuer");
	s.client_id = get(doc, "client_id");
	s.provider = get(doc, "provider");
	s.api_key = get(doc, "api_key");
	s.refresh_token = get(doc, "refresh_token");
	s.label = get(doc, "label");
	if (s.refresh_token.empty() && get(doc, "secret") == kSecretInKeychain && secrets_) {
		s.refresh_token = secrets_->get(account_for(p)).value_or(std::string());
	}
	if (s.server.empty() || s.issuer.empty() || s.refresh_token.empty()) {
		return std::nullopt;
	}
	return s;
}

void SessionStore::forget(const fs::path &p) {
	if (secrets_ && get(read_json(p), "secret") == kSecretInKeychain) {
		secrets_->erase(account_for(p));
	}
	std::error_code ec;
	fs::remove(p, ec);
}

fs::path SessionStore::path_for(const std::string &server, const std::string &issuer,
		const std::string &client_id) const {
	return dir_ / (hex(sha256(server + "|" + issuer + "|" + client_id)) + ".json");
}

std::optional<StoredSession> SessionStore::load(const std::string &server,
		const std::string &issuer, const std::string &client_id) const {
	auto s = read(path_for(server, issuer, client_id));
	// The file name is a hash, so confirm it really is this entry.
	if (s && (s->server != server || s->issuer != issuer || s->client_id != client_id)) {
		return std::nullopt;
	}
	return s;
}

bool SessionStore::save(const StoredSession &s) {
	const fs::path path = path_for(s.server, s.issuer, s.client_id);
	json doc = { { "server", s.server }, { "issuer", s.issuer },
		{ "client_id", s.client_id }, { "provider", s.provider }, { "api_key", s.api_key },
		{ "label", s.label } };
	if (secrets_ && secrets_->set(account_for(path), s.refresh_token)) {
		doc["secret"] = kSecretInKeychain;
	} else {
		if (secrets_) {
			secrets_->erase(account_for(path)); // never leave an older token behind
		}
		doc["refresh_token"] = s.refresh_token;
	}
	return write_private(path, doc.dump());
}

void SessionStore::erase(const std::string &server, const std::string &issuer,
		const std::string &client_id) {
	forget(path_for(server, issuer, client_id));
}

std::vector<StoredSession> SessionStore::list_for(const std::string &server) const {
	std::vector<StoredSession> out;
	for (auto &s : list()) {
		if (s.server == server) {
			out.push_back(std::move(s));
		}
	}
	return out;
}

void SessionStore::sign_out(const std::string &server) {
	for (const auto &s : list_for(server)) {
		erase(s.server, s.issuer, s.client_id);
	}
}

std::vector<StoredSession> SessionStore::list() const {
	std::vector<StoredSession> out;
	std::error_code ec;
	for (const auto &e : fs::directory_iterator(dir_, ec)) {
		if (is_session_file(e.path())) {
			if (auto s = read(e.path())) {
				out.push_back(std::move(*s));
			}
		}
	}
	return out;
}

void SessionStore::clear() {
	std::error_code ec;
	std::vector<fs::path> files;
	for (const auto &e : fs::directory_iterator(dir_, ec)) {
		if (is_session_file(e.path())) {
			files.push_back(e.path());
		}
	}
	for (const auto &p : files) {
		forget(p);
	}
}

bool SessionStore::is_trusted(const std::string &server, const std::string &issuer) const {
	std::ifstream in(dir_ / "trust.json", std::ios::binary);
	if (!in) {
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const json doc = json::parse(text, nullptr, false);
	if (!doc.is_array()) {
		return false;
	}
	const std::string key = server + "\n" + issuer;
	for (const json &e : doc) {
		if (e.is_string() && e.get<std::string>() == key) {
			return true;
		}
	}
	return false;
}

bool SessionStore::trust(const std::string &server, const std::string &issuer) {
	if (is_trusted(server, issuer)) {
		return true;
	}
	json doc = json::array();
	{
		std::ifstream in(dir_ / "trust.json", std::ios::binary);
		if (in) {
			const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			json existing = json::parse(text, nullptr, false);
			if (existing.is_array()) {
				doc = std::move(existing);
			}
		}
	}
	doc.push_back(server + "\n" + issuer);
	return write_private(dir_ / "trust.json", doc.dump());
}

std::string label_from_id_token(const std::string &id_token) {
	const JwsParse p = parse_jws(id_token);
	if (!p.jws) {
		return {};
	}
	const json c = json::parse(p.jws->payload_json, nullptr, false);
	if (!c.is_object()) {
		return {};
	}
	for (const char *k : { "preferred_username", "name", "email", "sub" }) {
		const std::string v = get(c, k);
		if (!v.empty()) {
			return v.substr(0, 64);
		}
	}
	return {};
}

} // namespace vb::auth
