# STATE — Working Notes for Future Agents

> Living scratchpad of gotchas, landmines, open decisions, and small TODOs
> discovered while working in this repo. **Update this file** when you learn
> something non-obvious or fix something listed here. Keep this file lean
> (~300-400 lines) — new verbose writeups belong in `state/*.md`, linked from
> here, not pasted inline. See "Detail files" at the bottom for the index.
> **Hard cap: 500 lines** — over that, "dream" (move inactive entries into
> `state/*.md`) before ending the session; see `DREAMING.md`.
>
> Companion docs: `ARCHITECTURE_SPEC.md` (target design) · `REMAINING_TASKS.md`
> (implementation backlog). This file is for *traps and context*, not the plan.
>
> **If what you learned is tied to a specific physical machine** (a tool
> path, an agent-shell/Bash-vs-PowerShell quirk, which pre-built `build-*`
> dirs exist, a local toolchain oddity) **write it to `STATE.md.local`
> instead of here** — see that file's own header for why. This file (and
> `state/*.md`) is for gotchas that hold regardless of which machine an
> agent is running on.

---

## Standing priority: prefer e2e verification over reading code or unit tests alone

When fixing anything a player would actually see or feel (HUD layout, menu
screens, physics-driven policy like fall damage, anything behind
`client.screen_size()` or raw pixels), **drive it through the real windowed
automation harness (`tests/e2e`, `docs/e2e-automation.md`) and look at an
actual screenshot before calling it fixed**, not just reasoning about the code
or trusting a unit test that synthesizes the inputs.

(Five concrete misses from 2026-10-05 — an empty HUD from a stale `build-e2e` binary, a status-bar
overflow at the minimum window width, a fall-damage test that bypassed real physics, an immediate
absence-of-event assertion that passed against buggy code, and raw `key.press` no-opping without
`client.capture_mouse()` — are written up in `state/gotchas.md` "e2e verification: concrete misses".
Treat "I reasoned through the code and it looks right" as a hypothesis, not a result, for anything
a player would see or feel.)

---

## Standing priority: prefer engine (C++) work over content (Lua)

When picking what to work on next and multiple open items are roughly equally
ready (e.g. choosing among REMAINING_TASKS' several `[ ]` items with no other
constraint forcing one), **prioritize a task that changes the engine (C++
code under `src/`/`inc/`) over one that's purely `content/*.lua` policy/
content work.** Most remaining Lua-only gaps (PvP/mob damage policy, hunger,
more base-pack art/reskins, etc.) are content authors' problems the engine
already has the primitives for; the engine surface itself is where real
mechanism gaps still exist and where a wrong call is much more expensive to
unwind later. This is a standing preference for ambiguous "what next"
choices, not a rule to force an engine change where a task is inherently
content-only (e.g. the user explicitly asks for a content-pack feature).

---

## Current status (2026-10-07)

- **E2E automation (dev-only): E0–E6 all landed** (2026-10-02..04). Design
  `docs/e2e-automation.md` (gotchas in its §10), wire contract
  `docs/automation-protocol.md`. Hard rule: compiled out of production —
  `VB_WITH_AUTOMATION` defaults OFF, `VB_DISTRIBUTION` + automation is a
  configure error, and servers built without it refuse automation clients.
  **Implementing or changing any E-phase? Follow `docs/e2e-automation.md`
  §11.** The Linux CI `e2e` job runs on PRs and is green (2026-10-17; a
  feature-branch push alone triggers nothing — open a PR, push to a
  `*-workflow` branch, or dispatch manually). Windows (Winsock code) and
  macOS are unbuilt.
- **Main menu redesign (Phase 12) landed** 2026-10-31 (12.1–12.7): design
  `architecture_spec/rendering.md` §11.6, detail `remaining_tasks/phase12.md`.
  Traps: Settings is the only writer of `player_name` (`ClientApp::session_name_`
  holds `--name`/automation overrides); `MainMenu` loads its backdrop/fonts lazily
  on the first draw (needs a GL context); the macOS job is Clang `-Werror`
  (unused private fields) — check changed files with `clang++ -fsyntax-only -Wall
  -Wextra -Werror -Wunused-private-field` using a build dir's `flags.make`;
  menu screenshots can be taken without the harness via `xvfb-run` + `xdotool`
  (hold the button ~250 ms: a same-frame press/release is not seen by raygui).
