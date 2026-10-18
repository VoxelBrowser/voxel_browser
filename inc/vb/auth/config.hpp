#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// In-engine authentication, step 9.1 (architecture_spec/auth.md §4): the
// declarative `auth.lua` a content pack may ship at its root. Its *presence*
// makes authentication mandatory; this module only loads and validates the
// declaration into an AuthConfig. Handshake plumbing (9.2), token
// verification (9.3), the Lua surface (9.4) and client sign-in (9.5) are
// separate steps.
//
// `auth.lua` is data, not behavior: it runs once, at server startup, in a
// fresh minimal sandboxed Vm (no `vb.*` global, no require-able modules,
// small memory/instruction/wall-clock budgets) and must `return` a table.
// Everything is validated up front; a bad file is an error the server turns
// into a refusal to start -- never a silent downgrade to "no auth".

namespace vb::auth {

// Compile-time facts the server's startup gate consults.
#if defined(VB_WITH_AUTH)
inline constexpr bool kBuiltWithAuth = true;
#else
inline constexpr bool kBuiltWithAuth = false;
#endif

// True when this build can verify tokens (step 9.3: Mbed TLS + libcurl behind
// VB_WITH_AUTH). A server whose pack declares auth must refuse to start
// without it rather than admit unverified players (spec §2 goal 2).
inline constexpr bool kVerifierAvailable = kBuiltWithAuth;

// The pack-root filename. Excluded from the pack-VM walk and the asset
// manifest (it runs in its own VM and holds nothing a client needs).
inline constexpr std::string_view kAuthLuaFilename = "auth.lua";

enum class Provider : std::uint8_t {
	kOidc,
	kKeycloak,
	kFirebase,
};

enum class FirebaseSignIn : std::uint8_t {
	kPassword,
	kGoogle,
};

std::string_view provider_name(Provider p);

// Deployment-specific values `server.toml [auth]` may override so one pack
// works against staging and production realms. Empty = not overridden. These
// can change *where* identity is checked, never *whether* it is.
struct AuthOverrides {
	std::string issuer;
	std::string client_id;
	std::string project_id;
	std::string api_key;
};

// The validated declaration. Presets (`provider`) only fill defaults; for
// Firebase `issuer`/`client_id` are derived from `project_id`.
struct AuthConfig {
	Provider provider = Provider::kOidc;
	std::string display_name; // sign-in screen branding (text only)
	std::string issuer;
	std::string client_id;
	std::string project_id; // firebase only
	std::string api_key; // firebase only; public by design, still not logged
	std::vector<std::string> scopes; // oidc/keycloak; always contains "openid"
	std::vector<FirebaseSignIn> sign_in; // firebase only
	// firebase + sign_in "google": the OAuth client of a Google Cloud "Desktop
	// app" (the client signs in to Google with it, then exchanges the Google ID
	// token for a Firebase one). Both public by design for installed apps.
	std::string google_client_id;
	std::string google_client_secret;
	std::string name_claim;
	std::vector<std::string> claims; // allowlist of claims exposed to Lua
	std::uint32_t max_token_age_seconds = 300;
	std::uint32_t reauth_interval_seconds = 900; // 0 disables periodic re-auth
	std::uint32_t reauth_grace_seconds = 120;
};

// Outcome of load_auth_lua. `present == false` means the pack has no
// auth.lua (authentication off, behavior unchanged). `present == true` with a
// non-empty `error` means the file exists but is unusable -- the caller must
// refuse to start. Otherwise `config` is set.
struct AuthLoad {
	bool present = false;
	std::optional<AuthConfig> config;
	std::string error; // precise, operator-facing; empty on success
};

// Pure validation + loading seams (the second is what tests drive directly).
AuthLoad load_auth_lua(const std::filesystem::path &pack_dir,
		const AuthOverrides &overrides = {});
AuthLoad parse_auth_lua(std::string_view source, const AuthOverrides &overrides = {});

// Multi-line, operator-facing summary for the startup log. Secrets (api_key)
// are redacted.
std::string describe(const AuthConfig &config);

} // namespace vb::auth
