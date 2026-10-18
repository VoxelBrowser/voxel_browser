// In-engine authentication, step 9.5 (architecture_spec/auth.md §7): the
// client-side sign-in building blocks -- PKCE, the authorization URL, the
// loopback redirect, code exchange, Firebase password sign-in -- against fake
// HTTP and a real loopback socket standing in for the browser.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "vb/auth/coordinator.hpp"
#include "vb/auth/firebase.hpp"
#include "vb/auth/http.hpp"
#include "vb/auth/jwt.hpp"
#include "vb/auth/oidc_client.hpp"
#include "vb/auth/secret_store.hpp"
#include "vb/auth/session_store.hpp"
#include "vb/auth/signin.hpp"
#include "vb/core/version.hpp"
#include "vb/net/handshake.hpp"

#if defined(VB_WITH_AUTH)

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace vb::auth;
using nlohmann::json;

namespace {

const std::string kIssuer = "https://id.example/realms/vb";

class FakeIdp final : public HttpFetcher {
public:
	std::vector<std::string> posts; // bodies
	std::string token_status_body = R"({"id_token":"ID.TOKEN.VALUE","refresh_token":"R1"})";
	int token_status = 200;
	HttpResult get(const std::string &url) override {
		HttpResult r;
		if (url == kIssuer + "/.well-known/openid-configuration") {
			r.status = 200;
			r.body = json{ { "issuer", kIssuer },
				{ "authorization_endpoint", kIssuer + "/auth" },
				{ "token_endpoint", kIssuer + "/token" } }
							 .dump();
		} else {
			r.error = "no route";
		}
		return r;
	}
	HttpResult post(const std::string &url, const std::string &content_type,
			const std::string &body) override {
		HttpResult r;
		last_url = url;
		last_content_type = content_type;
		posts.push_back(body);
		r.status = token_status;
		r.body = token_status_body;
		return r;
	}
	std::string last_url, last_content_type;
};

std::string query_param(const std::string &url, const std::string &key) {
	const std::string needle = key + "=";
	auto pos = url.find("?" + needle);
	if (pos == std::string::npos) {
		pos = url.find("&" + needle);
	}
	if (pos == std::string::npos) {
		return {};
	}
	pos += needle.size() + 1;
	const auto end = url.find('&', pos);
	return url.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

#if !defined(_WIN32)
// Plays the browser: one GET to 127.0.0.1:<port><path>; returns the response.
std::string http_get_local(int port, const std::string &path) {
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_port = htons(static_cast<std::uint16_t>(port));
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	std::string out;
	if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0) {
		const std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
		::send(fd, req.data(), req.size(), 0);
		char buf[1024];
		for (;;) {
			const auto n = ::recv(fd, buf, sizeof(buf), 0);
			if (n <= 0) {
				break;
			}
			out.append(buf, static_cast<std::size_t>(n));
		}
	}
	::close(fd);
	return out;
}
#endif

} // namespace

