# Voxel Browser — Architecture Specification

> Status: **Draft v0.1** — derived from `README.md`. Describes the target
> architecture; see `REMAINING_TASKS.md` for the gap between this and the
> current codebase, and `STATE.md` for session-to-session gotchas.
>
> **This file is the lean core.** Verbose write-ups (full diagrams, struct
> definitions, API surfaces, design rationale, resolution history) live in
> `architecture_spec/<topic>.md`, linked from each section below. Read this
> file for the actionable rules and current status; open a linked file only
> when you need the full reference behind a section.
>
> **Hard cap: 500 lines.** Over that, "dream": move inactive detail into
> `architecture_spec/*.md` — see `DREAMING.md`.

---

## 1. Purpose & Scope

Voxel Browser is a multiplayer-first voxel engine with an authoritative
server and a thin, content-agnostic client (the "browser"). The base engine
ships almost no game content — blocks, items, world generation rules,
entities, and UI are defined by **Lua content packs** that live on the server
and are streamed to clients on connect.

Covers: process topology, module boundaries, wire protocol, core subsystems
(world model, worldgen, ECS, networking/replication, asset sync, scripting,
rendering, UI, physics), threading, configuration, security, testing.

Non-goals for the first playable base: persistence to disk beyond a simple
region format, server-side anti-cheat beyond input validation, dedicated
matchmaking/lobby services, audio.

---

## 2. Design Principles

1. **Server authoritative.** The server owns all mutable game state. The
   client renders a replicated, interpolated view and predicts only its own
   player.
2. **Content is data, shipped over the wire.** The client trusts no local
   content. Everything gameplay-facing (block registry, textures, UI
   layouts, scripts) is synchronized from the server and content-addressed
   by hash.
3. **Thin client.** Rendering, input, interpolation, prediction, and a
   sandboxed Lua VM for *client-side UI callbacks only*. No authoritative
   game logic runs on the client.
4. **Deterministic-ish core, tolerant netcode.** World generation is
   deterministic from `(seed, chunk coord, pack version)`. Movement uses
   client prediction + server reconciliation; other entities are
   interpolated.
5. **Small hard-coded surface.** The C++ engine exposes stable primitives
   (voxels, chunks, entities, events, network); gameplay is assembled in
   Lua.
6. **Modular build.** Engine code is a library; `client` and `server` are
   separate executables linking a shared `core` library.

---

## 3. Process Topology & Repository Layout

Server (authoritative, headless): Lua VM + worldgen worker pool feed an EnTT
ECS + voxel world; replication (librg) + asset sync + GNS transport push
state to clients over UDP. Client ("browser"): GNS transport + hash-indexed
asset cache + replication receiver feed a client world/chunk store + entity
interpolation + local prediction; a hand-rolled mesher + raygui HUD render
it, with a separate sandboxed Lua VM driving UI only.

**No separate singleplayer code path** — the same binary hosts an integrated
server on `localhost` for singleplayer and connects to it through the normal
socket path.
Phase 11: this in-process path is a hidden diagnostic, `--loopback` (alias
`--singleplayer`, no menu button), for isolating netcode — not a play mode.

Repo layout: `inc/vb/<module>/` mirrors `src/<module>/`
(`core/world/worldgen/ecs/net/replication/assetsync/script/protocol/render`);
`content/base/` is the shipped Lua content pack; `tests/{unit,integration}/`.
CMake targets: `vb_core` (static lib), `vb_render` (static lib, depends on
`vb_core` + raylib), `voxel_browser_server`/`voxel_browser` (exes),
`vb_tests`. Deps via pinned `FetchContent` + `find_package` fallback.

Full ASCII process diagram + complete directory tree + CMake target table:
`architecture_spec/topology-and-layout.md`.

---

## 4. Core Data Model

- `BlockId` — `uint16_t`, index into the per-session **block registry** sent
  by the server (`0` = air). Assigned by the server at pack load, stable for
  the session's lifetime.
