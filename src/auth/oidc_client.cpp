#include "vb/auth/oidc_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
// Order matters: winsock2.h, then windows.h, then shellapi.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>
extern char **environ;
#endif

#include "vb/auth/crypto.hpp"
#include "vb/auth/jwt.hpp"
#include "vb/core/log.hpp"

namespace vb::auth {

namespace {

using nlohmann::json;

#if defined(_WIN32)
using Sock = SOCKET;
constexpr std::intptr_t kBadSock = static_cast<std::intptr_t>(INVALID_SOCKET);
void close_sock(std::intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
#else
using Sock = int;
constexpr std::intptr_t kBadSock = -1;
void close_sock(std::intptr_t s) { ::close(static_cast<int>(s)); }
#endif

SignInResult failure(std::string error) {
	SignInResult r;
	r.done = true;
	r.error = std::move(error);
	return r;
}

std::string trim_slash(std::string s) {
	while (!s.empty() && s.back() == '/') {
		s.pop_back();
	}
	return s;
}

std::string json_str(const json &o, const char *key) {
	if (o.is_object()) {
		auto it = o.find(key);
		if (it != o.end() && it->is_string()) {
			return it->get<std::string>();
		}
	}
	return {};
}

int hex_val(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

std::string url_decode(std::string_view in) {
	std::string out;
	for (std::size_t i = 0; i < in.size(); ++i) {
		if (in[i] == '%' && i + 2 < in.size() + 0 && hex_val(in[i + 1]) >= 0 &&
				hex_val(in[i + 2]) >= 0) {
			out.push_back(static_cast<char>(hex_val(in[i + 1]) * 16 + hex_val(in[i + 2])));
			i += 2;
		} else if (in[i] == '+') {
			out.push_back(' ');
		} else {
			out.push_back(in[i]);
		}
	}
	return out;
}

} // namespace

std::string url_encode(std::string_view in) {
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (const char ch : in) {
		const auto c = static_cast<unsigned char>(ch);
		if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back(ch);
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0xF]);
		}
	}
	return out;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>> &fields) {
	std::string out;
	for (const auto &[k, v] : fields) {
		if (!out.empty()) {
			out.push_back('&');
		}
		out += url_encode(k) + "=" + url_encode(v);
	}
	return out;
}

std::optional<OidcEndpoints> oidc_discover(HttpFetcher &http, const std::string &issuer,
		std::string *error) {
	auto fail = [&](const char *msg) -> std::optional<OidcEndpoints> {
		if (error != nullptr) {
			*error = msg;
		}
		return std::nullopt;
	};
	const HttpResult res = http.get(trim_slash(issuer) + "/.well-known/openid-configuration");
	if (res.status != 200) {
		return fail("could not reach the identity provider");
	}
	const json doc = json::parse(res.body, nullptr, false);
	if (!doc.is_object() || trim_slash(json_str(doc, "issuer")) != trim_slash(issuer)) {
		return fail("identity provider returned an unexpected issuer");
	}
	OidcEndpoints ep;
	ep.issuer = issuer;
	ep.authorization_endpoint = json_str(doc, "authorization_endpoint");
	ep.token_endpoint = json_str(doc, "token_endpoint");
	if (!url_allowed(ep.authorization_endpoint) || !url_allowed(ep.token_endpoint)) {
		return fail("identity provider endpoints are not acceptable");
	}
	return ep;
}

Pkce pkce_from_verifier(std::string verifier) {
	Pkce p;
	p.verifier = std::move(verifier);
	const auto h = sha256(p.verifier);
	p.challenge = base64url_encode(h);
	return p;
}

Pkce make_pkce() {
	const auto bytes = random_bytes(32);
	if (bytes.empty()) {
		return {};
	}
	return pkce_from_verifier(base64url_encode(bytes));
}