- **In-engine auth (Phase 9) landed** 2026-10-05/06 — see
  `REMAINING_TASKS.md` Phase 9 for what is still open, `docs/auth.md` for
  operators.
- **macOS CI `VB_WITH_NET`** (2026-09-30) is green on real runners since
  2026-10-17 (run 39044682584), as are the ASan/TSan legs.
- **`CMAKE_POLICY_VERSION_MINIMUM=3.5` shim stays** — lz4 v1.9.4 needs it
  now, not doctest (bumped to v2.5.3). Drop it only once lz4 is bumped.
- **Don't remove `SOL_FUNCTION_CALL_VALUE_SEMANTICS=1`** on the `sol2` target
  (`cmake/Dependencies.cmake`). Without it sol2 passes `PlayerHandle`
  arguments as pointers into dead C++ stack frames, and a pack that stores
  one reads garbage later. Regression test in
  `pack_runtime_integration_test.cpp`.
- `WorldReplicator::set_send_budget_bytes` (`server.toml`
  `chunk_send_budget_bytes_per_tick`, 0 = unlimited) caps per-tick chunk
  send bytes; the ingest side is `set_chunk_ingest_budget()`.

- **Spawn position must come from the world's own generator** (2026-10-07).
  `--singleplayer` used to compute it with a pipeline-less `WorldGenerator`
  whose height field differs from `content/base`'s Lua pipeline -- feet up to
  14 blocks under (or ~19 above) the real surface depending on seed.
  `default_spawn_position` now also checks the *generated* chunks (structures,
  carvers) for solid ground + two air voxels, so it's computed once per
  server, not per join. `ServerSession::teleport_player` is no longer
  automation-only: Lua `player:set_pos(x,y,z)` / `player:get_spawn_pos()`.

- **Asset cache keys downloads by content hash, but paths are many-to-one**
  (2026-10-07): identical files under two paths used to be requested twice,
  and the server's second copy failed the join. `ClientAssetCache` now
  requests each hash once and writes it to every path. It also remembers
  each full manifest's entry list, so the server's reconnect fast path (no
  entries) can rebuild the virtual FS; `last_known_manifest_hash_for` stays
  zero unless that list is held. Nothing sends a known hash yet.
- **Chunk alpha: cutout in the opaque pass, blend only for glass-like art**
  (2026-10-08). The chunk shader discards below `alphaCutoff` (0.5 opaque
  pass, 0.01 blended pass) so clear sprite texels write no depth. The
  blended pass is chosen per block by `TextureAtlas::is_translucent` (>25%
  of visible texels partially transparent), else the fallback colour alpha
  (base:leaves). The old "hole" only shows when the background is in a
  chunk drawn after the sprite's -- test across chunk boundaries.
- **A world save is tied to its pack's block registry** (2026-10-08):
  region files hold raw block ids, so `<world_dir>/blocks.txt` records the
  names by id and `world::check_world_registry` refuses a pack whose ids mean
  other blocks (appending blocks is fine). `vb host --pack <other>` and
  `vb pack dev` use `<instance>/worlds/<pack>-<hash>/` (the instance's own
  pack keeps `world/`); `vb launch -- --content-pack` likewise. Server and
  singleplayer both check; the server logs its world dir at startup.
- **Gameplay frames can overtake `S2C_JoinAccept`** (2026-10-08): they ride
  other lanes, and a lost JoinAccept is resent late, so under packet loss the
  client's handshake used to fail with "expected JoinAccept" (flaky
  `test_gameplay_survives_lag_jitter_and_loss`). `ClientSession` now holds
  them (`early_gameplay_`, max 8192) and applies them right after joining;
  never drop them -- the server doesn't resend chunks it thinks it sent.
- **Every e2e timeout must scale with `VB_E2E_TIMEOUT_SCALE`, both processes'
  included** (2026-10-08): the server's fixed 10 s per-step handshake limit
  (now `handshake_timeout_seconds`) intermittently dropped a second client on
  the sanitized e2e leg while the harness and client were already stretched 3x.
- **Saved sign-ins are per server** (2026-10-08): `auth::SessionStore` keys
  refresh tokens by `(server "host:port", issuer, client_id)`, not by
  provider, so two servers on one Keycloak realm no longer share a login and
  the menu's "Signed in to <server> as ..." / Sign out cover the selected
  server only. Old per-provider files are deleted when the store opens.
