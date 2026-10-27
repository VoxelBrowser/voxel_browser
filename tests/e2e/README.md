# End-to-end multiplayer tests

Real `voxel_browser_server` and `voxel_browser` processes, driven from Python over the
development-only automation channel (`docs/automation-protocol.md`). Design and the rules
that keep all of this out of production binaries: `docs/e2e-automation.md`.

```python
from vbtest import expect

def test_block_break_replicates(server, clients):
    alice, bob = clients(2, names=["Alice", "Bob"])   # real processes, joined, cold asset caches
    expect(alice).to_be_on_ground()
    server.set_block((4, 70, 4), "base:stone")        # server-side setup
    expect(bob).to_see_block((4, 70, 4), "base:stone")

    alice.break_block((4, 70, 4))                     # aims and clicks like a player
    expect(bob).to_see_block((4, 70, 4), "base:air")  # auto-waits, no sleeps
```

## Authentication tests

Four files, one idea: a real server whose pack ships an `auth.lua`, real headless clients, and an
identity provider behind the `IdpBackend` interface (`vbtest/idp.py`).

| File | What | Needs |
|---|---|---|
| `test_auth.py` | the smoke suite: token-file sign-in, bad tokens, the browser flow, one wall-clock re-auth | emulator |
| `test_auth_keycloak.py` | sign-in scenarios, revocation (admin logout, disabled user, idle session, key and refresh-token rotation), IdP outages and bad bodies, loopback | emulator, or real Keycloak for everything not `mock_only` |
| `test_mock_keycloak.py` | self-tests of the emulator (crypto, protocol, faults); **no game binaries**, runs in seconds | nothing |
| `test_mock_keycloak_matches_real.py` | emulator vs a real Keycloak: same discovery, JWKS, claims, error bodies | `--vb-idp=keycloak` |

**The emulator** (`vbtest/mock_keycloak.py`, stdlib only) speaks Keycloak's URL layout, claims (`azp`,
`typ`, `sid`, `realm_access.roles`, groups), error bodies, SSO sessions with idle/max lifespans,
refresh-token rotation, RS256 and ES256 with key rotation, a login form, and fault injection
(`idp.faults.latency/status/body/drop/discovery_issuer`). It signs with the **test-only** keys in
`fixtures/` (`gen_keys.py` makes them); never use them for anything real. Every request it serves is in
`idp.request_log()`, written to `idp.requests.jsonl` (and shown as a column in `trace.html`) when a test
fails. It is checked against a real Keycloak by `test_mock_keycloak_matches_real.py`, so it cannot drift.

**Markers:** `auth` (every auth test), `slow` (waits on a real clock: the 60 s re-auth interval, the
60 s unknown-key refresh limit), `mock_only` (needs `mint()` or fault injection), `keycloak_real`
(only with `--vb-idp=keycloak`), `realm(...)` (realm options for the `idp` fixture: `alg="ES256"`,
`audience_mapper`, `extra_audience`, `group_paths`, `interactive`, `redirect_pattern`,
`sso_session_idle_timeout`, ...; a backend that cannot do one skips the test).

```bash
python3 -m pytest tests/e2e/test_mock_keycloak.py                        # emulator self-tests, seconds
python3 -m pytest tests/e2e -m "auth and not slow" --vb-build-dir build-e2e   # the fast auth suite
python3 -m pytest tests/e2e -m auth --vb-build-dir build-e2e                   # + the wall-clock tests
```

**Time.** The periodic re-auth has a 60 s minimum interval, so tests do not wait for it: the server's
automation command `advance_reauth {player, seconds}` fast-forwards the tick-time timers and the real
`system_reauth` path runs (`reauth_round()` in `test_auth_keycloak.py` is the pattern). Only
`test_revoked_login_is_kicked_after_the_grace_period` and `test_key_rotation_mid_session` wait on the clock.

**Signing in.** `idp.obtain_tokens("alice", "pw")` returns a token-file token through the real code flow
(no nonce, so it is bound by the freshness rule). A test that needs the client to hold a *refresh token*
(anything re-authenticating against the IdP) plays the browser instead: `BrowserPlayer(idp, url_file,
user, password)` waits for the URL the client writes to `VB_AUTH_URL_FILE`, signs in at the IdP and
delivers the redirect, exactly what a player's browser does. `make_clients(server)(1, home="alice")`
reuses one client data directory, i.e. "the same install" (saved sign-ins, trust).

