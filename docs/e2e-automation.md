# Voxel Browser — End-to-End Client Automation (Design)

> Status: **In progress**; E0 (input seam), E1 (`ClientApp`), E2 (build
> flags, JSON-lines host, predicate engine, read-only queries), E2b
> (handshake `client_flags`, protocol v27) and E3 (client actions, server
> admin commands) and E4 (pytest `vbtest` harness, `e2e` CTest label, CI job)
> and E5 (`--net-sim`, windowed clients with menu commands and screenshots, typed
> chat, trace viewer) and E6 (TCP attach, session recorder) are implemented
> (contract: `docs/automation-protocol.md`; how to write tests: `tests/e2e/README.md`).
> Everything is landed; the Linux CI job runs green on every PR (first confirmed green
> 2026-10-17, run 39044682584). Windows/macOS: see §10 item 19. Covers a
> Playwright-style harness that drives real `voxel_browser` clients against a
> real `voxel_browser_server` in multiplayer, so gameplay can be tested by
> scripts instead of by hand. Complements spec §17 (Testing Strategy); the
> in-process `IntegratedGame`/`LoopbackNetwork` tests in `tests/unit/` stay
> as they are.

## 1. Problem

Tests today are in-process: `netcode_test.cpp`, `soak_test.cpp`, and
`blockedit_test.cpp` wire `ServerSession` and `ClientSession` together over
`LoopbackNetwork` and call `tick()` by hand. They cover the session and
protocol layer well. They do **not** cover:

- **The client app layer.** All of `src/client/main.cpp`'s windowed loop is
  untested: the `AppState` machine (menu → connecting → loading → playing),
  `sample_input_cmd`, mouse capture, chat box, hotbar keys, block targeting
  and break/place, `UiRuntime` screens and HUD, and the crack overlay.
  `--headless` goes through a separate, minimal `run_headless()` that skips
  all of it.
- **Real multi-process multiplayer.** That means two or more client
  processes talking to a dedicated server over GNS/UDP, with real asset sync
  into a cold cache, real timing, and real packet loss. Only the
  `*_smoke` CTest entries come close, and they only check that a join line
  gets printed.

Playwright solves the browser version of this problem in four parts:

1. A **driver channel** into the running app.
2. **Locators**, which find UI by meaning rather than by pixels.
3. **Auto-waiting assertions**, so tests don't need `sleep`.
4. A **test runner** with fixtures, parallel runs, and failure artifacts.

This design copies that shape for a native raylib game client.

## 2. Goals / non-goals

**Goals**

- Write tests like "client A breaks a block → client B sees it gone", "A
  says hi → B's chat shows it", or "open the inventory, click craft →
  planks appear" in a short, readable script.
- Drive the **same code path a player uses**. Synthetic input goes into the
  same `InputCmd` sampling, the same `UiRuntime::report_click`, and so on. It
  never bypasses them by sending protocol messages directly.
- Run headless in CI on Linux/macOS/Windows with no GPU. Screenshots are an
  optional extra (Xvfb leg).
- Run N clients against one server, with optional simulated lag/loss.
- **Development only.** Automation is compiled in only when
  `VB_WITH_AUTOMATION=ON` (default `OFF`). Production builds can't contain
  it, a production server rejects clients built with it, and the release
  pipeline checks both. See §7.

**Non-goals (for now)**

- Clicking by pixel coordinates or doing image-diff golden tests. Both are
  brittle against a voxel scene; see §10 for a limited screenshot story.
- Bit-exact deterministic replays across processes. Real UDP is
  non-deterministic by nature. Use auto-waiting instead (§5.2). In-process
  deterministic tests stay where they are.
- Bots/AI players for load testing. The harness makes this easy later, but
  it's out of scope.

## 3. Architecture

```
            pytest  (tests/e2e/*.py)
              │  vbtest package: fixtures, Client/Server handles, expect()
              │
   ┌──────────┼───────────────────────────┬──────────────────────┐
   │ stdio    │ (JSON lines)              │ stdio                │ stdio
   ▼          ▼                           ▼                      ▼
voxel_browser_server             voxel_browser #A        voxel_browser #B
  --automation stdio               --automation stdio      --automation stdio
  ├ AutomationHost (server)        ├ AutomationHost (client)
  │   state / admin cmds           │   queries / actions / waits
  └ ServerSession  ◀── GNS/UDP ──▶ └ ClientApp ── InputSource = Synthetic
```

Four pieces, each independently useful:

| # | Piece | Where | Purpose |
|---|---|---|---|
| A | `InputSource` seam + `ClientApp` refactor | `src/client/` | Makes the real game loop drivable and headless-runnable |
| B | `AutomationHost` (client + server) | `src/automation/` (new module) | The driver channel: parse commands, run them, reply |
| C | Wire protocol | `docs/automation-protocol.md` | JSON-lines RPC, versioned |
| D | `vbtest` harness | `tests/e2e/` | Python/pytest: process orchestration, Playwright-like API |

### 3.1 Why stdio (pipe) transport

Playwright itself drives Chromium over `--remote-debugging-pipe`, and the
same reasons apply here:

- **No exposed port.** The server-side automation commands are effectively
  god-mode (`set_block`, `teleport`, `run_lua`). A pipe owned by the parent
  process can't be reached by anything else, so there's no auth problem to
  solve.
- **Cross-platform with no dependencies.** It's `std::cin`/`std::cout` on a
  reader thread. No sockets library is needed on the side that isn't
  `VB_WITH_NET`.
- **Lifetime is free.** When the harness dies, stdin hits EOF and the
  process exits. CI never leaves orphaned servers behind.