std::string build_authorization_url(const OidcEndpoints &ep, const std::string &client_id,
		const std::string &redirect_uri, const std::vector<std::string> &scopes,
		const std::string &state, const std::string &nonce,
		const std::string &code_challenge) {
	std::string scope;
	for (const auto &s : scopes) {
		scope += (scope.empty() ? "" : " ") + s;
	}
	const char sep = ep.authorization_endpoint.find('?') == std::string::npos ? '?' : '&';
	return ep.authorization_endpoint + sep +
			form_encode({ { "response_type", "code" }, { "client_id", client_id },
					{ "redirect_uri", redirect_uri }, { "scope", scope }, { "state", state },
					{ "nonce", nonce }, { "code_challenge", code_challenge },
					{ "code_challenge_method", "S256" } });
}

namespace {

TokenResponse parse_token_response(const HttpResult &res) {
	TokenResponse out;
	if (res.status == 0) {
		out.error = "could not reach the identity provider";
		out.retryable = true;
		return out;
	}
	if (res.status >= 500 || res.status == 429) {
		out.error = "the identity provider is unavailable";
		out.retryable = true;
		return out;
	}
	const json doc = json::parse(res.body, nullptr, false);
	if (res.status != 200 || !doc.is_object()) {
		// RFC 6749 error codes are safe to relay; descriptions are not.
		const std::string code = json_str(doc, "error");
		out.error = code.empty() ? "sign-in was rejected" : "sign-in was rejected (" + code.substr(0, 40) + ")";
		return out;
	}
	out.id_token = json_str(doc, "id_token");
	out.refresh_token = json_str(doc, "refresh_token");
	if (out.id_token.empty()) {
		out.error = "the identity provider returned no ID token";
		return out;
	}
	out.ok = true;
	return out;
}

} // namespace

TokenResponse exchange_code(HttpFetcher &http, const OidcEndpoints &ep,
		const std::string &client_id, const std::string &redirect_uri,
		const std::string &code, const std::string &verifier, const std::string &client_secret) {
	std::vector<std::pair<std::string, std::string>> form = { { "grant_type", "authorization_code" },
		{ "code", code }, { "redirect_uri", redirect_uri }, { "client_id", client_id },
		{ "code_verifier", verifier } };
	if (!client_secret.empty()) {
		form.emplace_back("client_secret", client_secret);
	}
	return parse_token_response(http.post(ep.token_endpoint, "application/x-www-form-urlencoded",
			form_encode(form)));
}

TokenResponse refresh_id_token(HttpFetcher &http, const OidcEndpoints &ep,
		const std::string &client_id, const std::string &refresh_token) {
	return parse_token_response(http.post(ep.token_endpoint,
			"application/x-www-form-urlencoded",
			form_encode({ { "grant_type", "refresh_token" },
					{ "refresh_token", refresh_token }, { "client_id", client_id } })));
}

RedirectParse parse_redirect_request(std::string_view head, std::string_view expected_state) {
	RedirectParse out;
	const std::size_t eol = head.find("\r\n");
	const std::string_view line = head.substr(0, eol == std::string_view::npos ? head.size() : eol);
	// "GET /callback?code=..&state=.. HTTP/1.1"
	if (line.rfind("GET ", 0) != 0) {
		out.error = "unexpected request";
		return out;
	}
	const std::size_t sp = line.find(' ', 4);
	const std::string_view target = line.substr(4, sp == std::string_view::npos ? std::string_view::npos : sp - 4);
	const std::size_t q = target.find('?');
	if (target.substr(0, q) != "/callback" || q == std::string_view::npos) {
		out.error = "unexpected request";
		return out;
	}
	std::string code;
	std::string state;
	std::string err;
	std::string_view query = target.substr(q + 1);
	while (!query.empty()) {
		const std::size_t amp = query.find('&');
		const std::string_view pair = query.substr(0, amp);
		const std::size_t eq = pair.find('=');
		const std::string key = url_decode(pair.substr(0, eq));
		const std::string val = eq == std::string_view::npos ? std::string() : url_decode(pair.substr(eq + 1));
		if (key == "code") {
			code = val;
		} else if (key == "state") {
			state = val;
		} else if (key == "error") {
			err = val;
		}
		query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
	}
	// Constant-shape check; a mismatched state means this redirect was not for
	// our request (CSRF / stray tab) and must not be exchanged.
	if (state.empty() || state != expected_state) {
		out.error = "the sign-in response did not match this request";
		return out;
	}
	if (!err.empty()) {
		out.error = "sign-in was cancelled or denied";
		return out;
	}
	if (code.empty()) {
		out.error = "the sign-in response had no code";
		return out;
	}
	out.code = std::move(code);
	return out;
}

