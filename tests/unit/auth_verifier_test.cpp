// In-engine authentication, step 9.3 (architecture_spec/auth.md §5.3): JWT
// parsing, JWKS parsing, the TokenVerifier rules 1-7 and the AuthService key
// cache. Tokens are minted in-test with freshly generated keys.

#include <doctest/doctest.h>

#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "vb/auth/crypto.hpp"
#include "vb/auth/http.hpp"
#include "vb/auth/jwks.hpp"
#include "vb/auth/jwt.hpp"
#include "vb/auth/service.hpp"
#include "vb/auth/verifier.hpp"

#if defined(VB_WITH_AUTH)

using namespace vb::auth;
using nlohmann::json;
using vb::auth::testing::TestSigner;

namespace {

constexpr std::int64_t kNow = 1'800'000'000;
constexpr const char *kIssuer = "https://id.example/realms/vb";

const TestSigner &rsa_key() {
	static const TestSigner k = TestSigner::rsa(2048);
	return k;
}
const TestSigner &ec_key() {
	static const TestSigner k = TestSigner::p256();
	return k;
}

std::string b64(const std::string &s) {
	return base64url_encode({ reinterpret_cast<const std::uint8_t *>(s.data()), s.size() });
}

std::string sign_token(const TestSigner &key, const json &header, const json &payload) {
	const std::string input = b64(header.dump()) + "." + b64(payload.dump());
	const auto sig = key.sign(input);
	return input + "." + base64url_encode(sig);
}

json good_claims() {
	return json{ { "iss", kIssuer }, { "aud", "voxel" }, { "sub", "user-1" }, { "typ", "ID" },
		{ "exp", kNow + 600 }, { "iat", kNow - 10 }, { "preferred_username", "alice" },
		{ "email", "a@example.com" }, { "secret", "not exposed" } };
}

std::string good_token(const TestSigner &key, json claims = good_claims(),
		const std::string &kid = "k1") {
	const char *alg = key.alg() == JwsAlg::kRS256 ? "RS256" : "ES256";
	return sign_token(key, json{ { "alg", alg }, { "kid", kid }, { "typ", "JWT" } }, claims);
}

AuthConfig config() {
	AuthConfig c;
	c.provider = Provider::kKeycloak;
	c.issuer = kIssuer;
	c.client_id = "voxel";
	c.name_claim = "preferred_username";
	c.claims = { "email" };
	c.max_token_age_seconds = 300;
	return c;
}

KeySet keyset() {
	KeySet ks;
	ks["k1"] = rsa_key().public_key();
	ks["e1"] = ec_key().public_key();
	return ks;
}

VerifyResult run(const std::string &token, const std::string &nonce = "n1",
		std::int64_t now = kNow) {
	return verify_id_token(config(), keyset(), token, nonce, now);
}

std::string jwk_rsa(const PublicKey &k, const std::string &kid) {
	return json{ { "kty", "RSA" }, { "kid", kid }, { "use", "sig" },
		{ "n", base64url_encode(k.rsa_n) }, { "e", base64url_encode(k.rsa_e) } }
			.dump();
}

} // namespace

TEST_CASE("base64url: round trip, canonical form only") {
	for (const std::string s : { "", "f", "fo", "foo", "foob", "fooba", "foobar" }) {
		const std::string enc = b64(s);
		CHECK(enc.find('=') == std::string::npos);
		auto dec = base64url_decode(enc);
		REQUIRE(dec);
		CHECK(std::string(dec->begin(), dec->end()) == s);
	}
	CHECK_FALSE(base64url_decode("Zg=="));   // padding
	CHECK_FALSE(base64url_decode("Zm+/"));   // standard alphabet
	CHECK_FALSE(base64url_decode("Zm 9"));   // whitespace
	CHECK_FALSE(base64url_decode("Zh"));     // non-zero trailing bits
	CHECK_FALSE(base64url_decode("A"));      // impossible length
}