Game logs move to **stderr** when `--automation stdio` is active (stdout is
reserved for protocol frames). A TCP mode (`--automation tcp[:PORT]`, **implemented in E6**) attaches to a client a
human is already running: it binds `127.0.0.1` only (no option for anything else), requires a
random token as the first line, serves one connection at a time and keeps the game running when
it drops. Details and the auth handshake: `docs/automation-protocol.md`.

## 4. Piece A — make the client drivable

This is the main prerequisite, and also the largest change.

### 4.1 `InputSource`

Today, raylib input is read directly in about 17 places in
`src/client/main.cpp` (`IsKeyDown`, `IsKeyPressed`,
`IsMouseButtonDown/Pressed`, `GetMouseDelta`) and in the keybindings screen's
`GetKeyPressed()` in `src/render/main_menu.cpp`. Introduce:

```cpp
// inc/vb/client/input.hpp
struct InputFrame {
    std::bitset<512> keys_down, keys_pressed;   // raylib KeyboardKey values
    std::bitset<8>   mouse_down, mouse_pressed;
    Vector2          mouse_delta{}, mouse_pos{};
    std::u32string   chars;                     // GetCharPressed() drain
};

class InputSource {
public:
    virtual ~InputSource() = default;
    virtual InputFrame poll() = 0;              // called once per frame
};
class RaylibInput   final : public InputSource { ... };  // today's behavior
class SyntheticInput final : public InputSource {        // automation-fed
public: void hold(Key, int frames); void press(Key); void type(std::string);
        void look_delta(Vector2); ...
};
```

`sample_input_cmd(...)` and the chat, hotbar, and mouse-capture logic then
read from `const InputFrame&` instead of calling raylib. This change is a
pure refactor with no behavior change, and it also makes those functions
unit-testable.

**raygui is a special case.** Widgets read raylib's input state internally,
and raylib has no public injection API. So automation doesn't try to fake
pixel clicks on raygui. It goes one level up, to the place each widget's
result is consumed:

- Pack UI and HUD: call `UiRuntime::report_click/report_change/
  report_list_change(id)` (and the `_hud` variants), the same calls
  `main.cpp` makes when `UiRenderer::draw` reports a hit.
- Main menu: `MainMenu::draw_*` results become a value the automation layer
  can inject (`MenuResult{.connect=true}`), and the text fields
  (`player_name`, server address) get setters.
- Chat input box: `chat_buf` is filled from `InputFrame::chars`, and Enter
  submits it. Typing therefore goes through the real path.

### 4.2 `ClientApp`: one loop for windowed and headless

`run_headless()` is a separate, cut-down loop, so automation against it
would test something players never run. Pull the windowed loop's state and
logic out of `main()` into a `ClientApp` class:

```cpp
class ClientApp {
public:
    ClientApp(ClientConfig, AppOptions, InputSource&, bool render);
    void frame(double dt);          // one iteration of today's while-loop body
    AppState state() const;
    ClientSession* session();       // null until connecting
    UiRuntime& ui();
    const std::deque<std::string>& chat_log() const;
    std::optional<RaycastHit> target_block() const;
    ...
};
```

When `render == false`, all `window.begin_frame()`, draw, and GPU-upload
calls are skipped (mesh building can be skipped too, behind a flag).
Everything else runs the same way: the state machine, input sampling,
prediction, UI runtime, and chat. `run_headless()` then becomes "build
`ClientApp` with `render=false`, then call `frame()` N times". The
`*_smoke` tests keep their expected output.

### 4.3 Clock control

`--automation-clock real|manual`:

- `real` (default): the frame loop runs freely at fixed `dt = 1/60` when
  headless, or `GetFrameTime()` when windowed. Commands are applied at the
  start of the next frame.
- `manual`: the loop blocks until it receives `step {frames: n}`. This is
  for tests that need exact frame counts, such as "hold W for exactly 30
  frames, expect ≥ X metres moved". The **server** still runs on wall
  clock, so manual mode only removes client-side jitter.

## 5. Piece B/C — the automation protocol

JSON lines with request/response correlation and server-pushed events.
nlohmann/json is already a dependency (currently fetched only under
`VB_WITH_LUA`). Gate it on `VB_WITH_AUTOMATION` as well.

```jsonc
→ {"id":1,"cmd":"hello","proto":1}
← {"id":1,"ok":true,"result":{"role":"client","engine":"0.7.0","features":["ui","chat","screenshot"]}}
→ {"id":2,"cmd":"wait_for","args":{"pred":{"chat_contains":"hi"},"timeout_ms":5000}}
← {"id":2,"ok":false,"error":{"code":"timeout","last":{"chat":["<A> hello"]}}}
← {"event":"log","level":"warn","msg":"..."}           // unsolicited
```

All commands run on the **main thread** between frames. The stdin reader
thread only enqueues parsed requests into a mutex-guarded queue. This
keeps automation clear of the threading gotchas in `state/gotchas.md`
(mesh worker pool, worldgen pool).

### 5.1 Client commands

