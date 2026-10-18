// Fuzz target: the auth parsers that see attacker-controlled bytes -- ID
// tokens (JWS parsing, base64url, kid peeking, redaction, full verification),
// JWKS documents fetched from the identity provider, the HTTP request head the
// loopback sign-in listener reads, and player-name sanitizing.
//
// Input layout: byte 0 picks the parser, the rest is its input.

#include "fuzz.hpp"

#include <set>
#include <string>
#include <string_view>

#include "vb/auth/crypto.hpp"
#include "vb/auth/jwks.hpp"
#include "vb/auth/jwt.hpp"
#include "vb/auth/oidc_client.hpp"
#include "vb/auth/session_store.hpp"
#include "vb/auth/verifier.hpp"

using namespace vb::auth;

namespace {

constexpr std::int64_t kNow = 1'800'000'000;
constexpr const char *kIssuer = "https://id.example/realms/vb";
constexpr const char *kState = "s7aTe";

enum Selector : std::uint8_t {
	kToken = 0,
	kJwks = 1,
	kRedirect = 2,
	kName = 3,
	kSelectorCount = 4,
};

struct Fixture {
	testing::TestSigner rsa = testing::TestSigner::rsa(2048);
	testing::TestSigner ec = testing::TestSigner::p256();
	AuthConfig config;
	KeySet keys;
	std::set<std::string> valid_tokens; // the only tokens allowed to verify

	Fixture() {
		config.provider = Provider::kKeycloak;
		config.issuer = kIssuer;
		config.client_id = "voxel";
		config.name_claim = "preferred_username";
		config.claims = { "email" };
		config.max_token_age_seconds = 300;
		keys["k1"] = rsa.public_key();
		keys["e1"] = ec.public_key();
		valid_tokens.insert(token(rsa, "RS256", "k1"));
		valid_tokens.insert(token(ec, "ES256", "e1"));
	}

	static std::string b64(std::string_view s) {
		return base64url_encode({ reinterpret_cast<const std::uint8_t *>(s.data()), s.size() });
	}

	std::string token(const testing::TestSigner &key, const char *alg, const char *kid) const {
		const std::string header = std::string(R"({"alg":")") + alg + R"(","kid":")" + kid + R"(","typ":"JWT"})";
		const std::string payload = std::string(R"({"iss":")") + kIssuer +
				R"(","aud":"voxel","sub":"user-1","typ":"ID","exp":)" + std::to_string(kNow + 600) +
				R"(,"iat":)" + std::to_string(kNow - 10) +
				R"(,"nonce":"n1","preferred_username":"alice","email":"a@example.com"})";
		const std::string input = b64(header) + "." + b64(payload);
		return input + "." + base64url_encode(key.sign(input));
	}

	std::string jwks() const {
		const PublicKey &k = rsa.public_key();
		return std::string(R"({"keys":[{"kty":"RSA","kid":"k1","use":"sig","alg":"RS256","n":")") +
				base64url_encode(k.rsa_n) + R"(","e":")" + base64url_encode(k.rsa_e) +
				R"("},{"kty":"oct","kid":"x","k":"AAAA"}]})";
	}
};

const Fixture &fixture() {
	static const Fixture f;
	return f;
}

std::vector<std::uint8_t> seed(Selector s, std::string_view body) {
	std::vector<std::uint8_t> out{ s };
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	if (size == 0) {
		return 0;
	}
	const auto selector = static_cast<Selector>(data[0] % kSelectorCount);
	const std::string_view in(reinterpret_cast<const char *>(data + 1), size - 1);
	const Fixture &f = fixture();
	switch (selector) {
		case kToken: {
			(void)base64url_decode(in);
			(void)peek_kid(in);
			(void)label_from_id_token(std::string(in));
			const std::string redacted = redact_token(in);
			// Redaction never reproduces a whole token longer than its kept prefix.
			VB_FUZZ_CHECK(in.size() < 16 || redacted != in);
			if (auto parsed = parse_jws(in); parsed.jws) {
				VB_FUZZ_CHECK(parsed.error.empty());
			}
			const VerifyResult r = verify_id_token(f.config, f.keys, in, "n1", kNow);
			VB_FUZZ_CHECK(!r.ok() || f.valid_tokens.count(std::string(in)) == 1);
			break;
		}
		case kJwks: {
			const JwksParse p = parse_jwks(in);
			VB_FUZZ_CHECK(p.keys.empty() != p.error.empty());
			break;
		}
		case kRedirect: {
			const RedirectParse p = parse_redirect_request(in, kState);
			VB_FUZZ_CHECK(p.code.empty() || p.error.empty());
			break;
		}
		case kName: {
			const std::string name = sanitize_player_name(in);
			// Sanitizing is idempotent.
			VB_FUZZ_CHECK(sanitize_player_name(name) == name);
			break;
		}
		default:
			break;
	}
	return 0;
}

std::vector<std::vector<std::uint8_t>> vb_fuzz_seeds() {
	const Fixture &f = fixture();
	std::vector<std::vector<std::uint8_t>> seeds;
	for (const std::string &t : f.valid_tokens) {
		seeds.push_back(seed(kToken, t));
	}
	seeds.push_back(seed(kToken, "eyJhbGciOiJub25lIn0.e30."));
	seeds.push_back(seed(kJwks, f.jwks()));
	seeds.push_back(seed(kRedirect, std::string("GET /callback?code=abc%20def&state=") + kState + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
	seeds.push_back(seed(kRedirect, "GET /callback?error=access_denied&state=x HTTP/1.1\r\n\r\n"));
	seeds.push_back(seed(kName, "Alice the Great!"));
	seeds.push_back(seed(kName, "\xc3\xa9t\xc3\xa9#2  "));
	return seeds;
}