// ---------------------------------------------------------------------------
// LoopbackListener
// ---------------------------------------------------------------------------

std::unique_ptr<LoopbackListener> LoopbackListener::create(std::string *error) {
#if defined(_WIN32)
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		if (error != nullptr) {
			*error = "network init failed";
		}
		return nullptr;
	}
#endif
	const std::intptr_t fd = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, 0));
	if (fd == kBadSock) {
		if (error != nullptr) {
			*error = "could not open a local port";
		}
		return nullptr;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = 0; // ephemeral
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
	socklen_t len = sizeof(addr);
	if (::bind(static_cast<Sock>(fd), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
			::listen(static_cast<Sock>(fd), 4) != 0 ||
			::getsockname(static_cast<Sock>(fd), reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
		close_sock(fd);
		if (error != nullptr) {
			*error = "could not listen on a local port";
		}
		return nullptr;
	}
	std::unique_ptr<LoopbackListener> l(new LoopbackListener);
	l->fd_ = fd;
	l->port_ = ntohs(addr.sin_port);
	return l;
}

LoopbackListener::~LoopbackListener() {
	if (client_fd_ != kBadSock) {
		close_sock(client_fd_);
	}
	if (fd_ != kBadSock) {
		close_sock(fd_);
	}
}

std::string LoopbackListener::redirect_uri() const {
	return "http://127.0.0.1:" + std::to_string(port_) + "/callback";
}

namespace {

// Waits up to timeout_ms for `fd` to be readable; false on timeout/error.
bool wait_readable(std::intptr_t fd, int timeout_ms) {
#if defined(_WIN32)
	WSAPOLLFD p{ static_cast<SOCKET>(fd), POLLRDNORM, 0 };
	return WSAPoll(&p, 1, timeout_ms) > 0;
#else
	pollfd p{ static_cast<int>(fd), POLLIN, 0 };
	return ::poll(&p, 1, timeout_ms) > 0;
#endif
}

void send_all(std::intptr_t fd, const std::string &data) {
	std::size_t off = 0;
	while (off < data.size()) {
		const auto n = ::send(static_cast<Sock>(fd), data.data() + off,
#if defined(_WIN32)
				static_cast<int>(data.size() - off), 0);
#else
				data.size() - off, MSG_NOSIGNAL);
#endif
		if (n <= 0) {
			return;
		}
		off += static_cast<std::size_t>(n);
	}
}

std::string http_response(int status, const char *reason, const std::string &body) {
	return "HTTP/1.1 " + std::to_string(status) + " " + reason +
			"\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
			std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

} // namespace

std::optional<std::string> LoopbackListener::wait_for_callback(
		std::chrono::milliseconds timeout, const std::atomic<bool> &cancelled) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
		if (!wait_readable(fd_, 100)) {
			continue;
		}
		const std::intptr_t c = static_cast<std::intptr_t>(::accept(static_cast<Sock>(fd_), nullptr, nullptr));
		if (c == kBadSock) {
			continue;
		}
		std::string head;
		char buf[1024];
		// Read just the request head, bounded in size and time.
		const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos &&
				std::chrono::steady_clock::now() < read_deadline) {
			if (!wait_readable(c, 200)) {
				continue;
			}
			const auto n = ::recv(static_cast<Sock>(c), buf,
#if defined(_WIN32)
					static_cast<int>(sizeof(buf)), 0);
#else
					sizeof(buf), 0);
#endif
			if (n <= 0) {
				break;
			}
			head.append(buf, static_cast<std::size_t>(n));
		}
		if (head.rfind("GET /callback", 0) == 0) {
			client_fd_ = c;
			return head;
		}
		send_all(c, http_response(404, "Not Found", "<!doctype html><title>Not found</title>"));
		close_sock(c);
	}
	return std::nullopt;
}

