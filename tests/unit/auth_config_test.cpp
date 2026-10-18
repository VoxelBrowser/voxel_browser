// In-engine authentication, step 9.1 (architecture_spec/auth.md §4): the
// auth.lua declaration loader and its validation matrix.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "vb/auth/config.hpp"

using vb::auth::AuthLoad;
using vb::auth::AuthOverrides;
using vb::auth::FirebaseSignIn;
using vb::auth::Provider;

TEST_CASE("auth.lua: a pack without the file has authentication off") {
	const auto dir = std::filesystem::temp_directory_path() / "vb_auth_cfg_none";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	const AuthLoad r = vb::auth::load_auth_lua(dir);
	CHECK_FALSE(r.present);
	CHECK(r.error.empty());
	CHECK_FALSE(r.config.has_value());
	std::filesystem::remove_all(dir);
}

TEST_CASE("auth.lua: a directory named auth.lua fails closed rather than reading as absent") {
	const auto dir = std::filesystem::temp_directory_path() / "vb_auth_cfg_dir";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir / "auth.lua");
	const AuthLoad r = vb::auth::load_auth_lua(dir);
	CHECK(r.present);
	CHECK_FALSE(r.error.empty());
	std::filesystem::remove_all(dir);
}

#if !VB_WITH_LUA

TEST_CASE("auth.lua: without VB_WITH_LUA a present file is an error, never skipped") {
	const AuthLoad r = vb::auth::parse_auth_lua("return {}");
	CHECK(r.present);
	CHECK_FALSE(r.error.empty());
	CHECK_FALSE(r.config.has_value());
}

#else

namespace {

constexpr const char *kKeycloak = R"(
return {
	provider     = "keycloak",
	display_name = "Example Realm",
	issuer       = "https://id.example.com/realms/game",
	client_id    = "voxel-browser",
	scopes       = { "openid", "profile", "email" },
	name_claim   = "preferred_username",
	claims       = { "email", "email_verified", "groups" },
	max_token_age_seconds   = 120,
	reauth_interval_seconds = 600,
	reauth_grace_seconds    = 60,
}
)";

// Asserts the load failed (present + error + no config) and the message
// mentions `needle`.
void check_rejected(const std::string &src, const std::string &needle,
		const AuthOverrides &ov = {}) {
	CAPTURE(src);
	const AuthLoad r = vb::auth::parse_auth_lua(src, ov);
	CHECK(r.present);
	CHECK_FALSE(r.config.has_value());
	CHECK_MESSAGE(r.error.find(needle) != std::string::npos, "error was: ", r.error);
}

} // namespace

TEST_CASE("auth.lua: a full keycloak declaration loads") {
	const AuthLoad r = vb::auth::parse_auth_lua(kKeycloak);
	REQUIRE(r.config.has_value());
	CHECK(r.present);
	CHECK(r.error.empty());
	const auto &c = *r.config;
	CHECK(c.provider == Provider::kKeycloak);
	CHECK(c.display_name == "Example Realm");
	CHECK(c.issuer == "https://id.example.com/realms/game");
	CHECK(c.client_id == "voxel-browser");
	CHECK(c.scopes == std::vector<std::string>{ "openid", "profile", "email" });
	CHECK(c.name_claim == "preferred_username");
	CHECK(c.claims == std::vector<std::string>{ "email", "email_verified", "groups" });
	CHECK(c.max_token_age_seconds == 120);
	CHECK(c.reauth_interval_seconds == 600);
	CHECK(c.reauth_grace_seconds == 60);
}

TEST_CASE("auth.lua: oidc defaults fill in") {
	const AuthLoad r = vb::auth::parse_auth_lua(
			"return { provider = 'oidc', issuer = 'https://idp.example.com', client_id = 'c' }");
	REQUIRE(r.config.has_value());
	const auto &c = *r.config;
	CHECK(c.provider == Provider::kOidc);
	CHECK(c.display_name == "idp.example.com");
	CHECK(c.scopes == std::vector<std::string>{ "openid", "profile" });
	CHECK(c.name_claim == "preferred_username");
	CHECK(c.claims.empty());
	CHECK(c.max_token_age_seconds == 300);
	CHECK(c.reauth_interval_seconds == 900);
	CHECK(c.reauth_grace_seconds == 120);
}

TEST_CASE("auth.lua: the firebase preset derives issuer and audience from project_id") {
	const AuthLoad r = vb::auth::parse_auth_lua(R"(
		return {
			provider = "firebase", project_id = "my-game-1234", api_key = "AIzaSyExample",
			sign_in = { "password", "google" }, claims = { "email" },
			google_client_id = "123-abc.apps.googleusercontent.com", google_client_secret = "GOCSPX-x",
		})");
	REQUIRE(r.config.has_value());
	const auto &c = *r.config;
	CHECK(c.provider == Provider::kFirebase);
	CHECK(c.issuer == "https://securetoken.google.com/my-game-1234");
	CHECK(c.client_id == "my-game-1234");
	CHECK(c.name_claim == "name");
	CHECK(c.sign_in == std::vector<FirebaseSignIn>{ FirebaseSignIn::kPassword, FirebaseSignIn::kGoogle });
	CHECK(c.google_client_id == "123-abc.apps.googleusercontent.com");
	CHECK(c.google_client_secret == "GOCSPX-x");
	CHECK(vb::auth::describe(c).find("AIzaSyExample") == std::string::npos); // redacted
}