- **`db/` and `storage.json` are runtime state, never assets** (2026-10-08).
  The manifest used to scan them: every client could download the server's
  `vb.db` (player records), and the first `vb.db` write after startup made
  cold-cache joins fail with "asset transfer failed (hash mismatch ...)".
  `core::is_pack_runtime_state` is the one rule (manifest + `vb pack dev`
  watcher); a `world_dir` inside the pack is excluded too. A listed file that
  changes anyway is refused (logged) and the manifest rebuilt -- the old
  storage-revision rebuild trigger is gone.
- **Crack overlay draws for every damaged block** (2026-10-07), not just the
  aimed one, pulled 1% toward the eye instead of a fixed 0.002 bulge.
  `GenMeshCube` meshes are already uploaded (static) -- copy, don't re-upload.

Full write-ups for all of the above: `state/changelog-part4.md` (newest),
then `state/changelog-part3.md` and the other detail files below.

---

## 0–1. Repo snapshot and Phase 0 build bugs — retired

Both sections were historical (Phase 0, 2026-09-10) and moved to
`state/changelog-part4.md`. First tag `v0.0.1` exists since 2026-09-28.

## 2. Naming — ✅ resolved

`PROJECT_NAME` is `voxel_browser`. Executables: `voxel_browser` (client),
`voxel_browser_server`. CI artifacts are
`voxel_browser-<target>-<arch>-<build_type>`.

---

## 3. CI landmines

- **Tags:** `setup_metadata.yml` runs `git describe --tags --abbrev=0`, which
  errors on a history with no tags; `v0.0.1` exists since 2026-09-28, so
  `bundle`/`publish` now get a real version.
- `publish.yml` triggers only on `v*.*.*` tags and **rebuilds at the tag** by
  calling `runner.yml` (`workflow_call`), then downloads the `${project_name}`
  bundle artifact from the same run. It used to pull the latest `main` build,
  whose shallow checkout had no tags, so archives/`release.toml` said `v0.0.0`
  and `vb install` 404'd. `setup_metadata.yml` now uses `fetch-depth: 0`.
  Releases cut before this fix (all `v0.0.x` so far) are broken: re-tag.
- **Distribution builds must pass `-DVB_WITH_COMPRESSION=ON`** (default
  OFF). Without it asset sync is a `kDisabled` stub: joining clients get no
  `ui/` scripts (no HUD, so no visible chat/hotbar) and untextured blocks.
  v0.1.2 shipped like that. `--version` now reports `+asset-sync`, the three
  build workflows' release verify step requires it, and the server warns at
  startup when it's missing.
- `lint.yml` installs clang-format via `pip install` then runs it via
  `pipx run clang-format` (dead weight from the pip install). Lints `src/**`
  with `--Werror` — every new file under `src/` must be clang-format-clean.
- Build workflows use `actions/checkout@v4 submodules: recursive`, but
  **there are no submodules** — deps are `FetchContent`. Harmless.
- `build_linux.yml` installs X11/GL dev packages for raylib, plus
  `libssl-dev libprotobuf-dev protobuf-compiler` for `VB_WITH_NET`.
- **`VB_WITH_NET` CI coverage:** Linux (apt) and Windows (vcpkg) build it;
  macOS builds it via the `build_net_deps` job (universal protobuf/OpenSSL
  via `lipo`), green on real runs since 2026-10-17.
- **Never call `find_package(Protobuf REQUIRED)` a second time anywhere in
  the tree.** GameNetworkingSockets' own `src/CMakeLists.txt` already calls
  it; a second call (even as a "fail fast" convenience) fatal-errors on
  some protobuf installs (Homebrew, not vcpkg) with "Some (but not all)
  targets in this export set were already defined." Fixed 2026-09-11 by
  deleting the redundant call — don't reintroduce it.
- **Machine-local build/toolchain/agent-shell gotchas (vcvars64.bat path,
  Bash-vs-PowerShell-tool quirks, which pre-built `build-*` dirs actually
  exist, macOS-specific findings) live in `STATE.md.local`, not here** —
  gitignored (`*.local`), specific to whatever physical machine an agent
  session runs on; check it first when setting up a build in an agent
  shell, and add to it rather than here when you hit a new one.

---

## 4. Config / style quirks