- `EntityId` — EnTT `entt::entity` server-side; compact `uint32_t` network id
  on the wire (assigned by librg / the replication layer).
- `ChunkCoord` — `ivec3` in chunk units. `AssetHash` — 128-bit xxHash3 hex.
- **`BlockType`** carries name/id/solid/opaque/light_emission/collision/model/
  textures/Lua callbacks, plus `max_damage` (0 = instant break, default) and
  `crack_texture` (§10.7-equivalent, see the scripting doc). Authored in Lua
  (`vb.register_block{...}`), frozen at server start, sent in the join
  handshake, reconstructed read-only client-side.
- **Chunks**: `CHUNK_DIM = 32`. `PalettedChunkStore` — per-chunk palette +
  bit-packed indices (1/2/4/8/16 bits/voxel); homogeneous chunks collapse to
  one palette entry. Per-chunk: dirty flags (terrain/light/mesh), gen state
  (`Ungenerated → Generating → Generated → Populated`), light volume (4 bits
  sky + 4 bits block per voxel), monotonic `revision` for delta replication.
  `World` = `ChunkCoord → Chunk` map + load/unload driven by player interest
  spheres.
- **Lighting**: flood-fill (sky + block), recomputed on generation and
  incrementally on edits, server-side; replicated alongside block data so
  the client never recomputes it.

Full `BlockType` struct + storage detail: `architecture_spec/data-model.md`.

---

## 5. World Generation

Deterministic per-chunk pipeline from `(world_seed, chunk_coord,
pack_version)`, run on worldgen worker threads:

1. Base density/heightmap — FastNoise2 node trees, pack-configured via
   `vb.worldgen.set_pipeline{...}`.
2. Biome selection — Voronoi-cell partitioning, adjacency-weighted random
   draw (WFC-flavored, **never backtracks**: adjacency weights are soft
   multipliers, never hard exclusions, so a cell can always resolve in one
   pass). Resolution order is a hash of `(world_seed, cell_id)`, never
   exploration order, to keep the pipeline coordinate-deterministic.
3. Surface pass (biome surface/filler blocks).
4. Carvers (caves/ravines, 3D noise threshold, optional).
5. Vein/scatter pass (ore placement, pack-defined tables).
6. Decoration/population pass (trees/structures, schematic or procedural,
   runs after neighbor chunks generate so structures may cross borders).
7. Initial lighting flood fill.

Full rationale (why non-backtracking, adjacency-weight design, deferred
structure-tool direction): `architecture_spec/worldgen.md`.

---

## 6. Entity Component System (EnTT)

Server runs a fixed-tick simulation, default **20 Hz** (`TICK_DT = 50 ms`).

**Base components:** `Position` (dvec3), `Velocity`, `Rotation` (yaw/pitch),
`AABB`, `Collider` (gravity/step-height/on_ground flags), `PlayerInput`
(ring buffer of `InputCmd`), `PlayerTag`, `NetReplicated`, `EntityKind`
(drives billboard sprite/animation selection, §11.3), `ScriptState` (per-
entity `LuaRef` table — Lua's only extension point; it cannot define new C++
components), `Health`/`Inventory`/`ItemStack`.

**Systems, in tick order:** `IngestInputSystem` (drain/validate `InputCmd`,
fires `vb.on("player_input", ...)` — veto or replace, see scripting doc) →
`ScriptPreTickSystem` (`on_tick` for entity kinds + global timers) →
`MovementIntegrationSystem` (gravity, velocity) → `VoxelCollisionSystem`
(swept AABB, `on_ground`, step-up) → `BlockEditSystem` (apply queued
break/place, bump revision, dirty light+mesh, fire `on_break`/`on_place`) →
`InterestManagementSystem` (per-player visible chunk/entity sets) →
`ReplicationSystem` (build per-player snapshots) → `ScriptPostTickSystem`
(deferred Lua actions, spawn/despawn commits) → `ChunkLifecycleSystem`
(load/generate/unload chunks around players).