TEST_CASE("auth.lua: firebase defaults to password sign-in") {
	const AuthLoad r = vb::auth::parse_auth_lua(
			"return { provider = 'firebase', project_id = 'p', api_key = 'k' }");
	REQUIRE(r.config.has_value());
	CHECK(r.config->sign_in == std::vector<FirebaseSignIn>{ FirebaseSignIn::kPassword });
}

TEST_CASE("auth.lua: http issuer is allowed only for local development hosts") {
	CHECK(vb::auth::parse_auth_lua(
			"return { provider='oidc', issuer='http://127.0.0.1:8080/realms/x', client_id='c' }")
					.config.has_value());
	CHECK(vb::auth::parse_auth_lua(
			"return { provider='oidc', issuer='http://localhost/x', client_id='c' }")
					.config.has_value());
	check_rejected("return { provider='oidc', issuer='http://idp.example.com', client_id='c' }",
			"https");
	check_rejected("return { provider='oidc', issuer='ftp://idp.example.com', client_id='c' }",
			"https");
	// A host that merely starts like localhost must not pass.
	check_rejected("return { provider='oidc', issuer='http://localhost.evil.com', client_id='c' }",
			"https");
}

TEST_CASE("auth.lua: malformed issuer URLs are rejected") {
	for (const char *bad : { "idp.example.com", "https://", "https://user:pw@idp.example.com",
				 "https://idp.example.com/?x=1", "https://idp.example.com/#frag",
				 "https://idp .example.com", "https://idp.example.com:port" }) {
		check_rejected(std::string("return { provider='oidc', client_id='c', issuer='") + bad + "' }",
				"issuer");
	}
}

TEST_CASE("auth.lua: required keys, unknown keys and bad providers") {
	check_rejected("return {}", "provider");
	check_rejected("return { provider = 'saml' }", "unknown provider");
	check_rejected("return { provider = 5 }", "provider");
	check_rejected("return { provider='oidc', client_id='c' }", "issuer");
	check_rejected("return { provider='oidc', issuer='https://a.example.com' }", "client_id");
	check_rejected("return { provider='firebase', api_key='k' }", "project_id");
	check_rejected("return { provider='firebase', project_id='p' }", "api_key");
	check_rejected(
			"return { provider='oidc', issuer='https://a.example.com', client_id='c', secret='s' }",
			"unknown key 'secret'");
	// Keys of another provider's preset are unknown here.
	check_rejected("return { provider='firebase', project_id='p', api_key='k', issuer='https://a.example.com' }",
			"unknown key 'issuer'");
	check_rejected(
			"return { provider='oidc', issuer='https://a.example.com', client_id='c', api_key='k' }",
			"unknown key 'api_key'");
}

TEST_CASE("auth.lua: type and range validation") {
	const std::string base = "provider='oidc', issuer='https://a.example.com', client_id='c', ";
	check_rejected("return { " + base + "scopes = 'openid' }", "scopes");
	check_rejected("return { " + base + "scopes = { 'profile' } }", "openid");
	check_rejected("return { " + base + "scopes = { 'openid', 'openid' } }", "duplicate");
	check_rejected("return { " + base + "scopes = {} }", "scopes");
	check_rejected("return { " + base + "scopes = { 'a b' } }", "scopes");
	check_rejected("return { " + base + "claims = { 1 } }", "claims");
	check_rejected("return { " + base + "claims = { [1]='a', [3]='b' } }", "gaps");
	check_rejected("return { " + base + "claims = { a = 'x' } }", "claims");
	check_rejected("return { " + base + "name_claim = '' }", "name_claim");
	check_rejected("return { " + base + "display_name = 12 }", "display_name");
	check_rejected("return { " + base + "max_token_age_seconds = 5 }", "max_token_age_seconds");
	check_rejected("return { " + base + "max_token_age_seconds = 1.5 }", "whole number");
	check_rejected("return { " + base + "max_token_age_seconds = '300' }", "whole number");
	check_rejected("return { " + base + "reauth_interval_seconds = 10 }", "reauth_interval_seconds");
	check_rejected("return { " + base + "reauth_grace_seconds = 1 }", "reauth_grace_seconds");
	check_rejected("return { " + base + "bad = true }", "unsupported value type");
	check_rejected("return { " + base + "bad = function() end }", "unsupported value type");
	check_rejected("return { " + base + "bad = { { 'nested' } } }", "list of strings");
	check_rejected("return { " + base + "'positional' }", "keys must be strings");
	check_rejected("return { provider='firebase', project_id='p', api_key='k', sign_in={'sms'} }",
			"sign_in");
	// "google" needs the Desktop-app OAuth client it signs in to Google with.
	check_rejected("return { provider='firebase', project_id='p', api_key='k', sign_in={'google'} }",
			"google_client_id");
	check_rejected("return { provider='firebase', project_id='p', api_key='k', google_client_secret='s' }",
			"google_client_secret");
	check_rejected("return { provider='keycloak', issuer='https://id.example/realms/x', client_id='c', "
				   "google_client_id='g' }",
			"google_client_id");
	check_rejected("return { provider='firebase', project_id='p q', api_key='k' }", "invalid characters");

	// reauth_interval_seconds = 0 is the documented "disabled" value.
	const AuthLoad off = vb::auth::parse_auth_lua(
			"return { " + base + "reauth_interval_seconds = 0 }");
	REQUIRE(off.config.has_value());
	CHECK(off.config->reauth_interval_seconds == 0);
}

