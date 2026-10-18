# In-Engine Authentication — Design & Phased Plan

> Full detail for this topic; the backlog entry is `REMAINING_TASKS.md`
> Phase 9. Status: **in progress — 9.0 (docs) and 9.1 (`auth.lua` loading, fail-closed
> startup) landed 2026-10-05; 9.2 (protocol v29 + handshake plumbing) landed; 9.3 (server token verification) landed; 9.4 (Lua exposure) landed; 9.5 (client sign-in) landed; 9.6 (sessions, re-auth, revocation) landed; 9.7 (singleplayer, `vb`, e2e) and 9.8 (hardening) landed; remaining items are listed per step in `REMAINING_TASKS.md`.** Supersedes
> the 2026-09-17 direction on spec §18 Q6 ("engine owns no auth concept") —
> see §11.

## 1. Idea in one paragraph

A content pack may ship one small file, **`auth.lua`**, that *declares* how
players authenticate: which identity provider (Keycloak, Firebase, any
OpenID Connect issuer…), the client id, scopes, which claim becomes the
player's name, and which claims scripts may see. **If that file exists,
authentication is mandatory**: the engine signs the player in and verifies
the result *during the handshake*, before any pack asset, block registry,
chunk or entity is sent — a player who is not authenticated never sees the
world. Every pack script then gets the verified login data through one
accessor, `player:get_login()`, which returns a table when the server is
authenticating and **`nil` when it is not** (no `auth.lua`). That nil check
is the only thing a script needs:

```lua
vb.on("player_join", function(name, login)
	if login then -- mandatory auth is on, and this player passed it
		-- persist by subject (stable), never by name (display only)
		local profile = vb.db.get("user:" .. login.subject) or { joins = 0 }
		profile.joins = profile.joins + 1
		vb.db.set("user:" .. login.subject, profile)
	end
end)
```

## 2. Goals / non-goals

**Goals**

1. Declarative, pack-owned auth config: one `auth.lua`, no engine flag or
   operator step needed to turn it on. Absent file = today's behavior,
   byte-for-byte.
2. Fail closed. A pack with `auth.lua` never lets an unverified player join,
   including when the build lacks auth support, when `auth.lua` is
   malformed, or when the provider is unreachable.
3. Auth runs **before the world loads**: before `C2S_AssetManifestRequest`,
   so before assets, registry, `S2C_JoinAccept` and chunks.
4. Login data is **verified server-side** (signature + claims) and exposed
   read-only to Lua. The client never decides who it is.
5. Provider-agnostic core (OpenID Connect + JWT), with thin presets for
   common providers (`keycloak`, `firebase`, generic `oidc`).
6. No client secrets anywhere: public-client flows only (PKCE), so `auth.lua`
   is safe to ship in a pack that also gets synced to clients.

**Non-goals (for now)**

- Engine-run username/password databases. Packs that want that keep using
  `vb.db` + `vb.crypto.hash` (the 2026-09-17 approach still works, it just
  is not "mandatory auth").
- Authorization/roles as an engine concept. The engine hands claims to Lua;
  what a `role` claim *means* is the pack's business.
- Pack-customizable login UI. Pack UI scripts are not synced yet at login
  time (auth precedes asset sync), so the sign-in screen is engine-drawn
  (like the 7.1 loading screen) with only text branding from `auth.lua`.
- Token introspection / confidential clients / mTLS-bound tokens.
- An embedded web view. Browser flows use the system browser.

## 3. What exists today (constraints)

