## Phase 4 — The "Browser" Engine (Scripting & Assets)

> Full history for this phase; linked from `REMAINING_TASKS.md`. Ground truth for [x] items — do not duplicate here.

Goal: server logic + content defined in Lua; client auto-downloads pack assets;
Lua-defined UI.

### 4.1 Lua runtime (`vb_core/script`)  🚧

- [x] Embed Lua 5.4 (`cmake/lua` wrapper) + sol2 **v3.5.0** (bumped from 3.3.0 —
      Clang ≥ 18 incompat). `VB_WITH_LUA` on in all three CI build workflows.
- [x] `vb::script::Vm` — pImpl over `sol::state`; `do_string` (source only,
      traceback capture), `ScriptResult` / `core::ScriptError`. Stub build when
      `VB_WITH_LUA` is off.
- [x] Sandbox env (§10.2): curated libs, `os`/`io`/`load`/`require`/`package`/
      `collectgarbage` nilled, `debug` → `traceback` only, instruction-count
      hook, ceiling allocator.
- [x] Sandbox-escape tests (`tests/unit/script_test.cpp`): os/io/load absent,
      runaway loop → `kBudgetExceeded`, memory bomb → `kOutOfMemory` (recoverable).
- [x] Custom `require` over the pack's own virtual module filesystem +
      per-callback wall-clock budget — landed 2026-09-27. Reframed from the
      original plan: rather than reusing `ClientAssetCache`'s synced-FS map
      (4.4), which only ever exists on a *remote client*, never on the
      server/`PackRuntime` side that actually needs `require`, the virtual
      FS here is a small in-memory `path -> source text` map that
      `vb::script::load_content_pack` (`src/script/pack_loader.cpp`) builds
      once from its own recursive directory walk (every `.lua` under the
      pack root except `ui/*.lua`, which runs in its own restricted
      `UiRuntime` VM) and installs via the new `PackRuntime::
      set_pack_modules()` -> `Vm::install_require()`. `Vm` (`inc/vb/script/
      vm.hpp`, `src/script/vm.cpp`) reinstates a safe `require` global right
      after `strip_sandbox()` nils the stock one: a dotted or slash-separated
      module name resolves only against that map (`.` -> `/`, `.lua`
      appended if missing), rejects a name containing `..` or a leading `/`,
      caches a module's return value across repeated `require()` calls the
      same way stock Lua's `package.loaded` does (no explicit `return`
      caches as `true`), and detects a require cycle via an in-progress name
      stack rather than deadlocking or stack-overflowing. Wall-clock budget:
      `VmLimits` gains `wall_clock_budget_ms` (default 250); the existing
      instruction-count hook (`count_hook`) now fires far more often
      (`kHookPeriod` = 1000 instructions, independent of the caller's own,
      possibly huge, `instruction_budget`) and checks a `std::chrono::
      steady_clock` deadline first, so a callback with few but individually
      slow instructions still gets cut off in real time, not just by VM
      instruction count — both failure modes still classify to the existing
      `core::ScriptError::kBudgetExceeded` (no new enum value; the two
      distinct internal marker strings, `vb:instruction-budget-exceeded` and
      `vb:wall-clock-budget-exceeded`, are what `classify()` keys off of).
      `AllocState` (`inc/vb/script/vm_internal.hpp`) now carries the
      hook's own running state (`instructions_run`, `deadline`,
      `time_boxed`) since it's the one piece of state already reachable from
      inside the hook via `lua_getallocf`. `Vm::sandbox_intact()`'s old
      `absent("require")` assertion is inverted to `require` being present
      and a function (`sol::type::function`) — everything else it checks
      is unchanged. Existing packs are unaffected (`content/base`/
      `kitchen_sink` never call `require`); this is additive only. Verified:
      full `vb_tests` 381/381 green (10 new cases: 8 `script_test.cpp`
      `Vm`-level cases covering resolve/dotted-path/caching/no-return-
      defaults-true/not-found/path-traversal/circular-dependency/
      hot-swap-drops-cache, 1 wall-clock-vs-huge-instruction-budget case,
      and 1 `content_pack_test.cpp` end-to-end case driving a real
      `require('lib.util')` through `load_content_pack` and confirming the
      result reaches `vb.storage`), clean `-Werror` build of `vb_tests`/
      `voxel_browser`/`voxel_browser_server`.
- [ ] Decision #1 (sol2) — recorded; **done**.

### 4.2 Server Lua API (§10.3)  🚧

