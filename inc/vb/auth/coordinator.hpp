#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "vb/auth/http.hpp"
#include "vb/auth/session_store.hpp"
#include "vb/auth/signin.hpp"
#include "vb/net/handshake.hpp"

// Client-side glue between the handshake's sign-in ticket and the UI
// (auth.md §7). The UI reads phase()/challenge() and calls start_browser() /
// start_password() / cancel(); the handshake polls the ticket from
// provider(). A failed attempt returns to kChoosing with last_error() so a
// mistyped password or a closed browser tab can be retried without
// reconnecting; only cancel() (or the server's auth timeout) ends the join.
//
// With a SessionStore the coordinator also (a) asks the player to trust a
// (server, issuer) pair the first time, (b) tries a silent refresh with the
// cached refresh token before showing any UI, and (c) answers the server's
// periodic S2C_ReauthRequest the same way (silent first, then a non-blocking
// "sign in again" prompt) -- auth.md §5.6.

namespace vb::auth {

// Reads a whole token from `path` (trims whitespace; <= 16 KiB). nullopt if
// the file is missing/empty/oversized. Used by --auth-token-file, re-read for
// every sign-in so tests can rotate or withhold the token.
std::optional<std::string> read_token_file(const std::filesystem::path &path);

// Exchanges a stored refresh token for a fresh ID token (no UI). For OIDC the
// issuer's discovery document supplies the token endpoint. `refresh_token` in
// the result is the rotated one if the IdP sent it, else the one passed in.
SignInResult silent_sign_in(HttpFetcher &http, const protocol::S2CAuthChallenge &challenge,
		const std::string &refresh_token);

class SignInCoordinator {
public:
	enum class Phase : std::uint8_t {
		kIdle, // no challenge yet
		kChoosing, // waiting for the player to pick a method / retry / trust
		kWorking, // browser round trip, REST call or silent refresh in flight
		kFinished, // token produced, or cancelled
	};

	struct Options {
		std::shared_ptr<HttpFetcher> http;
		// Headless/automation: skip the UI and use this file's token.
		std::optional<std::filesystem::path> token_file;
		std::function<bool(const std::string &)> open_browser; // default: system browser
		// Refresh-token cache + trust list. Null = no persistence, no prompt.
		std::shared_ptr<SessionStore> store;
		std::string server_id; // "host:port": keys both the trust list and the saved login
		// A silent re-auth refresh that failed for a transient reason (IdP down, 5xx) is
		// retried this often while the server's request is open; the stored refresh
		// token is kept. Tests shorten it.
		std::chrono::milliseconds reauth_retry_interval = std::chrono::seconds(5);
		// Firebase "google" sign-in signs in to this issuer first. Tests point it at a fake.
		std::string google_issuer = "https://accounts.google.com";
	};

	explicit SignInCoordinator(Options options);
	~SignInCoordinator();

	// Plug into ClientSession::set_sign_in_provider / set_reauth_provider. The
	// coordinator must outlive the session.
	std::function<net::TokenTicket(const protocol::S2CAuthChallenge &)> provider();
	std::function<net::TokenTicket(const protocol::S2CReauthRequest &)> reauth_provider();

	Phase phase() const;
	std::optional<protocol::S2CAuthChallenge> challenge() const;
	std::string last_error() const; // from the previous attempt, empty if none

	// First use of this (server, issuer): the UI must ask before anything is
	// sent to the identity provider. trust() remembers the answer.
	bool needs_trust() const;
	void trust();

	// A periodic re-auth request is open and the silent refresh did not
	// settle it: the UI shows a non-blocking "sign in again" prompt.
	bool reauth_prompt_active() const;
	// The prompt is the same browser/password flow, bound to the request nonce.

	bool supports_browser() const; // oidc / keycloak
	bool supports_password() const; // firebase with "password" in sign_in
	// firebase with "google" in sign_in: the browser flow against Google, then
	// the Google ID token is exchanged for a Firebase one.
	bool supports_google() const;
	void start_browser();
	void start_password(const std::string &email, const std::string &password);
	void start_google();
	void cancel();

private:
	net::TokenPoll poll_ticket();
	void update_locked();
	void begin_silent_locked();
	void save_session_locked(const SignInResult &r);

	Options options_;
	mutable std::mutex mu_;
	Phase phase_ = Phase::kIdle;
	std::optional<protocol::S2CAuthChallenge> challenge_;
	std::string active_nonce_; // challenge nonce, or the open re-auth request's
	std::string error_;
	std::string token_;
	bool token_ready_ = false;
	bool cancelled_ = false;
	bool needs_trust_ = false;
	bool reauth_ = false; // a re-auth request (not the join sign-in) is active
	bool silent_ = false; // the task in flight is a silent refresh
	// Transient silent-refresh failures while a re-auth request is open (see
	// Options::reauth_retry_interval). The prompt waits for a few of them.
	int transient_failures_ = 0;
	bool retry_pending_ = false;
	std::chrono::steady_clock::time_point retry_at_{};
	std::unique_ptr<SignInTask> task_;
	// A cancelled task is parked (not joined under the lock, which could stall
	// the UI behind a slow HTTP call) and joined when the coordinator dies.
	std::vector<std::unique_ptr<SignInTask>> parked_;
};

} // namespace vb::auth