void LoopbackListener::respond(bool success) {
	if (client_fd_ == kBadSock) {
		return;
	}
	const std::string page = success
			? "<!doctype html><meta charset=utf-8><title>Signed in</title>"
			  "<body style='font-family:sans-serif;text-align:center;margin-top:20vh'>"
			  "<h2>Signed in</h2><p>You can close this tab and return to the game.</p></body>"
			: "<!doctype html><meta charset=utf-8><title>Sign-in failed</title>"
			  "<body style='font-family:sans-serif;text-align:center;margin-top:20vh'>"
			  "<h2>Sign-in did not complete</h2><p>Return to the game and try again.</p></body>";
	send_all(client_fd_, http_response(success ? 200 : 400, success ? "OK" : "Bad Request", page));
	close_sock(client_fd_);
	client_fd_ = kBadSock;
}

// ---------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------

bool open_system_browser(const std::string &url) {
	if (url.rfind("https://", 0) != 0 && url.rfind("http://127.0.0.1", 0) != 0 &&
			url.rfind("http://localhost", 0) != 0) {
		return false;
	}
#if defined(_WIN32)
	const std::wstring wide(url.begin(), url.end()); // URL is percent-encoded ASCII
	return reinterpret_cast<std::intptr_t>(
				   ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
#if defined(__APPLE__)
	const char *opener = "open";
#else
	const char *opener = "xdg-open";
#endif
	// No shell: the URL is a single argv element.
	char *argv[] = { const_cast<char *>(opener), const_cast<char *>(url.c_str()), nullptr };
	pid_t pid = 0;
	return posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) == 0;
#endif
}

SignInResult oidc_browser_sign_in(const OidcSignInParams &params,
		const std::atomic<bool> &cancelled) {
	if (!params.http) {
		return failure("sign-in is unavailable in this build");
	}
	std::string err;
	const auto endpoints = oidc_discover(*params.http, params.issuer, &err);
	if (!endpoints) {
		return failure("Could not reach the identity provider");
	}
	const Pkce pkce = make_pkce();
	const auto state_bytes = random_bytes(16);
	if (pkce.verifier.empty() || state_bytes.empty()) {
		return failure("Could not generate secure sign-in values");
	}
	const std::string state = base64url_encode(state_bytes);

	auto listener = LoopbackListener::create(&err);
	if (!listener) {
		return failure("Could not start the local sign-in listener");
	}
	const std::string redirect_uri = listener->redirect_uri();
	const std::string url = build_authorization_url(*endpoints, params.client_id,
			redirect_uri, params.scopes, state, params.nonce, pkce.challenge);

	const bool opened = params.open_browser ? params.open_browser(url) : open_system_browser(url);
	if (!opened) {
		return failure("Could not open your web browser");
	}

	// A stray request (wrong state) is answered and ignored; keep waiting.
	const auto deadline = std::chrono::steady_clock::now() + params.timeout;
	for (;;) {
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
				deadline - std::chrono::steady_clock::now());
		if (cancelled.load()) {
			return failure("Sign-in cancelled");
		}
		if (remaining.count() <= 0) {
			return failure("Sign-in timed out");
		}
		const auto head = listener->wait_for_callback(remaining, cancelled);
		if (!head) {
			return failure(cancelled.load() ? "Sign-in cancelled" : "Sign-in timed out");
		}
		const RedirectParse parsed = parse_redirect_request(*head, state);
		if (!parsed.error.empty()) {
			listener->respond(false);
			if (parsed.error == "sign-in was cancelled or denied") {
				return failure("Sign-in cancelled");
			}
			continue; // not our redirect; keep listening
		}
		listener->respond(true);
		const TokenResponse tok = exchange_code(*params.http, *endpoints, params.client_id,
				redirect_uri, parsed.code, pkce.verifier, params.client_secret);
		if (!tok.ok) {
			return failure("The identity provider rejected the sign-in");
		}
		SignInResult r;
		r.done = true;
		r.ok = true;
		r.id_token = tok.id_token;
		r.refresh_token = tok.refresh_token;
		return r;
	}
}

} // namespace vb::auth
