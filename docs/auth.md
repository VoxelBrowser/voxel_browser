# Player authentication — operator guide

A content pack that ships an `auth.lua` at its root makes **sign-in mandatory**: the
server verifies an identity-provider (IdP) ID token during the handshake, before it
sends a single asset, block or chunk. Pack scripts then see *who* the player is
(`player:get_login()`, `docs/lua-api.md`). Design and rationale:
`architecture_spec/auth.md`.

There is no switch to turn it off — delete `auth.lua` to run without authentication.
A pack without `auth.lua` behaves exactly as before (`get_login()` is `nil` for everyone).

## 1. `auth.lua`

```lua
return {
	provider     = "keycloak",            -- "oidc" | "keycloak" | "firebase"
	display_name = "Example Realm",       -- shown on the sign-in screen
	issuer       = "https://id.example.com/realms/game",
	client_id    = "voxel-browser",       -- public client; no secret ever goes in a pack
	scopes       = { "openid", "profile", "email" },
	name_claim   = "preferred_username",  -- becomes the in-game name
	claims       = { "email", "email_verified", "groups" },  -- exposed to Lua, nothing else is
	max_token_age_seconds   = 300,        -- optional: how fresh a token must be (default 300)
	reauth_interval_seconds = 900,        -- optional: revocation check, min 60, 0 = off (default 900)
	reauth_grace_seconds    = 120,        -- optional: time to answer a re-auth (min 10, default 120)
}
```

Unknown keys, wrong types, an `http://` issuer (except `127.0.0.1`/`localhost`, for development)
or a missing required key make the server **refuse to start** with a precise message. It never
silently falls back to "no auth".

### Deployment overrides (`server.toml`)

So one pack works against staging and production, these may be overridden — they can change
*where* identity is checked, never *whether*:

```toml
[auth]
issuer    = "https://id.prod.example.com/realms/game"
client_id = "voxel-browser-prod"
# firebase: project_id, api_key
```

With the `vb` tool: `vb server config <name> set auth.issuer https://id.prod.example.com/realms/game`
(`auth.client_id`, `auth.project_id`, `auth.api_key`; `get` never prints the API key).

## 2. Keycloak

1. Realm → **Clients → Create client**, type *OpenID Connect*, client id = `client_id` above.
2. **Client authentication: OFF** (public client). **Standard flow: ON**; everything else off.
3. **Valid redirect URIs:** `http://127.0.0.1/*` (the client listens on an ephemeral loopback
   port, RFC 8252). Advanced → *Proof Key for Code Exchange Code Challenge Method:* **S256**.
4. Pick `name_claim` (`preferred_username` is the usual choice). For group-based logic add a
   *Group Membership* mapper (claim `groups`, *Add to ID token* ON) and list `"groups"` in `claims`.
   Keycloak leaves the claim out entirely for a user who is in no group (and *Full group path*
   decides between `admins` and `/admins`), so scripts should read it as `login.claims.groups or {}`.
5. `issuer` is the realm URL, `https://<host>/realms/<realm>`.

Revocation: *Sessions → Sign out* (or disabling the user) makes the IdP refuse the player's
refresh token; the server notices at the next periodic re-auth (see §5).

## 3. Firebase Authentication

```lua
return {
	provider   = "firebase",
	project_id = "my-game-1234",
	api_key    = "AIza...",                -- web API key (public by design)
	sign_in    = { "password" },           -- engine-drawn e-mail/password form
	name_claim = "name",
	claims     = { "email", "email_verified" },
}
```

Enable *Email/Password* in the console. The issuer/audience are derived from `project_id`; keys
come from Google's published `securetoken` JWKS. The password goes from the player's machine
straight to Google, never to your server. (`"google"` sign-in is listed in the design but not
implemented yet — the screen only offers `password`.) Revoke with `revokeRefreshTokens` or by
disabling the account.

## 4. What the server checks

Algorithm allowlist (RS256/ES256 only; `none`/`HS*` rejected), key by `kid` from the IdP's JWKS
(one rate-limited refresh when an unknown `kid` shows up), signature, `iss`, `aud`/`azp`, `exp`
(60 s skew), `iat` no older than `max_token_age_seconds`, the per-connection `nonce` when the token
has one, a non-empty `sub`, and a usable name.