TEST_CASE("parse_jws rejects structurally bad tokens without crashing") {
	for (const char *bad : { "", "a", "a.b", "a.b.c.d", "..", "a..c", "!.@.#" }) {
		CHECK_FALSE(parse_jws(bad).jws);
	}
	CHECK_FALSE(parse_jws(std::string(kMaxJwtBytes + 1, 'a')).jws);
	const auto ok = parse_jws(good_token(rsa_key()));
	REQUIRE(ok.jws);
	CHECK(json::parse(ok.jws->payload_json)["sub"] == "user-1");
	CHECK(redact_token("abc.def.ghi") == "<jwt len=11>");
}

TEST_CASE("parse_jwks keeps usable keys and skips the rest") {
	const std::string doc = std::string("{\"keys\":[") + jwk_rsa(rsa_key().public_key(), "r1") +
			"," +
			json{ { "kty", "EC" }, { "crv", "P-256" }, { "kid", "e1" },
				{ "x", base64url_encode(ec_key().public_key().ec_x) },
				{ "y", base64url_encode(ec_key().public_key().ec_y) } }
					.dump() +
			",{\"kty\":\"oct\",\"kid\":\"h\",\"k\":\"AAAA\"}" +
			",{\"kty\":\"RSA\",\"kid\":\"small\",\"n\":\"AQAB\",\"e\":\"AQAB\"}" +
			",{\"kty\":\"RSA\",\"n\":\"AQAB\",\"e\":\"AQAB\"}" +
			",{\"kty\":\"EC\",\"crv\":\"P-384\",\"kid\":\"p384\",\"x\":\"AA\",\"y\":\"AA\"}" +
			",{\"kty\":\"RSA\",\"kid\":\"enc\",\"use\":\"enc\"}]}";
	const JwksParse p = parse_jwks(doc);
	CHECK(p.error.empty());
	CHECK(p.keys.size() == 2);
	CHECK(p.keys.count("r1") == 1);
	CHECK(p.keys.count("e1") == 1);
	CHECK(p.skipped == 5);

	CHECK_FALSE(parse_jwks("not json").error.empty());
	CHECK_FALSE(parse_jwks("{}").error.empty());
	CHECK_FALSE(parse_jwks("{\"keys\":[]}").error.empty());
	CHECK_FALSE(parse_jwks("[]").error.empty());

	// A parsed key verifies a token end to end.
	KeySet ks = parse_jwks(doc).keys;
	CHECK(verify_id_token(config(), ks, good_token(rsa_key(), good_claims(), "r1"), "n1",
				  kNow)
					.ok());
}

TEST_CASE("verifier accepts valid RS256 and ES256 tokens and exposes only allowlisted claims") {
	const VerifyResult r = run(good_token(rsa_key()));
	REQUIRE(r.ok());
	CHECK(r.login.subject == "user-1");
	CHECK(r.login.name == "alice");
	CHECK(r.login.issuer == kIssuer);
	CHECK(r.login.provider == Provider::kKeycloak);
	CHECK(r.login.issued_at == kNow - 10);
	CHECK(r.login.expires_at == kNow + 600);
	const json claims = json::parse(r.login.claims_json);
	CHECK(claims.size() == 1);
	CHECK(claims["email"] == "a@example.com");
	CHECK(r.login.claims_json.find("secret") == std::string::npos);

	CHECK(run(good_token(ec_key(), good_claims(), "e1")).ok());
}

TEST_CASE("rule 1: algorithm allowlist and kid") {
	const json c = good_claims();
	// alg=none, HS256, anything else.
	for (const char *alg : { "none", "HS256", "PS256", "ES384", "RS512" }) {
		const std::string t = sign_token(rsa_key(), json{ { "alg", alg }, { "kid", "k1" } }, c);
		CHECK(run(t).error == VerifyError::kAlgorithm);
	}
	CHECK(run(sign_token(rsa_key(), json{ { "kid", "k1" } }, c)).error == VerifyError::kAlgorithm);
	CHECK(run(sign_token(rsa_key(), json{ { "alg", "RS256" } }, c)).error ==
			VerifyError::kMissingKid);
	CHECK(run(sign_token(rsa_key(), json{ { "alg", "RS256" }, { "kid", "" } }, c)).error ==
			VerifyError::kMissingKid);
	// alg claims ES256 but kid names the RSA key: type confusion is refused.
	CHECK(run(sign_token(rsa_key(), json{ { "alg", "ES256" }, { "kid", "k1" } }, c)).error ==
			VerifyError::kAlgorithm);
	CHECK(run("garbage").error == VerifyError::kMalformed);
}

