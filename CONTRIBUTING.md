# Contributing to Voxel Browser

Thanks for looking at the code. This doc is the practical "how do I work in
this repo" reference — for *what's designed* see `ARCHITECTURE_SPEC.md`,
for *what's left to build* see `REMAINING_TASKS.md`, and for *gotchas,
landmines, and past investigations* see `STATE.md`. Read `STATE.md` before
touching anything networking-, threading-, or dependency-related — several
subtle bugs have already been found and fixed there, and re-discovering them
wastes time.

Those three files are each kept under 500 lines; when one grows past that,
compact it into its vault directory as described in `DREAMING.md`.

## Module map

The engine is split into two static libraries plus three executables
(`CMakeLists.txt` §top comment has the one-line version of this):

| Target                 | What                                                          |
| ------------------------ | -------------------------------------------------------------- |
| `vb_core`               | Engine core. No rendering, no window — must build and run headless. |
| `vb_render`              | Client-only rendering (raylib + raygui). Never linked by the server. |
| `voxel_browser`          | The client ("the browser") executable — `src/client/main.cpp` (CLI/window setup) + `ClientApp` (`client_app.cpp`, the per-frame state machine shared by windowed and `--headless`). |
| `voxel_browser_server`   | The authoritative, headless server executable — `src/server/main.cpp`. |
| `vb_structure_editor`    | The structure editor — `src/editor/app/` (optional, see the `editor` row below). |
| `vb_tests`               | Unit + integration tests (doctest) — `tests/unit/*.cpp`.       |

`vb_core` is assembled from per-module subdirectories, each with its own
`src/<module>/CMakeLists.txt` and mirrored `inc/vb/<module>/` headers:

| Module         | Directory                     | Purpose                                                    |
| --------------- | ------------------------------ | ------------------------------------------------------------ |
| `core`         | `src/core/`, `inc/vb/core/`     | Math, `Result<T,E>`/error types, logging, TOML config, ids, generated version header. |
| `protocol`     | `src/protocol/`, `inc/vb/protocol/` | Hand-written wire codecs (`ByteReader`/`ByteWriter`), every message struct. |
| `net`          | `src/net/`, `inc/vb/net/`       | `Transport` abstraction, `LoopbackTransport`/`GnsTransport`, handshake FSMs, `ServerSession`/`ClientSession`. |
| `world`        | `src/world/`, `inc/vb/world/`   | Voxel data model, chunk store, lighting, meshing, block registry, item drops. |
| `worldgen`     | `src/worldgen/`, `inc/vb/worldgen/` | Terrain generation (hand-rolled noise pipeline today; `FastNoise2` is the gated future backend). |
| `physics`      | `src/physics/`, `inc/vb/physics/` | Shared movement + swept-AABB voxel collision (one implementation, server + client prediction both use it). |
| `replication`  | `inc/vb/replication/` (header-only) | Interest-grid culling (hand-rolled; `librg` is the gated future backend). |
| `ecs`          | `inc/vb/ecs/` (header-only)     | Component structs (`Position`, `Inventory`, ...). No registry driving them yet — `ServerSession` still simulates players directly. |
| `assetsync`    | `src/assetsync/`, `inc/vb/assetsync/` | Content-pack manifest hashing + client-side content-addressed cache. |
| `script`       | `src/script/`, `inc/vb/script/` | Embedded Lua VM (`vb::script::Vm`), the server pack API (`PackRuntime`), the client UI VM (`UiRuntime`), the content-pack loader. |
| `automation`   | `src/automation/`, `inc/vb/automation/` | **Dev-only** (`VB_WITH_AUTOMATION`): JSON-lines host, command/predicate engine. Linked into the two executables only when the flag is on; endpoints live in `src/client/automation_endpoint.hpp` and `src/server/automation_endpoint.hpp`. |
| `editor`       | `src/editor/model/`, `inc/vb/editor/`; app in `src/editor/app/` | The structure editor (`docs/structure-editor.md`). `vb_editor_model` is a headless library (volume, structure document, writer, block catalog, workspace, edit session with undo, generators, terrain preview, validation) built whenever tests are, and linked by `vb` for `vb structure`; the `vb_structure_editor` executable (raylib + raygui, `VB_BUILD_EDITOR`, on by default with the client and Lua, off for `VB_HEADLESS`) is only drawing and input on top of it. |
| `render`       | `src/render/`, `inc/vb/render/` | Window/camera, input seam (`InputSource`/`InputFrame`, `sample_input_cmd`), chunk/entity renderers, raygui-backed UI + main menu. `vb_render` only. |

