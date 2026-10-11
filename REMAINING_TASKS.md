# Voxel Browser — Remaining Tasks

> Companion to `ARCHITECTURE_SPEC.md`. This is the implementation backlog to get
> from the current state to the "Minimum Playable Base" described in `README.md`.
>
> **This file is the lean core.** Every phase's full history (the detailed
> `[x]` write-ups — what shipped, why, which files, which tests) lives in its
> own `remaining_tasks/phaseN.md`, linked from each phase section below. Read
> this file for current status and what's actually left; open the linked file
> only when you need the historical detail behind a specific `[x]` line.
>
> **Hard cap: 500 lines.** Over that (or after closing a phase), "dream":
> move inactive detail into `remaining_tasks/*.md` — see `DREAMING.md`.

## Current State (baseline)

- [x] Modular CMake: `vb_core` / `vb_render` / `voxel_browser` /
      `voxel_browser_server` / `vb_tests`, git-tag versioning, `PROJECT_NAME`
- [x] `cmake/{Dependencies,Warnings,Sanitizers}.cmake`; deps via pinned `FetchContent`
- [x] Runnable client + server skeletons (arg parsing, banners, loops, `--headless`)
- [x] CI: lint + Linux/macOS/Windows build matrix **+ ctest**, bundle, publish
- [x] `.clang-format` (tabs, `ColumnLimit 0`, `Standard: c++20`)
- [ ] Networking / world / ECS / scripting — Phase 1 onward

Legend: `[ ]` todo · `[~]` in progress · `[x]` done · **(spike)** = timeboxed
investigation, may change the plan.

---

## Phase 0 — Project Restructure & Build System ✅ done (2026-09-10)

Prerequisite for everything else, not in the README's phase list. Full split
target/build system: `vb_core`/`vb_render`/client/server/tests, pinned
`FetchContent` deps gated behind `VB_WITH_*`, warnings/sanitizer cmake
modules, `VB_HEADLESS`, CI workflows, project renamed to `voxel_browser`.
Full detail: `remaining_tasks/phase0.md`.

**Remaining:**
- [x] `git tag v0.0.1` (2026-09-28); doctest bumped to v2.5.3 (2026-09-30).
      The `CMAKE_POLICY_VERSION_MINIMUM=3.5` shim stays — lz4 v1.9.4 still
      needs it. Detail: `remaining_tasks/phase0.md`.
- [ ] Explicit source lists instead of relying on re-running CMake (already
      explicit; keep it that way as modules grow).

---

## Phase 1 — Core Foundation & Networking ✅ met (2026-09-11)

Goal: reliable client↔server handshake; two clients "see" each other in
network space; client window + render loop alive. Core primitives, transport
(`LoopbackTransport` + real-UDP `GnsTransport`), handshake FSMs, hand-rolled
`InterestGrid` replication (librg wired in behind `VB_WITH_REPLICATION`),
client shell (raylib window, `FirstPersonController`). Both binaries wired up
end-to-end; verified with two live processes over real UDP.
Full detail: `remaining_tasks/phase1.md`.

**Remaining:**
- [x] Protocol-version mismatch shown in the connect UI (already wired,
      confirmed 2026-09-28); two-client replication re-run over
      `GnsTransport` (2026-09-28).
- [x] macOS CI builds `VB_WITH_NET` (2026-09-30, `build_net_deps` job in
      `build_macos.yml`) — **not yet verified by a real Actions run**.

---

## Phase 2 — World State & Terrain Generation ✅ met (2026-09-11)

Goal: server generates terrain, streams chunks, client meshes and renders
them. `PalettedChunkStore`, `World`, fBm heightmap worldgen (determinism gate
green on all 3 platforms), per-chunk `LightEngine` + cross-chunk vertical
sky occlusion, chunk replication + `ClientChunkStore`, hand-rolled
face-culled mesher with a background mesh worker pool (Cellulose/greedy-merge
was tried and reverted — see `ARCHITECTURE_SPEC.md` §18 Q2 — hand-rolled
meshing is permanent).
Full detail: `remaining_tasks/phase2.md`.