TEST_CASE("rules 2-3: unknown kid and signature") {
	CHECK(run(good_token(rsa_key(), good_claims(), "nope")).error == VerifyError::kUnknownKid);
	CHECK(peek_kid(good_token(rsa_key())) == "k1");

	// Tampered payload keeps the old signature.
	const std::string t = good_token(rsa_key());
	const auto d1 = t.find('.');
	const auto d2 = t.find('.', d1 + 1);
	json evil = good_claims();
	evil["sub"] = "admin";
	const std::string forged = t.substr(0, d1 + 1) + b64(evil.dump()) + t.substr(d2);
	CHECK(run(forged).error == VerifyError::kBadSignature);

	// Signed by a different key under a known kid.
	static const TestSigner other = TestSigner::rsa(2048);
	CHECK(run(good_token(other)).error == VerifyError::kBadSignature);
	// ES256 signature of the wrong length.
	std::string es = good_token(ec_key(), good_claims(), "e1");
	es.resize(es.size() - 4);
	CHECK_FALSE(run(es).ok());
}

TEST_CASE("rule 4: issuer, audience, authorized party") {
	json c = good_claims();
	c["iss"] = "https://evil.example";
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kIssuer);
	c = good_claims();
	c.erase("iss");
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kIssuer);

	c = good_claims();
	c["aud"] = "someone-else";
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kAudience);
	c["aud"] = json::array({ "other", "voxel" });
	CHECK(run(good_token(rsa_key(), c)).ok());
	c["aud"] = json::array({ "other" });
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kAudience);
	c.erase("aud");
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kAudience);

	c = good_claims();
	c["azp"] = "voxel";
	CHECK(run(good_token(rsa_key(), c)).ok());
	c["azp"] = "other-client";
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kAuthorizedParty);
}

TEST_CASE("rule 5: expiry, not-before, issued-at, freshness") {
	json c = good_claims();
	c["exp"] = kNow - 120;
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kExpired);
	c["exp"] = kNow - 30; // inside the 60 s skew
	CHECK(run(good_token(rsa_key(), c)).ok());
	c.erase("exp");
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kExpired);
	c["exp"] = "soon";
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kExpired);

	c = good_claims();
	c["nbf"] = kNow + 30;
	CHECK(run(good_token(rsa_key(), c)).ok());
	c["nbf"] = kNow + 120;
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kNotYetValid);

	c = good_claims();
	c["iat"] = kNow + 120;
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kNotYetValid);
	c["iat"] = kNow - 301; // older than max_token_age_seconds
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kTooOld);
	c["iat"] = kNow - 300;
	CHECK(run(good_token(rsa_key(), c)).ok());
	c.erase("iat");
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kTooOld);
}

TEST_CASE("rule 6: nonce binds the token to the connection when present") {
	json c = good_claims();
	c["nonce"] = "n1";
	CHECK(run(good_token(rsa_key(), c), "n1").ok());
	CHECK(run(good_token(rsa_key(), c), "other").error == VerifyError::kNonce);
	CHECK(run(good_token(rsa_key(), c), "").error == VerifyError::kNonce);
	c["nonce"] = 5;
	CHECK(run(good_token(rsa_key(), c), "n1").error == VerifyError::kNonce);
	// Absent (refresh-derived / Firebase password): bounded by freshness instead.
	CHECK(run(good_token(rsa_key()), "n1").ok());
}