**Against a real Keycloak** (`--vb-idp=keycloak`, or `VB_IDP=keycloak`): needs `docker`, which starts the
pinned image with `fixtures/keycloak/realm-e2e.json` imported and published on `127.0.0.1` only; or set
`VB_KEYCLOAK_URL=http://127.0.0.1:8180` (plus `VB_KEYCLOAK_ADMIN` / `VB_KEYCLOAK_ADMIN_PASSWORD`) for one
you started with `kc.sh start-dev --import-realm`. `VB_KEYCLOAK_IMAGE` picks another image. A recording
proxy sits in front, so `idp.requests()` / `idp.grants()` work the same; the realm is reset between
tests. In CI: `.github/workflows/auth_keycloak.yml` (auth PRs, manual, weekly canary on the newest
release).

```bash
python3 -m pytest tests/e2e -m "keycloak_real or (auth and not mock_only)" --vb-idp=keycloak --vb-build-dir build-e2e
```

## Running

You need a build with `-DVB_WITH_AUTOMATION=ON -DVB_WITH_NET=ON -DVB_WITH_LUA=ON
-DVB_WITH_COMPRESSION=ON` (and protobuf for networking), and `pip install -r requirements.txt`.
**Use a separate build directory and never ship it**: it contains admin commands
(`set_block`, `teleport`, `run_lua`, ...). Release builds set `VB_DISTRIBUTION=ON`, which
makes that combination a configure error.

```bash
cmake -S . -B build-e2e -DVB_WITH_AUTOMATION=ON -DVB_WITH_NET=ON -DVB_WITH_LUA=ON \
      -DVB_WITH_COMPRESSION=ON -DVB_WITH_REPLICATION=ON
cmake --build build-e2e
ctest --test-dir build-e2e -L e2e --output-on-failure      # or:
python3 -m pytest tests/e2e --vb-build-dir build-e2e -k break
```

| Option / env | Meaning |
|---|---|
| `--vb-build-dir` / `VB_BUILD_DIR` | where `voxel_browser{,_server}` are (default `build`) |
| `--vb-artifacts` / `VB_E2E_ARTIFACTS` | where failed tests' logs go (default `tests/e2e/artifacts`) |
| `--vb-keep-artifacts` | keep artifacts of passing tests too |
| `--vb-allow-remote-host` | let clients connect to a non-loopback host (a dedicated **dev** server only) |
| `VB_E2E_TIMEOUT_SCALE` | multiply every wait, plus the client's connect timeout and the server's `handshake_timeout_seconds` (CTest sets 3 for sanitizer builds) |
| `DISPLAY` | windowed tests need one; they skip themselves without it. Run the suite under `xvfb-run -a -s "-screen 0 1280x720x24" ...` (software GL is enough) |

A failed test leaves, per process, `<name>.stderr.log`, `<name>.trace.jsonl` (every request
and reply with timestamps) and `<name>.final_state.json` under the artifacts directory, plus
**`trace.html`**: one self-contained page with a column per process in wall-clock order
(requests blue, replies green, errors red), the quickest way to see "Alice did X but Bob never
saw it". The path is printed with the failure; rebuild a page with
`python3 -m vbtest.traceview <artifact dir>`.

## Writing tests

- **Fixtures**: `server` (a fresh dedicated server per test, free loopback port, private copy
  of `content/base`, no persistence) and `clients(n, names=None)` / `client`. Mark a test
  `@pytest.mark.vb_server(view_distance=2)` to add `server.toml` keys, and
  `@pytest.mark.vb_pack_files({"zz_test.lua": "..."})` to add files to the server's pack copy.
- **Latency**: `test_latency.py` times click → crack update from `break_block`'s `confirm_ms`.
  `VB_E2E_BENCH=1` also runs the slow "punch while chunks stream" benchmark (prints numbers).