A build without a phase's heavy dependency (`VB_WITH_NET`/`_LUA`/
`_WORLDGEN`/`_COMPRESSION`/`_MESHING`, all default `OFF`) links a stub that
returns a `kDisabled`/`kBackendUnavailable`-shaped error instead of failing
to compile — see any `vm.cpp`/`gns_transport.cpp`-style file for the
pattern. Keep that pattern when adding a new gated dependency: the rest of
the engine, and both binaries, must keep building and running (headless)
with everything off.

## Building and running tests

See `README.md`'s "Getting Started" for the full build-from-scratch steps
and per-dependency system requirements. The short version:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

A few things worth knowing that aren't obvious from a single build tree:

- **Most interesting engine behavior is gated behind `VB_WITH_LUA`.** A
  default build (`VB_WITH_LUA=OFF`) still builds and passes every test, but
  `PackRuntime`/`UiRuntime` compile to no-op stubs — you won't be exercising
  the content-pack/scripting path at all unless you configure with
  `-DVB_WITH_LUA=ON`.
- **Keep more than one build directory around** if you're touching anything
  gated — e.g. one with `VB_WITH_LUA=ON` and one with it `OFF`, rather than
  reconfiguring back and forth. This project's own agent sessions do exactly
  this (see `STATE.md` for examples); it's much faster than a full
  reconfigure+rebuild every time you need to check the other side of a
  `#if VB_WITH_LUA` boundary. `VB_BUILD_CLIENT=OFF` on a Lua-focused tree
  also skips the (slower) raylib build if you're not touching rendering.