TEST_CASE("rule 7: subject and player name") {
	json c = good_claims();
	c.erase("sub");
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kSubject);
	c["sub"] = "";
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kSubject);
	c["sub"] = std::string(257, 'x');
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kSubject);

	// Missing name claim falls back to a subject-derived name.
	c = good_claims();
	c.erase("preferred_username");
	c["sub"] = "abc-123/!!";
	auto r = run(good_token(rsa_key(), c));
	REQUIRE(r.ok());
	CHECK(r.login.name == "abc-123");

	// Nothing usable at all.
	c["sub"] = "///";
	CHECK(run(good_token(rsa_key(), c)).error == VerifyError::kName);

	// Control characters are stripped, long names truncated on a UTF-8 boundary.
	CHECK(sanitize_player_name("  Al\nic\te  ") == "Alice");
	CHECK(sanitize_player_name("\x01\x02").empty());
	const std::string long_name(40, 'a');
	CHECK(sanitize_player_name(long_name).size() == 32);
	// The 32-byte cut must not leave a trailing space (sanitizing is idempotent).
	CHECK(sanitize_player_name(std::string(31, 'a') + " bcd") == std::string(31, 'a'));
	const std::string utf8 = std::string(31, 'a') + "\xC3\xA9\xC3\xA9"; // é é
	const std::string cut = sanitize_player_name(utf8);
	CHECK(cut.size() == 31);
	c = good_claims();
	c["preferred_username"] = "Bob\n";
	CHECK(run(good_token(rsa_key(), c)).login.name == "Bob");
}

TEST_CASE("public reasons are coarse") {
	CHECK(public_reason(VerifyError::kExpired) == "sign-in expired");
	CHECK(public_reason(VerifyError::kTooOld) == "sign-in expired");
	CHECK(public_reason(VerifyError::kBadSignature) == "not accepted by this server");
	CHECK(public_reason(VerifyError::kUnknownKid) == "authentication service unavailable");
}

// ---------------------------------------------------------------------------
// AuthService: discovery, JWKS cache, pending verification
// ---------------------------------------------------------------------------

namespace {

class FakeHttp final : public HttpFetcher {
public:
	std::map<std::string, HttpResult> responses;
	std::vector<std::string> requests;
	HttpResult get(const std::string &url) override {
		requests.push_back(url);
		auto it = responses.find(url);
		if (it == responses.end()) {
			HttpResult r;
			r.error = "no route";
			return r;
		}
		return it->second;
	}
	int count(const std::string &url) const {
		int n = 0;
		for (const auto &r : requests) {
			n += r == url ? 1 : 0;
		}
		return n;
	}
};

const std::string kDiscoveryUrl = std::string(kIssuer) + "/.well-known/openid-configuration";
const std::string kJwksUrl = "https://id.example/realms/vb/certs";

HttpResult ok(std::string body) {
	HttpResult r;
	r.status = 200;
	r.body = std::move(body);
	return r;
}

struct ServiceFixture {
	std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
	std::int64_t clock = kNow;
	std::unique_ptr<AuthService> svc;

	explicit ServiceFixture(bool with_keys = true) {
		http->responses[kDiscoveryUrl] = ok(
				json{ { "issuer", kIssuer }, { "jwks_uri", kJwksUrl } }.dump());
		if (with_keys) {
			set_keys({ { "k1", rsa_key().public_key() } });
		}
		AuthService::Options o;
		o.start_thread = false;
		o.clock = [this] { return clock; };
		svc = std::make_unique<AuthService>(config(), http, o);
	}
	void set_keys(const std::map<std::string, PublicKey> &keys) {
		std::string body = "{\"keys\":[";
		bool first = true;
		for (const auto &[kid, k] : keys) {
			body += (first ? "" : ",") + jwk_rsa(k, kid);
			first = false;
		}
		http->responses[kJwksUrl] = ok(body + "]}");
	}
	void load() {
		while (svc->pump()) {
		}
	}
};

} // namespace

TEST_CASE("service: discovery then JWKS makes it ready; a good token verifies") {
	ServiceFixture f;
	CHECK_FALSE(f.svc->ready());
	f.load();
	CHECK(f.svc->ready());
	CHECK(f.http->count(kDiscoveryUrl) == 1);
	CHECK(f.http->count(kJwksUrl) == 1);

	const Verdict v = f.svc->verify_now(good_token(rsa_key()), "n1");
	CHECK(v.ok);
	CHECK(v.login.subject == "user-1");
	CHECK_FALSE(f.svc->new_nonce().empty());
	CHECK(f.svc->new_nonce() != f.svc->new_nonce());
}