It also checks **what kind of token** it was given. A JWS header `typ` of `at+jwt` (an RFC 9068
access token) is refused for every provider preset, and for `provider = "keycloak"` the payload
claim `typ` must be `"ID"`. Keycloak signs access tokens (`typ: "Bearer"`) and logout tokens with
the same realm key; with an audience mapper an access token's `aud` contains your client id, so
without this rule anything that holds a player's access token could present it as a login.
Real players are unaffected (the client only ever sends the `id_token`); `--auth-token-file`
users who paste an access token by mistake get a clear refusal in the server log. Players are identified by `(issuer, subject)` —
**never by name**. Two accounts that want the same name get `alex` and `alex#2`; a second
sign-in of the same account kicks the older session ("signed in elsewhere").

If the key set cannot be loaded (IdP down at startup) the server still starts, retries with
backoff, and answers joins with "authentication service unavailable" until it has keys.

## 5. Revocation: periodic re-authentication

ID tokens cannot be revoked once issued; the IdP session behind them can. Every
`reauth_interval_seconds` (±10% per player) the server asks the client to prove its login again.
The client does this silently with its refresh token; if the IdP refuses (revoked, disabled,
password changed), the player sees "Your sign-in expired — Sign in again" and the server kicks
them when `reauth_grace_seconds` runs out. **Worst-case revocation latency = interval + grace**
(default ≈ 17 minutes). An IdP *outage* is not a revocation: if Keycloak cannot be reached (or answers
5xx) the client keeps its refresh token and retries every few seconds, and the player stays in as long
as the IdP is back before the grace period ends; after that the server kicks (fail closed).
Lower the interval if you need tighter revocation; instant push
revocation (back-channel logout / introspection) is out of scope.

Pack-side bans don't need this: a `player_join` veto keyed on `login.subject` keeps a banned
account out at the next join.

## 6. Player side

- Browser flow (Keycloak/OIDC): the engine opens the system browser; the redirect lands on a
  loopback listener (`127.0.0.1` only) and the token is exchanged with PKCE.
- First time a given server asks for a given IdP, the player is asked whether to trust that
  pairing. Nothing — not even a saved refresh token — is sent to the IdP before they agree.
- Refresh tokens are cached per `(server, issuer, client_id)`: in the OS keychain (macOS Keychain,
  Windows Credential Manager, or the Secret Service — GNOME Keyring, KWallet, KeePassXC — on Linux),
  with only metadata in owner-only files under the user config dir (`auth/`). Without a keychain
  (no libsecret or session bus, or `VB_KEYCHAIN=off`) the token stays in that file. *Sign out* in
  the main menu deletes both. Rejoining within the refresh window opens no browser.
- Singleplayer with an auth pack runs the same real sign-in.

## 7. Development & testing

- `voxel_browser_server --insecure-skip-auth` / `voxel_browser --insecure-skip-auth`
  (singleplayer): ignore `auth.lua`; `get_login()` is `nil`. Logged loudly, compiled out of
  distribution builds (`VB_DISTRIBUTION`).
- `voxel_browser --auth-token-file <file>`: sign in with the ID token in that file (re-read for every
  sign-in and re-auth). Automation builds only.
- `tests/e2e/test_auth.py` (smoke) and `tests/e2e/test_auth_keycloak.py` run the whole thing against a
  Keycloak emulator (`tests/e2e/vbtest/mock_keycloak.py`), and against a real Keycloak in docker with
  `--vb-idp=keycloak` (CI: `.github/workflows/auth_keycloak.yml` on auth PRs, plus a weekly canary on
  Keycloak's newest release). How it all fits together: `docs/auth-keycloak-testing.md`,
  `tests/e2e/README.md` ("Authentication tests").

## 8. Operational notes

- The server needs outbound HTTPS to the issuer (discovery, JWKS). Tokens are never logged or stored
  server-side and never reach a Lua VM.
- Sign-in attempts are rate-limited per peer IP (30/min by default) on top of the connection cap and
  the message flood guard; at most one JWKS fetch is in flight and failed fetches back off.
- Known limit: a hostile *server* can claim any issuer in its challenge and relay a token it receives.
  The per-connection nonce, the freshness bound and the trust prompt reduce this; full server
  binding (DPoP-style proof of possession) is out of scope.