- **Run a single test / a subset** with doctest's own filter, not ctest's:
  `./build/vb_tests --test-case='*crafting*'` (glob, not substring — wrap in
  `*...*`; quote it so your shell doesn't glob-expand it first).
- **`server_smoke`/`client_smoke`/`singleplayer_smoke` (runs `--loopback`)** (registered in
  `tests/CMakeLists.txt`) start the real binaries with `--headless` and
  assert on stdout/exit behavior — they need `VB_BUILD_SERVER`/
  `VB_BUILD_CLIENT` on (both default `ON`).
- **Automation tests (`automation_test.cpp`) and the e2e harness (pytest
  `tests/e2e/`, `ctest -L e2e`; see `tests/e2e/README.md`).** The automation driver (`src/automation/`,
  JSON-lines over `--automation stdio`, contract in
  `docs/automation-protocol.md`) is compiled only with
  `-DVB_WITH_AUTOMATION=ON`; `automation_test.cpp` is added to `vb_tests`
  then. Use a dedicated build directory (e.g.
  `cmake -S . -B build-e2e -DVB_WITH_AUTOMATION=ON`; a real two-process
  multiplayer run also needs `-DVB_WITH_NET=ON -DVB_WITH_LUA=ON
  -DVB_WITH_COMPRESSION=ON`, protobuf, and `pip install -r
  tests/e2e/requirements.txt` -- the `e2e` CTest entry is only registered when
  all of that is present, and the windowed tests additionally want `xvfb-run` (they skip without a display); a build without automation refuses `--net-sim`, `--automation-record` and `--automation tcp` as well as `--automation`; the TCP listener binds `127.0.0.1` only and needs a token; `-DVB_WITH_WORLDGEN=ON` +
  `-Werror` trips a FastNoise2 warning on newer GCC) and **never** enable that
  flag in a build you intend to ship; the release/`debug` CI legs set
  `VB_DISTRIBUTION=ON`, which makes the combination a configure error, and a
  default build's `server_rejects_automation`/`client_rejects_automation`
  CTest entries check that `--automation` is refused. See `docs/e2e-automation.md`; if you implement
  any part of it, finish its §11 documentation checklist in the same change.
- **Fuzz targets (`tests/fuzz/`)** cover the decoders that parse untrusted
  bytes: every `vb/protocol` message, frame parsing, the chunk codec and LZ4
  (`fuzz_protocol`), and ID tokens, JWKS documents, the loopback sign-in
  request and player names (`fuzz_auth`). Every build runs them as the
  `fuzz_*` ctest entries (seeds + `tests/fuzz/regressions/` + 20000
  deterministic mutations). For real coverage-guided fuzzing configure with
  Clang and `-DVB_BUILD_FUZZERS=ON` (the CI `fuzz` job: 3 minutes per
  target), then `./fuzz_protocol_replay --write-seeds corpus &&
  ./fuzz_protocol corpus -max_total_time=300`. A new wire struct goes into
  `fuzz_protocol.cpp`'s `Messages` list; a crash input goes into
  `regressions/<target>/` with its fix.
- **`content_pack_test.cpp`** loads the real `content/base` files (not
  inline Lua strings) — if you edit that pack, this is the test that
  catches a syntax error or a registration-count regression.
- **Testing `content/base` itself** (see `docs/content-base-testing.md` for
  the full design): put a new check in the lowest of these three layers
  that can hold it — promote to a higher layer only when the thing under
  test *is* the round trip (asset sync, input → server → UI push,
  rendering), not to duplicate a lower-layer case for its own sake.
  - **`content_base_data_test.cpp`** / **`content_base_behaviour_test.cpp`**
    (Layer 1, doctest, `VB_WITH_LUA`): server-side pack checks — block
    properties/textures/biomes/entity registrations, drops, crafting,
    `mechanics.lua`'s rising-edge punch/place, fall damage, keybinds,
    `vb.storage` persistence. Both share `content_base_fixture.hpp`'s
    `BasePackFixture` (a real `PackRuntime` + `ServerSession` + one joined
    `ClientSession` over `LoopbackNetwork`, loading the real
    `content/base/` directory) — extend that fixture rather than
    re-deriving its ~40 lines of setup in a new test file.
  - **`content_base_ui_test.cpp`** (Layer 2, doctest, `VB_WITH_LUA`): loads
    `content/base/ui/*.lua` into a real `UiRuntime` the same way
    `src/client/client_app.cpp`'s singleplayer path does, and asserts the
    rendered widget trees. This is the only place that opens these files at
    all — before it existed, a syntax error in `hud.lua` passed the entire
    suite.
  - **`tests/e2e/test_base_pack.py`** (Layer 3, pytest, `ctest -L e2e`):
    reserved for checks that genuinely need two real processes and a cold
    cache (asset sync, a real windowed render, a second client observing
    replication) — see `tests/e2e/README.md`.

## Code style

- `.clang-format` is checked in — tabs, no column-wrap (`ColumnLimit: 0`),
  run it before committing. CI (`lint.yml`) runs `clang-format --Werror` over
  `src/**`, so an unformatted new file fails CI, not just style review.
  **Caveat:** a locally-installed `clang-format` may disagree with whatever
  version CI's `pip`/`pipx` resolves (see `STATE.md` §4) — if a bare
  `--dry-run` on your edited file shows a wall of unrelated violations,
  diff against the *unmodified* file with the same command before assuming
  your edit is the problem.
- No exceptions on engine hot paths — fallible operations return
  `vb::core::Result<T, E>` (see `inc/vb/core/result.hpp`). Lua-facing code
  (`sol2` bindings) is the one place `sol::error`/exceptions are normal,
  since that's how sol2 reports a Lua-level error.
- Namespaces are `vb::<module>` (`vb::net`, `vb::world`, `vb::script`, ...),
  mirrored 1:1 between `inc/vb/<module>/` and `src/<module>/`.
- Both binaries must stay buildable and runnable `--headless` — CI's smoke
  tests and several integration tests depend on this.

## Adding a new wire message

Every struct in `inc/vb/protocol/` is a normative part of the network
contract. When adding or changing one:

1. Add the struct + `encode`/`decode` (see any existing file in
   `inc/vb/protocol/` for the pattern — `ByteWriter`/`ByteReader`,
   bounds-checked, error-accumulating).
2. Register it in `MessageType` (`inc/vb/protocol/message.hpp`) — values are
   append-only, never renumber or reuse an id once shipped. Assign a `Lane`
   in `lane_for()` if it needs anything other than the reliable-ordered
   control lane.
3. Add a round-trip test in `tests/unit/protocol_test.cpp` (encode → decode,
   assert the fields match) plus a truncation/malformed-input case if the
   struct has anything the existing helpers (`ByteReader`) don't already
   generically cover.
4. **Bump `kEngineProtocolVersion`** in `cmake/version.hpp.in` and add an
   entry to `docs/protocol.md`'s changelog at the top, explaining what
   changed and why — this file is the wire format's source of truth, not a
   changelog nobody reads.
5. If it's consumed by `ServerSession`/`ClientSession`, add or extend an
   integration test over `LoopbackTransport` (see `tests/unit/netcode_test.cpp`
   or `pack_runtime_integration_test.cpp` for the shape: construct a real
   `ServerSession` + `ClientSession` pair, `tick()` both in a loop, assert on
   observable state).

## Working with the Lua content API

`content/base` is the reference pack and the thing every Lua-facing test
loads for real (not inline strings) via `vb::script::load_content_pack`. If
you're adding a new `vb.*`/`ui.*` binding:

- Prefer a **generic, game-agnostic primitive** over something that encodes
  a specific game's rules — the engine is meant to stay a host for
  arbitrary content, not grow special cases for "the base game." A recent
  concrete example: crafting was implemented as a `player:take()` primitive
  (generic, symmetric with the existing `give()`) plus a small
  `load_content_pack` change that lets a pack drop any extra top-level
  `*.lua` module in (also generic) — with every actual game rule (recipes,
  matching, the `/craft` command) living entirely in
  `content/base/crafting.lua`. If you're tempted to add something like
  `vb.craft_item(...)` directly to the engine, ask whether a smaller,
  more general primitive plus content-side logic gets you the same result.
- **Add the stub in the same commit** (`sdk/lua/library/*.lua`, LuaCATS): a
  one-line summary, `---@param`/`---@return` for every argument, a
  ```` ```lua ```` example, `---@vb since <x.y.z>` (the first release that
  ships it; `--check-pack` warns packs whose `engine_version_req` admits an
  older engine) and `---@vb context load|runtime|ui` (usertype
  methods as `function Player:name(...) end`, definition tables as
  `---@class` with one `---@field` per key). Then run
  `python3 scripts/gen_lua_docs.py` and commit the regenerated
  `docs/lua-reference/`, `sdk/lua/api_index.txt` and
  `src/server/check_pack_data.inc` (the `since` versions and every
  `---@class`'s keys, which `--check-pack` uses). Two checks keep this
  honest: `lua_api_surface_test` (live `vb`/`ui`/`client` tables and usertype
  methods must equal `api_index.txt`; doc examples must compile) and the
  `lua-docs` lint job (`gen_lua_docs.py --check`). A binding that is
  deliberately internal goes in `sdk/lua/api_ignore.txt`.
- Put narrative / design context in `docs/lua-api.md`; it stays the guide,
  the generated reference is the lookup.
- A new `vb` command or flag: add it to the command table *and* its entry in
  `src/cli/help.cpp`, then `vb help --markdown > docs/cli.md`
  (`dev_cli_help_test` fails on a stale file). Errors are `error:` + optional
  `hint:` on stderr; commands that report state take `--json`.
- Changing what a pack author can observe on the wire (`S2C_ServerInfo`, ...)
  bumps `kEngineProtocolVersion` as usual; `pack.toml`'s `engine_version_req`
  is enforced against `kVersionNumeric` (`vb::core::VersionReq`).
- Add a demonstrating example to `content/base` if it's the kind of thing a
  pack author would actually use (see `crafting.lua`'s heavy comments for
  the expected level of explanation — a new contributor should be able to
  read the pack and understand *why*, not just copy *what*).

## Filing issues / picking up work

`REMAINING_TASKS.md` is the live backlog, organized by phase, with `[ ]`/
`[~]`/`[x]` checkboxes and a "Cross-Cutting / Continuous" section for things
that apply everywhere (protocol version discipline, sanitizer coverage,
headless-must-work, etc.). If you start on an item, it's fine to leave its
checkbox as-is until it's actually done — the file's own convention is
"landed and verified," not "started."