TEST_CASE("service: fails closed while no key set has loaded; pending resolves after load") {
	ServiceFixture f;
	auto pending = f.svc->begin(good_token(rsa_key()), "n1");
	CHECK_FALSE(pending->poll().has_value()); // cold start: waits
	CHECK_FALSE(pending->poll().has_value());
	f.load();
	auto v = pending->poll();
	REQUIRE(v.has_value());
	CHECK(v->ok);
}

TEST_CASE("service: pending verify gives up with 'unavailable' when keys never load") {
	ServiceFixture f(/*with_keys=*/false);
	f.http->responses.erase(kJwksUrl);
	auto pending = f.svc->begin(good_token(rsa_key()), "n1");
	CHECK_FALSE(pending->poll().has_value());
	f.load();
	f.clock += 11; // past verify_wait_seconds
	auto v = pending->poll();
	REQUIRE(v.has_value());
	CHECK_FALSE(v->ok);
	CHECK(v->reason == "authentication service unavailable");
	CHECK(v->detail.find("eyJ") == std::string::npos); // never echoes token bytes
}

TEST_CASE("service: discovery with a mismatched issuer is refused") {
	ServiceFixture f;
	f.http->responses[kDiscoveryUrl] =
			ok(json{ { "issuer", "https://evil.example" }, { "jwks_uri", kJwksUrl } }.dump());
	f.load();
	CHECK_FALSE(f.svc->ready());
	CHECK(f.http->count(kJwksUrl) == 0);

	// A non-https jwks_uri is refused too.
	f.clock += 400;
	f.http->responses[kDiscoveryUrl] = ok(
			json{ { "issuer", kIssuer }, { "jwks_uri", "http://evil.example/keys" } }.dump());
	f.load();
	CHECK_FALSE(f.svc->ready());
	CHECK(f.http->count(kJwksUrl) == 0);
}

TEST_CASE("service: failed fetches back off instead of hammering the IdP") {
	ServiceFixture f;
	f.http->responses.erase(kDiscoveryUrl);
	f.load();
	const int after_first = f.http->count(kDiscoveryUrl);
	CHECK(after_first == 1);
	f.load(); // same instant: not due yet
	CHECK(f.http->count(kDiscoveryUrl) == after_first);
	f.clock += 11;
	f.load();
	CHECK(f.http->count(kDiscoveryUrl) == after_first + 1);
}

TEST_CASE("service: unknown kid triggers one rate-limited JWKS refresh") {
	ServiceFixture f;
	f.load();
	REQUIRE(f.svc->ready());
	CHECK(f.http->count(kJwksUrl) == 1);

	// Key rotation: the IdP now also publishes "k2". Inside the 60 s window the
	// refresh is refused, so the unknown kid fails at once.
	static const TestSigner rotated = TestSigner::rsa(2048);
	f.set_keys({ { "k1", rsa_key().public_key() }, { "k2", rotated.public_key() } });
	auto early = f.svc->begin(good_token(rotated, good_claims(), "k2"), "n1");
	auto r0 = early->poll();
	REQUIRE(r0.has_value());
	CHECK_FALSE(r0->ok);
	CHECK(f.http->count(kJwksUrl) == 1);

	f.clock += 61;
	auto p = f.svc->begin(good_token(rotated, good_claims(), "k2"), "n1");
	CHECK_FALSE(p->poll().has_value()); // waits for the refresh
	// A second unknown-kid token while the refresh is pending shares it.
	auto p2 = f.svc->begin(good_token(rotated, good_claims(), "k2"), "n1");
	CHECK_FALSE(p2->poll().has_value());
	f.load();
	CHECK(f.http->count(kJwksUrl) == 2); // one fetch served both
	auto v = p->poll();
	REQUIRE(v.has_value());
	CHECK(v->ok);
	CHECK(p2->poll()->ok);

	// A hostile kid never makes the engine refresh again inside the window.
	auto bad = f.svc->begin(good_token(rotated, good_claims(), "nope"), "n1");
	auto vb = bad->poll();
	REQUIRE(vb.has_value());
	CHECK_FALSE(vb->ok);
	f.load();
	CHECK(f.http->count(kJwksUrl) == 2);
}