- **Handles**: `Server` (`set_block`, `fill`, `teleport`, `give`, `set_time`, `set_health`,
  `kick`, `run_lua`, `block_at`, `state`) and `Client` (`key_press`, `walk_to`, `break_block`,
  `place_block`, `select_slot`, `chat`, `ui("id").click()`, `look_at`, `state`, ...). Every call
  is a real command from `docs/automation-protocol.md`; errors raise `AutomationError` with the
  process's error code.
- **Assertions**: `expect(handle).to_...(...)`; `.not_` negates. Each is *one* `wait_for` that the
  process re-checks every frame, so write no `sleep`s and no polling loops. On timeout the
  message says what the process last saw.

### Things that will bite you (each one did)

- **Wait for landing.** Players spawn above the ground and fall. Use
  `expect(c).to_be_on_ground()` before reading `feet`; a position sampled mid-fall is a
  block off. `feet.y` of a landed player is `62.9999`-ish: round with a small epsilon.
- **Build only in loaded chunks.** The server refuses edits outside them (it would otherwise
  create empty chunks), and its world can lag a moment behind the client's join. Wait with
  `expect(server).to_have_chunk_loaded(pos)` for *every* chunk your scene touches, and check
  `fill`'s `not_loaded == 0`.
- **Players spawn on the same spot.** A punch hits the nearest *player* before any block, so
  teleport the other player away before `break_block`.
- **Pack UI needs a dedicated server.** `--loopback` never installs the pack's keybinds
  (`docs/e2e-automation.md` §10), so `key_press("inventory")` only works against `server`.
- Click actions hold their aim steady for a few frames first (the server applies a command's
  look direction *after* running the pack's input hook), so `break_block`/`place_block` take
  about a tenth of a second longer than the raw round trip.
- **Windowed clients** (`clients(1, windowed=True)`, or `via_menu=True` to start on the main
  menu and call `menu_connect`) open a real window: slower to reach `playing` (a loading screen
  meshes the first chunks), and they need a display. `client.screenshot(path)` saves a PNG;
  `vbtest.png.stats(path)` gives size, distinct colours and mean colour, enough to assert a
  frame isn't blank (a flat UI has few colours: don't expect photo-like numbers).
- `client.type("text")` fills the chat box like keystrokes; `key_press("enter")` sends it.

### Attaching over TCP, and recording

`clients(1, tcp=True)` starts the client with `--automation tcp` and attaches over loopback with a
token; `client.detach()` drops the connection while the game keeps running and `client.reattach()`
reconnects (see `test_attach.py`). `Client("name", None, artifact_dir, attach={"host":..., "port":...,
"token":...})` attaches to any running game, e.g. one a human is playing.

`voxel_browser --automation-record session.py` writes what a player does as a vbtest script
(`walk_to`, `break_block`, `place_block`, `chat`, `ui(...).click()`, plus `expect(...)` after results
it could see). It is a starting point: add scene setup with `server`, run it with
`pytest session.py` (put it in `tests/e2e`). `test_recorder.py` records a session and replays the
generated script on a fresh server. Details: `docs/automation-protocol.md`.

### Bad networks

`@pytest.mark.net_sim(lag_ms=100, jitter_ms=20, loss_pct=3)` starts the server and every client
with `--net-sim`. Each side only delays what *it sends*, so `lag_ms=100` is about 200 ms of round
trip: assert it with `expect(client).to_have_rtt_at_least(150)`, and give waits generous
timeouts. See `test_bad_network.py`.

## Layout

```
conftest.py       fixtures: binaries check, artifacts, server, clients, loopback guard
vbtest/process.py JSON-lines client for one process (+ trace/stderr logs)
vbtest/handles.py Server / Client / Locator
vbtest/expect.py  expect(...).to_see_block(...) etc.
vbtest/net.py     free UDP port, "never aim bots at a real server" guard
vbtest/scene.py   build-a-scene helpers (block_under_feet, clear_corridor, step_aside)
vbtest/png.py     tiny PNG reader for screenshot assertions
vbtest/stack.py   start_server / ClientFactory: what the fixtures use, also usable outside them
vbtest/traceview.py  failed-test trace.html generator
test_*.py         the tests (basics, bad network, windowed, trace viewer, attach, recorder, `vb pack dev`)
```