**Client-side ECS** is a lightweight mirror: `NetId`, `Position`
(current+previous for interp), `Rotation`, `EntityKind`, `RenderHandle`
(`SpriteVisual` def + per-entity anim state). The local player additionally
holds `PredictedState` + an unacked `InputCmd` history for reconciliation.

---

## 7. Networking

**Layering:** application messages (versioned structs) → channel router
(reliable/unreliable/chunk-stream/asset-stream) → GameNetworkingSockets
(`ISteamNetworkingSockets`) → UDP. `librg` sits beside this (interest/
visibility + create/update/destroy events), not under it — envelope/routing
is ours, librg's serialization is used only for entity component blobs.

**Lanes:** 0 `control` (reliable ordered — handshake/auth/registry/chat/RPC),
1 `world` (reliable ordered — chunk add/update/remove, block edits, light),
2 `snapshot` (unreliable seq-gated — entity snapshots, reconciliation),
3 `assets` (reliable ordered — manifest + file transfer), 4 `input`
(unreliable seq — client→server `InputCmd` batches).

**Handshake order:** `Connect → C2S_Hello → S2C_ServerInfo → C2S_Auth →
S2C_AuthResult → C2S_AssetManifestRequest → S2C_AssetManifest →
C2S_AssetRequest → S2C_AssetData×N → C2S_Ready → S2C_BlockRegistry →
S2C_JoinAccept → S2C_ChunkAdd×N + S2C_EntitySnapshot(full) → normal play`.

**Snapshot model:** every tick, `ReplicationSystem` emits
`S2C_EntitySnapshot` on lane 2 (`server_tick`, `last_acked_input_seq`,
`{net_id, kind?, pos, rot, vel, flags, script_delta?}[]`) — unreliable, but
spawn/despawn records re-send until acked. Client keeps the two most recent
and renders at `now - interpolation_delay` (default 100 ms). Local player:
server includes authoritative state + last-processed input seq; client
discards acked inputs, snaps, replays unacked inputs through shared
movement/collision code (prediction with reconciliation).

**World replication:** `S2C_ChunkAdd` (palette+LZ4, ~0.5-3 KB),
`S2C_ChunkDelta` (small voxel-count changes), `S2C_ChunkRemove`.
`C2S_BlockEdit{predicted_seq, pos, action, block_id?, face}` — client may
optimistically apply locally; server validates (reach, tool, Lua veto,
protection) and replies `S2C_BlockEditResult` + authoritative
`S2C_ChunkDelta`; client rolls back on rejection. For a `max_damage > 0`
block this is only the *completion* of the shared-damage flow — see the
scripting doc's block-damage section.

Full handshake ASCII diagram + snapshot/world-replication/time-sync detail:
`architecture_spec/networking.md`.

---

## 8. Asset Sync Protocol

Goal: after connecting, the client has byte-identical copies of every file
the server's content pack needs, without re-downloading what it already
cached (from this or another server).

Server hashes every pack file at startup (xxHash3-128) into a manifest
(itself hashed, so a reconnecting client can skip the whole exchange with
one comparison). Client keeps a content-addressed cache
(`~/.cache/voxel_browser/assets/<hh>/<hash>`, hash → `{size, last_used}`
index, LRU eviction) shared across all servers. Transfer: client requests
missing hashes, server streams `S2C_AssetData` on lane 3 (~48 KB chunks,
bytes-in-flight window); client verifies each file's hash before committing
to cache, aborts the connection on mismatch. Security: server rejects `..`/
absolute/symlink-escaping paths; client only ever writes into the CAS, never
to a manifest-supplied path; per-file and total size caps reject hostile
servers; script assets execute only inside the §9 sandbox.

Full manifest struct + transfer/security detail: `architecture_spec/asset-sync.md`.

---

## 9. Scripting (Lua)

**Runtime:** Lua 5.4 + sol2, one VM server-side for the content pack
(authoritative), one **restricted** VM client-side for `ui/*.lua` callbacks
only (no world/fs/os/io/network access — just a `ui` table + read-only
client state).

