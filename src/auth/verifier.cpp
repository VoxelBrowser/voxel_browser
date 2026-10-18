#include "vb/auth/verifier.hpp"

#include <cctype>
#include <cmath>

#include <nlohmann/json.hpp>

#include "vb/auth/crypto.hpp"
#include "vb/auth/jwt.hpp"

namespace vb::auth {

namespace {

using nlohmann::json;

VerifyResult fail(VerifyError e, std::string detail) {
	VerifyResult r;
	r.error = e;
	r.detail = std::move(detail);
	return r;
}

std::string lower(std::string s) {
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

// RFC 9068 access tokens carry the JWS header `typ` "at+jwt" (or the full media type).
bool is_access_token_typ(std::string typ) {
	typ = lower(std::move(typ));
	constexpr std::string_view kPrefix = "application/";
	if (typ.rfind(kPrefix, 0) == 0) {
		typ.erase(0, kPrefix.size());
	}
	return typ == "at+jwt";
}

// Integer NumericDate (seconds). JWT allows fractions; we floor them.
std::optional<std::int64_t> numeric_date(const json &o, const char *key) {
	auto it = o.find(key);
	if (it == o.end() || !it->is_number()) {
		return std::nullopt;
	}
	const double d = it->get<double>();
	if (!std::isfinite(d) || std::fabs(d) > 1e12) {
		return std::nullopt;
	}
	return static_cast<std::int64_t>(std::floor(d));
}

} // namespace

std::string_view to_string(VerifyError e) {
	switch (e) {
		case VerifyError::kNone:
			return "none";
		case VerifyError::kMalformed:
			return "malformed";
		case VerifyError::kAlgorithm:
			return "algorithm";
		case VerifyError::kMissingKid:
			return "missing_kid";
		case VerifyError::kTokenType:
			return "token_type";
		case VerifyError::kUnknownKid:
			return "unknown_kid";
		case VerifyError::kBadSignature:
			return "bad_signature";
		case VerifyError::kIssuer:
			return "issuer";
		case VerifyError::kAudience:
			return "audience";
		case VerifyError::kAuthorizedParty:
			return "authorized_party";
		case VerifyError::kExpired:
			return "expired";
		case VerifyError::kNotYetValid:
			return "not_yet_valid";
		case VerifyError::kTooOld:
			return "too_old";
		case VerifyError::kNonce:
			return "nonce";
		case VerifyError::kSubject:
			return "subject";
		case VerifyError::kName:
			return "name";
		case VerifyError::kClaims:
			return "claims";
	}
	return "unknown";
}

std::string_view public_reason(VerifyError e) {
	switch (e) {
		case VerifyError::kExpired:
		case VerifyError::kNotYetValid:
		case VerifyError::kTooOld:
			return "sign-in expired";
		case VerifyError::kUnknownKid:
			return "authentication service unavailable";
		default:
			return "not accepted by this server";
	}
}

std::string sanitize_player_name(std::string_view raw) {
	std::string out;
	for (const char ch : raw) {
		const auto c = static_cast<unsigned char>(ch);
		if (c < 0x20 || c == 0x7F) {
			continue; // control characters (incl. newlines/tabs)
		}
		out.push_back(ch);
	}
	// Trim spaces.
	std::size_t b = 0;
	while (b < out.size() && out[b] == ' ') {
		++b;
	}
	std::size_t e = out.size();
	while (e > b && out[e - 1] == ' ') {
		--e;
	}
	out = out.substr(b, e - b);
	if (out.size() > 32) {
		std::size_t cut = 32;
		// Back up over UTF-8 continuation bytes so we never split a character.
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
			--cut;
		}
		out.resize(cut);
		// The cut can land after a space; trim again so the result is stable.
		while (!out.empty() && out.back() == ' ') {
			out.pop_back();
		}
	}
	return out;
}

std::optional<std::string> peek_kid(std::string_view token) {
	const JwsParse p = parse_jws(token);
	if (!p.jws) {
		return std::nullopt;
	}
	const json h = json::parse(p.jws->header_json, nullptr, false);
	if (h.is_discarded() || !h.is_object()) {
		return std::nullopt;
	}
	auto it = h.find("kid");
	if (it == h.end() || !it->is_string()) {
		return std::nullopt;
	}
	return it->get<std::string>();
}

VerifyResult verify_id_token(const AuthConfig &config, const KeySet &keys,
		std::string_view token, std::string_view expected_nonce,
		std::int64_t now) {
	// Rule 1: structure + algorithm allowlist + kid.
	const JwsParse parsed = parse_jws(token);
	if (!parsed.jws) {
		return fail(VerifyError::kMalformed, parsed.error);
	}
	const json header = json::parse(parsed.jws->header_json, nullptr, false);
	if (header.is_discarded() || !header.is_object()) {
		return fail(VerifyError::kMalformed, "JWS header is not a JSON object");
	}
	const auto alg_it = header.find("alg");
	if (alg_it == header.end() || !alg_it->is_string()) {
		return fail(VerifyError::kAlgorithm, "JWS header has no alg");
	}
	const std::string alg = alg_it->get<std::string>();
	JwsAlg want{};
	if (alg == "RS256") {
		want = JwsAlg::kRS256;
	} else if (alg == "ES256") {
		want = JwsAlg::kES256;
	} else {
		return fail(VerifyError::kAlgorithm, "alg '" + alg.substr(0, 16) + "' is not allowed");
	}
	const auto kid_it = header.find("kid");
	if (kid_it == header.end() || !kid_it->is_string() ||
			kid_it->get<std::string>().empty()) {
		return fail(VerifyError::kMissingKid, "JWS header has no kid");
	}

	// Rule 1b: an access token is not an ID token, whoever signed it.
	if (const auto typ_it = header.find("typ"); typ_it != header.end() && typ_it->is_string() &&
			is_access_token_typ(typ_it->get<std::string>())) {
		return fail(VerifyError::kTokenType, "JWS header typ marks an access token (at+jwt)");
	}

	// Rule 2: key by kid.
	const std::string kid = kid_it->get<std::string>();
	const auto key_it = keys.find(kid);
	if (key_it == keys.end()) {
		return fail(VerifyError::kUnknownKid, "no key with that kid");
	}
	// The alg must match the key's type (blocks RSA/EC confusion).
	if (key_it->second.alg != want) {
		return fail(VerifyError::kAlgorithm, "alg does not match the key type");
	}

	// Rule 3: signature.
	if (!verify_signature(key_it->second, parsed.jws->signing_input,
				parsed.jws->signature)) {
		return fail(VerifyError::kBadSignature, "signature check failed");
	}

	const json claims = json::parse(parsed.jws->payload_json, nullptr, false);
	if (claims.is_discarded() || !claims.is_object()) {
		return fail(VerifyError::kMalformed, "JWT payload is not a JSON object");
	}

	// Rule 1b (Keycloak): the realm key also signs access and logout tokens, which can carry
	// our client in `aud` once an audience mapper is configured. Only `typ: "ID"` is a login.
	if (config.provider == Provider::kKeycloak) {
		const auto it = claims.find("typ");
		if (it == claims.end() || !it->is_string() || it->get<std::string>() != "ID") {
			std::string seen = "missing";
			if (it != claims.end()) {
				seen = it->is_string() ? "'" + it->get<std::string>().substr(0, 16) + "'" : "not a string";
			}
			return fail(VerifyError::kTokenType, "typ claim is " + seen + ", not 'ID'");
		}
	}

	// Rule 4: iss / aud / azp.
	{
		auto it = claims.find("iss");
		if (it == claims.end() || !it->is_string() ||
				it->get<std::string>() != config.issuer) {
			return fail(VerifyError::kIssuer, "iss does not match the configured issuer");
		}
	}
	{
		auto it = claims.find("aud");
		bool ok = false;
		if (it != claims.end()) {
			if (it->is_string()) {
				ok = it->get<std::string>() == config.client_id;
			} else if (it->is_array()) {
				for (const json &a : *it) {
					if (a.is_string() && a.get<std::string>() == config.client_id) {
						ok = true;
						break;
					}
				}
			}
		}
		if (!ok) {
			return fail(VerifyError::kAudience, "aud does not contain the client id");
		}
	}
	if (auto it = claims.find("azp"); it != claims.end()) {
		if (!it->is_string() || it->get<std::string>() != config.client_id) {
			return fail(VerifyError::kAuthorizedParty, "azp does not match the client id");
		}
	}

	// Rule 5: time bounds and freshness.
	const auto exp = numeric_date(claims, "exp");
	if (!exp) {
		return fail(VerifyError::kExpired, "exp missing or invalid");
	}
	if (*exp <= now - kClockSkewSeconds) {
		return fail(VerifyError::kExpired, "token expired");
	}
	if (claims.contains("nbf")) {
		const auto nbf = numeric_date(claims, "nbf");
		if (!nbf || *nbf > now + kClockSkewSeconds) {
			return fail(VerifyError::kNotYetValid, "nbf is in the future");
		}
	}
	const auto iat = numeric_date(claims, "iat");
	if (!iat) {
		return fail(VerifyError::kTooOld, "iat missing or invalid");
	}
	if (*iat > now + kClockSkewSeconds) {
		return fail(VerifyError::kNotYetValid, "iat is in the future");
	}
	if (now - *iat > static_cast<std::int64_t>(config.max_token_age_seconds)) {
		return fail(VerifyError::kTooOld, "token older than max_token_age_seconds");
	}

	// Rule 6: nonce, when the token carries one.
	if (auto it = claims.find("nonce"); it != claims.end()) {
		if (!it->is_string() || expected_nonce.empty() ||
				it->get<std::string>() != expected_nonce) {
			return fail(VerifyError::kNonce, "nonce does not match this connection");
		}
	}

	// Rule 7: subject and name.
	auto sub_it = claims.find("sub");
	if (sub_it == claims.end() || !sub_it->is_string() ||
			sub_it->get<std::string>().empty() || sub_it->get<std::string>().size() > 256) {
		return fail(VerifyError::kSubject, "sub missing or invalid");
	}
	const std::string subject = sub_it->get<std::string>();

	std::string name;
	if (auto it = claims.find(config.name_claim); it != claims.end() && it->is_string()) {
		name = sanitize_player_name(it->get<std::string>());
	}
	if (name.empty()) {
		// Fallback: a stable, printable prefix of the subject.
		std::string fb;
		for (const char ch : subject) {
			if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-') {
				fb.push_back(ch);
			}
			if (fb.size() >= 16) {
				break;
			}
		}
		name = sanitize_player_name(fb);
	}
	if (name.empty()) {
		return fail(VerifyError::kName, "no usable player name");
	}

	// Allowlisted claims for Lua (§6); everything else is dropped here.
	json exposed = json::object();
	for (const std::string &c : config.claims) {
		if (auto it = claims.find(c); it != claims.end()) {
			exposed[c] = *it;
		}
	}
	std::string claims_json = exposed.dump();
	if (claims_json.size() > kMaxClaimsJsonBytes) {
		return fail(VerifyError::kClaims, "allowlisted claims too large");
	}

	VerifyResult ok;
	ok.login.provider = config.provider;
	ok.login.issuer = config.issuer;
	ok.login.subject = subject;
	ok.login.name = std::move(name);
	ok.login.claims_json = std::move(claims_json);
	ok.login.issued_at = *iat;
	ok.login.expires_at = *exp;
	return ok;
}

} // namespace vb::auth