TEST_CASE("PKCE: RFC 7636 appendix B vector, and fresh verifiers are well-formed") {
	const Pkce p = pkce_from_verifier("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
	CHECK(p.challenge == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");

	const Pkce a = make_pkce();
	const Pkce b = make_pkce();
	CHECK(a.verifier.size() == 43);
	CHECK(a.verifier != b.verifier);
	CHECK(a.challenge == pkce_from_verifier(a.verifier).challenge);
}

TEST_CASE("url/form encoding and the authorization URL") {
	CHECK(url_encode("a b&c=d/é~-_.") == "a%20b%26c%3Dd%2F%C3%A9~-_.");
	CHECK(form_encode({ { "k", "v 1" }, { "x", "&" } }) == "k=v%201&x=%26");

	OidcEndpoints ep;
	ep.authorization_endpoint = kIssuer + "/auth";
	const std::string url = build_authorization_url(ep, "voxel", "http://127.0.0.1:5/callback",
			{ "openid", "profile" }, "STATE", "NONCE", "CHAL");
	CHECK(url.rfind(kIssuer + "/auth?", 0) == 0);
	CHECK(query_param(url, "response_type") == "code");
	CHECK(query_param(url, "client_id") == "voxel");
	CHECK(query_param(url, "redirect_uri") == "http%3A%2F%2F127.0.0.1%3A5%2Fcallback");
	CHECK(query_param(url, "scope") == "openid%20profile");
	CHECK(query_param(url, "state") == "STATE");
	CHECK(query_param(url, "nonce") == "NONCE");
	CHECK(query_param(url, "code_challenge") == "CHAL");
	CHECK(query_param(url, "code_challenge_method") == "S256");
}

TEST_CASE("redirect parsing: state must match; errors and strays are rejected") {
	const auto ok = parse_redirect_request(
			"GET /callback?code=ab%2Fc&state=S1 HTTP/1.1\r\nHost: x\r\n\r\n", "S1");
	CHECK(ok.error.empty());
	CHECK(ok.code == "ab/c");

	CHECK_FALSE(parse_redirect_request("GET /callback?code=c&state=EVIL HTTP/1.1\r\n\r\n", "S1").error.empty());
	CHECK_FALSE(parse_redirect_request("GET /callback?code=c HTTP/1.1\r\n\r\n", "S1").error.empty());
	CHECK_FALSE(parse_redirect_request("GET /callback?state=S1 HTTP/1.1\r\n\r\n", "S1").error.empty());
	CHECK_FALSE(parse_redirect_request("GET /other?code=c&state=S1 HTTP/1.1\r\n\r\n", "S1").error.empty());
	CHECK_FALSE(parse_redirect_request("POST /callback?code=c&state=S1 HTTP/1.1\r\n\r\n", "S1").error.empty());
	CHECK_FALSE(parse_redirect_request("", "S1").error.empty());
	const auto denied = parse_redirect_request(
			"GET /callback?error=access_denied&state=S1 HTTP/1.1\r\n\r\n", "S1");
	CHECK(denied.error == "sign-in was cancelled or denied");
}

TEST_CASE("discovery requires a matching issuer and acceptable endpoints") {
	FakeIdp idp;
	std::string err;
	auto ep = oidc_discover(idp, kIssuer, &err);
	REQUIRE(ep);
	CHECK(ep->token_endpoint == kIssuer + "/token");
	CHECK_FALSE(oidc_discover(idp, "https://other.example", &err));
}

TEST_CASE("code exchange sends the verifier; refresh and errors are mapped") {
	FakeIdp idp;
	OidcEndpoints ep;
	ep.token_endpoint = kIssuer + "/token";
	const auto t = exchange_code(idp, ep, "voxel", "http://127.0.0.1:5/callback", "CODE", "VERIFIER");
	REQUIRE(t.ok);
	CHECK(t.id_token == "ID.TOKEN.VALUE");
	CHECK(t.refresh_token == "R1");
	CHECK(idp.last_content_type == "application/x-www-form-urlencoded");
	CHECK(idp.posts.back().find("grant_type=authorization_code") != std::string::npos);
	CHECK(idp.posts.back().find("code_verifier=VERIFIER") != std::string::npos);
	CHECK(idp.posts.back().find("client_id=voxel") != std::string::npos);

	const auto r = refresh_id_token(idp, ep, "voxel", "R1");
	CHECK(r.ok);
	CHECK(idp.posts.back().find("grant_type=refresh_token") != std::string::npos);

	idp.token_status = 400;
	idp.token_status_body = R"({"error":"invalid_grant","error_description":"secret detail"})";
	const auto bad = exchange_code(idp, ep, "voxel", "x", "c", "v");
	CHECK_FALSE(bad.ok);
	CHECK(bad.error.find("invalid_grant") != std::string::npos);
	CHECK(bad.error.find("secret detail") == std::string::npos);
}

#if !defined(_WIN32)

namespace {

OidcSignInParams make_params(const std::shared_ptr<FakeIdp> &idp,
		std::function<bool(const std::string &)> opener) {
	OidcSignInParams p;
	p.http = idp;
	p.issuer = kIssuer;
	p.client_id = "voxel";
	p.scopes = { "openid", "profile" };
	p.nonce = "server-nonce";
	p.open_browser = std::move(opener);
	p.timeout = std::chrono::seconds(10);
	return p;
}

} // namespace

TEST_CASE("browser flow: full PKCE round trip over a real loopback socket") {
	auto idp = std::make_shared<FakeIdp>();
	std::string auth_url;
	std::thread browser;
	std::string page;
	auto opener = [&](const std::string &url) {
		auth_url = url;
		const std::string redirect = query_param(url, "redirect_uri");
		const std::string port_s = redirect.substr(redirect.find("%3A", 10) + 3);
		const int port = std::stoi(port_s);
		const std::string state = query_param(url, "state");
		browser = std::thread([&, port, state] {
			// A stray tab hitting the listener first must not break the flow.
			http_get_local(port, "/favicon.ico");
			http_get_local(port, "/callback?code=EVIL&state=wrong");
			page = http_get_local(port, "/callback?code=GOODCODE&state=" + state);
		});
		return true;
	};
	std::atomic<bool> cancelled{ false };
	const SignInResult r = oidc_browser_sign_in(make_params(idp, opener), cancelled);
	browser.join();

	REQUIRE(r.ok);
	CHECK(r.id_token == "ID.TOKEN.VALUE");
	CHECK(r.refresh_token == "R1");
	CHECK(page.find("200 OK") != std::string::npos);
	CHECK(page.find("Signed in") != std::string::npos);
	// The server's nonce and a S256 challenge went to the IdP; the code
	// exchange carried the matching verifier and the same redirect URI.
	CHECK(query_param(auth_url, "nonce") == "server-nonce");
	REQUIRE(idp->posts.size() == 1);
	CHECK(idp->posts[0].find("code=GOODCODE") != std::string::npos);
	const std::string vkey = "code_verifier=";
	const auto vpos = idp->posts[0].find(vkey);
	REQUIRE(vpos != std::string::npos);
	const std::string verifier = idp->posts[0].substr(vpos + vkey.size());
	CHECK(pkce_from_verifier(verifier).challenge == query_param(auth_url, "code_challenge"));
	CHECK(idp->posts[0].find("redirect_uri=" + query_param(auth_url, "redirect_uri")) !=
			std::string::npos);
}

TEST_CASE("browser flow: denial, browser failure, timeout and cancel are clean failures") {
	std::atomic<bool> cancelled{ false };
	{
		auto idp = std::make_shared<FakeIdp>();
		std::thread browser;
		const SignInResult r = oidc_browser_sign_in(
				make_params(idp, [&](const std::string &url) {
					const std::string redirect = query_param(url, "redirect_uri");
					const int port = std::stoi(redirect.substr(redirect.find("%3A", 10) + 3));
					const std::string state = query_param(url, "state");
					browser = std::thread([port, state] {
						http_get_local(port, "/callback?error=access_denied&state=" + state);
					});
					return true;
				}),
				cancelled);
		browser.join();
		CHECK_FALSE(r.ok);
		CHECK(r.error == "Sign-in cancelled");
		CHECK(idp->posts.empty());
	}
	{
		auto idp = std::make_shared<FakeIdp>();
		const SignInResult r = oidc_browser_sign_in(
				make_params(idp, [](const std::string &) { return false; }), cancelled);
		CHECK_FALSE(r.ok);
		CHECK(r.error == "Could not open your web browser");
	}
	{
		auto idp = std::make_shared<FakeIdp>();
		OidcSignInParams p = make_params(idp, [](const std::string &) { return true; });
		p.timeout = std::chrono::milliseconds(300);
		const SignInResult r = oidc_browser_sign_in(p, cancelled);
		CHECK(r.error == "Sign-in timed out");
	}
	{
		auto idp = std::make_shared<FakeIdp>();
		SignInTask task([&](const std::atomic<bool> &c) {
			return oidc_browser_sign_in(make_params(idp, [](const std::string &) { return true; }), c);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		CHECK_FALSE(task.poll().done);
		task.cancel();
		SignInResult r;
		for (int i = 0; i < 100 && !(r = task.poll()).done; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		CHECK(r.done);
		CHECK(r.error == "Sign-in cancelled");
	}
}

TEST_CASE("loopback listener binds 127.0.0.1 on an ephemeral port only") {
	std::string err;
	auto l = LoopbackListener::create(&err);
	REQUIRE(l);
	CHECK(l->port() > 0);
	CHECK(l->redirect_uri() == "http://127.0.0.1:" + std::to_string(l->port()) + "/callback");
}

#endif // !_WIN32

TEST_CASE("firebase password sign-in: success, mapped errors, no leakage") {
	struct Fake final : HttpFetcher {
		HttpResult get(const std::string &) override { return {}; }
		HttpResult post(const std::string &url, const std::string &ct,
				const std::string &body) override {
			last_url = url;
			last_ct = ct;
			last_body = body;
			HttpResult r;
			r.status = status;
			r.body = reply;
			return r;
		}
		int status = 200;
		std::string reply = R"({"idToken":"FB.ID.TOKEN","refreshToken":"FR"})";
		std::string last_url, last_ct, last_body;
	} fake;

	const auto ok = firebase_password_sign_in(fake, "AIzaKEY", "a@b.io", "pw");
	REQUIRE(ok.ok);
	CHECK(ok.id_token == "FB.ID.TOKEN");
	CHECK(ok.refresh_token == "FR");
	CHECK(fake.last_url ==
			"https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=AIzaKEY");
	CHECK(fake.last_ct == "application/json");
	const json sent = json::parse(fake.last_body);
	CHECK(sent["email"] == "a@b.io");
	CHECK(sent["returnSecureToken"] == true);

	fake.status = 400;
	fake.reply = R"({"error":{"message":"INVALID_LOGIN_CREDENTIALS"}})";
	const auto bad = firebase_password_sign_in(fake, "k", "a@b.io", "wrong");
	CHECK_FALSE(bad.ok);
	CHECK(bad.error == "Wrong e-mail or password");
	fake.reply = R"({"error":{"message":"USER_DISABLED"}})";
	CHECK(firebase_password_sign_in(fake, "k", "a@b.io", "pw").error ==
			"This account has been disabled");
	CHECK(firebase_password_sign_in(fake, "k", "", "pw").error == "Enter your e-mail and password");

	fake.status = 200;
	fake.reply = R"({"id_token":"NEW","refresh_token":"R2"})";
	const auto r = firebase_refresh(fake, "k", "R1");
	REQUIRE(r.ok);
	CHECK(r.id_token == "NEW");
	CHECK(fake.last_body.find("grant_type=refresh_token") != std::string::npos);
}

namespace {

// Tiny frame plumbing for driving a ClientHandshake by hand.
template <typename Msg>
vb::protocol::Frame frame_of(const Msg &msg, std::vector<std::byte> &storage) {
	std::vector<std::byte> payload;
	msg.encode(payload);
	storage.clear();
	vb::protocol::write_frame(storage, Msg::kType, payload);
	std::size_t consumed = 0;
	return *vb::protocol::read_frame(storage, consumed);
}

struct SignInRig {
	std::shared_ptr<FakeIdp> idp = std::make_shared<FakeIdp>();
	std::unique_ptr<SignInCoordinator> coord;
	std::unique_ptr<vb::net::ClientHandshake> hs;
	std::vector<std::byte> storage;

	explicit SignInRig(SignInCoordinator::Options o) {
		o.http = idp;
		coord = std::make_unique<SignInCoordinator>(std::move(o));
		vb::net::HandshakeClientConfig cc;
		cc.player_name = "P";
		hs = std::make_unique<vb::net::ClientHandshake>(cc);
		hs->set_sign_in_provider(coord->provider());
		hs->start();
	}
	vb::net::ClientHandshakeStep deliver_challenge(const std::string &provider,
			std::vector<std::pair<std::string, std::string>> params = {}) {
		vb::protocol::S2CServerInfo info;
		info.engine_protocol_version = vb::kEngineProtocolVersion;
		info.auth_mode = vb::protocol::AuthMode::kExternal;
		hs->on_frame(frame_of(info, storage));
		vb::protocol::S2CAuthChallenge ch;
		ch.provider = provider;
		ch.issuer = kIssuer;
		ch.client_id = "voxel";
		ch.scopes = { "openid" };
		ch.nonce = "n-1";
		ch.params = std::move(params);
		return hs->on_frame(frame_of(ch, storage));
	}
	// Polls the handshake until it produces output or `ms` elapse.
	vb::net::ClientHandshakeStep poll_for(int ms) {
		for (int i = 0; i < ms / 10; ++i) {
			auto step = hs->poll();
			if (!step.send.empty() || step.failed) {
				return step;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return {};
	}
};

} // namespace

TEST_CASE("token file: trimmed, bounded, re-read every time") {
	const auto path = std::filesystem::temp_directory_path() / "vb_auth_token_file.txt";
	std::filesystem::remove(path);
	CHECK_FALSE(read_token_file(path));
	{
		std::ofstream(path) << "  aaa.bbb.ccc \n";
	}
	CHECK(read_token_file(path) == "aaa.bbb.ccc");
	{
		std::ofstream(path) << "rotated.token.x";
	}
	CHECK(read_token_file(path) == "rotated.token.x");
	{
		std::ofstream(path) << "   \n";
	}
	CHECK_FALSE(read_token_file(path));
	{
		std::ofstream(path) << std::string(20000, 'a');
	}
	CHECK_FALSE(read_token_file(path));
	std::filesystem::remove(path);
}

TEST_CASE("handshake: a token-file provider signs in on the first poll") {
	const auto path = std::filesystem::temp_directory_path() / "vb_auth_token_file2.txt";
	{
		std::ofstream(path) << "file.id.token";
	}
	SignInCoordinator::Options o;
	o.token_file = path;
	SignInRig rig(std::move(o));
	const auto step = rig.deliver_challenge("keycloak");
	REQUIRE(step.send.size() == 1);
	CHECK(rig.hs->status() == vb::net::ClientHandshakeStatus::kAuthenticating);
	std::size_t consumed = 0;
	auto frame = vb::protocol::read_frame(step.send[0].bytes, consumed);
	auto auth = vb::protocol::C2SAuth::decode(frame->payload);
	REQUIRE(auth);
	CHECK(auth->token == "file.id.token");

	// A missing file fails the join with a clear reason, never a silent no-auth.
	std::filesystem::remove(path);
	SignInCoordinator::Options o2;
	o2.token_file = path;
	SignInRig rig2(std::move(o2));
	const auto step2 = rig2.deliver_challenge("keycloak");
	CHECK(step2.failed);
	CHECK(step2.failure_reason == "could not read the sign-in token file");
}

TEST_CASE("coordinator: firebase password form, retry after a wrong password, then success") {
	struct Idp final : HttpFetcher {
		HttpResult get(const std::string &) override { return {}; }
		HttpResult post(const std::string &, const std::string &, const std::string &body) override {
			HttpResult r;
			if (body.find("goodpw") != std::string::npos) {
				r.status = 200;
				r.body = R"({"idToken":"FB.TOKEN.1","refreshToken":"r"})";
			} else {
				r.status = 400;
				r.body = R"({"error":{"message":"INVALID_PASSWORD"}})";
			}
			return r;
		}
	};
	SignInCoordinator::Options o;
	SignInRig rig(std::move(o));
	// Swap in the form-aware fake (the rig's coordinator was built with FakeIdp).
	SignInCoordinator::Options o2;
	o2.http = std::make_shared<Idp>();
	rig.coord = std::make_unique<SignInCoordinator>(std::move(o2));
	rig.hs = std::make_unique<vb::net::ClientHandshake>(vb::net::HandshakeClientConfig{});
	rig.hs->set_sign_in_provider(rig.coord->provider());
	rig.hs->start();

	CHECK(rig.coord->phase() == SignInCoordinator::Phase::kIdle);
	rig.deliver_challenge("firebase", { { "api_key", "k" }, { "sign_in", "password,google" } });
	CHECK(rig.hs->status() == vb::net::ClientHandshakeStatus::kSigningIn);
	CHECK(rig.coord->phase() == SignInCoordinator::Phase::kChoosing);
	CHECK(rig.coord->supports_password());
	CHECK_FALSE(rig.coord->supports_browser());
	CHECK(rig.hs->poll().send.empty()); // nothing chosen yet

	rig.coord->start_password("a@b.io", "badpw");
	CHECK(rig.coord->phase() == SignInCoordinator::Phase::kWorking);
	for (int i = 0; i < 200 && rig.coord->phase() == SignInCoordinator::Phase::kWorking; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(rig.coord->phase() == SignInCoordinator::Phase::kChoosing); // retryable
	CHECK(rig.coord->last_error() == "Wrong e-mail or password");
	CHECK(rig.hs->status() == vb::net::ClientHandshakeStatus::kSigningIn);

	rig.coord->start_password("a@b.io", "goodpw");
	const auto step = rig.poll_for(2000);
	REQUIRE(step.send.size() == 1);
	CHECK(rig.hs->status() == vb::net::ClientHandshakeStatus::kAuthenticating);
	std::size_t consumed = 0;
	auto frame = vb::protocol::read_frame(step.send[0].bytes, consumed);
	CHECK(vb::protocol::C2SAuth::decode(frame->payload)->token == "FB.TOKEN.1");
}

#if !defined(_WIN32)
// Firebase "google": the browser flow runs against Google with the pack's
// Desktop-app client (secret included in the code exchange), then the Google
// ID token is traded for a Firebase one, which is what reaches the server.
TEST_CASE("coordinator: firebase google sign-in goes through Google, then signInWithIdp") {
	const std::string google = "https://google.example";
	struct Idp final : HttpFetcher {
		std::string google;
		std::vector<std::pair<std::string, std::string>> posts; // url, body
		HttpResult get(const std::string &url) override {
			HttpResult r;
			if (url == google + "/.well-known/openid-configuration") {
				r.status = 200;
				r.body = json{ { "issuer", google }, { "authorization_endpoint", google + "/o/oauth2/v2/auth" },
					{ "token_endpoint", google + "/token" } }
								 .dump();
			}
			return r;
		}
		HttpResult post(const std::string &url, const std::string &, const std::string &body) override {
			posts.emplace_back(url, body);
			HttpResult r;
			r.status = 200;
			if (url == google + "/token") {
				r.body = R"({"id_token":"GOOGLE.ID.TOKEN","refresh_token":"google-refresh"})";
			} else if (url.find("accounts:signInWithIdp?key=k") != std::string::npos &&
					body.find("id_token=GOOGLE.ID.TOKEN") != std::string::npos &&
					body.find("providerId=google.com") != std::string::npos) {
				r.body = R"({"idToken":"FB.FROM.GOOGLE","refreshToken":"fb-refresh"})";
			} else {
				r.status = 400;
				r.body = R"({"error":{"message":"INVALID_IDP_RESPONSE"}})";
			}
			return r;
		}
	};
	auto idp = std::make_shared<Idp>();
	idp->google = google;
	std::string auth_url;
	std::thread browser;
	const auto options = [&] {
		SignInCoordinator::Options o;
		o.http = idp;
		o.google_issuer = google;
		o.open_browser = [&](const std::string &url) {
			auth_url = url;
			const std::string redirect = query_param(url, "redirect_uri");
			const int port = std::stoi(redirect.substr(redirect.find("%3A", 10) + 3));
			const std::string state = query_param(url, "state");
			browser = std::thread([port, state] { http_get_local(port, "/callback?code=G&state=" + state); });
			return true;
		};
		return o;
	};
	SignInRig rig(SignInCoordinator::Options{});
	const auto restart = [&] {
		rig.coord = std::make_unique<SignInCoordinator>(options());
		rig.hs = std::make_unique<vb::net::ClientHandshake>(vb::net::HandshakeClientConfig{});
		rig.hs->set_sign_in_provider(rig.coord->provider());
		rig.hs->start();
	};

	// Without a google_client_id param the method is not offered.
	restart();
	rig.deliver_challenge("firebase", { { "api_key", "k" }, { "sign_in", "password,google" } });
	CHECK_FALSE(rig.coord->supports_google());

	restart();
	rig.deliver_challenge("firebase", { { "api_key", "k" }, { "sign_in", "google" }, { "google_client_id", "gid.apps.googleusercontent.com" }, { "google_client_secret", "gsecret" } });
	CHECK(rig.coord->supports_google());
	CHECK_FALSE(rig.coord->supports_browser());
	CHECK_FALSE(rig.coord->supports_password());
	rig.coord->start_google();
	const auto step = rig.poll_for(5000);
	if (browser.joinable()) {
		browser.join();
	}
	REQUIRE(step.send.size() == 1);
	std::size_t consumed = 0;
	auto frame = vb::protocol::read_frame(step.send[0].bytes, consumed);
	CHECK(vb::protocol::C2SAuth::decode(frame->payload)->token == "FB.FROM.GOOGLE");
	CHECK(auth_url.rfind(google + "/o/oauth2/v2/auth?", 0) == 0);
	CHECK(query_param(auth_url, "client_id") == "gid.apps.googleusercontent.com");
	REQUIRE(idp->posts.size() == 2);
	CHECK(idp->posts[0].second.find("client_secret=gsecret") != std::string::npos);
	CHECK(idp->posts[1].first.find("signInWithIdp") != std::string::npos);
}
#endif

TEST_CASE("coordinator: cancel ends the join with 'sign-in cancelled'") {
	SignInCoordinator::Options o;
	SignInRig rig(std::move(o));
	rig.deliver_challenge("keycloak");
	CHECK(rig.coord->supports_browser());
	rig.coord->cancel();
	CHECK(rig.coord->phase() == SignInCoordinator::Phase::kFinished);
	const auto step = rig.hs->poll();
	CHECK(step.failed);
	CHECK(step.failure_reason == "sign-in cancelled");

	// Cancelling through the handshake (window closed) behaves the same.
	SignInCoordinator::Options o2;
	SignInRig rig2(std::move(o2));
	rig2.deliver_challenge("keycloak");
	const auto c = rig2.hs->cancel_sign_in();
	CHECK(c.failed);
	CHECK(rig2.hs->status() == vb::net::ClientHandshakeStatus::kFailed);
}

namespace {

std::filesystem::path fresh_dir(const char *name) {
	auto d = std::filesystem::temp_directory_path() / (std::string("vb_auth_store_") + name);
	std::filesystem::remove_all(d);
	return d;
}

// Fake IdP whose token endpoint accepts exactly one refresh token.
struct RefreshIdp final : HttpFetcher {
	std::string valid_refresh = "R-valid";
	int refreshes = 0;
	HttpResult get(const std::string &url) override {
		HttpResult r;
		if (url == kIssuer + "/.well-known/openid-configuration") {
			r.status = 200;
			r.body = json{ { "issuer", kIssuer }, { "authorization_endpoint", kIssuer + "/auth" },
				{ "token_endpoint", kIssuer + "/token" } }
							 .dump();
		}
		return r;
	}
	HttpResult post(const std::string &, const std::string &, const std::string &body) override {
		++refreshes;
		HttpResult r;
		if (body.find("refresh_token=" + valid_refresh) != std::string::npos) {
			r.status = 200;
			r.body = R"({"id_token":"NEW.ID.TOKEN","refresh_token":"R-rotated"})";
		} else {
			r.status = 400;
			r.body = R"({"error":"invalid_grant"})";
		}
		return r;
	}
};

} // namespace

TEST_CASE("session store: round trip, owner-only file, sign out, trust list") {
	const auto dir = fresh_dir("rt");
	SessionStore store(dir);
	CHECK_FALSE(store.load("srv:1", kIssuer, "voxel"));
	StoredSession s;
	s.server = "srv:1";
	s.issuer = kIssuer;
	s.client_id = "voxel";
	s.provider = "keycloak";
	s.refresh_token = "R1";
	s.label = "alice";
	REQUIRE(store.save(s));
	const auto back = store.load("srv:1", kIssuer, "voxel");
	REQUIRE(back);
	CHECK(back->refresh_token == "R1");
	CHECK(back->label == "alice");
	CHECK_FALSE(store.load("srv:1", kIssuer, "other-client"));
	CHECK(store.list().size() == 1);
#if !defined(_WIN32)
	for (const auto &e : std::filesystem::directory_iterator(dir)) {
		const auto perms = std::filesystem::status(e.path()).permissions();
		CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
				std::filesystem::perms::none);
	}
#endif
	CHECK_FALSE(store.is_trusted("srv:1", kIssuer));
	REQUIRE(store.trust("srv:1", kIssuer));
	CHECK(store.is_trusted("srv:1", kIssuer));
	CHECK_FALSE(store.is_trusted("srv:2", kIssuer)); // per (server, issuer)
	CHECK(store.list().size() == 1); // the trust file is not a session

	store.clear(); // "Sign out"
	CHECK(store.list().empty());
	CHECK(store.is_trusted("srv:1", kIssuer)); // trust survives sign-out
	std::filesystem::remove_all(dir);

	CHECK(label_from_id_token("not a jwt").empty());
}

// With a SecretBackend (the OS keychain) the refresh token lives there; the
// file keeps only metadata. A keychain that refuses the token leaves it in the
// file, and tokens an older client left in files move into the keychain.
TEST_CASE("session store: refresh tokens go to the OS keychain when there is one") {
	const auto dir = fresh_dir("keychain");
	auto secrets = std::make_shared<MemorySecretBackend>();
	StoredSession s;
	s.server = "srv:1";
	s.issuer = kIssuer;
	s.client_id = "voxel";
	s.refresh_token = "R-secret";
	const auto file_text = [&] {
		std::string all;
		for (const auto &e : std::filesystem::directory_iterator(dir)) {
			std::ifstream in(e.path());
			all += std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}
		return all;
	};
	{
		SessionStore store(dir, secrets);
		CHECK(store.secrets_name() == "memory");
		REQUIRE(store.save(s));
		CHECK(secrets->secrets.size() == 1);
		CHECK(file_text().find("R-secret") == std::string::npos);
		REQUIRE(store.load("srv:1", kIssuer, "voxel"));
		CHECK(store.load("srv:1", kIssuer, "voxel")->refresh_token == "R-secret");
		CHECK(store.list().size() == 1);
	}
	{
		// Another config dir never sees this one's keychain entry.
		const auto other = fresh_dir("keychain_other");
		SessionStore store(other, secrets);
		CHECK_FALSE(store.load("srv:1", kIssuer, "voxel"));
		std::filesystem::remove_all(other);
	}
	{
		// The keychain refuses: the token stays in the (0600) file, and the
		// stale keychain entry is dropped.
		secrets->fail = true;
		SessionStore store(dir, secrets);
		s.refresh_token = "R-file";
		REQUIRE(store.save(s));
		CHECK(file_text().find("R-file") != std::string::npos);
		secrets->fail = false;
		CHECK(store.load("srv:1", kIssuer, "voxel")->refresh_token == "R-file");
	}
	{
		// Opening the store with a working keychain moves the file token there.
		SessionStore store(dir, secrets);
		CHECK(file_text().find("R-file") == std::string::npos);
		CHECK(store.load("srv:1", kIssuer, "voxel")->refresh_token == "R-file");
		store.sign_out("srv:1");
		CHECK(secrets->secrets.empty());
		CHECK_FALSE(store.load("srv:1", kIssuer, "voxel"));
	}
	{
		// clear() removes keychain entries too.
		SessionStore store(dir, secrets);
		REQUIRE(store.save(s));
		CHECK(secrets->secrets.size() == 1);
		store.clear();
		CHECK(secrets->secrets.empty());
	}
	std::filesystem::remove_all(dir);
}

// The real platform keychain. Opt-in (VB_TEST_REAL_KEYCHAIN=1, set by the
// macOS and Windows CI legs) so a developer's test run never writes to theirs.
TEST_CASE("session store: the platform keychain round-trips a token") {
	const char *opt_in = std::getenv("VB_TEST_REAL_KEYCHAIN");
	if (opt_in == nullptr || std::string(opt_in) != "1") {
		return;
	}
	auto secrets = os_secret_backend();
	REQUIRE(secrets);
	const auto dir = fresh_dir("real_keychain");
	SessionStore store(dir, secrets);
	StoredSession s;
	s.server = "keychain-test:1";
	s.issuer = kIssuer;
	s.client_id = "voxel";
	s.refresh_token = "R-real-" + std::to_string(std::random_device{}());
	REQUIRE(store.save(s));
	const auto back = store.load("keychain-test:1", kIssuer, "voxel");
	REQUIRE(back);
	CHECK(back->refresh_token == s.refresh_token);
	for (const auto &e : std::filesystem::directory_iterator(dir)) {
		std::ifstream in(e.path());
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK(text.find(s.refresh_token) == std::string::npos); // really in the keychain
	}
	store.clear();
	CHECK_FALSE(store.load("keychain-test:1", kIssuer, "voxel"));
	std::filesystem::remove_all(dir);
}

// Saved logins are per server: two servers on the same identity provider
// don't share one, and signing out of one leaves the other.
TEST_CASE("session store: logins are per server, and sign_out is per server") {
	const auto dir = fresh_dir("per_server");
	{
		// A login saved before logins were per server (no "server" field).
		std::filesystem::create_directories(dir);
		std::ofstream legacy(dir / "0123abcd.json");
		legacy << R"({"issuer":"https://id.example","client_id":"voxel","refresh_token":"R-old"})";
	}
	SessionStore store(dir);
	CHECK_FALSE(std::filesystem::exists(dir / "0123abcd.json")); // removed, never reused
	CHECK(store.list().empty());

	for (const char *server : { "a.example:1", "b.example:1" }) {
		StoredSession s;
		s.server = server;
		s.issuer = kIssuer;
		s.client_id = "voxel";
		s.refresh_token = std::string("R-") + server;
		REQUIRE(store.save(s));
	}
	CHECK(store.load("a.example:1", kIssuer, "voxel")->refresh_token == "R-a.example:1");
	CHECK(store.load("b.example:1", kIssuer, "voxel")->refresh_token == "R-b.example:1");
	CHECK_FALSE(store.load("c.example:1", kIssuer, "voxel"));
	CHECK(store.list_for("a.example:1").size() == 1);

	store.sign_out("a.example:1");
	CHECK_FALSE(store.load("a.example:1", kIssuer, "voxel"));
	CHECK(store.load("b.example:1", kIssuer, "voxel"));
	std::filesystem::remove_all(dir);
}

TEST_CASE("coordinator: another server on the same provider does not reuse a saved login") {
	const auto dir = fresh_dir("other_server");
	auto store = std::make_shared<SessionStore>(dir);
	StoredSession s;
	s.server = "a.example:1";
	s.issuer = kIssuer;
	s.client_id = "voxel";
	s.provider = "keycloak";
	s.refresh_token = "R-valid";
	store->save(s);
	store->trust("b.example:1", kIssuer);

	auto idp = std::make_shared<RefreshIdp>();
	SignInCoordinator::Options o;
	o.http = idp;
	o.store = store;
	o.server_id = "b.example:1";
	SignInCoordinator coord(std::move(o));
	vb::protocol::S2CAuthChallenge ch;
	ch.provider = "keycloak";
	ch.issuer = kIssuer;
	ch.client_id = "voxel";
	ch.nonce = "n";
	auto ticket = coord.provider()(ch);
	CHECK_FALSE(coord.needs_trust());
	CHECK(coord.phase() == SignInCoordinator::Phase::kChoosing); // asks: no silent sign-in
	CHECK(idp->refreshes == 0); // a.example's refresh token never left the client
	CHECK_FALSE(ticket().done);
	std::filesystem::remove_all(dir);
}

TEST_CASE("coordinator: first use asks for trust, then a cached refresh token signs in silently") {
	const auto dir = fresh_dir("silent");
	auto store = std::make_shared<SessionStore>(dir);
	StoredSession s;
	s.server = "play.example:27015";
	s.issuer = kIssuer;
	s.client_id = "voxel";
	s.provider = "keycloak";
	s.refresh_token = "R-valid";
	store->save(s);

	auto idp = std::make_shared<RefreshIdp>();
	SignInCoordinator::Options o;
	o.http = idp;
	o.store = store;
	o.server_id = "play.example:27015";
	auto coord = std::make_unique<SignInCoordinator>(std::move(o));
	vb::net::ClientHandshake hs(vb::net::HandshakeClientConfig{});
	hs.set_sign_in_provider(coord->provider());
	hs.start();
	std::vector<std::byte> storage;
	vb::protocol::S2CServerInfo info;
	info.engine_protocol_version = vb::kEngineProtocolVersion;
	info.auth_mode = vb::protocol::AuthMode::kExternal;
	hs.on_frame(frame_of(info, storage));
	vb::protocol::S2CAuthChallenge ch;
	ch.provider = "keycloak";
	ch.issuer = kIssuer;
	ch.client_id = "voxel";
	ch.scopes = { "openid" };
	ch.nonce = "n";
	hs.on_frame(frame_of(ch, storage));

	// Untrusted server: nothing is sent to the IdP, not even the refresh token.
	CHECK(coord->needs_trust());
	CHECK(coord->phase() == SignInCoordinator::Phase::kChoosing);
	CHECK(idp->refreshes == 0);
	coord->start_browser(); // refused until trusted
	CHECK(coord->phase() == SignInCoordinator::Phase::kChoosing);

	coord->trust();
	CHECK_FALSE(coord->needs_trust());
	for (int i = 0; i < 400 && hs.status() == vb::net::ClientHandshakeStatus::kSigningIn; ++i) {
		auto step = hs.poll();
		if (!step.send.empty()) {
			std::size_t consumed = 0;
			auto f = vb::protocol::read_frame(step.send[0].bytes, consumed);
			CHECK(vb::protocol::C2SAuth::decode(f->payload)->token == "NEW.ID.TOKEN");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(hs.status() == vb::net::ClientHandshakeStatus::kAuthenticating);
	CHECK(idp->refreshes == 1);
	// The rotated refresh token was stored.
	CHECK(store->load("play.example:27015", kIssuer, "voxel")->refresh_token == "R-rotated");
	std::filesystem::remove_all(dir);
}

TEST_CASE("coordinator: a dead refresh token is forgotten and falls back to the chooser") {
	const auto dir = fresh_dir("dead");
	auto store = std::make_shared<SessionStore>(dir);
	StoredSession s;
	s.server = "srv:1";
	s.issuer = kIssuer;
	s.client_id = "voxel";
	s.provider = "keycloak";
	s.refresh_token = "R-revoked";
	store->save(s);
	store->trust("srv:1", kIssuer);

	auto idp = std::make_shared<RefreshIdp>();
	SignInCoordinator::Options o;
	o.http = idp;
	o.store = store;
	o.server_id = "srv:1";
	SignInCoordinator coord(std::move(o));
	vb::protocol::S2CAuthChallenge ch;
	ch.provider = "keycloak";
	ch.issuer = kIssuer;
	ch.client_id = "voxel";
	ch.nonce = "n";
	auto ticket = coord.provider()(ch);
	for (int i = 0; i < 400 && coord.phase() == SignInCoordinator::Phase::kWorking; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(coord.phase() == SignInCoordinator::Phase::kChoosing);
	CHECK(coord.last_error() == "Your saved sign-in expired");
	CHECK_FALSE(store->load("srv:1", kIssuer, "voxel")); // forgotten
	CHECK_FALSE(ticket().done);
	std::filesystem::remove_all(dir);
}

TEST_CASE("coordinator: a re-auth request is answered silently, else it raises the prompt") {
	const auto dir = fresh_dir("reauth");
	auto store = std::make_shared<SessionStore>(dir);
	StoredSession s;
	s.server = "srv:1";
	s.issuer = kIssuer;
	s.client_id = "voxel";
	s.provider = "keycloak";
	s.refresh_token = "R-valid";
	store->save(s);
	store->trust("srv:1", kIssuer);

	auto idp = std::make_shared<RefreshIdp>();
	SignInCoordinator::Options o;
	o.http = idp;
	o.store = store;
	o.server_id = "srv:1";
	SignInCoordinator coord(std::move(o));
	vb::protocol::S2CAuthChallenge ch;
	ch.provider = "keycloak";
	ch.issuer = kIssuer;
	ch.client_id = "voxel";
	ch.nonce = "join-nonce";
	auto join_ticket = coord.provider()(ch);
	vb::net::TokenPoll p;
	for (int i = 0; i < 400 && !(p = join_ticket()).done; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	REQUIRE(p.done);
	CHECK(p.token == "NEW.ID.TOKEN");

	// Silent re-auth works while the rotated refresh token is valid.
	idp->valid_refresh = "R-rotated";
	vb::protocol::S2CReauthRequest req;
	req.nonce = "reauth-nonce";
	req.grace_seconds = 120;
	auto ticket = coord.reauth_provider()(req);
	for (int i = 0; i < 400 && !(p = ticket()).done; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	REQUIRE(p.done);
	CHECK(p.token == "NEW.ID.TOKEN");
	CHECK_FALSE(coord.reauth_prompt_active());

	// Revoked at the IdP: silent refresh fails, a non-blocking prompt appears and
	// the ticket stays pending until the player signs in again (or the server's
	// grace period runs out).
	idp->valid_refresh = "nothing-matches";
	ticket = coord.reauth_provider()(req);
	for (int i = 0; i < 400 && coord.phase() == SignInCoordinator::Phase::kWorking; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(coord.reauth_prompt_active());
	CHECK_FALSE(ticket().done);
	CHECK(coord.supports_browser());
	std::filesystem::remove_all(dir);
}

#endif // VB_WITH_AUTH