- `.clang-format` is **Godot's** clang-format file verbatim. The C++ rules
  that matter: **tabs** (`UseTab: Always`, `TabWidth 4`), `ColumnLimit: 0`
  (no auto-wrap), `AccessModifierOffset: -4`, pointers right-aligned,
  `Cpp11BracedListStyle: false`. `Standard: c++17` but CMake sets
  `CMAKE_CXX_STANDARD 20` — fine unless C++20-only syntax confuses the
  formatter; bump to `c++20` if that happens.
- `.gitignore` ignores `build`, `*.local`, `compile_commands.json`,
  `.vscode/*` (except `extensions.json`). `CMAKE_EXPORT_COMPILE_COMMANDS
  ON` is set — symlink/copy `build/compile_commands.json` to root for
  tooling.
- `CMakeLists.txt:54` uses `file(GLOB_RECURSE SOURCE_FILES ...)` — adding a
  `.cpp` doesn't trigger reconfigure. Keep `cmake` re-runs in the loop.
- `libfantastic` (the lib target) is `add_library(... INTERFACE)` — assumes
  header-only. `vb_core` is a real STATIC lib; don't carry the INTERFACE
  assumption forward.
- `.gitignore`/`tests/CMakeLists.txt` are checked in with CRLF endings
  (every other text file is LF-only) despite `core.autocrlf=input` locally
  — `git add` warns harmlessly today, but the first real edit through a
  CRLF-preserving tool will silently flip the whole file's line endings,
  burying the actual diff. No `.gitattributes` forces this repo-wide. If
  ever cleaned up, use `git add --renormalize <path>` after adding a
  `.gitattributes` rule, not a manual find/replace.

**Longer-form gotchas (compiler/toolchain traps, full detail in
`state/gotchas.md`):**
- `std::erase`/`std::remove` on a `std::vector<ChunkCoord>` (or any small
  trivially-copyable struct that size/alignment shape) fails to compile
  under this repo's clang-targeting-MSVC-STL toolchain
  (`static_assert(false, "unexpected size")` inside `<xutility>`). Use a
  manual erase loop instead.
- A local `clang-format --dry-run --Werror` binary may not be trustworthy
  as-is against this repo's `.clang-format` — verify by diffing against an
  unmodified `HEAD` file before trusting either a pass or a wall of
  violations.
- `doctest`'s `--test-case=` filter is a **glob pattern**, not a substring
  match — wrap in `*...*` and quote it (zsh glob-expands an unquoted
  pattern itself).
- MSVC (`cl.exe`) rejects a ternary between two different instantiations of
  a templated smart-pointer type with converting constructors (`C2445`) —
  use plain `if`/`else` instead. Clang/GCC not cross-checked.
- A sibling file's "isn't implemented yet" comment can go stale the moment
  a later phase lands it, without the comment ever being updated — grep the
  actual current binding (e.g. `src/script/pack_runtime.cpp`) before
  reusing a sibling's "this doesn't work yet" framing in new code.
- A client built without `VB_WITH_COMPRESSION` can never asset-sync
  against a server built with it on (`hash_bytes()` stubs to all-zero) —
  every real (non-`--singleplayer`) connection needs **matching**
  `VB_WITH_COMPRESSION` on both binaries; there's no runtime negotiation.
- A second, independent bug produces the identical `"asset transfer failed
  (hash mismatch or size cap)"` text even with matching compression flags:
  the server used to build the asset manifest *before* `vb.storage`'s
  deferred first-tick flush reached disk. Fixed by calling
  `pack_runtime.flush_storage()` before `build_manifest()` in
  `src/server/main.cpp`. **This error string has (at least) two unrelated
  root causes** — check both before assuming which one you've hit.

---

## 5. Dependency notes / unknowns

- **Cellulose** (`github.com/RechieKho/cellulose`) — evaluated as a greedy
  mesher, then reverted after it reproduced an NVIDIA-driver VAO/VBO
  heap-corruption crash (greedy-merged geometry blows past the renderer's
  buffer-reuse headroom far more often than the hand-rolled per-face
  mesher). Hand-rolled `vb::world::chunk_mesher` is the **permanent**
  meshing backend now, not a placeholder. `VB_WITH_MESHING` scaffolding was
  later removed outright at user request (2026-09-17). Full story in
  `state/changelog-part2.md`.