TEST_CASE("auth.lua: must return a table and must parse") {
	check_rejected("return 5", "table");
	check_rejected("return", "table");
	check_rejected("x = 1", "table");
	check_rejected("return {", "auth.lua");
	check_rejected("error('boom')", "boom");
}

TEST_CASE("auth.lua: runs in a sandbox with no vb API, no host access, no bytecode, tight budget") {
	check_rejected("return { provider = vb.something }", "vb");
	check_rejected("os.exit(0)", "os");
	check_rejected("io.open('/etc/passwd')", "io");
	check_rejected("local f = load('return 1'); return f()", "load");
	check_rejected("return require('anything')", "not found");
	check_rejected("while true do end", "ran too long");
	check_rejected("local s = 'x' while true do s = s .. s end", "auth.lua");
	check_rejected(std::string(20 * 1024, ' ') + "return {}", "larger than");
	check_rejected(std::string("\x1bLua", 4), "auth.lua"); // bytecode header refused
}

TEST_CASE("auth.lua: server.toml [auth] overrides replace deployment values") {
	AuthOverrides ov;
	ov.issuer = "https://prod.example.com/realms/game";
	ov.client_id = "voxel-prod";
	const AuthLoad r = vb::auth::parse_auth_lua(kKeycloak, ov);
	REQUIRE(r.config.has_value());
	CHECK(r.config->issuer == "https://prod.example.com/realms/game");
	CHECK(r.config->client_id == "voxel-prod");

	// An override can satisfy a key the pack omitted.
	const AuthLoad omitted = vb::auth::parse_auth_lua("return { provider = 'oidc' }", ov);
	REQUIRE(omitted.config.has_value());
	CHECK(omitted.config->client_id == "voxel-prod");

	// ...but is validated like the original (no weakening to plain http).
	AuthOverrides weak;
	weak.issuer = "http://prod.example.com";
	check_rejected(kKeycloak, "https", weak);

	// Firebase overrides.
	AuthOverrides fb;
	fb.project_id = "prod-project";
	fb.api_key = "prod-key";
	const AuthLoad f = vb::auth::parse_auth_lua(
			"return { provider = 'firebase', project_id = 'staging', api_key = 'k' }", fb);
	REQUIRE(f.config.has_value());
	CHECK(f.config->project_id == "prod-project");
	CHECK(f.config->issuer == "https://securetoken.google.com/prod-project");

	// An override that doesn't apply to the provider is an error, not ignored.
	check_rejected("return { provider = 'firebase', project_id = 'p', api_key = 'k' }",
			"does not apply", ov);
	AuthOverrides fb_on_oidc;
	fb_on_oidc.api_key = "k";
	check_rejected(kKeycloak, "does not apply", fb_on_oidc);
}

TEST_CASE("auth.lua: load_auth_lua reads the file at the pack root") {
	const auto dir = std::filesystem::temp_directory_path() / "vb_auth_cfg_file";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	{
		std::ofstream f(dir / "auth.lua", std::ios::binary);
		f << kKeycloak;
	}
	const AuthLoad ok = vb::auth::load_auth_lua(dir);
	REQUIRE(ok.config.has_value());
	CHECK(ok.config->client_id == "voxel-browser");

	{
		std::ofstream f(dir / "auth.lua", std::ios::binary | std::ios::trunc);
		f << "return { provider = 'oidc' }";
	}
	const AuthLoad bad = vb::auth::load_auth_lua(dir);
	CHECK(bad.present);
	CHECK_FALSE(bad.config.has_value());
	CHECK_FALSE(bad.error.empty());
	std::filesystem::remove_all(dir);
}

TEST_CASE("auth.lua: describe() summarises without leaking the api key") {
	const AuthLoad r = vb::auth::parse_auth_lua(kKeycloak);
	REQUIRE(r.config.has_value());
	const std::string d = vb::auth::describe(*r.config);
	CHECK(d.find("provider=keycloak") != std::string::npos);
	CHECK(d.find("client_id=voxel-browser") != std::string::npos);
}

#endif // VB_WITH_LUA
