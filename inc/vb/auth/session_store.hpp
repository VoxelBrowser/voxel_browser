#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Client-side persistence for sign-in (auth.md §7): the refresh-token cache
// per (server, issuer, client_id) and the first-use trust list per (server,
// issuer). Each server is its own sign-in: two servers that happen to use the
// same identity provider never share a saved login, and signing out of one
// leaves the others alone. Refresh tokens are credentials: with a
// SecretBackend (the OS keychain, secret_store.hpp) the token lives there and
// the file keeps only the metadata; without one, or when the keychain refuses
// it, the token stays in the file. Files are created owner-only (0600) either
// way and the token is never logged.

namespace vb::auth {

class SecretBackend;

struct StoredSession {
	std::string server; // "host:port" (or "singleplayer"), as the client connected
	std::string issuer;
	std::string client_id;
	std::string provider; // "oidc" | "keycloak" | "firebase"
	std::string api_key; // firebase refresh needs it; public by design
	std::string refresh_token;
	std::string label; // "Signed in as ..." text (display only, unverified)
};

class SessionStore {
public:
	// `dir` is created on demand (usually user_config_dir()/"auth"). Saved
	// logins from before they were per server (no "server" field) are deleted
	// here: they would otherwise sign the player in to any server using that
	// provider. With `secrets`, tokens saved in files by an older client (or
	// while the keychain was unavailable) are moved into it here.
	explicit SessionStore(std::filesystem::path dir, std::shared_ptr<SecretBackend> secrets = nullptr);

	std::optional<StoredSession> load(const std::string &server, const std::string &issuer,
			const std::string &client_id) const;
	bool save(const StoredSession &session);
	void erase(const std::string &server, const std::string &issuer, const std::string &client_id);
	std::vector<StoredSession> list() const;
	std::vector<StoredSession> list_for(const std::string &server) const;
	void sign_out(const std::string &server); // every saved login for this server
	void clear(); // sign out of everything (keeps the trust list)

	// "<server> may ask me to sign in with <issuer>" -- remembered per pair.
	bool is_trusted(const std::string &server, const std::string &issuer) const;
	bool trust(const std::string &server, const std::string &issuer);

	const std::filesystem::path &dir() const { return dir_; }
	// Where refresh tokens go: the keychain's name, or "files".
	std::string_view secrets_name() const;

private:
	std::filesystem::path path_for(const std::string &server, const std::string &issuer,
			const std::string &client_id) const;
	std::optional<StoredSession> read(const std::filesystem::path &p) const;
	void forget(const std::filesystem::path &p);
	std::filesystem::path dir_;
	std::shared_ptr<SecretBackend> secrets_;
};

// A display label from the (unverified) ID-token payload: preferred_username,
// name, email, then sub. For the menu only; never used for identity.
std::string label_from_id_token(const std::string &id_token);

} // namespace vb::auth