TEST_CASE("service: a bad token resolves immediately with a coarse reason") {
	ServiceFixture f;
	f.load();
	json c = good_claims();
	c["exp"] = kNow - 1000;
	auto p = f.svc->begin(good_token(rsa_key(), c), "n1");
	auto v = p->poll();
	REQUIRE(v.has_value());
	CHECK_FALSE(v->ok);
	CHECK(v->reason == "sign-in expired");
	CHECK(v->detail.rfind("expired", 0) == 0);
}

TEST_CASE("service: Firebase preset fetches the fixed Google JWKS") {
	auto http = std::make_shared<FakeHttp>();
	const std::string url =
			"https://www.googleapis.com/service_accounts/v1/jwk/securetoken@system.gserviceaccount.com";
	http->responses[url] = ok(std::string("{\"keys\":[") + jwk_rsa(rsa_key().public_key(), "k1") + "]}");
	AuthConfig c = config();
	c.provider = Provider::kFirebase;
	c.issuer = "https://securetoken.google.com/my-game";
	c.client_id = "my-game";
	AuthService::Options o;
	o.start_thread = false;
	o.clock = [] { return kNow; };
	AuthService svc(c, http, o);
	while (svc.pump()) {
	}
	CHECK(svc.ready());
	CHECK(http->count(url) == 1);
	json claims = good_claims();
	claims["iss"] = c.issuer;
	claims["aud"] = "my-game";
	CHECK(svc.verify_now(good_token(rsa_key(), claims), "n").ok);
}

TEST_CASE("url policy: https or loopback http only") {
	CHECK(url_allowed("https://id.example/x"));
	CHECK(url_allowed("http://127.0.0.1:8080/x"));
	CHECK(url_allowed("http://localhost/x"));
	CHECK_FALSE(url_allowed("http://id.example/x"));
	CHECK_FALSE(url_allowed("http://localhost.evil.example/x"));
	CHECK_FALSE(url_allowed("file:///etc/passwd"));
	CHECK_FALSE(url_allowed("https://"));
}

// Deterministic mutation fuzz (same policy as the protocol decoders): flipping,
// truncating and splicing bytes of valid inputs must never crash or accept.
TEST_CASE("fuzz: mutated tokens and JWKS documents never crash and never verify") {
	const std::string token = good_token(rsa_key());
	const std::string jwks = std::string("{\"keys\":[") + jwk_rsa(rsa_key().public_key(), "k1") + "]}";
	std::uint32_t rng = 0x12345678u;
	auto next = [&] {
		rng = rng * 1664525u + 1013904223u;
		return rng >> 8;
	};
	for (int i = 0; i < 400; ++i) {
		std::string t = token;
		std::string j = jwks;
		for (int m = 0; m < 1 + static_cast<int>(next() % 4); ++m) {
			t[next() % t.size()] = static_cast<char>(next());
			j[next() % j.size()] = static_cast<char>(next());
		}
		if (i % 5 == 0) {
			t.resize(next() % t.size());
			j.resize(next() % j.size());
		}
		const VerifyResult r = run(t);
		// A mutated token may only still verify if the mutation was a no-op.
		CHECK((!r.ok() || t == token));
		(void)parse_jwks(j);
		(void)parse_jws(t);
		(void)base64url_decode(t);
	}
}

TEST_CASE("log redaction: tokens are never part of a verdict detail") {
	ServiceFixture f;
	f.load();
	const std::string token = good_token(ec_key(), good_claims(), "bogus-kid");
	const Verdict v = f.svc->verify_now(token, "n1");
	CHECK_FALSE(v.ok);
	CHECK(v.detail.find(token.substr(0, 20)) == std::string::npos);
	CHECK(v.reason.find(token.substr(0, 20)) == std::string::npos);
}

#endif // VB_WITH_AUTH