| Fact | Where | Consequence |
| --- | --- | --- |
| Handshake already has `S2C_ServerInfo{…, auth_mode}` → `C2S_Auth{player_name, token}` → `S2C_AuthResult{ok, reason}` **before** asset sync | `inc/vb/protocol/handshake.hpp`, `src/net/handshake.cpp` | The slot exists and is in the right place. Auth = filling it in, not reordering the handshake. |
| `AuthMode{kNone=0, kToken=1}`; `kToken` reserved, never verified | `handshake.hpp`, `src/core/config.cpp` (`server.toml auth_mode`) | Add a new value, `kExternal=2`. Leave `kToken` reserved. |
| `HandshakeServerHost::authenticate(name, token) -> AuthOutcome` is synchronous | `inc/vb/net/handshake.hpp` | Verification may need a JWKS fetch (network) → needs an async/pending outcome (§6.3). |
| `PackRuntime::install_join_veto` wraps `authenticate` so `player_join(name)` can veto pre-join | `docs/lua-api.md` | `player_join` gains a second arg, `login`. |
| One global `handshake_timeout_seconds` per connection | `src/net/session.cpp` `system_handshake_timeouts` | A human in a browser needs minutes → per-state timeout for `kAwaitingAuth`. |
| Pack loader runs every root `*.lua` in the pack VM; asset manifest syncs pack files to clients | `src/script/pack_loader.cpp`, `src/assetsync/manifest.cpp` | `auth.lua` must be special-cased: excluded from the pack-VM walk (it runs in its own VM first). Syncing it is harmless (no secrets) but pointless — exclude it too. |
| Deps already present: libcurl (CLI only), nlohmann_json, `vb::core::sha256`, orlp/ed25519 | `cmake/Dependencies.cmake` | Need: HTTPS in client+server (reuse curl), RSA/ECDSA signature verify (new dep, §5.4). |
| Client `kConnecting → kError` path renders `failure_reason()` via `MainMenu::draw_error()` | `src/client/main.cpp`, `src/render/main_menu.cpp` | Auth failures reuse it; a new `AppState::kSigningIn` sits between connect and asset sync. |
| `--singleplayer` runs an integrated server over `LoopbackTransport`, same handshake | `src/net/integrated.cpp` | Same code path → singleplayer with an `auth.lua` pack signs in for real (§7). |

## 4. `auth.lua` — the declaration

Lives at the pack root next to `init.lua`. It is **data, not behavior**: it
runs once at server startup in a fresh, minimal sandboxed VM (no `vb.world`,
no `vb.db`, no events, no `require`, tiny instruction budget) and must
`return` a table:

```lua
-- content/<pack>/auth.lua
return {
	provider     = "keycloak",                 -- "oidc" | "keycloak" | "firebase"
	display_name = "Example Realm",            -- shown on the sign-in screen
	issuer       = "https://id.example.com/realms/game",
	client_id    = "voxel-browser",            -- public client, PKCE, no secret
	scopes       = { "openid", "profile", "email" },
	name_claim   = "preferred_username",       -- becomes the in-game player name
	claims       = { "email", "email_verified", "groups" }, -- exposed to Lua
	max_token_age_seconds = 300,               -- optional, default 300
	reauth_interval_seconds = 900,             -- optional, revocation check (§5.6)
	reauth_grace_seconds    = 120,             -- optional
}
```

```lua
-- Firebase preset: issuer/JWKS derived from project_id
return {
	provider     = "firebase",
	project_id   = "my-game-1234",
	api_key      = "AIza...",                   -- Firebase web API key (public by design)
	sign_in      = { "password", "google" },    -- engine-drawn form / browser flow
	google_client_id     = "1234-abc.apps.googleusercontent.com", -- "google": a Desktop-app OAuth client
	google_client_secret = "GOCSPX-...",        -- Google requires it for Desktop apps; not confidential
	name_claim   = "name",
	claims       = { "email", "email_verified" },
}
```

Rules:

- **Presence ⇒ mandatory.** No `enabled = false` switch; delete the file to
  turn auth off. One way to read the config: is the file there?
- Validated at startup into a C++ `vb::auth::AuthConfig`. Unknown keys,
  wrong types, non-`https` issuer (except `http://127.0.0.1`/`localhost`
  for development), or a missing required key ⇒ **server refuses to start**
  with a precise error. Never silently downgraded to no-auth.
- `server.toml` may **override** deployment-specific values under `[auth]`
  (`issuer`, `client_id`, `project_id`, `api_key`), so one pack works against
  staging and production realms. It cannot *disable* auth for a pack that
  ships `auth.lua`. The single escape hatch is a dev-only
  `--insecure-skip-auth` server flag, compiled out under `VB_DISTRIBUTION`
  (same rule as `VB_WITH_AUTOMATION`) and logged loudly every startup. With
  it, `get_login()` returns `nil`, the same as when no pack declares auth.
- Built without `VB_WITH_AUTH` and the pack has `auth.lua` ⇒ refuse to
  start ("this pack requires authentication; rebuild with VB_WITH_AUTH").
- The legacy `server.toml auth_mode` key: `none` stays the default, `token`
  stays reserved. `auth_mode` on the wire is *derived* from `auth.lua`'s
  presence, not configured.