**Sandbox** (both VMs): `os.execute/exit/getenv`, `io.*`, `load`/
`loadstring`, `debug.*` (except `traceback`), raw `package` removed;
`require` reimplemented to resolve only the pack's virtual (synced)
filesystem; instruction-count hook + per-callback wall-clock budget abort
runaway scripts; ceiling allocator turns OOM into a Lua error, not a crash.

**Registration API (pack-load time):** `vb.register_block/item/entity/
biome/craft/keybind`, `vb.worldgen.set_pipeline`. An entity **kind is a
class**: each `vb.world.spawn(kind, pos)` call creates an independent
**object** — its `ScriptState` holds a per-instance table (`self`) passed to
every `on_spawn`/`on_tick`/`on_hit`/`on_death` callback.

**Runtime API:** `vb.world.{get_block,set_block,raycast,spawn}`;
`entity:{get_pos,set_velocity,remove,get_inventory}`;
`player:{send_message,open_ui,give,take,get_name}`; event bus
`vb.on("player_join"|"player_leave"|"block_break"|"block_place"|
"player_interact"|"chat"|"tick"|"player_input"|"block_break_begin"|
"block_break_tick"|"block_health_tick", handler)` (return `false` to veto;
`player_input` may return a replacement input table; damage-tick handlers
return numbers); `vb.after`/`vb.every` scheduling; `vb.storage` (pack-global
persisted table) vs. `vb.db.get/set/delete(key)` (generic per-key store, no
built-in `vb.db` login concept; engine-verified external auth is the
separate, planned pack-level `auth.lua` — `architecture_spec/auth.md`).

**Client UI VM:** `ui.define(name, render_fn)` — `render_fn(state)` runs
**every UI frame** the screen is open and returns the widget tree (raygui
immediate-mode, no virtual-DOM diff needed); `on_click`/`on_change`/
`on_close` round-trip through `C2S_UiEvent` to the server VM.

**Custom input channel:** `vb.register_keybind(name)` builds a frozen
registry synced at handshake; the wire only ever carries a bounded bitset
indexed by registration order (closed schema = the flood defense, not a
post-receipt filter). Fires `vb.on("player_input", ...)` before movement
integration.