- **FastNoise2**: real upstream is `Auburn/FastNoise2` (the README's
  `electronicarts/fastnoise` link is a fork, don't use it). Pin is
  `v0.10.0-alpha`, not `v0.10.0` (that tag doesn't exist — a real,
  previously-unnoticed bug fixed in Phase 6.14, see `state/changelog-part1.md`).
- **GameNetworkingSockets** — resolved (Phase 1.2, 2026-09-11), the single
  hardest dependency in the project. Protobuf cannot be `FetchContent`-ed
  (must be a real package-manager install: vcpkg/apt/brew). GNS pinned to
  `v1.6.0` (not `v1.4.1`, which doesn't compile under a modern stdlib). ICE/
  WebRTC disabled. Windows crypto is BCrypt; Linux/macOS use system
  OpenSSL. Linked statically. **Full story, including local-verification
  toolchain quirks and the Homebrew double-`find_package` crash, is in
  `state/changelog-part1.md`'s Phase 1.2 entries — read those before
  touching `cmake/Dependencies.cmake`'s GNS block again.**
- **librg** is `zpl-c/librg`, pinned `v7.4.0`, wired in as the default
  entity-replication backend since 2026-09-17 (`VB_WITH_REPLICATION`
  default ON; the original hand-rolled linear scan remains as an explicit
  opt-out). Known limitation: chunk ids need `chunkamount.x*y*z` to fit
  signed int32 and each axis casts to `int16_t` — picked 1024 chunks/axis
  (±512×cell_size world units); an entity straying outside that range is
  silently excluded from interest until it re-enters. Full detail in
  `state/changelog-part2.md`.
- **Lua**: PUC-Lua has no upstream CMake; needs a wrapper `CMakeLists.txt`.
  sol2 is the chosen binding layer (pinned `v3.5.0`, not `v3.3.0` — that
  version's bundled optional impl fails under Clang ≥ 18).
- **raygui** is header-only (`raysan5/raygui`), needs exactly one TU with
  `#define RAYGUI_IMPLEMENTATION` in its own target (`vb_raygui_impl`) so
  project `-Werror` never touches vendored code — same pattern used for
  librg's `LIBRG_IMPL`.

---

## 6. Design decisions

Tracked in `ARCHITECTURE_SPEC.md` §18 (the source of truth). Settled: sol2
for Lua bindings; hand-rolled meshing is permanent (Cellulose reverted);
librg for interest culling only, own payload codec; LZ4 for chunk/wire
compression and region-file framing (2026-09-28); flat per-region world
files (`RegionStore`), used by both the dedicated server and
`--singleplayer`; auth via pack `auth.lua` (Phase 9, replacing the old
"engine owns no auth" direction). Pre-dream wording: `state/changelog-part4.md`.

Other undecided:
- Test framework: doctest (in use) vs. Catch2 — never revisited since
  Phase 0's pick.
- Whether the client embeds the server for singleplayer as a library or
  spawns a child process — **currently: in-process library**
  (`Singleplayer` in `src/client/session_host.hpp` constructs
  `LoopbackNetwork`/`ServerSession`/`ClientSession` directly).
  Direction (2026-10-25): it's a netcode-isolation diagnostic, not a play
  mode — Phase 11 (11.1–11.3 done 2026-10-27) hid it from the menu and
  renamed the flag `--loopback` (`--singleplayer` alias); `menu.loopback`
  automation command, `menu.singleplayer` alias kept.

---

## 7. Conventions to follow

- Namespaces `vb::<module>` (`vb::net`, `vb::world`, ...); headers under
  `inc/vb/<module>/`, mirrored by `src/<module>/`.
- No exceptions on hot paths; `Result<T,E>` for fallible ops. Never call
  `.error()` when a `Result` holds a value (union UB) — `REQUIRE(result)`
  then deref in tests, not `REQUIRE_MESSAGE(result, msg(err))`.
- Wire structs live in `inc/vb/protocol/`; every one gets a round-trip
  test and bumps `ENGINE_PROTOCOL_VERSION` + `docs/protocol.md` when
  changed. Concrete step-by-step for adding one: struct → `MessageType` →
  round-trip test → version bump → `docs/protocol.md` entry → integration
  test (see `CONTRIBUTING.md`).
- Keep both binaries buildable/runnable with `--headless` (CI +
  integration tests depend on it).
- Match `.clang-format`: tabs, no column limit, run clang-format before
  commit (CI `--Werror` on `src/**`).
- New Lua bindings should be **generic primitives, not game-specific
  ones** — game rules belong in `content/*` packs (e.g. `player:take()` is
  a generic inventory primitive; the actual crafting recipe list lives
  entirely in `content/base/crafting.lua`, not in engine code). See
  `CONTRIBUTING.md` for the worked example.
- Heavy deps (GNS, librg, Lua/sol2, FastNoise2, LZ4/xxHash) are declared in
  `Dependencies.cmake` but gated behind `VB_WITH_*` (several now default
  ON — check `CMakeLists.txt` for current defaults, don't assume OFF).
- doctest + MSVC STL: comparing/streaming a `std::string_view` in a
  `CHECK` needs `#include <ostream>` in that test TU (instantiates
  `toString<string_view>`).
- GCC gotcha: `uint64_t` (`unsigned long` on LP64) vs. `...ULL` literals
  trips `-Wsign-conversion` — always name wide constants `constexpr
  std::uint64_t`.
- Don't store `const BlockRegistry&` — callers pass `BlockRegistry::base()`
  temporaries; hold it by value.
- **xxHash/lz4 link order matters**: lz4's vendored source ships its own
  old `lib/xxhash.h` with no `XXH3_128bits`. `VB_WITH_COMPRESSION` links
  `xxHash::xxhash` **before** `LZ4::lz4` on purpose so the real header
  wins `-I` search order — don't reorder this.
- `std::vector<std::byte>` can't be built directly from
  `std::istreambuf_iterator<char>` — use the `read_whole_file()` helper in
  `assetsync/cache.cpp` instead of re-deriving it.
- **Clang: a nested class's `Foo() = default;` can't be used as a default
  argument value (`= {}`) on a member function of the *enclosing* class
  declared before that enclosing class is complete** — Clang errors
  wanting the nested class's field default-member-initializers "within
  definition of enclosing class" to compute the defaulted ctor's implicit
  exception spec, which isn't available yet at that point. Hit this in
  `inc/vb/world/lighting.hpp`'s `LightEngine::Neighbours() = default;`
  used as `relight_chunk(Chunk&, const Neighbours& = {})`'s default arg.
  Fix: give the nested ctor a user-provided empty body (`Neighbours() {}`)
  instead of `= default` — a user-provided ctor's noexcept-ness doesn't
  need the deferred computation, so no reordering/out-of-line-definition
  dance is required.

---

## Detail files

Every file below is full-detail material moved out of this core. Current
work is summarized above under "Current status"; the full write-ups are
here, organized as it was originally written (mostly chronological,
newest-first within each file):

- **`state/changelog-part4.md`** — newest: the 2026-10-06 dream's moves —
  the E2E automation status log (E0–E6), the 2026-09-30 entries (doctest
  bump, macOS CI `VB_WITH_NET` + first-run suspects, `WorldReplicator` send
  budget, the `PlayerHandle`/sol2 root cause), and the retired §0/§1, stale
  §3 bullets and old §6 text. Continues into part 3.
- **`state/changelog-part3.md`** — newest-first writeups moved out of
  "Current status" above: mob damage (and the `PlayerHandle` corruption bug
  it surfaced, since fixed — see part 4), the hunger primitive, the "No PvP"
  confirmation, and back through all of Phase 7 (7.1-7.6), 6.18-6.21, and
  the visual/despawn/kind-rendering follow-ups, down to 6.18 (discrete
  punch combat) — see that file's own header for the full topic list;
  `changelog-recent.md` below picks up from there.
- **`state/changelog-recent.md`** — full writeups for the block-self-heal
  follow-up, Phase 6.18 (discrete punch combat), Phase 6.17 (decoupled
  block breaking + `MovementBindings`), Phase 1.3 networking polish
  (two-process smoke test, per-IP connection cap, hostname resolution),
  Phase 6.15 (kitchen-sink example pack), Phase 6.14 (Lua-driven worldgen/
  FastNoise2), Phase 6.13 (read-only server config visibility) — these
  phases were never logged into the older §8 structure below and are not
  duplicated in the next two files.
- **`state/changelog-part1.md`** — Phase 6.1/6.4/6.5/6.6/6.7/6.8/6.9/6.11/
  6.16 landed-feature writeups; the 2026-09-15 investigations (NVIDIA
  driver VAO/VBO heap corruption, FPS dip during chunk streaming,
  cross-chunk sky-light band, near-black flash on predicted breaks, chunk
  meshing moved off the main thread, GNS send-buffer overflow root cause);
  the 2026-09-10/11 Phase 0-4 bootstrap entries (config, transport,
  handshake, session layer, physics/netcode, worldgen, entity visuals,
  block editing, Lua VM); "Gotchas learned this pass".
- **`state/changelog-part2.md`** — Phase 5.1 `content/base` pack + the gap
  it found (neither binary loaded a pack at all before this); Phase 5.2-5.5
  (main menu, chat, player list/join-leave, day/night, death/respawn, sfx
  docs, real inventory sync + hotbar, hold-to-break progress, dropped-item
  entity, crafting recipes, `--singleplayer` running the real content pack,
  documentation pass); the Cellulose meshing spike and revert; librg wired
  in as the interest backend and flipped to default; Phase 6 design-only
  passes (entity classes, UI immediate-mode, keybind channel, `vb.db`,
  block-damage breaking, worldgen biome-selection redesign).
- **`state/gotchas.md`** — long-form compiler/toolchain traps (and the e2e-verification
  "concrete misses" write-up) referenced
  from §4 above (the `std::erase` MSVC-STL bug, the MSVC ternary
  ambiguity, the two independent asset-sync hash-mismatch bugs, the
  stale-comment lesson, the clang-format-untrustworthy-locally note).

Anything **not** listed above and not inline in this file no longer exists
in `STATE.md`'s history — if you're looking for something and can't find
it here, check `git log -- STATE.md` for when it might have been removed,
or `REMAINING_TASKS.md`/`ARCHITECTURE_SPEC.md` for design-level context.

## Phase 10 (developer experience) gotchas — 2026-10-06

- The Lua API has **three** things to keep in step: the C++ binding, its stub in `sdk/lua/library/`, and the
  regenerated `docs/lua-reference/` + `sdk/lua/api_index.txt` (`python3 scripts/gen_lua_docs.py`).
  `lua_api_surface_test` (needs `VB_WITH_LUA`) fails on any mismatch; the `lua-docs` lint job on stale output.
- `vb help --markdown > docs/cli.md` after touching the command table or `src/cli/help.cpp`
  (`dev_cli_help_test` checks it).
- `kEngineProtocolVersion` is 30: `S2CServerInfo` ends with `engine_version_req`. Aggregate-initialised
  `S2CServerInfo{...}` in tests must now list it (or use member assignment).
- `src/script/lua_global_scan.c` reads Lua 5.4 internals (`#error` on another version). Bumping the pinned Lua
  means re-checking it.
- `vb.noise.value`/`cellular` take a number **or** `{ frequency = n }` (since 2026-10-18; the table used to
  be silently ignored). Frequencies multiply down the graph: an fbm's `source` belongs at 1.
- The asset manifest skips dot-entries and pack-root README.md/AGENTS.md; a pack file that must reach clients
  cannot start with a dot.
- Local tooling: CI's clang-format is the newest pip release (23.x); Ubuntu's 18.x disagrees on existing
  files. Use `pip install clang-format` and run it on the files you touched.
- Mouse capture is owned by `ClientApp`; the UI VM only *requests* it (`UiRuntime::take_capture_request`, fed
  by `ui.close{capture_mouse=true}` / `capture_mouse_on_close` / `client.capture_mouse`). A request waits while
  a screen or chat is open; the frame that captures masks `kInputPrimary` until the button is released.
- `player_leave` runs after `ServerSession` dropped the conn, so session lookups by net id fail there.
  `SessionPlayerLeft` carries the `name` and `last_state` captured at disconnect; `PlayerHandle` falls back to them.
- `S2C_BlockDamage` rides `Lane::kFeedback` (GNS lane 1, no Nagle), *not* ordered against `kWorld` chunk
  messages. Client-side damage goes through `net::BlockDamageTracker`, which uses the message's chunk `revision`
  vs the block's last change revision; don't write `ClientSession::block_damage_` directly.
- Chunk ingest (insert + relight) is capped per tick by time as well as count: the dedicated server gives it half
  a tick, singleplayer 20 ms per frame. `LightEngine` caches per-block-id transmittance/emission and short-cuts
  one-block chunks; keep `relight_chunk` output identical when touching it (lighting_test pins the fast paths).
  `VB_E2E_BENCH=1 pytest tests/e2e/test_latency.py` measures punch latency while another player explores.