**Remaining:**
- [x] Horizontal cross-chunk sky-light propagation (2026-09-28; block light
      still doesn't cross chunk borders — deliberately out of scope);
      frustum culling + sorted transparent pass (2026-09-27).

---

## Phase 3 — Entity Component System & Physics ✅ substantially met (2026-09-11)

Goal: server-simulated players with voxel collision, movement synced to
clients with prediction/interpolation. EnTT registry wiring on the server
(2026-09-17), fixed 20 Hz tick loop (including `--singleplayer`'s integrated
server), swept-AABB voxel physics (`vb/physics/movement.hpp`), input
pipeline + prediction/reconciliation, remote-entity interpolation, and
billboard-sprite presentation (direction buckets, anim priority) for remote
entities — mechanism shipped and tested, real multi-window art not yet
eyeballed live.
Full detail: `remaining_tasks/phase3.md`.

**Remaining:**
- [x] All done: `SystemRunner`, client-side registry, per-player flood guard
      (`max_messages_per_second`), step-up camera smoothing, wall-clock
      `server_time_est`, sprite visual state and real billboard art.

---

## Phase 4 — The "Browser" Engine (Scripting & Assets) ✅ met — mechanism, not content

Goal: server logic + content defined in Lua; client auto-downloads pack
assets; Lua-defined UI. Sandboxed Lua 5.4 + sol2 VM, `PackRuntime`
registration/event-bus/scheduling API, `S2C_BlockRegistry` replication, full
Asset Sync protocol (manifest, chunked transfer, content-addressed client
cache, virtual pack FS), and a separate client-side `UiRuntime` + raygui
renderer driven by `player:open_ui`. All five subsystems (4.1-4.5) work
end-to-end over `LoopbackTransport`; what was missing was content, closed by
Phase 5.1.
Full detail: `remaining_tasks/phase4.md`.

**Remaining:**
- [x] All done: custom `require` + wall-clock budget, `EntityKind` callbacks
      in `SystemRunner`, `visual = {...}` and per-instance `visual_override`
      (spawn-time only — no live update/clear path), real base-pack art, the
      texture atlas, manifest staleness fix (not for `--singleplayer`), and the
      `icon` item-grid widget.
- [ ] Follow-up: full base-pack reskin (dirt/grass/sand/wood/leaves) — only
      `base:stone`/`base:water` use real textures so far.

---

## Phase 5 — Minimum Playable Base ✅ met over `LoopbackTransport`

Goal: a small, coherent, playable multiplayer sandbox. `content/base` (blocks,
biomes, crafting, dropped items, real inventory, hotbar), block break/place
over the network with a Lua veto seam and hold-to-break timing, a full raygui
main menu / connect / settings flow, and play polish (day/night, chat, player
list, death/respawn). Verified via automated tests + `--headless` runs; the
literal "two real windows, playing together" manual pass across all 3
platforms has not been run by a human yet.
Full detail: `remaining_tasks/phase5.md`.

**Remaining:**
- [x] Done: sprite atlases, cross-chunk relight on edit, per-block tool
      break time (`block_damage`), keybindings screen, connect-screen byte
      progress bar.
- [ ] Live two-window manual playtest (chat + crafting + seeing each other,
      all at once, across all 3 platforms) — not yet run.

---

## Phase 6 — Lua-Driven Extensibility ✅ 6.1–6.16, 6.18, 6.20–6.21 done; 6.17 superseded

Goal: content packs override/extend engine defaults the way block
registration already does — biomes, entities, UI, input, data. Landed:
per-instance Lua entity objects (6.1), reactive `render(state)` UI (6.2),
custom keybinds (6.3), `vb.db`/`vb.crypto` (6.4), block-damage system (6.5),
player damage/death primitive (6.6), physics/day-night/inventory/chat/
item-drop override surfaces (6.7-6.11), read-only config visibility (6.13),
Lua-driven FastNoise2 worldgen pipeline + Voronoi biome selection (6.14), a
`content/examples/kitchen_sink` pack exercising all of the above (6.15), an
engine-neutral client HUD primitive (`ui.define_hud` + `kRect`, 6.16),
Growtopia-style discrete punch combat with block self-heal (6.18 — this is
the current shape of block breaking, **not** 6.17's original hold-to-break
plan, which was superseded same-day and is kept only for historical record),
right-click placing decoupled into content the same way (6.20 — closes
6.17/6.18's own "Placing... unaffected" gap; `player:place_block()` is now
the validated primitive, `content/base/mechanics.lua` decides when/what), and
a unified, pack-overridable interaction reach + physics/action read-back
surface (6.21 — `vb.action.set_params{reach=}`/`get_params()` and
`vb.physics.get_params()`).
Full detail: `remaining_tasks/phase6.md`.

**Remaining:**
- [x] Done since 6.21: fall damage (6.22, `player_landed`), PvP confirmed,
      hunger primitive (server-side only, no HUD/replication), zombie mob,
      health-based despawn, kind-specific rendering, keybind rate limit,
      replicated block damage + crack textures, movement keybinds in the
      registry (6.19), HUD click/change reporting, player list/chat/hotbar
      moved to `ui.define_hud`, opt-in punch cooldown, held item / hotbar
      selection, unified reach (6.21), entity text labels (protocol 32:
      `vb.world.spawn{text=}`, `entity:set_text`, `visual = false`
      text-only kinds), real `on_ground` flags for every entity,
      `entity:set_clip`, `entity:attach_to`/`detach`, sprite `layer`/
      `through_walls`, alpha-cutout + back-to-front entity drawing
      (all `S2C_EntityProps`, protocol 32).
- [ ] Still open: no swing animation, no PvP armor/knockback.
- [x] Paper-doll appearance (protocol 32): visual `layers` (below/rows/
      tint), `player:`/`entity:set_visual_override` at runtime, own outfit
      via the F5 third-person camera (`vb.render.set_third_person`).
- [ ] UI `sprite` widget (wardrobe/preview screens) and a
      `client.my_appearance()` UI binding -- the client side already has
      `ClientSession::my_visual_override()`.

---

## Phase 7 — World & UX Polish (7.1-7.6 done)

> User-requested (2026-09-19): four UX gaps, scoped as their own phase since
> none of them extend Phase 6's "default + override" system pattern the way
> 6.1-6.20 did — these are new engine surfaces (loading UI, fog, liquid
> collision/vision/regions) plus closing a real content gap found while
> reviewing Phase 6 (`ui/pause.lua`/`ui/inventory.lua`'s own "nothing opens
> this yet" comments).
Full detail: `remaining_tasks/phase7.md`.

- [x] 7.1 loading screen · 7.2 Lua-adjustable distance fog · 7.3 walkable
      liquids, underwater fog and `region_enter`/`region_exit` (no
      flowing-liquid physics — out of scope) · 7.4 base UI screens on
      Escape/E · 7.5 underwater fog tint (liquid colour by default,
      `underwater_tint` override) · 7.6 region-file persistence
      (`RegionStore`; LZ4 framing and singleplayer wiring closed 2026-09-28).

---

## Phase 8 — Developer CLI (`vb`) — planned (2026-10-04)

User-requested: a `vb` command-line tool that downloads and manages installed
copies of Voxel Browser in the **user's profile** (no admin rights), and
makes hosting a server a one-command affair (`vb host`, plus named
background instances via `vb server …`). Full design, layout, command
surface and per-phase task lists: `architecture_spec/dev-cli.md` §11.

- [x] **8.1 — Release pipeline produces installable artifacts** (prerequisite):
      per-platform `voxel_browser-<ver>-<os>-<arch>.zip` assets + a
      `release.toml` with SHA-256s; today's `bundle.yml` flat-merges every
      platform's identically-named files into one artifact.
- [x] **8.2 — `vb` skeleton + local version management**: `user_data_dir()`/
      `user_config_dir()`/`VB_HOME`, `vb list/use/which/uninstall/link/launch`.
- [x] **8.3 — Download & install**: curl + miniz behind `VB_BUILD_CLI`,
      verified atomic install transaction, `vb install/update/prune/doctor`.
- [x] **8.4 — Hosting**: engine `--stop-file` (Windows graceful stop) and
      client `--content-pack`/`--world-dir` for `--singleplayer`;
      `vb host`, `vb server new/start/stop/status/logs/rm`.
- [x] **8.5 — Polish**: `list --remote`, `--json`, `self update`, bootstrap
      scripts, server `--status-file`, completions, `vb host --watch`.
- [x] **8.6 — Hardening**: Ed25519-signed `release.toml` (verified by `vb` and
      `install.sh`; **inactive until the maintainer adds a key to
      `release_keys.txt` and the `RELEASE_SIGNING_KEY` secret** — steps in
      `architecture_spec/dev-cli.md`), `vb server service print`,
      `vb launch --connect` protocol warning. arm64: `vb`/scripts understand
      `linux-arm64`/`windows-arm64`, but the **CI legs are not added yet**
      (how-to in the design doc).

---

## Phase 9 — In-Engine Authentication (`auth.lua`) ✅ 9.0–9.9 done (2026-10-06)

User-requested: a pack-level `auth.lua` declares the identity provider
(generic OIDC, Keycloak, Firebase); its **presence makes authentication
mandatory**, and the server verifies the ID token during the handshake before
sending anything. Design: `architecture_spec/auth.md` §12; operator guide:
`docs/auth.md`; Keycloak testing plan: `docs/auth-keycloak-testing.md`.
Full detail: `remaining_tasks/phase9.md`.

- [x] 9.0–9.9: `auth.lua` loading + fail-closed startup, protocol v29
      handshake, server JWT/JWKS verification (Mbed TLS + curl), Lua
      `player:get_login()`, client sign-in (OIDC browser + PKCE, Firebase
      password), re-auth / revocation + `SessionStore`, singleplayer / `vb` /
      e2e, hardening (per-IP sign-in rate limit), mock-Keycloak testing
      checked against real Keycloak 26.7.5.
- [ ] Still open from 9.x: libFuzzer targets (a deterministic mutation test
      stands in); OS keychain for refresh tokens (0600 files today); Firebase
      `google` sign-in (needs a Google OAuth client id key in `auth.lua`);
      `state.login` in the client UI VM; first-use trust prompt at e2e level;
      manual sign-in runs and builds on Windows/macOS (socket/ShellExecute
      code is unbuilt).

---

## Phase 10 — Developer Experience (pack authors & agents) ✅ 10.A–10.G done (2026-10-06)

User-requested: make writing a content pack easy for people and agents —
`npm init`-style scaffolding, a quick-reference for the Lua API with examples,
editor type stubs, and markdown/JSON CLI docs agents can read. Single source of
truth: hand-written LuaCATS stubs in `sdk/lua/`, with the reference generated
from them and a runtime test that fails on undocumented bindings. Full design,
layout, command surface and per-phase task lists:
`architecture_spec/dev-experience.md` §5; what was built differently: §7.

- [x] **10.A — API inventory + drift test**: walk the live `vb`/`ui` tables and
      usertypes in a unit test, compare with `sdk/lua/api_index.txt`.
- [x] **10.B — LuaCATS stubs** for the server VM, UI VM, data scripts and
      `auth.lua`, one example per function; `.luarc.json`; LuaLS check in CI.
- [x] **10.C — Generated quick reference**: `scripts/gen_lua_docs.py` →
      `docs/lua-reference/` + one-line-per-function cheat sheet, `--check` in
      CI, doc examples syntax-checked.
- [x] **10.D — Headless pack validation + version enforcement**:
      `voxel_browser_server --check-pack <dir> [--json]` (`file:line`
      diagnostics) + `vb pack check`, including a static per-environment
      global check (`vb` in `ui/*.lua`, `ui` on the server, sandbox-removed
      builtins); the engine enforces `pack.toml`'s `engine_version_req`
      (server, singleplayer, and the client via a handshake field — protocol
      bump).
- [x] **10.E — Scaffolding**: `vb pack init [dir]` from templates embedded in
      `vb` (README, AGENTS.md, `.luarc.json`, stubs, `engine_version_req`),
      `vb pack types/info/dev`.
- [x] **10.F — CLI reference & agent docs**: richer command table → `vb help
      <cmd>`, generated `docs/cli.md`, `vb help --json`; uniform
      `error:`/`hint:`/`--json`/`--yes` contract; root `AGENTS.md`, `docs/llms.txt`.
      Independent of B–E.
- [x] **10.G — Ship it**: `sdk/` and `docs/` in the release archive, `vb docs`,
      README "Make your first pack", CONTRIBUTING "adding a binding".

---

Follow-ups:

- [ ] LuaLS `--check` job in CI (pin a LuaLS release; zero warnings in `sdk/lua`), and try a nested
      `ui/.luarc.json` for real per-environment editor warnings.
- [ ] `--check-pack` warnings: block without a texture, unknown keys in `def` tables; `since`-aware
      "API newer than your `engine_version_req`" warning (§3.7 "Later").
- [ ] Singleplayer should show an `engine_version_req` mismatch in the main menu (today: stderr + base set).
- [ ] `vb.noise.value{frequency=...}` ignores a table argument (the binding takes a number, so
      `content/examples/kitchen_sink` silently gets the default); make it accept both or fix the example.
- [ ] `vb pack dev` is untested end to end (needs a real server + client); only argument handling is covered.

---

## Cross-Cutting / Continuous

Full detail: `remaining_tasks/cross_cutting.md`.

- [~] **End-to-end multiplayer automation (dev-only)** — all phases E0–E6
      landed (design `docs/e2e-automation.md`). Still open: the CI `e2e` job
      has never run on a runner; Windows/macOS are unbuilt. Must stay
      compiled out of production builds (`VB_WITH_AUTOMATION`,
      `VB_DISTRIBUTION`). Doc-upkeep checklist: `docs/e2e-automation.md` §11.
- [x] `PlayerHandle` stored across ticks read corrupted data — fixed
      2026-09-30 with `SOL_FUNCTION_CALL_VALUE_SEMANTICS=1` on the `sol2`
      target; don't remove that define.
- [ ] Keep `ENGINE_PROTOCOL_VERSION` + `docs/protocol.md` in lockstep with every
      wire change.
- [ ] Every new `vb/protocol` struct gets a round-trip + fuzz test.
- [x] Sanitizer CI jobs (ASan/UBSan, TSan; Linux only, never verified by a
      real Actions run), soak test (`soak_test.cpp`), perf budget checks
      (`perf_budget_test.cpp`; frame time left out — needs a GL context).
- [ ] Determinism golden-value CI gate stays green across platforms.
- [ ] `--headless` stays functional for both binaries (CI + integration tests).
- [ ] Address the remaining open item(s) in `ARCHITECTURE_SPEC.md` §18 as
      their blocking phase arrives; record decisions in that section.

---

## Deferred (post first-playable)

region file format versioning/migration · audio subsystem + Lua sfx/music API · server-side
plugin hot-reload · entity-entity physics/mounts/projectiles · particle
system beyond block-break puffs · compression tuning (zstd, snapshot deltas,
bit-packed inputs) · dedicated server browser/master list · modding
(stacked packs, dependency resolution) · rule-based decorative structure
placement for worldgen (depends on 6.14, which landed the prerequisite —
this item itself not attempted) · Voronoi biome-cell resolution caching
(6.14, no perf problem observed yet) · CSS-like declarative layout for
`UiRuntime` widgets (today: absolute pixel positioning only).

Full reasoning for each: `remaining_tasks/deferred.md`.