| Group | Command | Notes |
|---|---|---|
| meta | `hello`, `step{frames}`, `quit` | `step` is manual clock only |
| menu | `menu.set_name`, `menu.connect{host,port}`, `menu.loopback` (alias `menu.singleplayer`) | Injects `MenuResult` |
| query | `state` | Returns one snapshot: `app_state`, `handshake`, `net_id`, `feet`, `yaw/pitch`, `health`, `hotbar`, `selected_slot`, `chat[]`, `remote_entities[{net_id,name,kind,pos}]`, `ui{name,widgets[{id,type,text,items,list_index}]}`, `hud{widgets}`, `chunks_loaded`, `target_block` |
| query | `block_at{pos}` | From `ClientChunkStore` (the client's view) |
| input | `key.hold{key,frames}`, `key.press{key}`, `mouse.hold{button,frames}`, `look{yaw,pitch}` / `look_at{pos}`, `type{text}` | Feeds `SyntheticInput`; key names map to logical bindings (`forward`, `jump`, `primary`) **or** raw raylib keys |
| high-level | `walk_to{pos,tolerance}`, `break_block{pos}`, `place_block{pos,face,slot}`, `select_slot{n}`, `chat.send{text}` | Built *on top of* the input commands: look at the target, then hold primary until the block changes. No protocol shortcuts. |
| ui | `ui.click{id}`, `ui.fill{id,text}`, `ui.select{id,index}`, `hud.click{id}` | Locator = widget `id` (unique per screen). Fails fast if the screen or widget isn't present. |
| wait | `wait_for{pred,timeout_ms}` | Predicate is evaluated **every frame in-process** (no polling over the pipe) |
| artifacts | `screenshot{path}` | Windowed builds only; returns `unsupported` when headless |

### 5.2 Predicates (the auto-wait vocabulary)

These are declarative and composable (`all`/`any`/`not`), and evaluated
in C++:

`joined`, `app_state{is}`, `chat_contains{text|regex}`,
`entity_visible{name}`, `entity_near{name,pos,radius}`,
`block_is{pos,block}` (block by registry *name*, e.g. `"base:air"`),
`ui_open{name}`, `widget{id,text?}`, `pos_near{pos,radius}`,
`inventory_has{item,count}`, `health{op,value}`, `chunks_loaded{min}`.

On timeout, the reply includes the predicate's last observed values. That
makes failures say *why* (`chat was ["<A> hello"]`), the way
`expect(...).toHaveText` does.

### 5.3 Server commands

The server takes `--automation stdio` too. It's mainly used for **setup
and verification**, because the server is the source of truth:

`hello` (returns the **actually bound port**, so tests can pass `--port 0`
and avoid collisions), `state` (players, positions, tick, time of day),
`block_at`, `set_block`, `fill`, `teleport{player,pos}`,
`give{player,item,count}`, `set_time`, `set_health`, `kick`,
`run_lua{code}` (in the pack VM, for pack-specific setup), and
`wait_for{pred}`, which uses the same predicate engine with server-side
predicates (`player_count`, `block_is`, `player_near`).

Every server command sits behind the stdio channel. With no
`--automation` flag the code paths can't be reached, and with
`VB_WITH_AUTOMATION=OFF` they aren't compiled at all. Unlike other gated
features, there is no stub; see §7.2.

### 5.4 Network conditions

`--net-sim lag_ms=120,jitter_ms=30,loss_pct=5` on both binaries, mapped onto GNS's
global `FakePacketLag/Jitter/Loss/Reorder/Dup_Send` settings (`GnsTransport::set_net_sim`,
`inc/vb/net/net_sim.hpp`). This lets prediction, reconciliation and interpolation be tested
under bad conditions, which could previously only be checked by hand. **Implemented in E5.**
Each side only delays what it *sends*, so tests give both binaries the spec. Two changes from
the original sketch: GNS has real jitter settings (no need to approximate), and there is **no
`LoopbackTransport` queue**: `--loopback` is refused, since it has no network to degrade.
`--net-sim` is development-only too and sits behind the same flag (a production build
refuses it, checked by CTest and the release legs).

## 6. Piece D — the `vbtest` harness

**Python + pytest.** CI runners on all three OSes already ship Python 3,
and pytest supplies the Playwright Test features we want: fixtures,
parametrization, `-n` parallelism (pytest-xdist), retries, and JUnit XML.
The alternative was TypeScript with `@playwright/test` used only as a
runner. It's closer to Playwright, but it brings a Node toolchain into a
pure C++/CMake/Lua repo.

**Implemented as a synchronous API** (decided in E4, a change from the async sketch this
section originally had). Threads read the pipes, so tests are plain functions with no
`await`/`pytest-asyncio` (pytest is the only dependency), and nothing is lost: retrying
still happens inside the process (`expect` is one `wait_for`), and tests almost never need
two things at once. The API mirrors Playwright's naming:

```python
# tests/e2e/test_multiplayer_basics.py
from vbtest import expect

def test_block_break_replicates(server, clients):
    alice, bob = clients(2, names=["Alice", "Bob"])    # spawn + join both
    x, y, z = block_under_feet(alice)                  # after expect(alice).to_be_on_ground()
    server.set_block((x, y, z - 2), "base:stone")
    expect(bob).to_see_block((x, y, z - 2), "base:stone")

    alice.break_block((x, y, z - 2))                   # aims + clicks like a player
    expect(bob).to_see_block((x, y, z - 2), "base:air")

def test_craft_planks_with_the_chat_command(server, clients):
    (alice,) = clients(1, names=["Alice"])
    server.give(alice, "base:wood", 1)
    alice.chat("/craft base:planks")                   # crafting is a chat command, not a UI
    expect(alice).to_have_inventory("base:planks", 4)

def test_inventory_screen_opens_and_closes(server, clients):
    (alice,) = clients(1, names=["Alice"])
    alice.key_press("inventory")                       # logical keybind name
    expect(alice).to_have_ui_open("base:inventory")
    alice.ui("close").click()                          # locator by widget id
    expect(alice).not_.to_have_ui_open("base:inventory")
```

(Bad-network tests use `@pytest.mark.net_sim(lag_ms=..., loss_pct=...)`; see `tests/e2e/test_bad_network.py`.)

**Fixtures (`tests/e2e/conftest.py`):**

- `binaries`: locates `voxel_browser{,_server}` from `--vb-build-dir` or
  the `VB_BUILD_DIR` env var (set by CTest).
- `server`: one per test (function scope). Uses a temp dir copy of
  `server.toml.example`, a fixed seed, a free UDP port picked by the harness
  (GNS can't listen on port 0, §10 item 7), and a private copy of the base pack
  (so `vb.storage` can't dirty the repo; `@pytest.mark.vb_server(...)` adds
  `server.toml` keys, `@pytest.mark.vb_pack_files({path: text})` adds files to
  that pack copy; other packs via `@pytest.mark.pack(...)` are not built yet).
  Persistence is off, so every test gets a fresh world.
- `clients(n, names=…)`: a factory. Each client gets its **own temp asset
  cache dir** (so asset sync is exercised cold) and its own
  `client.toml`, spawns with
  `--headless --automation stdio --server 127.0.0.1 --port <bound>`, and
  awaits `wait_for joined`.
- Teardown: `quit` → wait → kill. A failed test keeps each process's stderr
  log, JSONL command trace and final `state` snapshot under the artifacts
  directory (passing tests delete theirs) and prints the path.

The `vbtest` package itself (`tests/e2e/vbtest/`) is about 500 lines:
a subprocess wrapper with request/response correlation,
`Client`/`Server` handles, `Locator`, and an `expect()` that just builds a
predicate and sends one `wait_for` (so retrying happens in-process, every
frame).

### 6.1 Tracing (Playwright trace-viewer analogue; implemented in E5)

Every command and response gets a timestamp and is written to
`<test>/trace.jsonl` for each process. When the test fails, it also
includes the `state` snapshot taken just before each action. A small
static HTML viewer can lay out the clients' timelines side by side, which
is the main need when debugging "A did X, but B never saw it".

**Implemented as** `tests/e2e/vbtest/traceview.py`: every process already logs
`<name>.trace.jsonl` (now with an absolute `ts`), and when a test fails the fixtures write
`<artifacts>/<test>/trace.html`: a self-contained page with one column per process, rows in
wall-clock order, requests/replies/errors colour-coded, plus each process's final state and
stderr tail. Re-create one with `python3 -m vbtest.traceview <artifact dir>`.

## 7. Keeping automation out of production

Automation is a **development tool**. A shipped client or server must not
be able to drive or be driven by it, and our own test bots must never
reach a production server. There are four layers, each catching what the
one before it might miss.

### 7.1 One CMake flag, default `OFF`

```cmake
option(VB_WITH_AUTOMATION
  "Development/test-only automation driver. Never enable for shipped builds." OFF)
```

The flag doesn't depend on `VB_BUILD_TESTS`; the plain build from the
README's Getting Started gets no automation. Developers turn it on in a
separate build directory, which is the multi-build-dir workflow
`CONTRIBUTING.md` already recommends:

```bash
cmake -S . -B build-e2e -DVB_WITH_AUTOMATION=ON -DVB_WITH_NET=ON -DVB_WITH_LUA=ON
cmake --build build-e2e && ctest --test-dir build-e2e -L e2e
```

### 7.2 What "compiled out" means

**Gated by the flag. Absent from the binary when `OFF`:**

- The whole `src/automation/` module: the JSON-lines host, command table,
  and predicate engine. It isn't added to any target.
- `SyntheticInput`.
- CLI flags: `--automation`, `--automation-clock`, `--net-sim`.
- All server admin commands (`set_block`, `teleport`, `give`, `run_lua`, …).
- Setting the "automation client" handshake bit (§7.4).

There is **deliberately no stub**. Other `VB_WITH_*` features link a stub
that returns `kDisabled`, because the rest of the engine calls into them.
Nothing in the engine calls automation; only the two `main()` entry
points wire it in, inside `#if VB_WITH_AUTOMATION`. So with the flag
`OFF`, the binary contains no automation code, no command names, and no
admin entry points that could be reached by mistake.

**Stays in production (it's ordinary engine code, not automation):** the
`InputSource`/`RaylibInput` seam (§4.1) and the `ClientApp` refactor
(§4.2). Production uses them with real raylib input.

**A production binary given `--automation` exits with an error**
("built without VB_WITH_AUTOMATION"). It never silently ignores the flag,
so a misconfigured test setup fails loudly instead of quietly running
nothing.

### 7.3 Release pipeline can't ship it by accident

- **Hard configure error.** A new `VB_DISTRIBUTION` option is set by
  every build whose binaries get uploaded:
  ```cmake
  if(VB_DISTRIBUTION AND VB_WITH_AUTOMATION)
    message(FATAL_ERROR "VB_WITH_AUTOMATION must be OFF in distribution builds")
  endif()
  ```
- **Separate CI leg.** Today the `release`/`debug` legs of `build_*.yml`
  both run tests and upload the binaries that `publish.yml` releases. Those
  legs get `-DVB_DISTRIBUTION=ON` and keep automation off. e2e tests run in
  a **new `e2e` matrix leg**. Its binaries are never staged or uploaded:
  the "Stage binaries"/"Export binaries" steps stay limited to
  `release`/`debug`.
- **Binary self-report plus a CI check.** `describe_build()` (shown by
  `--version`) appends `+automation` when it's compiled in. After the
  "Stage binaries" step, a check runs `voxel_browser_server --version` and
  `voxel_browser --version` and fails the job if either says
  `automation`. It also checks that `voxel_browser_server --automation
  stdio` exits non-zero. This catches someone flipping the flag in a
  release leg, even if they also removed `VB_DISTRIBUTION`.

### 7.4 Production servers refuse automation clients

Even with all of the above, a developer could point an automation-enabled
client at a production address by mistake, such as a stale config or a
test fixture with the wrong host. Two guards:

- **Handshake bit.** `C2S_Hello` gains a `client_flags` field (protocol
  version bump), and clients built with automation set
  `kClientFlagAutomation`. A server built **without** automation rejects
  them during the handshake: *"automation clients are not accepted by this
  server"*. A server built with it (a dev server) accepts them. The
  rejection is sent before asset sync, so the rejected client costs the
  server almost nothing. **Implemented in E2b (protocol v27):** the flag is
  `protocol::kClientFlagAutomation` (bit 0), set automatically by
  `ClientHandshake::start()` in a `VB_WITH_AUTOMATION` build;
  `HandshakeServerConfig::accept_automation_clients` defaults to "this binary
  was built with automation" and is checked in `ServerHandshake::on_frame`
  before `S2C_ServerInfo`, so no auth or asset work happens. The refusal
  reuses `DisconnectReason::kBadHandshake` with the message above. Unknown
  flag bits are ignored. The loopback-only half is implemented in E4 (`tests/e2e/vbtest/net.py`; `clients(host=...)` raises `UnsafeHostError` unless `--vb-allow-remote-host`).
- **Loopback-only harness.** `vbtest` refuses to connect clients to any
  address other than `127.0.0.1`/`::1`/`localhost` unless
  `--vb-allow-remote-host` is passed explicitly, for running tests against
  a dedicated *dev* server on another machine.

### 7.5 What this doesn't protect against

This keeps **our** test tooling out of production. It doesn't stop bots in
general. The protocol is open and the client is open source, so anyone can
build their own client with the flag on and clear the handshake bit, or
write a bot from scratch. The handshake bit protects against accidents,
not attackers.

Protection against real bot floods has to be enforced by the server,
whatever the client claims:

- The existing per-connection flood guard
  (`ServerSession::set_max_messages_per_second`).
- `auth_mode` tokens for private servers.
- Separate follow-up work (not part of this design): a max-player cap, a
  per-IP connection limit, and a rate limit on new handshakes.

## 8. Build & CI integration

- `VB_WITH_AUTOMATION` and `VB_DISTRIBUTION` as in §7.
- `tests/CMakeLists.txt`: `add_test(NAME e2e COMMAND ${Python3_EXECUTABLE}
  -m pytest ${CMAKE_SOURCE_DIR}/tests/e2e --vb-build-dir ${CMAKE_BINARY_DIR}
  -q --junitxml=e2e.xml)` with label `e2e`, registered only when
  `VB_WITH_AUTOMATION AND VB_WITH_NET` and Python is found.
  `ctest -L e2e` / `ctest -LE e2e` select or skip it.
- `build_linux.yml`: add an `e2e` leg (`RelWithDebInfo` with ASan/UBSan
  plus `-DVB_WITH_AUTOMATION=ON`; ASan catches teardown and leak bugs in
  real multi-process shutdown). This leg never uploads binaries (§7.3).
  The `release`/`debug` legs get `-DVB_DISTRIBUTION=ON` and the
  `--version` check. The `tsan` leg keeps automation off at first, since
  GNS suppressions already make it noisy. Add a separate `xvfb-run` leg
  later for the windowed and screenshot tests.
- Windows and macOS: run after Linux is stable. macOS CI doesn't build
  `VB_WITH_NET` yet (README "Known gaps"), so it only gets the
  singleplayer variants (`clients(1, singleplayer=True)`).
- Upload `tests/e2e/**/artifacts/` on failure.

## 9. Phased plan

| Phase | Deliverable | Rough size | Verifiable by |
|---|---|---|---|
| **E0** | `InputSource` seam: `RaylibInput`, all direct raylib input calls in `src/client` + `main_menu` routed through `InputFrame`; unit tests for `sample_input_cmd` | S–M | Existing tests + manual play unchanged |
| **E1** (landed 2026-10-02) | `ClientApp` extraction; `run_headless` re-based on it with `render=false` | M–L (highest risk: touches the 1.7k-line `main.cpp`; land behind no flag, purely structural) | `*_smoke` tests unchanged; windowed manual check |
| **E2** (landed 2026-10-03) | `VB_WITH_AUTOMATION` / `VB_DISTRIBUTION` options + `+automation` in `--version` + release-leg check (§7.1–7.3) **first**; then `src/automation/`: JSON-lines host, stdin thread → main-thread queue, `hello/state/step/quit/wait_for` + predicate engine; `--automation stdio` on client + server; ~~`--port 0` reporting~~ (not possible: GNS has no ephemeral listen port, see §10) | M | doctest unit tests for predicate engine + command parsing; a default build's `--automation` exits non-zero |
| **E2b** (landed 2026-10-03) | `C2S_Hello.client_flags` + server-side rejection of automation clients (§7.4) | S | in-process `netcode_test` case: flagged client rejected by a non-automation server |
| **E3** (landed 2026-10-04) | Action commands (input, high-level, ui, chat) + server admin commands. `menu.*`, `screenshot` and typing into the chat box are windowed-only and moved to E5 | M | two real clients + a dedicated server driven by a throwaway script: break/place replicate, give/teleport/chat/walk/kick; unit test for deferred replies |
| **E4** (landed 2026-10-04) | `tests/e2e/vbtest` + fixtures + 10 tests (join + cold-cache asset sync, chat, break and place replicate, craft via chat, inventory screen, server teleport/health, walking, kick + reconnect, loopback-only guard); CTest `e2e` label; a Linux `e2e` CI job. New: `chunk_loaded`/`on_ground` predicates | M | 10/10, five runs in a row, locally; **CI itself not run** |
| **E5** (landed 2026-10-04) | `--net-sim`; windowed automation (`menu.*`, `screenshot`, `type`) under Xvfb; trace viewer (`trace.html`); `rtt_ms`; CI job runs under `xvfb-run` | M | 18 e2e tests (3 new bad-network/windowed files), 3 runs under Xvfb all green; **CI itself not run** |
| **E6** (landed 2026-10-04) | `--automation tcp[:PORT]` (loopback-only, token, survives disconnects) with `--automation-token/-info`; `vbtest` TCP attach (`clients(tcp=True)`, `detach`/`reattach`); `--automation-record out.py` recorder writing a runnable vbtest script, with assertions after observable results; `vbtest.stack` (server/client startup outside fixtures). The trace viewer shipped in E5 | M | 24 e2e tests; a recorded session replays green on a fresh server (3 runs); **CI itself not run** |

E0–E1 are worth doing on their own: they make the client loop testable
without any automation and remove the duplicated headless loop.

## 10. Risks & open questions

1. **`main.cpp` refactor risk (E1).** The windowed loop carries
   hard-won fixes (see `STATE.md`: the NVIDIA VAO/VBO churn bug, the inventory
   mouse-centering fix in a723d89). Do the extraction mechanically, as one
   commit that only moves code, and only then change behavior.
2. **Headless meshing.** Should `ClientApp(render=false)` still mesh chunks?
   Off by default is faster. Turn it on (`--mesh-headless`) for tests that
   want to catch mesher crashes from real replicated edits.
3. **Logical vs raw keys.** Tests should use logical names (`forward`,
   `inventory`) so they survive rebinding. Raw keys stay available for
   testing the keybindings screen itself.
4. **Flakiness under ASan/CI load.** Default `wait_for` timeouts scale with
   an env var (`VB_E2E_TIMEOUT_SCALE`, set to 3 on sanitizer legs). There
   are no fixed sleeps anywhere in `vbtest`, and that's enforced in review.
5. **Screenshots.** Raylib's `TakeScreenshot` works under Xvfb with Mesa
   llvmpipe. Use it only for debugging artifacts, never for pixel
   assertions (non-goal).
6. **Widget id uniqueness.** The locators depend on pack authors giving
   `ui.define` widgets stable, unique `id`s. `UiRuntime` should warn on
   duplicate ids within one frame. That's cheap, and useful even without
   automation.

13. **Spawn is a drop (found in E4).** Players spawn about a block above the ground, so
    right after join `feet.y` is mid-fall and a landed player's is e.g. `62.9999`.
    Anything that reads a position must `expect(c).to_be_on_ground()` first (new
    `on_ground` snapshot field/predicate) and round with an epsilon.
14. **The server applies a command's look *after* the pack's input hook (found in E4).**
    `player_input` handlers (and so `player:punch()`/`place_block` raycasts) see the
    *previous* command's yaw/pitch, so a click in the same frame as a look change goes the
    old way. Invisible to humans; automation holds its aim steady 8 frames before clicking
    (`AimedTask::kSettleFrames`). An engine ordering quirk, left alone.
15. **A punch hits the nearest player before any block (found in E4).** Both test players
    spawn on the same spot, so one's punches land on the other. Tests `teleport` the second
    player away first.
16. **Clicks are confirmed by a round trip (found in E4).** E3's `break_block`/`place_block`
    clicked every other frame until the client *saw* the change, so several extra clicks were
    already in flight: `place_block` spent 4 items and put stones in the player's own cell.
    Both now send one click and wait for feedback (`S2C_BlockDamage` for breaking; no retry
    for placing).
17. **Real engine crash fixed (found in E4): `ui.close()` with a session attached.**
    `UiRuntime::do_close()` passed a state-less `sol::lua_nil` to `lua_to_json()`, which
    dereferenced a null `lua_State` and segfaulted the client whenever a pack UI was closed
    from Lua with a live connection (every unit test ran without a session, which returns
    early). One-line fix plus a regression test in `ui_runtime_test.cpp`.
18. **CI had never compiled the automation code (found in E4).** Every existing leg builds
    with automation off, so `-Werror` problems in E2/E3 code (a `-Wdangling-reference` in
    `predicate.cpp`, missing-field-initializers in the tests, clang-format drift in the new
    endpoint files) only surfaced when the dev tree was built with `VB_WARNINGS_AS_ERRORS=ON`.
    Fixed; the new `e2e` job is the first to build it. Pre-existing files (`client_app.cpp`
    etc.) have clang-format violations under clang-format 18 that predate this work and were
    left alone.
19. **The CI job runs on Linux only.** `e2e` in `build_linux.yml` (ASan + UBSan, 3x
    timeouts) is green on PRs (confirmed 2026-10-17, run 39044682584). Windows and macOS
    do not run the e2e suite.

20. **raylib batches draw calls until `EndDrawing()` (found in E5).** A screenshot read
    before that returns only the clear colour (menu "blank" at 18,18,22). The capture
    flushes the batch (`rlDrawRenderBatchActive()`) first, and uses
    `LoadImageFromScreen` + `ExportImage` because `TakeScreenshot()` strips the directory
    from the path.
21. **A windowed client is "joined" long before it is "playing".** After the handshake it sits
    on a loading screen until its first chunks are meshed and uploaded, so `type`, clicks etc.
    need `app_state == playing` (the harness waits for it).
22. **GNS fake-network settings are process-wide and reset when the runtime shuts down**, so
    `GnsTransport::set_net_sim` stores them and re-applies on every init, which also lets it
    be called before any transport exists (the client creates its transport only on connect).
23. **A flat UI has few colours.** The menu screenshot has 6 distinct colours; "not blank"
    assertions use the mean colour and a low colour-count floor, not photo-like thresholds.

24. **TCP changes what "the harness died" means (found in E6).** Over stdio a closed stdin
    ends the process; a TCP client dropping must *not* (that is the point of attaching to a
    human's game), so the reader thread waits for the next connection and a detached
    script's pending replies are discarded. `quit` is the only way to end the process remotely.
25. **One connection at a time means a second one gets silence, not an error.** It sits in the
    listen backlog unauthenticated and unanswered until the first drops (so a wrong-token test
    must detach the real session first). A squatter that never authenticates is cut off after 5 s.
26. **A recorder needs waits too (found in E6).** A first recording clicked "close" the instant the
    inventory key was pressed, which a human never does; it now asserts that a screen opened
    (`to_have_ui_open`) the way it asserts that a block broke. It also needs the connection's
    name from the menu (`menu.player_name()`), not `config.player_name`, and rewrites its file
    when that becomes known, since it may be killed before any step is recorded.
27. **Replay needs a deterministic scene.** Breaking natural shoreline sand then "placing" back
    failed because the ground there is water; a server-built stone block (set up in both the
    recording and, as a user would add it, the replay) is stable. Seed 7 gives the same
    spawn and terrain, so absolute coordinates replay.

28. **A CI review found three jobs-killing problems before any runner saw them (E6 follow-up).**
    (a) The "verify no automation" step ran `--net-sim ... --version` / `--automation-record
    ... --version`; `--version` exits 0 before any flag is checked, so every release leg would
    have reported "accepted". It now uses the one-shot forms (`--headless --frames 1 --ticks 1`;
    `--automation-record` is client-only), and the step script was run against both a production
    and an automation build (passes / fails correctly). (b) **Lint**: with the clang-format the lint
    job fetches (latest, 23.x) the original baseline had 0 violations and this branch 75
    (`client_app.*`, `render/input.cpp`, `client/main.cpp`); every build job `needs: lint`, so
    nothing would have run. Formatted. Test lint with the same version, not whatever is installed.
    (c) **GCC 13 `-Wmaybe-uninitialized` false positive inside `<regex>`** when `predicate.cpp` is
    built with ASan -O2 under `-Werror`; silenced for that file on GCC (`src/automation/CMakeLists.txt`).
29. **CMake silently skips the `e2e` test if its python can't import pytest.** A green job that ran
    no e2e tests is worse than a red one, so the CI job now fails if `ctest -N -L e2e` doesn't
    list it, and uses `actions/setup-python` (Ubuntu 24.04's system pip refuses installs, PEP 668).
30. **Running the CI configuration locally found more (ASan + UBSan + Xvfb, 24 tests, twice green after
    the fixes).** (a) `break_block` re-clicked after 60 frames of silence; an instant-break block
    gives no feedback but vanishing, so a slow server turned the retry into a second punch that
    destroyed the block *behind* the target (recorder fixture failed: nothing to place against).
    **No blind retry now.** (b) Under ASan an action right after a `walk_to` takes ~7 s (0.3 s
    normally; the server is slow to settle after movement), so action timeouts doubled (break/place
    10 s, walk 20 s, x3 under sanitizers) and the recorder waits 15 s for a result before dropping an
    assertion. Aim settling tolerance loosened to 0.05 degrees. (c) The client's own hard-coded
    10 s connect deadline expired for the second client behind `--net-sim` on a sanitized build;
    dev builds read `VB_CONNECT_TIMEOUT_SECONDS` (the harness sets it from its timeout scale;
    shipped builds are still exactly 10 s). The server side has the same shape: it drops a join
    stuck in one handshake step after `handshake_timeout_seconds` (server.toml, default 10 s), and
    a sanitized server under load intermittently kicked a second client that way ("join failed:
    handshake timeout" in `test_same_account_signing_in_again_kicks_the_older_session`,
    2026-10-07); the harness now writes `10 * VB_E2E_TIMEOUT_SCALE` into every test server's
    config. UBSan also prints ~35 `invalid vptr` lines per client
    from `GnsRuntime` (`SteamNetworkingUtils()`), identical with and without this work and
    harmless to the tests, but they are noise worth a look (GNS hands out that interface in a way
    UBSan's vptr check dislikes).
31. **Still not verified by running it on GitHub:** `runner.yml` triggers only on pull requests, pushes
    to `main` and to branches named `*-workflow`, or manual dispatch, so a push to a feature branch
    runs **nothing**. The job structure, `xvfb`/`libgl1-mesa-dri` package names, `setup-python`, and
    the artifact upload have only been checked by YAML parsing and by running the same commands
    locally on Ubuntu 24.04 / GCC 13.3, which is what `ubuntu-latest` provides. 2-vCPU runners will be
    slower than this 4-core sandbox. Windows (Winsock in `tcp.cpp`) and macOS are unbuilt.

## 11. Documentation upkeep (for agents implementing this design)

This doc and several others describe the automation work as **planned**.
Every phase that lands must flip the relevant text from "planned" to fact in
the **same change** that ships the code. A phase isn't done until the
checklist below is. Don't write history for work you didn't do or verify.

### 11.1 Every phase (E0–E6)

- [ ] **This file:** update §9's phase row (mark landed + date), correct any
      section that the implementation contradicted (file names, flag names,
      command names, defaults), and move resolved items out of §10.
- [ ] **`REMAINING_TASKS.md`** "Cross-Cutting" entry: update the `[~]` line's
      progress; change to `[x]` only when E0–E6 (or the agreed scope) are all
      done. Put the long write-up in `remaining_tasks/cross_cutting.md` (what
      shipped, files, tests, how verified), not in the lean file.
- [ ] **`STATE.md`:** refresh the "Current status" automation entry (what has
      landed, what's next, any new gotcha). Put anything verbose in
      `state/changelog-recent.md` and link it. Machine-specific notes go in
      `STATE.md.local`, not here.
- [ ] **Tests:** say how you verified (which build dir/flags, which tests,
      counts). Report failures honestly.

### 11.2 Per-phase additions

| Phase | Also update |
|---|---|
| **E0** (landed) | `architecture_spec/rendering.md` §11.5, `CONTRIBUTING.md` module map — already done. |
| **E1** (landed) | Done: `ARCHITECTURE_SPEC.md` §10, `topology-and-layout.md`, `CONTRIBUTING.md`, `STATE.md`. |
| **E1** `ClientApp` | `ARCHITECTURE_SPEC.md` §10 (client loop description) and `architecture_spec/topology-and-layout.md` (new files); `STATE.md` note on `run_headless` now sharing the windowed loop; confirm README's `--headless` description is still true. |
| **E2** options + host (done) | `README.md` build-options table: drop *(planned)* from `VB_WITH_AUTOMATION`, add `VB_DISTRIBUTION`; `CONTRIBUTING.md` e2e bullet (real commands, `build-e2e`); `src/client/main.cpp` / `src/server/main.cpp` `--help` text for `--automation*` (only when compiled in); new `docs/automation-protocol.md` (the JSON-lines contract: every command, predicate, error code, `proto` version); `cmake/` option comments; `describe_build()` `+automation` documented in README's `--version` mention. |
| **E2b** handshake flag (done) | `docs/protocol.md`: add `client_flags` to the `C2S_Hello` row, add an entry to its changelog, **bump `kEngineProtocolVersion`** and record the old→new value (see "Adding a new wire message" in `CONTRIBUTING.md`); `architecture_spec/networking.md` §8.3 handshake notes; `content/` is unaffected. |
| **E3** actions + admin cmds (done) | Done: `docs/automation-protocol.md` tables, `STATE.md`, backlog. `lua-api.md` untouched (`run_lua` adds no pack API). Gating confirmed: every server admin primitive (`ServerSession::{player_health,set_player_health,teleport_player,set_time_of_day,kick_player}`, `PackRuntime::admin_give`) and every endpoint is under `#if defined(VB_WITH_AUTOMATION)`; the automation-off server binary has none of the command strings. |
| **E4** harness + CI (done) | Done: `tests/e2e/README.md`, `CONTRIBUTING.md`, `ARCHITECTURE_SPEC.md` §17, `automation-protocol.md` (new predicates/state), `STATE.md`, backlog, `.gitignore`. The CI job is `e2e` in `build_linux.yml`; its failure-log artifact is named `e2e-failure-logs` on purpose (bundle.yml merges `<project>-*`). |
| **E5** net-sim, traces, screenshots (done) | Done: `automation-protocol.md` (net-sim, windowed commands, `rtt_ms`), `tests/e2e/README.md` (markers, Xvfb, `trace.html`), `CONTRIBUTING.md`, `STATE.md`, backlog, CI (`xvfb-run`, `--net-sim` refusal checks in all three release legs, `*_rejects_net_sim` CTests). |
| **E6** recorder / TCP attach (done) | Done: `automation-protocol.md` (TCP attach, recording), `tests/e2e/README.md`, `CONTRIBUTING.md`, `STATE.md`, backlog, CI (`--automation tcp`/`--automation-record` refusal in all three release legs; 3 more `*_rejects_*` CTests). **§7 re-checked**: the listener is behind `VB_WITH_AUTOMATION`, binds only `127.0.0.1` (verified from `/proc/net/tcp` by a test), is token-protected, and a production binary refuses the flags. |

### 11.3 Invariants to re-verify and re-document whenever automation code changes

These are the safety claims in §7 and in `STATE.md`. If any stops being
true, fix the code first, then fix the docs. Never relax the docs to match a
regression.

- A default build (flag `OFF`) contains no automation code, rejects
  `--automation`, and does not print `+automation` in `--version`.
- `VB_DISTRIBUTION=ON` with `VB_WITH_AUTOMATION=ON` is a configure error.
- Release/`debug` CI legs set `VB_DISTRIBUTION=ON`, run the `--version`
  check, and are the only legs whose binaries are uploaded; the `e2e` leg
  never uploads.
- A server built without automation rejects automation clients.
- No automation listener binds a non-loopback address by default.

### 11.4 Cross-references to keep in sync

If you rename or move this file or renumber its sections, grep for
`e2e-automation` and `§7` across `*.md`, source comments (e.g.
`inc/vb/render/input.hpp` cites §4.1) and CI files, and update every hit.
Files that currently point here: `ARCHITECTURE_SPEC.md`,
`REMAINING_TASKS.md`, `remaining_tasks/cross_cutting.md`, `STATE.md`,
`CONTRIBUTING.md`, `README.md`, `docs/protocol.md`,
`architecture_spec/rendering.md`, `inc/vb/render/input.hpp`.
7. **No `--port 0` (found in E2).** `GnsTransport::listen(0)` fails with
   `kBindFailed`: GNS's direct-UDP listen has no ephemeral-port allocation.
   The harness must pick a free UDP port itself (§5.3's "reports the actually
   bound port" is satisfied only as an echo of the configured port).
8. **Stdio plumbing (found in E2).** Reading `std::cin` on the reader thread
   deadlocks `exit()` (it holds stdin's stdio lock while blocked), and
   redirecting only `std::cout` misses Lua's `print`, which writes to C
   `stdout`. The host therefore reads fd 0 with raw `read(2)` and duplicates
   fd 1 for protocol frames, pointing fd 1 itself at stderr.
9. **Windowed automation (E5).** The client can now run `--automation` in a real window
   (under `xvfb-run`, software GL). raygui can't be clicked by script, so menu commands
   inject the click's *result* and fill the menu's fields.
10. **`World::set_block` creates chunks on demand and returns "changed", not
   "chunk exists" (found in E3).** The server `set_block`/`fill` therefore check
   `has_chunk` first and refuse edits outside loaded chunks, instead of
   silently creating empty ones. Tests must build near a player.
11. **Loopback (`--loopback`) never installs the pack keybind registry (found in E3).**
   `make_singleplayer_host` calls `install_join_veto` and
   `install_entity_kind_registry` but not `install_keybind_registry` (only the
   dedicated server does), so pack keybinds (`base:inventory`, `base:pause`)
   don't reach a `--loopback` client: pressing the inventory key opens
   nothing. Pre-existing and left alone; UI tests must use a dedicated server.
12. **Headless clients skip modal-UI/HUD evaluation** (it only ran as a side
   effect of drawing). `ClientApp::set_headless_ui_eval()` turns it on; the
   automation endpoint does, plain `--headless` stays unchanged.
