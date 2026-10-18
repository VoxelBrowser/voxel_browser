#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vb/auth/http.hpp"
#include "vb/auth/signin.hpp"

// Client-side OIDC sign-in (auth.md §7): Authorization Code + PKCE (S256)
// with a loopback redirect (RFC 8252) in the system browser. Everything here is
// transport-agnostic and testable: HTTP goes through HttpFetcher, the browser
// is an injected callback, and the loopback listener is a small real socket.

namespace vb::auth {

struct OidcEndpoints {
	std::string issuer;
	std::string authorization_endpoint;
	std::string token_endpoint;
};

// Discovery at {issuer}/.well-known/openid-configuration; the document's
// `issuer` must equal the configured one and both endpoints must be https (or
// loopback http).
std::optional<OidcEndpoints> oidc_discover(HttpFetcher &http, const std::string &issuer,
		std::string *error);

struct Pkce {
	std::string verifier; // 43 chars of base64url (32 random bytes)
	std::string challenge; // base64url(SHA-256(verifier)), method S256
};
Pkce make_pkce(); // verifier empty if the OS entropy source failed
Pkce pkce_from_verifier(std::string verifier);

std::string url_encode(std::string_view in);
std::string form_encode(const std::vector<std::pair<std::string, std::string>> &fields);

std::string build_authorization_url(const OidcEndpoints &ep, const std::string &client_id,
		const std::string &redirect_uri, const std::vector<std::string> &scopes,
		const std::string &state, const std::string &nonce, const std::string &code_challenge);

struct TokenResponse {
	bool ok = false;
	std::string id_token;
	std::string refresh_token;
	std::string error; // coarse and token-free
	bool retryable = false; // transport failure, 5xx or 429: try again, the grant is not at fault
};
TokenResponse exchange_code(HttpFetcher &http, const OidcEndpoints &ep,
		const std::string &client_id, const std::string &redirect_uri,
		const std::string &code, const std::string &verifier,
		const std::string &client_secret = {});
TokenResponse refresh_id_token(HttpFetcher &http, const OidcEndpoints &ep,
		const std::string &client_id, const std::string &refresh_token);

// Parses the request head the browser sent to the loopback listener
// ("GET /callback?code=..&state=.. HTTP/1.1 ..."). The state must match.
struct RedirectParse {
	std::string code;
	std::string error; // empty on success
};
RedirectParse parse_redirect_request(std::string_view request_head,
		std::string_view expected_state);

// One-shot HTTP listener on 127.0.0.1:<ephemeral> (never any other interface).
class LoopbackListener {
public:
	static std::unique_ptr<LoopbackListener> create(std::string *error);
	~LoopbackListener();
	LoopbackListener(const LoopbackListener &) = delete;
	LoopbackListener &operator=(const LoopbackListener &) = delete;

	int port() const { return port_; }
	std::string redirect_uri() const;

	// Waits for one request whose target starts with "/callback" (other paths,
	// e.g. /favicon.ico, get a 404 and are ignored). Returns its head, or
	// nullopt on timeout/cancel.
	std::optional<std::string> wait_for_callback(std::chrono::milliseconds timeout,
			const std::atomic<bool> &cancelled);
	// Answers the pending callback request and closes it.
	void respond(bool success);

private:
	LoopbackListener() = default;
	std::intptr_t fd_ = -1; // SOCKET on Windows
	std::intptr_t client_fd_ = -1;
	int port_ = 0;
};

struct OidcSignInParams {
	std::shared_ptr<HttpFetcher> http;
	std::string issuer;
	std::string client_id;
	// Only for providers that want one even with PKCE (Google's "Desktop app"
	// clients); sent in the code exchange. Not a secret for installed apps.
	std::string client_secret;
	std::vector<std::string> scopes;
	std::string nonce; // the server's challenge nonce, put in the auth request
	std::chrono::milliseconds timeout = std::chrono::minutes(5);
	// Opens `url` in the system browser; false = could not. Defaults to
	// open_system_browser.
	std::function<bool(const std::string &)> open_browser;
};

// The whole flow, synchronously (run it from a SignInTask worker). Honors
// `cancelled` while waiting for the browser.
SignInResult oidc_browser_sign_in(const OidcSignInParams &params,
		const std::atomic<bool> &cancelled);

// xdg-open / open / ShellExecuteW without a shell. Only http(s) URLs.
bool open_system_browser(const std::string &url);

} // namespace vb::auth