Provider presets only fill defaults; everything reduces to one internal
shape:

| `provider` | Verification (server) | Sign-in (client) |
| --- | --- | --- |
| `oidc` | discovery at `{issuer}/.well-known/openid-configuration` → `jwks_uri`; `iss == issuer`, `aud`/`azp` ∋ `client_id` | Authorization Code + PKCE, loopback redirect (RFC 8252) in the system browser |
| `keycloak` | = `oidc` (Keycloak's realm URL *is* the issuer) | = `oidc` |
| `firebase` | JWKS `https://www.googleapis.com/service_accounts/v1/jwk/securetoken@system.gserviceaccount.com`, `iss == https://securetoken.google.com/<project_id>`, `aud == project_id` | `password`: engine-drawn email/password form → Identity Toolkit REST `accounts:signInWithPassword?key=<api_key>`; `google`: browser OIDC with Google, exchanged via `accounts:signInWithIdp` |

## 5. Server side

### 5.1 Startup order

```
parse server.toml/CLI
→ if <pack>/auth.lua: run it in the auth VM → AuthConfig (or exit 1)
→ AuthService::start(config): discovery + JWKS prefetch (background thread;
  failure is logged and retried, not fatal; joins fail closed until a
  key set is loaded)
→ PackRuntime load (auth.lua skipped by the walk) → worldgen → listen
```

### 5.2 Handshake (protocol v29; v28 was taken by S2C_PlayerStatus)

```
Client                                         Server
  │ C2S_Hello                                    │
  │ ───────────────────────────────────────────▶ │
  │          S2C_ServerInfo{…, auth_mode=External}│
  │          S2C_AuthChallenge{provider, display_name,
  │            issuer, client_id, scopes[], params{k:v},
  │            nonce (32 random bytes, b64url)}   │   ← new, only when External
  │ ◀─────────────────────────────────────────── │
  │   … player signs in (browser / form) …        │   server waits in
  │                                               │   kAwaitingAuth, timeout
  │                                               │   auth_timeout_seconds (300)
  │ C2S_Auth{player_name (ignored), token=ID JWT} │
  │ ───────────────────────────────────────────▶ │
  │                         verify (§5.3) — may be │   kVerifyingAuth (pending)
  │                         async on JWKS miss     │
  │          S2C_AuthResult{ok, reason,           │
  │            resolved_name}                     │   ← resolved_name new
  │ ◀─────────────────────────────────────────── │
  │ C2S_AssetManifestRequest … (unchanged)        │
```

- `S2C_AuthChallenge.params` carries preset extras (`api_key`, `sign_in`
  methods) as a bounded string map; client never needs to parse `auth.lua`.
- `C2S_Auth.token` cap: 16 KiB (JWTs with group claims get large; still
  bounded). `player_name` is ignored under `External`; the name comes from
  `name_claim`.
- New server FSM state `kVerifyingAuth`: `authenticate` becomes
  `begin_authenticate(...) -> AuthTicket`; the session polls the ticket each
  tick (resolved immediately on the fast path where the key is cached).
  `auth_mode = none` keeps the synchronous path unchanged.
- Failure ⇒ `S2C_AuthResult{ok=false}` + `kAuthFailed` disconnect (existing
  code path). Reasons shown to the player are coarse ("sign-in expired",
  "not accepted by this server"); exact cause goes to the server log only.

### 5.3 Token verification (`vb::auth::TokenVerifier`)

Pure function over (config, key set, token, expected nonce, now). The rules:

1. Compact JWS, 3 base64url parts, header `alg` ∈ {`RS256`, `ES256`}
   allowlist (reject `none`, `HS*`, anything else); `kid` required.
   1b. **Token type.** Reject a JWS header `typ` of `at+jwt` (any case, with or
   without the `application/` prefix) for every preset. For the `keycloak` preset
   the payload claim `typ` must equal `"ID"` (missing ⇒ reject). The realm key
   also signs access (`Bearer`) and logout (`Logout`) tokens; once an audience
   mapper puts our client in `aud` an access token would otherwise pass rules
   3-5 (same issuer, same key, `azp` ours). Exact cause in the server log; the
   player sees the coarse "not accepted by this server". (`VerifyError::kTokenType`.)
2. Key by `kid` from cached JWKS; unknown `kid` ⇒ one rate-limited JWKS
   refresh (≥ 60 s apart), then fail.
3. Signature valid.
4. `iss` == configured issuer; `aud` (string or array) contains
   `client_id` (`project_id` for Firebase); if `azp` present it equals
   `client_id`.
5. `exp` > now − 60 s skew; `nbf`/`iat` ≤ now + 60 s; `now − iat` ≤
   `max_token_age_seconds` (bounds replay of refresh-derived tokens).
6. `nonce` claim == the challenge nonce **when the token came from an
   interactive browser sign-in** (OIDC presets put the server nonce into the
   authorization request). Firebase password sign-in and refresh-token
   re-use cannot carry it; for those, rule 5's freshness bound is the replay
   limit. See §8 for what this does and doesn't stop.
7. `sub` non-empty; `name_claim` present (fallback: `sub` prefix) and passes
   the existing player-name validation (1–32 bytes, sanitized).

Output `LoginInfo{provider, issuer, subject, name, claims (allowlisted,
JSON), issued_at, expires_at}`. The raw token is dropped immediately and
never logged or exposed to Lua. `issuer`/`issued_at`/`expires_at` are
engine-internal (identity keying, re-auth scheduling); Lua sees only the
user data (§6).

### 5.4 Dependencies (`VB_WITH_AUTH`, default ON for client+server)

- **HTTPS:** reuse the CLI's libcurl block (system TLS on each OS) — hoist it
  from `VB_BUILD_CLI`-only to `VB_BUILD_CLI OR VB_WITH_AUTH`.
- **Signature verify:** **Mbed TLS 3.6 LTS** via `vb_fetch` (Apache-2.0,
  builds everywhere, RSA PKCS#1 v1.5 + ECDSA P-256). Rejected: OpenSSL
  (not shipped on Windows where curl uses Schannel; extra DLL),
  hand-rolled RSA (no). orlp/ed25519 stays CLI-only (no IdP signs with
  EdDSA by default).
- **JSON:** existing nlohmann_json.
- HTTP behind an injectable `vb::auth::HttpFetcher` interface so unit tests
  never touch the network.

### 5.5 Identity & names

- Engine keys identity by `(issuer, subject)`, never by name.
- Duplicate login (**decided 2026-10-05**): a second connection with an
  `(issuer, subject)` already in game **kicks the older session** ("signed
  in elsewhere") once the newcomer's verification succeeds. The newcomer is
  never refused, so a ghost session after a crash or network drop can't
  lock its owner out.
- Name collision between *different* subjects (two people called `alex`):
  the later one is shown as `alex#2`. Scripts must persist by
  `login.subject`, never by name (documented loudly).

### 5.6 Revocation: periodic live re-authentication

ID tokens are self-contained and can't be revoked after they're issued. The
IdP-side session behind them can be: a Keycloak admin logout, a disabled user,
a Firebase `revokeRefreshTokens` or a disabled account. The engine therefore
re-proves the login on a live connection on a timer, and a revoked
session fails at the next check.

```
Server                                         Client
  │ S2C_ReauthRequest{nonce, deadline_s}         │  every reauth_interval_seconds
  │ ───────────────────────────────────────────▶ │  (per-player jitter ±10%)
  │                                              │  silent: refresh-token grant
  │                                              │  at the IdP (no UI)
  │ C2S_Reauth{token}                            │
  │ ◀─────────────────────────────────────────── │
  │ verify (§5.3) + same (issuer, subject)       │
  │ + iat ≥ request_sent − skew                  │
```

- `auth.lua` keys: `reauth_interval_seconds` (default **900**, minimum 60;
  `0` disables, so a session is trusted until disconnect) and
  `reauth_grace_seconds` (default **120**).
- The refresh grant is the actual revocation check. A revoked session or
  disabled account makes the IdP refuse the refresh token, so no new ID
  token can be produced.
- If the silent refresh fails, the player keeps playing and sees a
  non-blocking in-game prompt: "Your sign-in expired. Sign in again". It
  opens the same browser or form flow, with the request's `nonce` applied.
  If no valid `C2S_Reauth` arrives before `deadline_s`, which is the grace
  period, the server kicks with `kAuthFailed` ("sign-in expired or
  revoked").
- Binding: refresh-derived ID tokens can't carry the nonce, so the server
  requires `iat` to be newer than the moment it sent the request (minus
  skew). An old token can't answer a new request. An interactive re-sign-in
  must also match the nonce.
- Subject change (a different account answering) ⇒ immediate kick.
- On success the session's `LoginInfo` is replaced. If any **allowlisted
  claim** changed (e.g. a group removed), the frozen table `get_login()`
  returns is swapped and `vb.on("login_changed", function(player, login))`
  fires, so packs can react, e.g. drop a moderator's powers. The in-game
  name stays fixed for the session; a changed `name_claim` takes effect on
  the next join.
- **Worst-case revocation latency = interval + grace** (default ≈ 17 min).
  Operators who need tighter revocation lower the interval. Instant push
  revocation (Keycloak back-channel logout, OAuth token introspection) needs
  an inbound HTTP endpoint or a confidential client on the game server. It is
  out of scope for this phase and listed in §13.
- Pack-side bans don't need this mechanism: a `player_join` veto keyed by
  `login.subject` keeps a banned account out at the next join.
- `--auth-token-file` (headless/automation) is re-read for each re-auth, so
  tests can rotate or withhold the token.
- **An IdP outage is not a revocation.** If the client's silent refresh fails
  for a transient reason (IdP unreachable, 5xx, 429, discovery failing) the
  stored refresh token is **kept** and the refresh is retried every 5 s while
  the request is open (`SignInCoordinator::Options::reauth_retry_interval`);
  the "sign in again" banner waits for three misses in a row. Only an answer
  that condemns the token (`invalid_grant`: "Session not active", "Token is
  not active", "Invalid refresh token", ...) forgets it. The server's grace
  period is the bound either way: an IdP down for longer than the grace
  period kicks (fail closed).

## 6. Lua surface (server pack VM)

| API | Returns |
| --- | --- |
| `player:get_login()` | `nil` when the server isn't authenticating, else a **frozen** table of user data only: `{ provider, subject, name, claims = {…allowlisted…} }` |
| `vb.on("player_join", function(name, login) … end)` | `login` same table or `nil`; `return false` still vetoes, *after* verification (e.g. allowlist by `login.claims.email_verified` or a group) |
| `vb.on("login_changed", function(player, login) … end)` | fires after a successful re-auth (§5.6) changed an allowlisted claim; notification only |
| `vb.auth.required()` | `true` iff `auth.lua` is active. Convenience; `get_login() ~= nil` gives the same answer per player |

Guarantee written into `docs/lua-api.md`: **when `auth.lua` is active,
`get_login()` is non-nil for every `Player` a script can ever obtain**.
No connection reaches `player_join_completed` without a verified login, so
scripts never need a "half-authenticated" branch.

**Lua stays high-level (decided 2026-10-05):** scripts see *who the player
is*, never how that was proven. Tokens, signatures, issuer URLs, expiry
times and the re-auth schedule are engine-internal and never reach any VM.
A pack that needs a field asks for it by adding the claim to `auth.lua`'s
`claims` list.

Client UI VM (`ui/*.lua`) gets `state.login = { name, subject }` (read-only,
own player only) for "Signed in as …" labels. No claims, no tokens.

## 7. Client side

- New `AppState::kSigningIn`, entered on `S2C_AuthChallenge`. The screen is
  engine-drawn raygui: `display_name`, the server address, provider host,
  buttons per sign-in method, Cancel. Cancel/timeout → `kError` with the
  reason (existing path).
- **Browser flow** (`vb::auth::OidcLoopbackFlow`): discovery → PKCE
  verifier/challenge (S256) + `state` + server `nonce` → listen on
  `127.0.0.1:<ephemeral>` → open system browser (`xdg-open` / `open` /
  `ShellExecuteW`) → receive `?code&state` → exchange at token endpoint →
  ID token. Runs on a worker thread; the render loop keeps drawing.
  Success page tells the player to return to the game.
- **Firebase password:** engine-drawn email/password form → Identity
  Toolkit REST → ID token. Password never touches the game server.
- **Session cache:** refresh token per `(server, issuer, client_id)` in
  `user_config_dir()/auth/<sha256(server|issuer|client_id)>.json`, mode 0600.
  The token itself goes to the OS keychain when there is one
  (`vb::auth::os_secret_backend()`: macOS Keychain, Windows Credential
  Manager, Secret Service via a dlopen'ed libsecret; since 2026-10-18), keyed
  by the hash of the file's absolute path; the file then holds only metadata
  and `"secret":"os"`. A keychain that is missing, refuses (e.g. a token over
  the Credential Manager's 2560-byte blob limit) or fails once keeps the token
  in the file; opening the store moves file tokens into a working keychain.
  `VB_KEYCHAIN=off` forces files (the e2e harness sets it). `server` is the address the client
  connected to (`host:port`, or `singleplayer`). Each server is its own
  sign-in: two servers on the same identity provider never share a saved
  login (it was per `(issuer, client_id)` until 2026-10-08; such files are
  deleted on startup). Re-joining the same server tries a silent refresh
  first; the token it yields carries no nonce, so it's accepted only within
  `max_token_age_seconds` (§5.3 rule 5). A join that fails for a non-auth
  reason (asset sync, timeout, kick) keeps the login; only a refused refresh
  token or "Sign out" removes it. The main menu shows the saved login for
  the server in its connect fields, and its "Sign out" forgets only that
  server's login.
- **First-use trust prompt:** first time a server address asks for a given
  issuer, show "*<server>* wants you to sign in with *<issuer host>*".
  Remembered per (server, issuer).
- **Headless/automation:** `--auth-token-file <path>` supplies a token
  directly (dev/CI; automation builds only); automation predicate gains
  `login.subject`.
- **Singleplayer** with an `auth.lua` pack: performs the same real sign-in
  (same handshake, one code path). No fake "local" login. A `nil` must always
  mean "no auth", so a pack never sees a synthetic login. Offline means you
  can't play that pack; `--insecure-skip-auth` exists for development.

## 8. Security notes

- **Fail closed everywhere** (§4). Server with `auth.lua` and no key set yet
  rejects joins with "authentication service unavailable".
- **Token forwarding by a malicious server.** A hostile server can claim
  any issuer/client_id in its challenge. If you sign in there, it can try
  to replay your ID token at the real server. Mitigations: the challenge
  nonce (the token only matches the connection whose nonce it carries,
  unless the hostile server relays that nonce in real time), the
  freshness bound, and the first-use prompt naming issuer and server. Full
  binding to the server's identity would need DPoP-style proof-of-possession
  and is out of scope. Recorded in the spec §17 table.
- GNS encrypts the transport, but with no server certificates it doesn't
  authenticate the server. Same root cause as the item above, and the same
  out-of-scope status.
- Revocation is periodic, not instant: worst case `reauth_interval_seconds
  + reauth_grace_seconds` (§5.6).
- Tokens never logged (log redaction test), never stored server-side, never
  exposed to Lua.
- Rate limit: auth attempts per IP (reuse `max_connections_per_ip` plumbing)
  + the existing handshake flood guard. `kVerifyingAuth` work is bounded:
  at most one in-flight JWKS refresh.
- JWT/JWKS parsers are fuzz targets (same policy as every `vb/protocol`
  decoder).

## 9. Code layout

```
inc/vb/auth/config.hpp        AuthConfig + load_auth_lua(pack_dir)
inc/vb/auth/jwt.hpp           JWS parse, base64url, claims
inc/vb/auth/jwks.hpp          KeySet, JwksCache (refresh, rate limit)
inc/vb/auth/verifier.hpp      TokenVerifier, LoginInfo
inc/vb/auth/http.hpp          HttpFetcher interface + curl impl
inc/vb/auth/oidc_client.hpp   client: discovery, PKCE, loopback flow
inc/vb/auth/firebase.hpp      client: Identity Toolkit password sign-in
src/auth/…                    vb_auth static lib (links vb_core, curl, mbedtls)
```

`vb_auth` is linked by the server, the client and the tests. Without
`VB_WITH_AUTH`, a stub lib keeps `load_auth_lua` working (so the
"refuse to start" check still runs) and has every verify/sign-in call fail.

## 10. Testing strategy

- **Unit (`vb_tests`):** `auth.lua` validation matrix; base64url/JWT parse
  edge cases; verifier rules 1–7 each with a failing token (test-only RSA and
  P-256 keys generated by Mbed TLS in-test, tokens signed in-test); plus
  `auth_keycloak_test.cpp` / `auth_keycloak_client_test.cpp`, which run the
  verifier, the JWKS cache and the sign-in coordinator over **golden
  Keycloak fixtures** (`tests/unit/fixtures/keycloak/`, generated by the e2e
  emulator, checked against a real Keycloak in CI);
  JWKS cache refresh/rate limit via a fake `HttpFetcher`; handshake FSM
  with `kVerifyingAuth` pending/resolve/timeout; protocol round-trip + fuzz
  for the new/changed messages.
- **Integration:** `LoopbackTransport` server+client with a fake IdP
  (in-process `HttpFetcher` + signer): join succeeds, `get_login()` non-nil
  in `player_join` and later callbacks; no-`auth.lua` pack ⇒ `nil`
  everywhere; bad token ⇒ no `S2C_AssetManifest` ever sent (assert the world
  never loads).
- **E2E (pytest harness):** mock OIDC provider (small stdlib HTTP server with
  pre-generated key material checked into `tests/e2e/fixtures/`), client via
  `--auth-token-file` and via a scripted loopback redirect.
- **Real Keycloak (automated):** the same e2e scenarios run against a pinned Keycloak
  image in docker (`.github/workflows/auth_keycloak.yml`: PRs that touch the auth
  paths, `workflow_dispatch`, and a weekly canary against the newest release), and
  `test_mock_keycloak_matches_real.py` keeps the emulator honest.
- **Manual:** a real Firebase project (and a real browser, once per platform).
  Steps recorded in `docs/auth.md`.

## 11. Relationship to the 2026-09-17 direction (§18 Q6)

That note said the engine owns no auth concept and that packs build login
on `vb.db`. This design changes the first half: the engine now owns
**external, mandatory, declarative** authentication. That is exactly the
"future `auth_mode = oidc`" the note anticipated, with the mode chosen by
the pack (`auth.lua`) instead of `server.toml`. The second half stands. A pack
can still build its own `vb.db` login for its own purposes, but only
`get_login()` is engine-verified.

## 12. Phased plan

Each step is independently shippable and keeps `main` green. Sizes: S ≈ ≤1
day, M ≈ 2–3 days, L ≈ a week.

### 9.0 — Decision & docs (S)
- [ ] Update spec §18 Q6, §17 security table, `open-questions.md`,
      `architecture_spec/networking.md` handshake diagram to point here.
- **Exit:** docs agree; no code.

### 9.1 — `auth.lua` loading & fail-closed startup (S/M)
- [ ] `vb::auth::AuthConfig` + `load_auth_lua()`: minimal sandboxed VM,
      return-a-table contract, full validation, presets fill defaults.
- [ ] Pack loader and asset manifest skip `auth.lua`.
- [ ] `server.toml [auth]` overrides; `--insecure-skip-auth` (compiled out
      under `VB_DISTRIBUTION`); refuse to start when `VB_WITH_AUTH` is off.
- [ ] `content/base/auth.lua.example` (inactive by name), documented.
- **Exit:** malformed `auth.lua` ⇒ exit 1 with a clear message; valid one ⇒
  config logged (redacted); no `auth.lua` ⇒ unchanged behavior.

### 9.2 — Protocol v29 + handshake plumbing, stub verifier (M)
- [ ] `AuthMode::kExternal`; `S2C_AuthChallenge`; `S2C_AuthResult.resolved_name`;
      `C2S_Auth.token` 16 KiB cap; `S2C_ReauthRequest` / `C2S_Reauth`
      (wire format only here, used in 9.6); bump `kEngineProtocolVersion`;
      `docs/protocol.md` in lockstep.
- [ ] Server FSM `kVerifyingAuth` + `begin_authenticate`/ticket; per-state
      `auth_timeout_seconds`; client FSM handles the challenge
      (`ClientHandshakeConfig` gets an `obtain_token` callback).
- [ ] Round-trip + fuzz tests; FSM tests with a fake verifier.
- **Exit:** loopback join with fake verifier passes; rejected token ⇒ no
  asset manifest sent.

### 9.3 — Server token verification (L)
- [ ] `VB_WITH_AUTH`; hoist curl; add Mbed TLS 3.6 via `vb_fetch`.
- [ ] base64url/JWT parser, JWKS parse (RSA `n/e`, EC `x/y`), `JwksCache`
      with background refresh + rate limit, OIDC discovery, Firebase preset.
- [ ] `TokenVerifier` rules 1–7; `LoginInfo`; log redaction.
- [ ] Tests with in-test keys; fuzz targets for JWT/JWKS.
- **Exit:** every rule has a passing and a failing test; a real Keycloak
  token verifies in a manual run.

### 9.4 — Lua exposure (M)
- [ ] `LoginInfo` on the session; `player:get_login()` (frozen table / nil);
      `player_join(name, login)`; `vb.auth.required()`; `state.login` in
      the client UI VM.
- [ ] Name resolution from `name_claim`, collision suffixing, duplicate-
      subject kick.
- [ ] `docs/lua-api.md` with the non-nil guarantee; base-pack example
      script that branches on `login`.
- **Exit:** integration test: auth pack ⇒ non-nil in every callback that gets
  a `Player`; plain pack ⇒ nil.

### 9.5 — Client sign-in (L)
- [ ] `AppState::kSigningIn` screen; cancel/timeout → existing error path.
- [ ] `OidcLoopbackFlow` (PKCE S256, state, nonce, ephemeral 127.0.0.1
      listener, system-browser open per OS, token exchange) on a worker.
- [ ] Firebase password form + Identity Toolkit call; Firebase `google`
      via browser + `signInWithIdp`.
- [ ] `--auth-token-file` for headless/automation builds.
- **Exit:** manual sign-in against Keycloak and Firebase on Linux, macOS,
  Windows; headless join with a token file in CI.

### 9.6 — Sessions, re-auth & revocation (M/L)
- [ ] Refresh-token cache (0600), silent re-login, "Signed in as … / Sign
      out" in the main menu, first-use trust prompt.
- [ ] Periodic re-auth (§5.6): server scheduler with jitter,
      `reauth_interval_seconds`/`reauth_grace_seconds`, `iat` freshness +
      same-subject checks, kick on deadline; client silent refresh +
      non-blocking re-sign-in prompt.
- [ ] `LoginInfo` swap + `login_changed` event when allowlisted claims change.
- [ ] Tests: revoked refresh token (fake IdP refuses) ⇒ kick after grace;
      stale `iat` rejected; different subject ⇒ immediate kick; claim
      change fires `login_changed` once.
- **Exit:** second join within the refresh window opens no browser; a
  Keycloak admin logout kicks the player within interval + grace in a
  manual run.

### 9.7 — Singleplayer, `vb`, e2e (M)
- [ ] Singleplayer with an auth pack runs the real flow; `vb host` passes
      `[auth]` overrides through `server.toml`.
- [ ] Mock IdP + e2e tests (token-file path and scripted loopback redirect);
      automation predicate `login.subject`.
- **Exit:** `e2e` CTest covers auth-on and auth-off packs.

### 9.8 — Hardening (S/M)
- [ ] Per-IP auth rate limit; at most one in-flight JWKS refresh; security
      table rows; `docs/auth.md` operator guide (Keycloak client setup:
      public client, PKCE, redirect `http://127.0.0.1/*`; Firebase setup).
- [x] Optional: OS keychain storage for refresh tokens (2026-10-18, see §7).
- **Exit:** fuzzers run clean for a CI-length budget; operator guide lets a
  new operator stand up Keycloak auth from scratch.

## 13. Decisions & open questions

Decided 2026-10-05:

1. **Duplicate subject → kick the older session** (§5.5).
2. **Lua is high-level:** user data only (`provider`, `subject`, `name`,
   allowlisted `claims`). No tokens, issuer or expiry (§6).
3. **Revocation: yes**, via periodic live re-auth on by default
   (15 min + 2 min grace), with `login_changed` for claim updates (§5.6).

Decided 2026-10-06 (Phase 9.9, `docs/auth-keycloak-testing.md` §6):

4. **Token type check:** reject a JWS header `typ` of `at+jwt` for every
   preset; the `keycloak` preset also requires the payload claim
   `typ == "ID"`, so an access token with our client in `aud` can't be used
   as a login (rule 1b in §5.3, lands with K3.4).
5. **Re-auth in tests** is driven by an automation command
   (`advance_reauth`) that advances the tick-time timers; the 60 s minimum
   interval in `auth.lua` stays as it is.
6. **Real Keycloak in CI:** pinned image on PRs touching auth paths, plus a
   weekly canary against the latest Keycloak release.

Still open:

7. **Instant revocation** (Keycloak back-channel logout or token
   introspection). It needs an inbound HTTP endpoint or a confidential client
   secret on the server. Revisit if operators find interval + grace too slow.