**Shared block-damage breaking** (`max_damage > 0` blocks): a shared damage
pool, not instant break — multiple players contribute concurrently, the
engine ships **zero built-in accrual/heal policy** (entirely `block_break_tick`/
`block_health_tick` handlers' call), and renders one baseline generic crack
overlay by default (override via `crack_texture`, landed 2026-09-27). Full
state-machine detail: see the scripting doc.

Full API tables, the block-break-event-flow walkthrough, and the complete
block-damage design: `architecture_spec/scripting.md`.

**Implemented (Phase 10, 2026-10-06):** `sdk/lua/` LuaCATS stubs are the source
of truth for the Lua API surface (`lua_api_surface_test` checks them against the
live tables); `docs/lua-reference/` is generated from them; `vb pack
init|check|dev|types|info` scaffolds and validates packs (`voxel_browser_server
--check-pack`); `pack.toml`'s `engine_version_req` is enforced by the server,
singleplayer and the client (`S2C_ServerInfo.engine_version_req`, protocol 30) —
`architecture_spec/dev-experience.md`.

---

## 10. Rendering (Client)

**Stack:** raylib owns window/GL/input/2D-UI; a **permanent** hand-rolled
face-culled mesher (`vb::world::chunk_mesher`/`chunk_mesh_snapshot`) owns
voxel meshing (Cellulose/greedy-merge was tried and reverted — see
`architecture_spec/open-questions.md` Q2); raygui draws menus/HUD.

**Client loop:** `vb::client::ClientApp` (`src/client/client_app.*`) owns the
menu → connecting → loading → playing state machine; `frame(InputFrame, dt)` is
one iteration. Windowed play and `--headless` run the same class — headless is
`render=false` (no menu, GL resources or draw calls).

**Chunk meshing:** `ClientChunkStore` mirrors replicated data; a mesh
worker-thread pool builds per-face-culled buffers with baked light/AO from
`(registry, block data, light volume, neighbor faces)`; GPU upload is
main-thread-only, budgeted per frame; frustum + distance culling; transparent
blocks in a back-to-front second pass.

**Entity rendering — billboard sprites (Q7, decided 2026-09-11):** players and Lua entity kinds
are Don't-Starve-style flat billboards (`DrawBillboardPro`), not 3D models; per-kind
`vb.register_entity{visual = {...}}` with a fixed animation priority. Full text, priority
order and out-of-scope list: `architecture_spec/rendering.md` "§10 text moved from
ARCHITECTURE_SPEC.md (2026-10-31)".

**Frame loop:** poll input → sample+push+send `InputCmd` → predict local
player → recv network → apply snapshots/reconcile/chunk-deltas → interpolate
remote entities → update dirty meshes (bounded) → render (sky → opaque
chunks → billboards → transparent chunks → particles → raygui HUD → Lua UI →
debug overlay) → present. Target 60 FPS, decoupled from the 20 Hz tick.

Full billboard/spritesheet design (frame-size variant table, per-kind
`visual` schema, per-instance override mechanics): `architecture_spec/rendering.md`.

**Menu screens (Phase 12, implemented 2026-10-31):** search-engine-style home (title,
address bar + Continue, 3 recent servers that open Settings › History,
Settings bottom-left) over a tiled dirt backdrop on every non-gameplay
screen; design in `architecture_spec/rendering.md` §11.6.

---

## 11. Physics

Broadphase: entity AABB expanded by motion ∩ voxel grid → candidate solid
cells. Narrowphase: swept AABB vs. axis-aligned voxel faces, resolved per
axis for clean wall-slide/ground behavior; player step-up height 0.6 m;
gravity from config. **Identical routine on server (`VoxelCollisionSystem`)
and client (prediction)** — lives in `vb_core`, takes a `BlockSolidQuery`
interface so both sides feed their own chunk store. No entity-entity physics
in the first playable base beyond optional simple player push-out.

---

## 12. Threading Model

**Server:** main/tick thread (ECS systems, Lua VM — touched only here,
block edits, replication build); network I/O thread (GNS poll, (de)serialize,
per-connection send queues); worldgen pool, N threads (pure, deterministic
chunk gen + initial lighting); asset-stream thread (pack file read/chunk/hash
at startup). Generated chunks + light results reach the tick thread via
lock-free queues.

**Client:** main/render thread (input, prediction, GL, raygui, mesh upload,
client Lua VM); network I/O thread (GNS poll, decode, apply to
double-buffered state); mesh pool, N threads (hand-rolled face-culled
meshing from chunk snapshots); asset-writer thread (verify hash + write
cache files).

---

## 13. Serialization & Protocol Versioning

Hand-written little-endian codecs in `vb/protocol/` (one read/write per
struct + varint helpers); no RTTI/exceptions on the hot path — decode
returns `Result<T, ProtocolError>`. Envelope: `{uint16 type, uint16 flags,
uint32 payload_len}`. **`ENGINE_PROTOCOL_VERSION` bumps on every wire
change**; handshake rejects mismatches with a human-readable reason.
Content-pack compatibility is separate — `pack_version` is informational,
the block registry is always sent in full so old clients can't desync on
content. `docs/protocol.md` is the normative reference; **update it in the
same commit as any message change.**

---

## 14. Configuration

**Server** (`server.toml`, CLI overrides): `bind_address`, `port`,
`content_pack`, `max_players`, `view_distance` (chunks), `tick_rate`,
`world_seed` (0 = random), `gravity`, `asset_max_file_mb`,
`asset_max_total_mb`.

**Client** (`client.toml`, local only, never synced): window size, vsync,
FOV, render distance (clamped to server), mouse sensitivity, keybindings,
asset cache size cap, default player name, server history (newest
first; the menu shows 3, Settings › History all).

Installing, versioning and hosting through the user-scoped `vb` developer
CLI (install layout under the per-user data/config dirs, server instances,
release manifest format): `architecture_spec/dev-cli.md`.

---

## 15. Content Pack Format (`content/base`)

`pack.toml` (name/version/engine_version_req/entry) + `init.lua` +
root-level `*.lua` modules (e.g. `crafting.lua`) + `blocks/*.lua` +
`entities/*.lua` + `ui/*.lua` + `textures/*.png` (+ `textures/entities/*.png`
billboard atlases). The host (`vb::script::load_content_pack`) still walks
`blocks/entities/biomes`/other root `.lua`/`init.lua` itself in that fixed
order, loading each into one shared Lua state — that's still the only way
`vb.register_block` et al. actually run. A real, sandboxed `require` (landed
2026-09-27) is layered on top for optional shared helper modules: every
`.lua` file under the pack root (except `ui/*.lua`, which runs in its own
restricted `UiRuntime` VM) is installed as a requirable module keyed by its
pack-relative path, so `require("lib.util")` resolves `lib/util.lua` against
that in-memory map only — never the real filesystem — with stock Lua's
package.loaded-style caching and cycle detection
(`vb::script::Vm::install_require`/`require_module`, `src/script/vm.cpp`).
The base pack is both the reference Lua-API implementation and the CI
smoke-test content.

Full loader-order rationale: `architecture_spec/content-pack-format.md`.

---

## 16. Security Considerations

| Surface | Threat | Mitigation |
| --- | --- | --- |
| Asset manifest paths | path traversal, symlink escape | normalize + reject `..`/abs/symlink; client writes only to CAS |
| Asset sizes | disk-fill DoS | per-file + total caps; progress abort |
| Lua (server pack) | host compromise via `os`/`io` | sandbox env, reimplemented `require`, no bytecode load |
| Lua (client UI) | malicious server script on client | separate restricted VM, no world/net/fs, instruction + time budget |
| Packet decode | malformed input crash | bounds-checked `Result` codecs, fuzz targets, size caps |
| Block edits | reach hacks, protected-area grief | server reach check, tool check, Lua veto, per-region protection API |
| Input flood | CPU DoS | per-connection input rate limit, `InputCmd` count clamp per tick |
| Custom keybind flood | wire bandwidth / dispatch DoS | closed schema (bounded bitset, §9), not a post-receipt filter; per-connection rate limit on top |
| Connection flood | resource exhaustion | GNS connection limits, handshake timeout, per-IP cap |
| Pack-implemented auth | weak/pure-Lua credential hashing | engine offers `vb.crypto.hash` so packs aren't rolling their own; engine itself takes no position on auth |
| Mandatory auth (`auth.lua`, Phase 9, implemented; operator guide `docs/auth.md`) | forged/replayed ID tokens, token forwarding by a hostile server | server-side JWS verify (RS256/ES256 allowlist), iss/aud/exp checks, per-connection nonce, freshness bound, periodic live re-auth for revocation, per-IP sign-in rate limit, fail closed; full server binding out of scope — `architecture_spec/auth.md` §8 |
| Block-break begin/stop spam | CPU DoS via many concurrent damage-pool entries | same reach/tool/protection gate as `C2S_BlockEdit`; sparse map bounded by actual contributors, not attacker-controlled |

The engine assumes a **trusted server operator** but an **untrusted network
and untrusted clients**. Client sandboxing protects players from malicious
servers to the extent practical (no code exec, no arbitrary FS writes).

---

## 17. Testing Strategy

- **Unit** (`tests/unit/`): palette chunk store round-trips, protocol codec
  round-trips + fuzz, voxel collision cases, worldgen determinism (same seed
  → same chunk hash), asset hashing, Lua sandbox escape attempts.
- **Integration** (`tests/integration/`): headless server + headless client
  in one process — join handshake, asset sync of the base pack, chunk
  streaming, a scripted block break propagating to a second client,
  prediction reconciliation converging.
- **Determinism gate in CI**: generate a fixed region, hash it, compare
  against a committed golden value across Linux/macOS/Windows.
- **Soak**: N simulated clients doing random walks + edits for M minutes,
  watch for leaks (ASan/LSan) and unbounded queue growth.
- **End-to-end (dev-only)**: real `voxel_browser` and
  `voxel_browser_server` processes driven over a stdio JSON-lines channel by
  a pytest harness (`tests/e2e/`: Playwright-style locators and auto-waiting;
  `ctest -L e2e`; its own Linux CI job; a loopback+token TCP attach mode and a session recorder
  that writes a vbtest script). Compiled in only with
  `VB_WITH_AUTOMATION=ON` (default `OFF`, rejected in `VB_DISTRIBUTION`
  builds), so shipped binaries never contain it. Design:
  `docs/e2e-automation.md`; usage: `tests/e2e/README.md`.
- Frameworks: doctest/Catch2 for unit; a small custom harness for
  integration. Sanitizer builds wired into the debug CI matrix.

---

## 18. Open Questions

| # | Question | Status |
| --- | --- | --- |
| 1 | Binding layer: raw Lua C API vs. sol2 | **Resolved 2026-09-10: sol2** |
| 2 | Cellulose (greedy mesher) API fit | **Resolved → reversed → removed.** Hand-rolled meshing is permanent (NVIDIA VAO/VBO-churn bug reproduced more under Cellulose's volatile vertex counts) |
| 3 | librg version/API | **Resolved 2026-09-10: v7.4.0**, interest culling + create/update/remove framing only; own payload codec/routing. Not yet wired — Phase 1 hand-rolled `InterestGrid` stands in |
| 4 | Chunk compression: LZ4 vs. zstd vs. palette-only | **Resolved 2026-09-28: LZ4, above a size threshold, only when it helps.** Measured: real worldgen terrain (smooth heightmap, no caves) RLE's down to ~12-19 bytes on its own — LZ4 on top only adds its 4-byte prefix + block overhead there, so it's skipped below a 128-byte threshold; a heavily-edited "swiss cheese" chunk (no long runs left for RLE to find) shrinks ~36% further under LZ4 (50832 → 32393 bytes in the measured case). See `architecture_spec/open-questions.md` for the full writeup |
| 5 | Persistence: region file format | **Resolved 2026-09-25: flat files, not LMDB** (reversing the 2026-09-17 direction note after implementation review) — `vb::world::RegionStore` groups chunks into one file per X/Z region (Y ungrouped), reusing the existing palette+RLE chunk codec as-is, no new FetchContent dependency. Only edited chunks (`Chunk::revision() > 0`) are ever written. See `REMAINING_TASKS.md`'s Phase 7.6 for the full writeup. LZ4 framing of region-file payloads (format version 1 -> 2) and `--singleplayer`'s integrated server wiring to a real `RegionStore` both resolved 2026-09-28 — no open sub-item left |
| 6 | Account/auth: `auth_mode = none \| token` | **Direction revised 2026-10-05:** a pack-level `auth.lua` declares an external identity provider (OIDC/Keycloak/Firebase); its presence makes engine-verified auth mandatory before world load, and scripts read `player:get_login()` (`nil` = no auth). Planned as Phase 9 — `architecture_spec/auth.md`. The 2026-09-17 note (engine owns no auth; packs build login on `vb.db`) is superseded for mandatory auth; `vb.db` logins still work as pack-level identity |
| 7 | Entity visual presentation: 3D models vs. 2D sprites | **Resolved 2026-09-11: Don't Starve-style billboards** — full design in §10 above |
| 8 | In-process server: product mode ("singleplayer") vs. diagnostic | **Diagnostic (Phase 11, 2026-10-27).** Removed from the menu, flag renamed `--loopback` (`--singleplayer` alias) — the product is multiplayer-first and `vb pack dev` already hosts a real server. Plan: `REMAINING_TASKS.md` Phase 11 |

Full resolution write-ups and reasoning for every row:
`architecture_spec/open-questions.md`.