- [x] `vb::script::PackRuntime` (`inc/vb/script/pack_runtime.hpp` +
      `src/script/pack_runtime.cpp`) — pImpl'd like `Vm`, stub build when
      `VB_WITH_LUA` is off. `Vm` gained `native_impl()` + a new internal
      `vm_internal.hpp` (shared `Vm::Impl`/`sol::state`) so binding code can
      reach the real Lua state without putting sol2 in any public header.
- [x] Registration API: `register_block` (idempotent by name via new
      `BlockRegistry::add_or_get`, rejected after `freeze()`), `register_item`,
      `register_entity` (captures `on_spawn`/`on_tick`/`on_hit`/`on_death` —
      **nothing dispatches them yet**, waits on 3.1's EnTT registry),
      `register_biome`/`register_craft` (captured; the engine itself still
      has no consumer, but as of 5.1 `content/base/crafting.lua` builds a
      real, working crafting feature on top of `register_craft` + the
      generic `player:give()`/`take()` primitives — see 5.1 below).
      `worldgen.set_pipeline` **not implemented** (out of scope — worldgen
      pipeline swap deferred, see below).
- [x] Freeze registries after pack load (`PackRuntime::freeze()`); stable
      `BlockId`s via `add_or_get`. **Known gap:** the server registry starts
      from `BlockRegistry::base()` and Lua-added ids beyond it are invisible
      to any client until 4.3 (`S2C_BlockRegistry`) ships — logged as a
      warning, not enforced.
- [x] Runtime world API: `get_block`/`set_block`/`raycast`/`spawn`.
      `raycast` shares one new `vb::world::raycast_voxel()` (extracted from
      `src/client/main.cpp`'s block-selection code, `inc/vb/world/raycast.hpp`)
      with the client. `spawn` logs + returns `nil` (no entity system to spawn
      into). `set_block` **known gap:** doesn't run the relight cascade
      `C2S_BlockEdit` does.
- [x] Entity + player API: one merged Lua usertype (`entity:`/`player:` are
      the same object — no non-player entity exists to justify two types)
      exposing `get_pos`/`set_velocity`/`remove` (no-op, logged)/
      `get_inventory`/`give`/`send_message`/`open_ui`/`get_name`. `send_message`
      / `open_ui` are real wire messages now — see 4.2's protocol addition
      below; no client handles them yet (4.5).
      `ServerSession` gained `set_player_velocity`/`conn_for_player`/
      `player_name` (+ a `name` field on its `Conn`, + `net_id` on
      `SessionPlayerLeft`) to support this.
- [x] Event bus: `vb.on("player_join"|"player_leave"|"block_break"|
      "block_place"|"player_interact"|"chat"|"tick", handler)` with veto
      returns. `player_join` wraps `HandshakeServerHost::authenticate` (a real
      pre-join veto, `install_join_veto`); `block_break`/`block_place` +
      per-block `on_break`/`on_place` fire from a new `WorldReplicator::
      BlockEditHooks` seam at `apply_block_edit` (closes the seam this doc
      called out under 5.2) — `on_break`/`on_place`'s return value (a
      would-be drop) is logged only, not materialized (waits on 5.1 items).
      `chat`/`player_interact` are wired generically
      (`dispatch_chat`/`dispatch_player_interact`) but nothing calls them yet
      — no `C2S_Chat`/interact message exists.
- [x] Scheduling: `vb.after`, `vb.every` (repeating, catches up on a stalled
      tick, capped at 8 fires/dispatch).
- [x] `vb.storage` — metatable-backed proxy over an `nlohmann::json` object
      (new dependency, header-only), persisted to `<content_pack>/storage.json`,
      flushed once per tick when dirty.
- [x] Protocol: `S2C_Chat` (101) + `S2C_OpenUi` (103) now have real codecs
      (`inc/vb/protocol/chat.hpp`) — `kEngineProtocolVersion` bumped 3 → 4,
      `docs/protocol.md` updated. Round-trip tested.
- [x] Unit tests (`tests/unit/pack_runtime_test.cpp`) + integration tests over
      a real `ServerSession`/`ClientSession`/`LoopbackTransport`
      (`tests/unit/pack_runtime_integration_test.cpp`): block-edit veto +
      `on_break` end-to-end, `player_join` veto end-to-end, `player_leave`
      dispatch with the right net id.
- [x] `EntityKind` tick/spawn/hit/death callbacks wired into real systems —
      landed 2026-09-27, now that `SystemRunner` itself exists (Phase 3.1,
      2026-09-25). `PackRuntime::dispatch_tick` (the `tick` event + entity
      `on_tick`/`on_hit`/`on_death` dispatch + global timers) used to be a
      separate call each embedder's own loop made *after* `ServerSession::
      tick()` had already run every phase for that tick (`server/main.cpp`,
      `client/main.cpp`'s `Singleplayer::tick()`) — a script-driven entity
      move only reached `sync_interest`/`broadcast_snapshots` one tick late.
      New `ServerSession::set_script_tick_hook(fn(double))` (`inc/vb/net/
      session.hpp`) is a new `"script_tick"` `SystemRunner` phase, placed
      right after `check_respawns` and before `update_item_drops`/
      `sync_interest`/`broadcast_snapshots` — same "`ServerSession` has no
      idea this is Lua-backed" decoupling every other hook here already uses
      (`set_landed_hook` et al.), not a direct `PackRuntime` reference.
      `PackRuntime::attach_session()` installs it unconditionally (unlike the
      conditional hooks above it — `dispatch_tick` does real work, global
      timers included, even for a pack with no entity-kind callbacks at
      all). Both embedders' loops no longer call `pack_runtime.dispatch_tick()`
      directly. Verified: full `vb_tests` 381/381 green (updated the ~19
      existing `attach_session`-using pump loops in `pack_runtime_integration_
      test.cpp`/`content_pack_test.cpp`/`kitchen_sink_pack_test.cpp` to drop
      their now-redundant explicit `rt.dispatch_tick()` call — leaving it in
      would have double-fired every entity tick/timer per pump step), clean
      `-Werror` build of `vb_tests`/`voxel_browser`/`voxel_browser_server`.
- [x] Lua-driven worldgen pipeline replaces the Phase 2 hardcoded one —
      moved to Phase 6.14 (extensibility push, FastNoise2 backend); tracked
      there now instead of here. Landed 2026-09-18 — see that item.
- [x] `register_entity`'s `visual = {...}` sub-table (variant/facings/clips)
      for 3.5's `entity_renderer` — landed 2026-09-25. Protocol version bumped
      **19 -> 20**: `EntityKindRegistryRecord` gains an optional `visual`
      field (`protocol::EntityVisualDef` — texture path, resolved
      frame_width/height, facings, origin, a `clips` list of
      `{clip, frames, fps}`), absent for a kind that never sets one.
      `PackRuntime`'s `vb["register_entity"]` binding parses and
      shape-validates `visual = {...}` at registration (unknown variant,
      missing texture, `facings` not 4/8, out-of-range `origin`, an empty
      `clips` array, or a non-positive `frames`/`fps` all throw a
      `sol::error`) — no image decoding happens on this, headless,
      pack-runtime side. Client-side, a new pure header
      `vb/render/entity_visual_layout.hpp` (`build_entity_visual_layout`)
      computes the running per-clip column layout and validates it against
      the *real* decoded PNG's pixel dimensions once
      `EntityRenderer::set_kind_visual()` decodes the synced/on-disk texture
      — a mismatch there logs `VB_WARN` and that kind keeps the flat
      placeholder billboard rather than failing pack load.
      `EntityRenderer::draw()` now picks its billboard's source rect from the
      resolved pose row and animation clip (new `render::anim_clip_name`
      bridges the existing `resolve_anim_clip()` enum to a pack's clip
      names) and frame index (`clip_time * fps`, wrapped via modulo). A kind
      with no `kind_visuals_` entry (players, or any kind that never set
      `visual`) renders exactly as before this landed. **Deliberately out of
      scope, kept for a follow-up:** per-instance `ScriptState.visual_override`
      (skins) and real base-pack art — this pass proved the mechanism with
      synthetically-generated-at-test-time PNGs, not new binary art checked
      into `content/`. Verified: full `vb_tests` 331/331 green (12 new cases:
      protocol round-trip + too-many-clips-cap, `pack_runtime_test.cpp`
      rejection-path + valid-visual-reaches-the-registry cases,
      `entity_visual_layout_test.cpp` pure layout math, `anim_clip_name`
      coverage), clean `-Werror` build. The actual rendered sprite/animation
      was **not** manually eyeballed — no GUI in this agent environment.
- [x] Per-instance `ScriptState.visual_override` (skins) -- landed 2026-09-25.
      `vb.world.spawn(kind, pos, { visual_override = {...} })` takes a third,
      optional options table whose `visual_override` is the same shape as
      `register_entity`'s `visual` but with every field independently
      optional -- an omitted field inherits the kind's own `visual` unchanged
      (`render::merge_visual_override`, `inc/vb/render/
      entity_visual_layout.hpp`), so a pack can override just `texture` (a
      player-skin variant) while keeping the kind's `facings`/`clips`/
      `origin`. Protocol bumped **20 -> 21**: `EntityRecord` (within
      `S2C_EntitySnapshot`) gains an optional `visual_override`, populated
      only on the one `entered` record a client gets when a NetId first
      enters their interest set -- `updated`/`local` records never carry it,
      and `ClientSession` caches whatever it first learned for the entity's
      whole replicated lifetime, same "learned once, immutable" posture as
      `EntityRecord.kind` itself. Client-side, `EntityRenderer::sync()`
      lazily decodes an override's texture the first time it sees a given
      NetId's override, cached in a new `instance_visuals` map that takes
      priority over `kind_visuals` in `draw()`. **Deliberately out of scope,
      left as a real follow-up:** `entity:set_visual_override()`/a
      live-update or clear path -- the override is fixed at spawn time only.
      Verified: full `vb_tests` 340/340 green (protocol round-trip incl. an
      `updated` record never carrying an override, `merge_visual_override`
      empty/texture-only/full-replace cases, 2 `pack_runtime_integration_
      test.cpp` cases -- override reaches a real client, malformed override
      rejects the whole spawn), clean `-Werror` build. The actual rendered
      skin swap was **not** manually eyeballed -- no GUI in this agent
      environment.
- [x] Real base-pack art for `base:player`/`base:dropped_item` -- landed
      2026-09-25. Neither ever went through the `vb.register_entity` kind
      mechanism (players hardcoded `EntityKindId::kInvalid` at join, drops
      carried the reserved `world::kItemDropKind` sentinel) -- fixed with a
      new generic `vb.register_entity{represents = "player" | "item_drop"}`
      field, not a hardcoded pack-name check in engine code.
      `content/base/entities/player.lua` (new) and `dropped_item.lua`
      (edited) register real, checked-in `visual = {...}` spritesheets
      (idle+walk, 4 facings) generated with a throwaway stdlib PNG writer (no
      art tools in this environment). Full `vb_tests` 334/334 green, clean
      `-Werror` build.
      **Follow-up (2026-09-27), user-requested:** the real art's front/side/
      back poses were too visually similar to tell apart by eye. `base:player`
      swapped to debug-styled art (a big F/R/B/L letter baked into each row)
      and a new `vb.register_entity{visual = {mirror = false}}` engine
      feature (default `true`) lets a kind author a real, distinct pose per
      facing instead of mirroring one "side" pose for both left and right —
      `kEngineProtocolVersion` 22 -> 23. Also fixed a real pre-existing bug
      this surfaced: `EntityRenderer` picked a facings=4 kind's pose using
      hardcoded facings=8 sector math the whole time. See `STATE.md`'s
      2026-09-27 entry for the full write-up.

### 4.3 Block registry replication  ✅ (name/solid/opaque/liquid/light only)

- [x] `S2C_BlockRegistry` (40) message (`inc/vb/protocol/world.hpp` +
      `src/protocol/world.cpp`): `varint n` + `n × {name, solid, opaque,
      liquid, light_emission}`, index == `BlockId`. Round-trip tested.
      `kEngineProtocolVersion` bumped 4 → 5. **No model/face-texture/
      collision-shape fields** — `BlockType` doesn't have them yet (waits on
      4.4 asset sync + 5.1 base pack); this phase only replicates what
      already exists.
- [x] Sent between `C2S_Ready` and `S2C_JoinAccept` (spec §8.3 order) via a
      new opt-in `HandshakeServerHost::block_registry` hook
      (`std::optional<vector<BlockRegistryRecord>>`, default `nullopt` = no
      frame sent at all) — every pre-4.3 host/test that never heard of this
      keeps identical wire behavior. `src/server/main.cpp` wires the
      dedicated server's live (possibly Lua-extended) registry into it,
      closing the exact gap `PackRuntime::freeze()` warns about (4.2).
- [x] Client (`ClientSession::tick`, `src/net/session.cpp`) intercepts
      `S2C_BlockRegistry` unconditionally (not through `ClientHandshake`'s
      strict per-state FSM) — robust to it arriving just before or after
      `JoinAccept`, since on real `GnsTransport` it travels on a different
      lane with no cross-lane ordering guarantee. Rebuilds a
      `world::BlockRegistry` in wire order and swaps it into
      `ClientChunkStore` via a new `set_registry()` setter.
- [x] Tests: `tests/unit/protocol_test.cpp` round-trip;
      `tests/unit/block_registry_test.cpp` (new) — a custom registry reaches
      a joined client end-to-end over `LoopbackTransport`, and a host that
      never opts in leaves the client on its own `base()` (regression guard
      for every other session-based test).
- [ ] Client meshing/atlas driven by the received registry beyond
      solid/opaque/light — still just untextured cubes (Phase 2 status);
      waits on a real texture/model concept (4.4/5.1).
- [ ] `src/client/main.cpp`'s singleplayer path doesn't wire this (no
      `PackRuntime` there yet, so its registry is always `base()` anyway —
      not a gap in practice, just unexercised).

### 4.4 Asset Sync Protocol (§9)  ✅ (progress accounting + UI deferred)

- [x] Server startup: walks the pack dir (`vb::assetsync::build_manifest`,
      `inc/vb/assetsync/manifest.hpp` + `src/assetsync/manifest.cpp`),
      rejects `..`/absolute paths/symlinks escaping the root, hashes every
      file (xxHash3-128, `XXH3_128bits`), builds `Manifest` + `manifest_hash`.
      Enforces `ServerConfig::asset_max_file_mb`/`asset_max_total_mb` as a
      hard startup failure, not a per-client thing. Gated behind
      `VB_WITH_COMPRESSION` (disabled build: `kDisabled`, whole feature
      no-ops gracefully like `VB_WITH_NET` off).
      **Dependency landmine fixed:** lz4 vendors its own private, older
      `xxhash.h` with no XXH3 API; `src/core/CMakeLists.txt` now links
      `xxHash::xxhash` *before* `LZ4::lz4` so `#include <xxhash.h>` resolves
      to the real one (see `docs/protocol.md`'s dependency table).
- [x] Messages: `C2S_AssetManifestRequest`, `S2C_AssetManifest`,
      `C2S_AssetRequest`, `S2C_AssetData` (chunked, lane 3) —
      `inc/vb/protocol/assetsync.hpp` + `src/protocol/assetsync.cpp`, round-trip
      tested. `kEngineProtocolVersion` bumped 5 → 6 (a structural handshake
      change, not just additive). Progress accounting (bytes-vs-manifest-total
      for a UI progress bar): not added — no connect-screen UI exists yet
      (waits on 5.3).
- [x] Client content-addressed cache (`vb::assetsync::ClientAssetCache`,
      `inc/vb/assetsync/cache.hpp` + `src/assetsync/cache.cpp`): disk layout
      `<cache_dir>/<2-hex-prefix>/<32-hex-hash>`, hand-rolled `index.bin`
      (write-to-.tmp-then-rename) mapping hash → `{size, last_used}` for LRU
      eviction to `ClientConfig::asset_cache_mb`. Default dir is a new
      `vb::core::user_cache_dir()` (`inc/vb/core/paths.hpp`, OS-appropriate:
      `%LOCALAPPDATA%`/`~/Library/Caches`/`$XDG_CACHE_HOME` or `~/.cache`),
      overridable via a new `ClientConfig::asset_cache_dir` +
      `--asset-cache-dir`.
- [x] Transfer state machine: three new `ServerHandshakeState` values
      (`kAwaitingAssetManifestRequest`/`kAwaitingAssetRequest`/
      `kStreamingAssets`) and two new `ClientHandshakeStatus` values
      (`kAwaitingAssetManifest`/`kSyncingAssets`) sit between `AuthResult`
      and `Ready` unconditionally now. `kStreamingAssets` is server-paced
      (`ServerHandshake::pump_assets()`, called once per tick from
      `ServerSession::tick()`'s existing non-playing-connection loop — a
      small per-tick chunk budget, not literal byte-in-flight windowing).
      Per-file hash verify + abort-on-mismatch/size-cap-breach lives in
      `ClientAssetCache::ingest_chunk` (spec §9.4, wired to `on_asset_chunk`
      returning `false` → `ClientHandshake::fail()`).
- [x] Virtual pack filesystem in memory (`ClientAssetCache::virtual_fs()`,
      exposed on `ClientSession` as `virtual_pack_fs()`) — assembled from
      cache reads + completed transfers. **Nothing consumes it yet**: Lua
      `require` (4.1), texture/model/UI loaders don't exist regardless of
      this landing.
- [x] Handshake ordering: `AssetManifestRequest → AssetManifest →
      AssetRequest → AssetData×N` sits between `AuthResult` and `Ready`
      (before `S2C_BlockRegistry`/`JoinAccept`, confirmed against the §8.3
      diagram) — not "before BlockRegistry/JoinAccept" loosely, exactly
      between Auth and Ready as spec'd.
- [x] Reconnect fast-path — **scoped down, confirmed with the user**: not
      persisted across process restarts. `C2S_AssetManifestRequest.
      known_manifest_hash` lets a *same-process* reconnect skip the manifest
      listing too, but a restarted client always gets the full listing —
      its on-disk CAS still makes `compute_missing()` return empty and the
      *data transfer* zero-byte regardless, which is what the integration
      test actually asserts. A literal cross-restart `manifest_hash` skip
      (persisting `HandshakeClientHost::last_known_manifest_hash`'s value to
      disk) is a cheap, unimplemented follow-up if wanted.
- [x] Integration test (`tests/unit/netcode_test.cpp`): a cold client
      downloads a real temp content pack and ends up byte-identical
      (`virtual_pack_fs()` vs. on-disk source); a second connection (fresh
      `ClientAssetCache` instance, same cache dir — simulating a process
      restart) asserts the server's `asset_file_bytes` hook is never called.
      Plus dedicated unit tests: `tests/unit/assetsync_manifest_test.cpp`
      (determinism, content-sensitivity, symlink-escape rejection, size
      caps) and `tests/unit/assetsync_cache_test.cpp` (missing-hash
      computation, hash-mismatch rejection leaves no stray file, LRU
      eviction under a tiny cap).
- [x] **Real bug found and fixed (2026-09-18):** the manifest is a
      startup-time snapshot (`src/server/main.cpp`'s `build_manifest` call,
      once), but `vb.storage`'s writes are deferred to the first
      `dispatch_tick()` (Phase 4.2's dirty-flag design) — which runs
      *after* the manifest is already built. Any pack that writes
      `vb.storage` during `load_content_pack` (both `content/base` and
      `content/examples/kitchen_sink` do, in their own `init.lua`'s
      boot-counter demo) hit a **guaranteed, first-connect asset-sync
      failure**: the manifest hashed `storage.json`'s stale pre-write bytes,
      then the first tick silently rewrote the file out from under that
      hash before any client's `asset_file_bytes` fetch, so verification
      failed with `"asset transfer failed (hash mismatch or size cap)"` —
      every time, on every machine, completely independent of build config
      (initially misdiagnosed as a `VB_WITH_COMPRESSION` mismatch across
      machines before being traced to this). Fixed with a single
      `pack_runtime.flush_storage()` call between `freeze()` and
      `build_manifest()`.
- [x] Manifest staleness: a pack that writes `vb.storage` *after* startup
      (not just at load time) used to go stale for the rest of that server
      process's life — landed 2026-09-28. New `PackRuntime::storage_revision()`
      (`inc/vb/script/pack_runtime.hpp`) is a monotonic counter bumped every
      time `Impl::flush_storage()` actually writes `storage.json` to disk —
      both the explicit startup call and `dispatch_tick()`'s own internal
      auto-flush when `storage_dirty()`. `src/server/main.cpp` polls it once
      a second (`manifest_check_ticks`, same "periodic backstop, not a hard
      real-time guarantee" shape `autosave_ticks` already has): on a change,
      it rebuilds the whole asset manifest (`vb::assetsync::build_manifest`)
      and atomically swaps it in via a new small `ManifestHolder`
      (mutex + `shared_ptr`) that `host.asset_manifest`/`host.asset_file_bytes`
      now read through by reference instead of each capturing a fixed
      `shared_ptr` by value at startup. Deliberately throttled rather than
      checked every tick: a pack that flushes `vb.storage` every tick would
      otherwise pay a full content-pack rescan+rehash at the tick rate.
      `--singleplayer` was out of scope — it never builds a manifest at all.
      Verified: full `vb_tests` 412/412 green (1 new `pack_runtime_test.cpp`
      case: `storage_revision()` bumps on an explicit `flush_storage()` and
      on `dispatch_tick()`'s implicit auto-flush, but *not* on a no-op tick
      with nothing dirty), clean `-Werror` build. The manifest-rebuild-on-a-
      live-server path itself was **not** exercised end-to-end —
      `src/server/main.cpp` isn't linked into `vb_tests`.

### 4.5 Client UI VM + raygui (§10.4)  ✅ (item grid + base pack deferred)

- [x] Separate restricted client VM: `vb::script::UiRuntime`
      (`inc/vb/script/ui_runtime.hpp` + `src/script/ui_runtime.cpp`) — a
      second `Vm` distinct from the server's `PackRuntime`, pImpl'd the same
      way, disabled-stub when `VB_WITH_LUA` is off. No world/net access of
      its own; outgoing events route through an attached `ClientSession&`.
- [x] `ui.define(name, render_fn)` declarative layout: `label`, `panel`,
      `button`, `list`, `text input` (spec's set minus **item grid**,
      deferred — needs real items, waits on 5.1). Originally evaluated once
      at `open()` and never re-laid-out afterward; superseded by Phase 6.2's
      per-frame `render(state)` reactive model below.
- [x] C++ renderer mapping layout → raygui: `vb::render::UiRenderer`
      (`inc/vb/render/ui_renderer.hpp` + `src/render/ui_renderer.cpp`),
      split across the core/render boundary from `UiRuntime` the same way
      `ClientChunkStore`/`ChunkRenderer` already are — `UiRuntime` emits a
      plain `Widget` list (no raygui), `UiRenderer` draws it (no sol2) and
      reports back which widgets fired an interaction this frame.
- [x] `player:open_ui(name, ctx)` server→client: `ClientSession` gained
      `take_open_ui()` (drains a pending `S2C_OpenUi`) + `send_ui_event()`.
      `on_click`/`on_change`/`on_close` → `ui.send_event(...)`/`ui.close()`
      → real `C2S_UiEvent{ui_name, widget_id, event_kind, value_json}`
      (`inc/vb/protocol/chat.hpp`, round-trip tested) →
      `ServerSession::set_ui_event_handler` (a plain `std::function`, no
      script dependency — same shape as `WorldReplicator::BlockEditHooks`)
      → `PackRuntime::dispatch_ui_event` → `vb.on("ui_event", handler)`
      (non-vetoable; spec gives no veto semantics for UI events).
      `kEngineProtocolVersion` bumped 6 → 7.
- [x] `src/client/main.cpp` wired end to end: `UiRuntime`/`UiRenderer`
      constructed unconditionally, drawn each frame between the 3D pass and
      `window.end_frame()`, mouse capture released while a UI is open.
      **No base pack (5.1) exists yet**, so nothing calls `ui.define` at
      real runtime today — same shipped-mechanism-ahead-of-content pattern
      as every prior 4.x phase.
- [x] Tests: `tests/unit/protocol_test.cpp` (`C2SUiEvent` round-trip),
      `tests/unit/ui_runtime_test.cpp` (layout evaluation, undefined-name
      no-op, `on_click`/`on_close` invocation), and a full end-to-end
      integration test in `tests/unit/pack_runtime_integration_test.cpp`:
      a server pack script's `block_break` handler calls `player:open_ui`
      (no earlier event hands a script a `PlayerHandle` for an online
      player), the client's `UiRuntime` opens it and a simulated button
      click round-trips to `vb.on("ui_event", ...)` on the server with the
      right `ui_name`/`widget_id`/`kind`/`value`.
- [ ] Base pack `ui/inventory.lua`, `ui/pause.lua` — waits on 5.1 (no
      content pack exists to put them in).
- [x] Item grid widget for `UiRuntime` — landed 2026-09-28. Not a baked-in
      "grid" concept: a new generic `icon` `WidgetType` draws one registered
      block/item id's real atlas texture at x/y/w/h (same atlas chunk
      meshes already use, via new `ChunkRenderer::has_atlas()`/
      `atlas_texture()`/`atlas_rect_for()` getters), and
      `content/base/ui/inventory.lua` composes a real item grid out of it
      plus `rect`/`text` (slot background/border, count label) — the same
      "compose it in Lua" posture Phase 6.16 already gave the hold-to-break
      progress bar out of `rect`. See `STATE.md` history (`state/changelog-*.md`) for the
      full writeup.
- [x] Base-pack reskin done 2026-10-17: every `content/base` block has a texture
      (`scripts/gen_base_textures.py` regenerates dirt/grass/sand/wood/planks/
      leaves/sticks; one texture per block, all faces).
- [x] Real texture/atlas system landed 2026-09-23 (see
      `state/changelog-recent.md`): `vb.register_block{texture=...}` ->
      `S2C_BlockRegistry` -> a per-session `vb::render::TextureAtlas` built
      from Asset Sync (or, `--singleplayer`, straight off disk) -> real
      per-face UVs in `ChunkRenderer`. Proved on `base:stone`/`base:water`
      only (user-scoped) — a full base-pack reskin (dirt/grass/sand/wood/
      leaves still flat placeholder colors) is a separate follow-up pass,
      not a mechanism gap.

**Phase 4 exit — met (mechanism, not content):** all five Lua/asset-sync
subsystems (4.1–4.5) are implemented and tested end-to-end over
`LoopbackTransport`. What's still missing to literally satisfy the exit
line ("server runs the base Lua pack ... opens a Lua-defined inventory
screen") is content, not mechanism: there is no `content/base` pack yet
(Phase 5.1) for a real server to load, so nothing calls `vb.register_block`/
`ui.define`/etc. outside of tests today.

---

### Moved from `REMAINING_TASKS.md`'s core (dream, 2026-10-06)

> Verbatim text of the core file's "Remaining" list for this section at the
> time of the move; the core now keeps a one-line summary.

- [x] Custom `require` over the pack's own virtual module filesystem +
      per-callback wall-clock budget — landed 2026-09-27. In-memory
      `path -> source text` map built from a recursive walk of the pack's
      `.lua` files (except `ui/*.lua`), installed via `Vm::install_require()`;
      cycle detection, `package.loaded`-style caching. `VmLimits` gains a
      250ms `wall_clock_budget_ms` checked from the existing instruction hook
      (`kHookPeriod` 1000), classified to the existing `kBudgetExceeded`.
- [x] `EntityKind` tick/spawn/hit/death callbacks wired into real systems —
      landed 2026-09-27, now that `SystemRunner` (Phase 3.1) exists.
      `ServerSession::set_script_tick_hook` is a new `"script_tick"`
      `SystemRunner` phase, so a script-driven entity move now reaches that
      same tick's `sync_interest`/`broadcast_snapshots` instead of one tick
      late.
- [x] `register_entity`'s `visual = {...}` sub-table (variant/facings/clips)
      for 3.5's `entity_renderer` — landed 2026-09-25 (protocol **19 -> 20**).
      Client-side `entity_visual_layout.hpp` computes per-clip layout,
      validated against the real decoded PNG; a mismatch falls back to the
      flat placeholder rather than failing pack load.
- [x] Per-instance `ScriptState.visual_override` (skins) — landed 2026-09-25
      (protocol **20 -> 21**). `vb.world.spawn(kind, pos, {visual_override=})`
      merges independently-optional fields over the kind's own `visual`;
      fixed at spawn time only — no live-update/clear path yet.
- [x] Real base-pack art for `base:player`/`base:dropped_item` — landed
      2026-09-25 via a new `vb.register_entity{represents="player"|
      "item_drop"}` field. **Follow-up (2026-09-27):** front/side/back poses
      were too similar to tell apart; `base:player` swapped to debug-styled
      F/R/B/L art plus a new `visual = {mirror = false}` option for a
      distinct pose per facing (protocol 22 -> 23) — also fixed a real
      pre-existing bug where a facings=4 kind was picking its pose with
      hardcoded facings=8 sector math.
- [x] Real texture/atlas system landed 2026-09-23: `vb.register_block{
      texture=...}` -> `S2C_BlockRegistry` -> per-session `TextureAtlas` ->
      real per-face UVs. Proved on `base:stone`/`base:water` only — a full
      base-pack reskin (dirt/grass/sand/wood/leaves) is a separate follow-up.
- [x] Manifest staleness: a pack writing `vb.storage` *after* startup used
      to go stale for the server process's life — landed 2026-09-28. New
      `PackRuntime::storage_revision()` counter, polled once/sec by
      `src/server/main.cpp` to rebuild + atomically swap the asset manifest
      via a new `ManifestHolder`. `--singleplayer` is out of scope (never
      builds a manifest).
- [x] Item grid widget for `UiRuntime` — landed 2026-09-28. A new generic
      `icon` `WidgetType` draws one registered block/item id's real atlas
      texture; `content/base/ui/inventory.lua` composes a real item grid out
      of it plus `rect`/`text`.
- [ ] `--singleplayer`'s registry-wiring gap is closed (Phase 5.1); no
      remaining item here.
