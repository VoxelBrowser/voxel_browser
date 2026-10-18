## Phase 6 — Lua-Driven Extensibility  ✅ 6.1–6.16 done (2026-09-18); 6.17 planned

> Full history for this phase; linked from `REMAINING_TASKS.md`. Ground truth for [x] items — do not duplicate here.

> Design agreed in discussion on 2026-09-17: four systems that let content
> packs override/extend engine defaults (biomes, entities, UI, input, data)
> the way the register-by-name registries already let blocks be overridden
> today. Builds on 3.1's EnTT wiring, 4.2's registration API, and 4.2's
> still-open "wire `register_entity` callbacks" item — see
> `ARCHITECTURE_SPEC.md` §7.1-7.2, §10.3-10.6, §17, §18 Q6 for the design.

### 6.1 Entity kinds as classes, spawned entities as objects  ✅ (2026-09-17)

> Landed without the EnTT registry the original design assumed (§7.1's
> `ScriptState` component / `ScriptPreTickSystem`/`ScriptPostTickSystem`
> classes don't exist — 3.1's generic registry wiring is still deferred, same
> as when 5.1's `ItemDropSystem` hit the exact same gap). Followed
> `item_drops.hpp`'s established precedent instead: a small hardcoded system
> (`PackRuntime::Impl::entities`, `src/script/pack_runtime.cpp`) replicated
> through `ServerSession::spawn_script_entity`/`set_script_entity_state`/
> `remove_script_entity` (`inc/vb/net/session.hpp`, mirrors `spawn_item_drop`
> exactly — another interest-grid entry, no new wire message, its own NetId
> range at `0x4000'0000` disjoint from players and item drops).

- [x] Per-instance Lua state: `self` is a plain Lua table (the spec's
      `ScriptState`, just not EnTT-component-backed) created by
      `vb.world.spawn(kind, pos)` and stored in `PackRuntime::Impl::entities`
      keyed by NetId — persists across every `on_tick` call for that
      instance (verified in the integration test below by accumulating a
      counter on `self` across ticks), so two spawned objects of the same
      kind track independent data.
- [x] `on_spawn`/`on_tick`/`on_hit`/`on_death` wired: `on_spawn` fires once
      from `vb.world.spawn`; `on_tick` fires once per `PackRuntime::dispatch_tick`
      per live instance (`Impl::dispatch_entity_tick`); `on_hit` fires from a
      new `self:damage(amount, cause)` (notification-only — the engine
      tracks no generic-entity health, same "engine takes no position"
      posture as the still-unbuilt 6.5); `on_death` fires from a new
      `self:remove(cause)`, which also despawns (interest-grid removal +
      map erase) right after.
- [x] Decided: arbitrary fields for custom data, accessors for engine-owned
      state — `self` is a plain table (`self.hp = 10` just works) whose
      metatable's `__index` points at a shared `entity_methods` table
      (`get_pos`/`set_pos`/`get_kind`/`damage`/`remove`), so both coexist
      unless a pack picks a method's exact field name.
- [x] Test: `pack_runtime_integration_test.cpp` — real `ServerSession`/
      `ClientSession`/`LoopbackTransport`, spawn via chat command, asserts
      `self` state persists and grows across many ticks (not rebuilt per
      call), `on_hit`/`on_death` fire with the right args, and the instance
      both replicates to a client while alive and disappears from
      `remote_entities()` after `self:remove()`.
- [x] Automatic despawn-on-health trigger for generic script entities —
      landed 2026-09-25. Opt-in per kind via `vb.register_entity{health=...}`
      (`EntityKindDef::max_health`, rejects a non-positive value at
      registration); a kind that never sets it keeps the exact pre-existing
      behavior (`entity:damage()` is notification-only, `on_hit` fires,
      nothing else). A kind that does opt in gets a per-instance current
      health (`ScriptEntity::health`, initialized from the kind default at
      `vb.world.spawn`), new `entity:get_health()`/`entity:set_health(v)`
      accessors (nil/error respectively for an untracked kind), and
      `entity:damage()` now decrements it and calls the existing
      `despawn_entity()` (fires `on_death`, deregisters) once it reaches 0 —
      the same despawn path `entity:remove()` already used, just triggered
      automatically. Server-side bookkeeping only, no protocol change.
      **Gotcha hit while wiring this up:** `on_hit` can itself call
      `self:remove()` (kitchen_sink's `sentry.lua` does exactly this) — the
      first draft reused the iterator taken before firing `on_hit` to then
      touch `.health` afterward, a use-after-erase if `on_hit` already
      despawned the entity (`kitchen_sink_pack_test.cpp`'s sentry test
      crashed on an MSVC STL iterator-debug assertion, not silently). Fixed
      by re-`find`ing after the `on_hit` call instead of reusing the
      pre-call iterator. Verified: full `vb_tests` 319/319 green (a
      `pack_runtime_integration_test.cpp` case spawns a `health=5` kind and
      an opted-out kind side by side, proves 2 hits of 3 despawn the tracked
      one exactly at 0 without an explicit `:remove()` while the untracked
      one survives 1000 damage notification-only), clean `-Werror` build.
- [x] Client-side kind-specific rendering for script entities — landed
      2026-09-25 (`EntityKind` id now drives the billboard's own
      width/height, via a new `S2C_EntityKindRegistry` (protocol version 19)
      and `vb.register_entity{width=, height=}`). Real per-kind sprite art
      (the `visual = {...}` schema) landed as its own separate pass the same
      day, see Phase 4's own entry.

### 6.2 Fully Lua-defined, immediate-mode reactive UI  ✅ (2026-09-17)

- [x] Reframed `ui.define(name, render_fn)`: `render_fn(state)` now runs
      once per UI frame for as long as the screen is open
      (`UiRuntime::render_frame()`, `src/script/ui_runtime.cpp`), not once at
      `open()` time. `open()` now only resolves the render function and
      seeds `state` (a persistent `sol::table`, built once from `ctx_json`)
      — it no longer evaluates anything itself, so there's exactly one
      evaluation per frame, not a double-evaluation on the opening frame.
      `main.cpp`'s existing per-frame UI block calls `render_frame()` right
      before `UiRenderer::draw(...)`, same call site as before, no new
      per-frame plumbing needed since `raygui` was already immediate-mode.
      The per-frame call is wrapped in `vm.begin_call_budget()`, same sandbox
      guard `report_click`/`report_change` already used for one-shot
      callbacks, now covering a function invoked continuously.
      **Client-VM-only change, no protocol/wire changes** — `C2S_UiEvent`/
      `S2C_OpenUi` are untouched; event handlers still call
      `ui.send_event`/`ui.close` exactly as before.
- [x] `content/base/ui/{inventory,pause}.lua` rewritten to the `render(state)`
      shape (parameter renamed `ctx` → `state`). `inventory.lua` also
      demonstrates real local reactivity: the list's `on_change` sets
      `state.selected` (no `ui.send_event`, purely cosmetic per the design),
      and the title label reads it back — proving a handler-mutated `state`
      value shows up on the very next frame with no reopen and no server
      round-trip.
- [x] Event handlers unchanged: `on_click`/`on_change`/`on_close` still
      route through `ui.send_event`/`ui.close` → the existing
      `C2S_UiEvent` path; only purely cosmetic state moved onto the
      persistent `state` table.
- [x] Tests (`tests/unit/ui_runtime_test.cpp`) rewritten to the new
      contract, including a new case that is the actual reactivity claim:
      a button's `on_click` increments `state.count`, a label renders it;
      `report_click` + a second `render_frame()` call show the label
      updated with no `open()`/reopen in between.

### 6.3 Server-side player-input interception, closed-schema custom keybinds  ✅ (2026-09-18, rate limiting still deferred)

- [x] `vb.register_keybind(name) -> index` at pack load — idempotent by name
      (same linear-scan-by-name shape as `register_entity`), rejected after
      `PackRuntime::freeze()`, capped at 32 registrations
      (`S2CKeybindRegistry::kMaxKeybinds`) so the bitset always fits one
      `InputCmd.keybinds` `u32` — a deliberate scope cap, same spirit as the
      existing `kMaxCmds`/`kMaxBlockRegistryRecords` guards.
- [x] Registered set synced at handshake as `S2C_KeybindRegistry` (47),
      wire shape mirrors `S2C_BlockRegistry` exactly: sent in the same
      `kAwaitingReady` step, `HandshakeServerHost::keybind_registry` hook
      (`nullopt` default = no frame, zero behavior change), wired by the new
      `PackRuntime::install_keybind_registry(host)` (`src/server/main.cpp`
      calls it right next to `install_join_veto`). Client applies it
      unconditionally in `ClientSession::tick()`, same reasoning as block
      registry (no cross-lane ordering guarantee vs. `JoinAccept`).
      `kEngineProtocolVersion` bumped 11 → 12.
- [x] Movement input (`PlayerInput`'s existing fields) untouched — additive
      `InputCmd.keybinds: u32` field alongside the existing `buttons: u8`.
- [x] `vb.on("player_input", handler)` fires inside `ServerSession::
      handle_input_batch`'s existing per-cmd loop (no `IngestInputSystem`/
      `MovementIntegrationSystem` split exists yet — §7.2's system runner is
      still `[ ]` — so this hooks the one function that loop already lives
      in, before its `physics::step_movement` call). `input` is
      `{move,yaw,pitch,buttons={...},keybinds={[name]=bool,...}}` (only
      registered names ever appear as `keybinds` keys); `return false`
      vetoes, a returned table overrides only the fields present (chained
      across multiple handlers in registration order, first veto wins).
      **Judgment call on veto semantics** (not spelled out in the spec):
      a veto drops the cmd's effect on movement/rotation entirely, but
      `input.last_seq` still advances so the cmd is durably consumed/acked
      instead of being silently reprocessed every batch forever — a literal
      "never happened, never acked" veto would desync client-side
      prediction/reconciliation with no way to converge.
      Only installed (`ServerSession::set_input_handler`) when a pack
      actually registers a `player_input` handler, so the hot per-tick input
      path pays zero extra cost otherwise (same conditional-install pattern
      6.6 used for `set_respawn_handler`).
      Tests: `tests/unit/pack_runtime_integration_test.cpp` (veto freezes
      authoritative position bit-exact; a chained replace reaches a second
      handler with only the overridden field changed); `tests/unit/
      pack_runtime_test.cpp` (idempotency/cap/freeze); `tests/unit/
      block_registry_test.cpp` (registry reaches a joined client / host
      that never opts in leaves it empty); protocol round-trip + lane tests.
- [x] Per-connection rate limiting on custom-keybind events — closed
      2026-09-27 as part of Phase 3's flood guard (see that phase's own
      entry): custom-keybind bits ride inside `InputCmd`/`C2SInputBatch`
      like every other input field, so the generic
      `set_max_messages_per_second` token bucket (one token per message, any
      type) covers a custom-keybind flood the same way it covers an
      input-batch or chat flood — no keybind-specific limiter was needed.

### 6.4 Generic per-key persistent storage (script-owned identity/auth)  ✅ (2026-09-18)

- [x] `vb.db.get(key)` / `vb.db.set(key, value)` / `vb.db.delete(key)` —
      arbitrary script-chosen keys (`"user:" .. name`, `"session:" .. token`,
      ...), distinct from the existing pack-global `vb.storage`. The engine
      has no concept of "logged in" — a connection stays just a connection
      (as today) until a pack's own login flow looks up a record and decides
      to recognize it. Joining a world isn't authenticating, the same way
      loading a webpage isn't.
- [x] Storage backend: **not SQLite** — `vb::script::ScriptDb`
      (`inc/vb/script/db.hpp`/`src/script/db.cpp`) instead, one file per key,
      content-addressed by `sha256(key)` under a 2-hex-prefix shard
      directory (`<content_pack>/db/<prefix>/<hash>`), reusing
      `ClientAssetCache`'s on-disk shape (§4.4) rather than adding a new
      dependency. No list/enumerate — spec only calls for get/set/delete by
      an already-known key, and this scales to "one record per identity"
      fine without one. A real embedded-database swap (SQLite still the
      leading candidate if this ever needs range queries or transactions)
      stays a pure implementation-detail change behind the same interface,
      per this item's original "implementation detail, not a design
      blocker" framing.
- [x] `vb.crypto.hash(data)` — SHA-256 hex digest (`vb::core::sha256_hex`,
      `inc/vb/core/sha256.hpp`/`src/core/sha256.cpp`, dependency-free) so
      packs implementing their own login don't roll credential hashing in
      pure Lua — the sandbox strips `os`/`io` deliberately (§10.2), and
      pure-Lua hashing is slow and easy to get wrong. The engine still takes
      no position on auth as a concept — see `ARCHITECTURE_SPEC.md` §18 Q6.
      Also used internally by `ScriptDb` for its key-to-filename hashing.

### 6.5 Shared block-damage breaking (default + override crack texture) ✅ (2026-09-18; damage-value replication + default crack overlay landed 2026-09-27; real crack-stage art + crack_texture override closed same day, see below)

- [x] `BlockType` gains `max_damage` (0 = today's instant break, the
      default — no behavior change for any existing block); `vb.register_block{...}`
      exposes it (`def.max_damage`, `inc/vb/world/block.hpp`,
      `src/script/pack_runtime.cpp`). No `crack_texture` field added — there's
      nowhere to put it yet (`BlockType` has no texture/model fields at all
      until 4.3/5.1's real atlas system lands), so it stays deferred alongside
      the rendering item below rather than added unused.
- [x] Sparse server-side damage map: `vb::world::BlockDamageSystem`
      (`inc/vb/world/block_damage.hpp` + `src/world/block_damage.cpp`), a
      pure `pos -> {damage, max_damage, last_touched_tick, contributors}` map
      holding only entries with damage > 0 — same posture as `ItemDropSystem`
      (no net/script dependency, unit-testable standalone,
      `tests/unit/block_damage_test.cpp`). Does not touch chunk revisions or
      mesh invalidation on its own.
- [x] `C2S_BlockBreakBegin{pos, face}` / `C2S_BlockBreakStop{pos}`
      (`inc/vb/protocol/world.hpp`, message ids 48/49; `BlockRegistryRecord`
      also gains `max_damage`, `kEngineProtocolVersion` 12 → 13) bracket a
      player holding a target;
      `ServerSession::handle_block_break_begin` gates entry with the same
      reach check `apply_block_edit` uses (`WorldReplicator::in_reach`, new)
      plus an engine-level `max_damage > 0` check, then
      `vb.on("block_break_begin", ...)` (vetoable) via
      `ServerSession::BlockBreakHooks::begin`.
- [x] `vb.on("block_break_tick", handler)` fires once per tick **per
      contributing player** (`ServerSession::update_block_damage`, called
      from `tick()` alongside `update_item_drops`) — returns the damage delta
      to add; multiple concurrent contributors ("breaking together") sum,
      and so do multiple registered handlers for the same call (an
      orthogonal case the spec didn't call out, summed the same way rather
      than picking one arbitrarily). No handler registered = zero built-in
      policy, damage never accrues.
- [x] `vb.on("block_health_tick", handler)` fires once per tick **per
      damaged block**, regardless of contributors — `(pos, damage,
      max_damage, ticks_since_last_hit)` in, a replacement damage value (or
      nothing = unchanged) out; the last handler to return a number wins if
      several are registered. No handler registered = permanent damage, no
      healing at all.
- [x] Completion (summed damage reaches `max_damage`) drives the *existing*,
      unchanged `C2S_BlockEdit`/`WorldReplicator::apply_block_edit`/`on_break`
      pipeline via a synthesized `C2S_BlockEdit{kBreak}` attributed to
      whichever player was contributing when it completed (arbitrary among
      concurrent contributors) — this system only gates *when* that fires,
      never replaces it.
- [x] Damage-*value* replication to nearby players — landed 2026-09-27, and
      targets the *live* mechanism (6.18's punch-based `block_punch_counts_`),
      not the legacy `BlockDamageSystem`/`C2S_BlockBreakBegin`/`Stop` path
      this bullet originally described, which no client has ever driven (see
      below). New `S2C_BlockDamage` (protocol 54, version 24 -> 25,
      `docs/protocol.md`): `IVec3 pos`, `u16 punches`, no `max_damage` field
      (the receiving client already knows `BlockType::max_damage` from its
      own chunk mirror + block registry). `ServerSession::
      broadcast_block_damage()` sends it from every `punch()` count change
      and from `update_block_punch_healing()`'s heal steps, to every playing
      connection currently mirroring the chunk (`WorldReplicator::
      player_has_chunk`) — not just the puncher, closing the actual gap this
      bullet was about. `punches == 0` (break or full heal) means "clear,"
      not "a value" — `ClientSession::block_damage()` erases the entry
      rather than storing a stale 0, and also drops any entry for a chunk
      that leaves the client's own view (`S2C_ChunkRemove`), since the
      server stops broadcasting updates for it at that point.
      `client.break_progress()` (Phase 6.16, previously always `nullopt`
      pending exactly this) now reports the currently-looked-at block's real
      `punches / max_damage` fraction. Default generic crack overlay: a
      translucent black cube over the targeted block, darkening with that
      fraction (`src/client/main.cpp`) — real texture-stage crack art and a
      `crack_texture` override are still open, see the bullet below.
      Verified: full `vb_tests` 355/355 green (a `protocol_test.cpp`
      round-trip case; two new `blockedit_test.cpp` cases — a second,
      non-punching client sees another player's live punch count and sees it
      cleared on break; the existing self-heal case now also asserts the
      replicated value clears on a full heal), clean `-Werror` build of
      `vb_tests`/`voxel_browser`/`voxel_browser_server`.
- [x] Real crack-stage texture art + `crack_texture` override — landed
      2026-09-27, closing 6.5 in full. Protocol bumped **25 -> 26**:
      `BlockRegistryRecord` (`S2C_BlockRegistry`) gains `string
      crack_texture`, mirroring `vb::world::BlockType::crack_texture`
      (`BlockRegistry::set_crack_texture()` mirrors `set_texture()`'s
      "attach without disturbing other already-frozen fields" posture).
      `vb.register_block{crack_texture=...}` wires it through the same way
      `texture` is. New `vb::render::CrackAtlas`
      (`inc/vb/render/crack_atlas.hpp`, pure `build()`/GPU `upload()` split
      like `TextureAtlas`): one shared row of `kStages` (8) cells holds the
      engine's own procedurally-generated default crack pattern; a block
      with a valid `crack_texture` gets its own extra row instead — a
      wrong-shaped or undecodable override falls back to the shared default
      row rather than failing pack load. New `vb::render::CrackOverlay`
      (GPU-only, not unit tested) replaces the old flat translucent-cube
      overlay: a persistent unit cube mesh whose texcoords are remapped
      into the resolved `CrackAtlas` rect, alpha still climbing with
      `break_progress`.
      **Real pre-existing bug found and fixed in the same pass, unrelated to
      `crack_texture` itself:** all three call sites that build or apply a
      `BlockRegistryRecord` used an aggregate-init listing only the record's
      first 5-6 fields — so `max_damage` (and now `crack_texture`) was
      silently dropped on every one of these three hops the whole time. In
      practice this meant no client ever received a real nonzero
      `max_damage` for any block, so `client.break_progress()` was
      permanently `nullopt` regardless — caught by a new regression test
      that failed against the unfixed code before the fix. Verified: full
      `vb_tests` 363/363 green (8 new cases: the end-to-end regression case
      for the bug above, `crack_texture=`'s re-declare-attach and stored/
      default-empty cases, a protocol round-trip case, and
      `crack_atlas_test.cpp` covering default-row/override-row/wrong-shape-
      fallback/missing-from-vfs-fallback/stage-clamping behavior), clean
      `-Werror` build of `vb_tests`/`voxel_browser`/`voxel_browser_server`.
      The actual rendered crack-stage overlay was **not** manually eyeballed
      — no GUI in this agent environment.

### 6.6 Player damage & death (foundational — split out from the rest below) ✅

> Audited 2026-09-17: `void_kill_y` (`inc/vb/core/config.hpp:31`) was
> the only damage source in the entire engine — no fall damage, no PvP, no
> mob damage, no hunger/starvation. This was upstream of combat, PvP, and
> hazards, so it was called out on its own rather than folded into the rest
> of 6.7-6.13 — most of that later content still depends on this having
> landed first.

- [x] A generic damage primitive: `player:damage(amount, cause)`
      (`PlayerHandle::damage`, `src/script/pack_runtime.cpp`) →
      `ServerSession::damage_player(NetId, float, string_view)`
      (`src/net/session.cpp`). `cause` is an opaque string threaded through
      unchanged to the respawn hook below — the engine takes no position on
      what "fall"/"pvp"/"void" mean. The void-kill check itself is now just
      another `apply_damage()` call with `cause = "void"`, not a separate
      code path.
- [x] `check_respawns()` no longer hardcodes the outcome: it fires
      `ServerSession::set_respawn_handler(fn(NetId, cause, health_before) ->
      RespawnDecision{heal_to, pos, message})` once health reaches 0 (any
      cause, void included) and applies whatever it returns. Unset (e.g.
      `--singleplayer` before any `PackRuntime` attaches one) falls back to
      the original behavior byte-for-byte — full heal, teleport to the join
      spawn point, the same `"* you died and respawned"` line — so every
      pre-6.6 caller/test (`netcode_test.cpp`'s void-kill test included)
      passes unmodified. `PackRuntime::attach_session` installs a real
      handler only when a pack actually registered
      `vb.on("player_death", ...)` (checked once, after `freeze()`, so every
      pack file has already had a chance to register).
      Inventory-drop is the *handler's* business, not the primitive's: a
      `drop_inventory = true` in the returned table spawns every slot as a
      real dropped-item entity (Phase 5.1's `ItemDropSystem`, reusing
      `spawn_item_drop`) at the player's position **at time of death**, not
      wherever they're about to respawn — dropping at the new respawn point
      instead was tried first and immediately self-picked-up by
      `ItemDropSystem`'s pickup radius since the player is standing right on
      it; caught by the new integration test below, not just reasoned about.
- [x] Spawn point: `check_respawns()`'s hardcoded reuse of the join spawn
      point is gone; `ServerSession::spawn_point(NetId)` exposes the join
      point as a value a respawn handler can read (e.g. to keep the old
      behavior on purpose), not something it's stuck with. A real
      bed/checkpoint pack feature still needs pack-side storage (`vb.db`,
      6.4, not yet landed) to remember a chosen point across respawns —
      *this* item was only about the engine no longer forcing one point.
- [x] No PvP toggle: still N/A, same reasoning as before — now meaningful to
      add once a pack actually calls `player:damage` on another player, not
      before.
- [x] Tests: `netcode_test.cpp`'s existing void-kill/respawn test passes
      unmodified (proves the no-handler-set fallback is byte-identical).
      New: `pack_runtime_integration_test.cpp` — `player:damage()` +
      `vb.on("player_death", ...)` end-to-end over a real
      `ServerSession`/`ClientSession`/`LoopbackTransport`: custom heal
      amount, custom respawn position, custom chat message, and
      `drop_inventory = true` actually dropping+clearing inventory (and not
      self-repicking-up) all asserted through the real wire messages a
      client receives.
- [ ] Not done, left for whichever pack/phase actually needs it: fall
      damage, PvP, mob damage, hunger — this item only adds the *primitive*
      (`player:damage`) and the *decision hook* (`player_death`); no content
      calls either yet (same "mechanism before content" posture as every
      other Phase 4/5 item). `content/base` has no `death.lua` — nothing
      currently overrides the built-in fallback in the shipped base pack.

### 6.7 Physics / movement parameters (default + override) ✅ (global override; per-entity-kind deferred)

- [x] `vb.physics.set_params{...}` (`src/script/pack_runtime.cpp`) lets a
      pack override any `physics::MoveParams` field (gravity, walk/sprint
      speed, accel, friction, jump speed, step height, fly speed, ...).
      **Scoped to one global override, not per-entity-kind:** no entity kind
      besides the player runs `step_movement` today (script entities from
      6.1 have no physics at all), so a per-kind table would have nowhere
      else to apply — revisit if/when a non-player kind gets real physics.
      `PackRuntime::effective_move_params(base)` applies only the fields the
      pack actually set on top of `base`, leaving the rest untouched.
- [x] `ServerConfig.gravity` reconciliation, decided: it's the *base* fed
      into `effective_move_params()` (`move_params.gravity = config.gravity`
      in `src/server/main.cpp`, before the pack override runs) — an
      operator's `server.toml` sets the engine default, a pack's explicit
      `vb.physics.set_params{gravity=...}` wins over it if set. Documented
      inline at the call site, not just here.
- [x] **Also closed, not originally scoped but found while replicating
      this:** the client's local prediction (`ClientSession::move_params_`)
      never received the server's `MoveParams` at all before this — every
      client (dedicated-server and `--singleplayer` alike) silently
      predicted with `physics::MoveParams{}`'s own hardcoded defaults
      regardless of `ServerConfig.gravity` or any future pack override,
      correctness relying entirely on reconciliation snapshots papering
      over the drift. New `S2C_MoveParams` (id 50, Phase 6.7,
      `kEngineProtocolVersion` 13 → 14) sent between `C2S_Ready` and
      `S2C_JoinAccept` alongside `S2C_BlockRegistry`/`S2C_KeybindRegistry`
      (`HandshakeServerHost::move_params`, `nullopt` default = no frame,
      zero behavior change) fixes this for both the dedicated server and
      `--singleplayer`'s in-process host.
      **Also found and fixed while wiring the client:** two call sites in
      `src/client/main.cpp` (`run_headless` and the windowed `enter_playing`)
      constructed a fresh default `physics::MoveParams` and called
      `client->set_move_params()` with it *after* join, unconditionally
      stomping whatever `S2C_MoveParams` had already applied moments
      earlier (it arrives in the same handshake step as `JoinAccept`). Fixed
      by reading back `client->move_params()` (new getter,
      `inc/vb/net/session.hpp`) instead of reconstructing a default.

### 6.8 Day/night cycle curve (default + override) ✅

- [x] `vb::world::DayNightCurve` (`inc/vb/world/daynight.hpp`) generalizes the
      old fixed 4-keyframe gradient into a `vector<DayNightKeyframe>` (tick +
      brightness + color); `sky_brightness()`/`sky_color_for_time()` gain
      curve-taking overloads (an empty curve falls back to
      `default_day_night_curve()`, which reproduces the original 4 keyframes
      exactly — every pre-6.8 call site/test is unaffected).
      `vb.daynight.set_curve{keyframes = {{tick=, brightness=, color={r,g,b}},
      ...}}` (`src/script/pack_runtime.cpp`) lets a pack override it; replicated
      to joining clients as `S2C_DayNightCurve` (51, `kEngineProtocolVersion`
      14 → 15) between `C2S_Ready` and `S2C_JoinAccept` alongside
      `S2C_BlockRegistry`/`S2C_MoveParams` (`HandshakeServerHost::
      day_night_curve`, `nullopt` default = no frame, zero behavior change).
      `src/client/main.cpp`'s sky-clear code reads `client->day_night_curve()`
      instead of the bare default overload.
- [x] `day_length_seconds` wired to both a config surface and a Lua surface,
      closing the second half of the gap this item called out:
      `ServerConfig::day_length_seconds` (`server.toml`, default 1200.0,
      matching `ServerSession`'s own hardcoded default exactly — extracted to
      a shared `vb::net::kDefaultDayLengthSeconds` constant so the two never
      drift) is the base; `vb.daynight.set_day_length(seconds)` overrides it
      on top (rejects `seconds <= 0`), the same config-then-pack-override
      shape 6.7's `gravity`/`vb.physics.set_params` established. Wired in both
      `src/server/main.cpp` (`config.day_length_seconds` base) and
      `--singleplayer`'s `Singleplayer` struct (`kDefaultDayLengthSeconds`
      base, no `server.toml` there) via `PackRuntime::
      effective_day_length_seconds(base)`.
- [ ] Not done, deliberately out of scope: a pack-supplied arbitrary *curve
      function* (Lua callback re-evaluated every read) — only data-driven
      keyframes, matching every other Phase 6 "default + override" item's
      shape (a table of values, not an executable hook) and avoiding a
      per-frame Lua call from the replication path. A pack wanting a
      non-piecewise-linear shape can still approximate it with more
      keyframes.

### 6.9 Inventory stacking (default + override) ✅

- [x] `world::kDefaultMaxStackSize` (64, `inc/vb/world/block.hpp`) + a new
      `BlockType::max_stack` field (default = that constant) —
      `vb.register_block{max_stack = N}` overrides it per block, same
      def-parsing shape as 6.5's `max_damage`. Not `register_item`: every
      holdable item is already a registered block (see
      `content/base/blocks/planks.lua`'s comment on why `register_item`
      never allocates its own id space), so that's where the override
      belongs today.
      A new `PackRuntime::Impl::give_item(id, item, count)` is the one place
      that actually adds to an inventory: fills existing under-cap slots for
      that item first, then starts as many new slots as needed for the
      remainder (each capped at `max_stack`). Both `player:give()` and the
      item-pickup handler (`attach_session()`, previously two separate
      `push_back` call sites) now call it, so picking a dropped item up
      stacks identically to a script handing it to you — closing a
      duplication the existing pickup-handler comment already claimed ("credits
      their inventory exactly like give() does") but the code didn't actually
      guarantee.
      No slot-count cap on the inventory itself — re-reading the item that
      opened this task, "stack cap ... and slot count" reads as one thing
      (how much fits in one slot), not a second cap on total slots; no
      evidence elsewhere of an intended max-slots limit.

### 6.10 Chat transform/moderation hook ✅ (2026-09-18, rate limiting still deferred)

- [x] `ServerSession::ChatHookResult{veto, replacement_text}` +
      `set_chat_handler(std::function<ChatHookResult(NetId, string_view)>)`
      (`inc/vb/net/session.hpp`) replace the old bool-veto-only handler type.
      `PackRuntime::dispatch_chat` now returns `ChatHookResult` (was `bool`);
      `PackRuntime::Impl::run_chat` chains every `vb.on("chat", handler)` in
      registration order — `return false` vetoes (first veto wins, same as
      `run_veto`), `return "text"` replaces what the *next* handler (and
      ultimately the broadcast) sees, `true`/`nil`/anything else passes the
      current text through unchanged. Same shape as `run_player_input`/
      `InputHookResult`, just one string field instead of a table.
      `handle_chat()` (`src/net/session.cpp`) applies the veto/replacement
      before formatting `"name: text"` and broadcasting `S2C_Chat`. No wire
      message changed — this is purely a server-side hook contract change,
      no `kEngineProtocolVersion` bump needed.
      Rate limiting still not implemented — a pack can build one on top of
      this hook (e.g. via `vb.db`/`vb.storage` timestamps), but the engine
      doesn't enforce one itself, matching the item's own framing ("or its
      own rate-limit policy instead of the engine's none-at-all").

### 6.11 Item drop parameters (default + override) ✅ (2026-09-18)

- [x] `ItemDropSystem::spawn()` now takes optional per-drop
      `pickup_radius`/`lifetime_seconds` overrides (`inc/vb/world/item_drops.hpp`);
      unset falls back to the system's own construction-time defaults (1.5 /
      120.0), byte-identical to every pre-6.11 caller. `BlockType` gains
      `pickup_radius`/`drop_lifetime_seconds` (both `-1.0` = "no override" —
      0 is a plausible real radius, so it can't double as the sentinel).
      `vb.register_block{pickup_radius=..., item_lifetime_seconds=...}`
      (`src/script/pack_runtime.cpp`) sets them; not `register_item` as
      originally sketched, same reasoning 6.9 already established — every
      holdable item is a registered block today.
      `ServerSession::spawn_item_drop` (`src/net/session.cpp`) looks the
      dropped item's id up in the live `WorldReplicator`'s registry and
      passes any override through; no `replicator_` (a bare test harness) or
      an unknown id both fall through to the engine defaults unchanged.
      Tests: 1 new `item_drops_test.cpp` case (a wide `pickup_radius`
      override collects from outside the system default; an overridden
      `lifetime_seconds` survives well past the system default) + 1 new
      `pack_runtime_integration_test.cpp` end-to-end case (a
      `register_block{pickup_radius=10}` item is picked up 8 blocks away, well
      outside the 1.5 default, over a real `ServerSession`/`ClientSession`/
      `LoopbackTransport`). Full `vb_tests` green (248/248) + all 4 CTest
      cases pass on `build-net-lua`.

### 6.12 Entity animation clip priority (cosmetic, low priority) ✅ (2026-09-18, no code change — decision already matches implementation)

- [x] `resolve_anim_clip()`'s fixed priority order (`kDead > kHurt > kActing
      > kJump/kFall > kRun > kWalk > kIdle`) and its speed thresholds
      (`AnimThresholds`, `inc/vb/render/entity_visual.hpp:44-48`) stay
      engine-fixed even once per-kind clip *assets* are pack-defined
      (already tracked separately under 4.2's `visual = {...}` sub-table) —
      *which* clip wins in a given state is a distinct, finer-grained
      concern. Cosmetic only; lowest priority in this section.
      Verified 2026-09-18: `resolve_anim_clip()` (`inc/vb/render/
      entity_visual.hpp:51-`) already implements exactly this fixed order
      with no Lua hook of any kind — this item was a "leave it engine-fixed"
      design decision that the code already matched, not a pending
      implementation. No change made.

### 6.13 Read-only server config visibility (not a pack-override surface) ✅ (2026-09-18)

- [x] `ServerConfig` (`tick_rate`, `view_distance`, `max_players`,
      `void_kill_y`, ...) are server-**operator** settings
      (`server.toml`/CLI), a different persona from a content-pack author —
      a pack should not be able to silently change `max_players` out from
      under the operator running the server. At most, expose a read-only
      `vb.config.get(key)` so a pack can *react* to these values (e.g. tune
      spawn density to view distance), not a full override registry like
      6.6-6.11 above.

### 6.14 Lua-driven worldgen pipeline (FastNoise2 backend) ✅ (2026-09-18)

> Moved here from Phase 4.2 (2026-09-17) — it's the same "override an
> engine default from a pack" shape as the rest of Phase 6, and `biomes`
> was already named in this phase's own intro note as one of the four
> systems in scope. World generation (biome selection, height params, block
> choice) is currently 100% hardcoded in `WorldGenerator::generate`
> (Phase 2's fBm heightmap pipeline); `vb.worldgen.set_pipeline` doesn't
> exist. FastNoise2 (`v0.10.0`, `VB_WITH_WORLDGEN`) has been a pinned
> dependency since Phase 0 but nothing constructs a node graph with it yet
> — the hand-rolled integer-hash noise in `vb/core/noise.hpp` is what
> `WorldGenerator` actually uses today.

- [x] `vb.worldgen.set_pipeline(fn)` — a pack-supplied stage function (or
      ordered list of stages) that replaces `WorldGenerator::generate`'s
      hardcoded body; falls back to the current hand-rolled fBm heightmap
      when no pack sets one, so an unmodified `content/base` keeps working.
      **Shipped as `set_pipeline(table)`, not `set_pipeline(fn)`** — Lua/
      sol2 is strictly single-threaded and `WorldGenWorkerPool` calls
      `generate()` from N worker threads with zero locking, so a literal
      per-chunk Lua callback was never viable; the table is compiled once,
      main thread, into an immutable `worldgen::PackWorldGenPipeline`. See
      `docs/lua-api.md`'s `vb.worldgen.set_pipeline` entry for the full
      writeup and `STATE.md` for the session notes.
- [x] Expose FastNoise2 node-graph construction to Lua (`vb.noise.*`) —
      the natural backend for pack-defined pipelines; the hand-rolled
      `vb/core/noise.hpp` path stays as the deterministic zero-dependency
      default (`VB_WITH_WORLDGEN` off), same posture as every other
      `VB_WITH_*`-gated optional backend. `vb.noise.*` builds a small
      portable node-graph IR (`vb/worldgen/noise_graph.hpp`'s `NoiseNode`)
      that compiles to either evaluator depending on the build flag —
      confirmed working end-to-end under `VB_WITH_WORLDGEN=ON` this session
      (real FastNoise2 v0.10.0-alpha fetched, linked, and exercised by the
      full test suite; see STATE.md). Fixed a real pre-existing bug found
      while wiring this: `cmake/Dependencies.cmake` pinned FastNoise2 to a
      tag (`v0.10.0`) that doesn't exist in `Auburn/FastNoise2` — the real
      tag is `v0.10.0-alpha` — unnoticed until now because nothing had ever
      actually fetched/built it before this item.
- [x] Biome selection (`ARCHITECTURE_SPEC.md` §6 stage 2): Voronoi-cell
      partitioning with adjacency-weighted probability (WFC-flavored,
      non-backtracking — design finalized 2026-09-17) reads `register_biome`
      entries (already captured by `PackRuntime`, 4.2) instead of nothing.
      `vb/worldgen/biome_selector.hpp`'s `BiomeSelector` implements this;
      deliberately **not** globally memoized/locked (recomputes its bounded
      neighbor recursion from scratch per query, trading cache-hit-rate for
      zero shared mutable state across worker threads) — see that header's
      own comment for the reasoning, and its Deferred-section entry below
      for the follow-up if profiling ever shows this matters.
- [x] Carvers + vein/scatter (new stage 5, ore/valuable-block placement) +
      decoration pass — each a pipeline stage a pack can plug in via the
      same `set_pipeline` mechanism. **Decoration scope narrowed to
      schematic-only** (pure data: a block-offset list scattered per chunk),
      not the spec's "procedural callbacks" — same threading reasoning as
      `set_pipeline` itself; see the Deferred section below.
- [x] Determinism gate (`tests/unit/worldgen_test.cpp`'s golden-hash test)
      needs a pack-driven pipeline case once this lands, alongside the
      existing hardcoded-pipeline golden. Added
      ("worldgen determinism gate, pack-driven pipeline (golden value)"),
      built directly against `worldgen::NoiseNode`/`PackWorldGenPipeline` in
      C++ (not through Lua) so it stays backend-evaluator-pinned (always the
      hand-rolled path) regardless of whether a given build links real
      FastNoise2 — the fixed-default-path golden test is completely
      untouched (byte-identical output confirmed both with and without
      `VB_WITH_WORLDGEN`).

### 6.15 Example Lua script demonstrating the Phase 6 "default + override" features ✅ (2026-09-18)

> Blocked on the rest of Phase 6 (6.9–6.14) landing — the "Lua overhaul": once
> every default-with-a-pack-override surface this phase introduced (entity
> kinds, reactive UI, custom keybinds, `vb.db`/`vb.crypto`, block damage,
> player damage/death, physics params, day/night curve, inventory stacking,
> chat transform, item-drop params, worldgen pipeline) is in place, write one
> `content/base`-sibling example pack that actually exercises them together,
> not just in isolation across scattered unit tests.

- [x] A standalone example content pack (e.g. `content/examples/kitchen_sink`
      or a `content/base/examples/` script loaded only in a demo mode) that
      calls each Phase 6 API at least once with a visible, in-game effect: a
      custom entity kind with `on_tick`/`on_hit`/`on_death`, a `ui.define`
      screen opened via `player:open_ui`, a custom keybind bound to an action,
      `vb.db`/`vb.crypto.hash` used for a small persistent counter, a
      `max_damage` block with a `block_break_tick` handler, a
      `player_death` handler with custom respawn/drop behavior,
      `vb.physics.set_params` tuning movement, `vb.daynight.set_curve`/
      `set_day_length` for a non-default sky, and (once 6.9-6.11 land) a
      stacking item, a chat filter, and tuned item-drop params.
      Shipped as `content/examples/kitchen_sink/` (the first-listed option).
      `entities/sentry.lua`'s `on_tick`/`on_hit`/`on_death` are real and
      dispatched (Phase 6.1, landed 2026-09-17 — no EnTT registry involved,
      still doesn't exist, Phase 3.1) — a `/sentry` chat command spawns,
      hits, and kills one for real. Corrected two stale docs found while
      verifying this: `content/base/entities/dropped_item.lua`'s own comment
      and `docs/lua-api.md`'s worked-example table both still claimed
      `vb.world.spawn` "just logs and returns nil", predating 6.1 — fixed
      both. Every 6.15 bullet has a real, running effect, confirmed by
      `tests/unit/kitchen_sink_pack_test.cpp` and a manual
      `voxel_browser_server --content-pack content/examples/kitchen_sink` run
      this session. Also folds in Phase 6.14's own worked-example gap
      (`vb.worldgen.set_pipeline` + `vb.register_biome` with real
      `probability`/`adjacency`, plus a carver and a vein) since 6.14 landed
      earlier this same session.
- [x] Should double as living documentation: comment each block with which
      `REMAINING_TASKS.md` phase/`docs/lua-api.md` section it demonstrates, so
      it stays a working reference alongside the prose docs rather than
      drifting out of sync with the actual API surface. `init.lua` carries a
      file-by-file index; every individual file's own header comment names
      the exact phase/doc section and, where relevant, contrasts with how
      `content/base` already demonstrates a *different* facet of the same
      API (e.g. `chat.lua`'s text-rewriting vs. `crafting.lua`'s veto-only
      chat use).
- [x] Not `content/base` itself — base pack content should stay minimal/
      production-shaped (spec §5.1); this is a separate, clearly-labeled demo
      pack an operator can point `--content-pack` at, or a devs-only mode, not
      something a real server loads by default. `content/base` itself is
      completely untouched by this item.

### 6.16 Client-local HUD mechanism (engine raw state, Lua presentation) ✅

> User-requested (2026-09-18): the hold-to-break progress bar added in 5.2
> was pure hardcoded C++ (`DrawRectangle` calls in `src/client/main.cpp`),
> which broke the project's "engine provides raw state, Lua deals with
> presentation" rule as much as anything in the codebase. Closing that gap
> needed a real mechanism first — `UiRuntime`'s existing `ui.define`/`open`/
> `close` model is for server-pushed modal screens (inventory, pause), not an
> always-on overlay — so this added one. **Revised same day, also
> user-requested:** the first pass added a `kProgressBar` widget type, which
> the user correctly called out as still baking a presentation *concept*
> into the engine (Lua only supplied the value, not the drawing). Replaced
> with a meaning-free `kRect` primitive — see the second bullet below.

- [x] `ui.define_hud(render_fn)` (`vb::script::UiRuntime`, distinct from
      `ui.define`'s named-screen registry): registers a single render
      function that's evaluated every UI frame unconditionally, independent
      of whatever modal screen `open()`/`close()` currently has up. Its own
      persistent `state` table (Phase 6.2's reactivity mechanism) is created
      once and never reset by a modal screen opening/closing alongside it.
      `UiRuntime::render_hud()` evaluates it and returns the widget list;
      `vb::render::UiRenderer` draws it exactly like a modal screen's widgets
      (a *second* `UiRenderer` instance, since one shared instance would
      thrash its per-widget-id text/list edit caches by seeing the drawn
      `ui_name` toggle between "hud" and the modal name every frame).
- [x] New `kRect` widget type — a raw filled rectangle (`fill_r/g/b/a`) with
      an optional 1px outline (`border_r/g/b/a`, alpha 0 = none), drawn with
      plain `DrawRectangle`/`DrawRectangleLines`, no raygui control involved.
      Deliberately the only widget type with **no semantic meaning at all**
      — not "a progress bar", not "a health bar", just a box at `(x,y,w,h)`.
      A pack composes whatever purely-visual element it wants (a progress
      bar is two of these: a background/border rect, and a fill rect sized
      by a fraction) entirely in Lua; the engine never bakes in what the
      rectangle *represents*. First new widget type since Phase 4.5/6.2
      shipped label/panel/button/textbox/list.
- [x] Raw client-local state exposed read-only to the UI Lua VM as `client.*`
      (new top-level table, `src/script/ui_runtime.cpp`) — nothing here draws
      a pixel, it's queried by whatever a HUD's `render_fn` chooses to show:
    - `client.break_progress()` — nil, or 0..1 while holding to break a
      block. The hold-timer/reach/target-tracking *logic* stays engine-side
      (`src/client/main.cpp`, unchanged from 5.2) since it's gameplay input
      handling, not cosmetics; only the *drawing* moved to Lua.
    - `client.screen_size()` — `{width=.., height=..}`, since widgets take
      absolute pixel positions and a HUD centering something needs the real
      window size rather than a hardcoded guess.
- [x] `content/base/ui/hud.lua` (new): reads `client.break_progress()` and
      builds the bar from two `rect` widgets (background+border, and a fill
      whose width is `bar_w * progress`) — the exact visual the old
      hardcoded C++ produced, but every pixel of it (position, size, both
      colors, and the two-rectangle composition itself) is a Lua-side
      decision now, not an engine one.
- [x] **Bug found and fixed while wiring this in, not just the requested
      change:** `--singleplayer` never asset-syncs (no `PackRuntime`/manifest
      on that in-process path, 4.3's known gap), so `ui/*.lua` was never
      loaded there at all — every existing Lua UI screen (`base:pause`,
      `base:inventory`) was already silently dead in the most common dev/test
      path, not just the new HUD. Fixed in `src/client/main.cpp`'s
      `enter_playing`: singleplayer now reads `ui/*.lua` directly off
      `kSingleplayerContentPack` from disk (client and integrated server
      share one filesystem there, so there's nothing to "sync"); a real
      multiplayer connection is unaffected, still reading
      `client->virtual_pack_fs()`.
- [x] Verified live, not just by unit test: launched `voxel_browser.exe
      --singleplayer` (windowed), captured the mouse, held LMB on a block,
      and screenshotted mid-hold — the progress bar renders correctly at the
      expected position/fill, with zero "ui pack file failed to load" lines
      in the log. Unit tests: 4 new cases in `tests/unit/ui_runtime_test.cpp`
      (hud renders nothing until `client.break_progress()` is set, hides
      again on `nullopt`, hud `state` persists independent of a modal
      screen's open/close cycle, disabled-build stub no-ops cleanly).
- [x] HUD widgets wired to `report_click`/`report_change`/`report_list_change`
      — landed 2026-09-27. See `STATE.md` history (`state/changelog-*.md`) for the write-up.
- [x] Player list / chat log / hotbar migrated off hardcoded C++ into
      `ui.define_hud` — landed 2026-09-27. New `WidgetType::kText` (`inc/vb/
      script/ui_runtime.hpp`) is a raw, colored, alignable text draw —
      distinct from `kLabel`, which goes through raygui's un-colorable
      `GuiLabel` — with an `align` (`left`/`center`/`right`) field so Lua can
      right-align/center text without measuring its own pixel width; the
      actual `MeasureText` call lives in `vb::render::UiRenderer::draw()`,
      the raylib-linked layer, not `vb::script::UiRuntime`. Six new
      `client.*` read-only accessors mirror `client.break_progress()`/
      `client.screen_size()`: `player_name()`, `players()`, `chat_log()`,
      `chat_open()`, `inventory()`, `selected_slot()`. `content/base/ui/
      hud.lua` now composes the player list (top-right), chat scrollback
      (bottom-left), and hotbar (bottom-center) out of `rect`/`text`
      widgets, replicating the old hardcoded layout exactly. The chat
      **input box** deliberately stays a plain `GuiTextBox` in
      `src/client/main.cpp` — real keyboard text-entry capture, never a
      `UiRuntime` widget. No new interactive HUD widgets were added, so
      `report_click`/`report_change` HUD wiring (the item above) was
      unaffected by this pass. Verified: full `vb_tests` 350/350 green (no
      test exercises `content/base/ui/hud.lua` directly — evaluated only at
      real client runtime), clean build. The actual rendered result was
      **not** manually eyeballed — no GUI in this agent environment.

### 6.17 Movement/break-place bindings and hold-to-break timing as default + override  ✅ landed with a different shape (2026-09-18) — see note below

> **Direction actually taken deviates from the plan below.** A later
> 2026-09-18 request explicitly reversed this item's original framing: rather
> than giving breaking an engine-level *default* damage/duration policy
> (this section's first bullet, as originally planned), the user asked to
> "make block breaking not the default" at all — the engine should have
> **zero** built-in breaking behavior, full stop, with a content pack
> required to implement it in Lua. That's what shipped. The bullets below are
> kept for the historical record of what was originally scoped; treat only
> this note + the "what actually shipped" summary as current.
>
> **What shipped:**
> - `src/client/main.cpp`'s hardcoded hold-to-break timer
>   (`breaking`/`break_target`/`break_progress`/`kBreakSeconds`, the whole
>   5.2-era block) is gone outright, not replaced with another client-side
>   timer. The client now only ever reports raw held-button state —
>   `InputCmd::buttons`' pre-existing-but-previously-unused `kInputPrimary`/
>   `kInputSecondary` bits (LMB/RMB), already round-tripped to Lua via
>   `input.buttons.primary`/`.secondary` in `vb.on("player_input", ...)`
>   (Phase 6.3's table shape, unchanged). No new keybind names were
>   registered for this — `kInputPrimary`/`kInputSecondary` already existed
>   in the wire protocol and were simply never set by the client before now.
> - New `PlayerHandle::break_block(x, y, z)` (bound as `player:break_block()`,
>   `src/script/pack_runtime.cpp`) and a new public
>   `ServerSession::apply_script_block_edit(editor, action, pos, block)`
>   (`inc/vb/net/session.hpp` + `src/net/session.cpp`) let Lua trigger a
>   block edit exactly as if a real `C2S_BlockEdit` had arrived — same reach
>   check, same `block_break`/`on_break` hooks, same item-drop/relight/
>   fan-out pipeline — without a wire frame. This is the one new engine
>   primitive; everything else is content.
> - `content/base/mechanics.lua` (new file, picked up automatically by
>   `pack_loader.cpp`'s "any other root-level `.lua`" pass) is content/base's
>   own hold-to-break implementation: a `vb.on("player_input", ...)` handler
>   tracks per-player hold time (keyed by `player:get_name()`, same
>   convention `content/examples/kitchen_sink/keybinds.lua` already used),
>   does its own `vb.world.raycast` from the player's position + yaw/pitch,
>   and calls `player:break_block()` once `BREAK_SECONDS` (0.35, matching the
>   old constant) elapses on the same target. A pack that never loads this
>   file sees `buttons.primary` do precisely nothing — proven by
>   `tests/unit/pack_runtime_integration_test.cpp`'s new "block breaking is
>   opt-in content, not an engine default" case.
> - `InputCmd`'s Lua-facing table (`build_input_table`,
>   `src/script/pack_runtime.cpp`) gained a `dt` field (the wall-clock time
>   that cmd covers) — needed by `mechanics.lua`'s hold-time accrual, since a
>   pack has no other way to know real elapsed time per `player_input` call.
>   Backward compatible (an additive table field).
> - Movement's WASD/jump/sprint keys were pulled out of `sample_input_cmd`'s
>   inline branching into a small `MovementBindings` struct (`src/client/
>   main.cpp`) with the exact same default keys — a client-local
>   physical-key-to-axis table, not a network-visible one. What the resulting
>   `InputCmd.move`/`buttons` *do* was already fully pack-overridable
>   server-side via `vb.on("player_input", ...)` before this change (see
>   `ServerSession::handle_input_batch`'s existing `hook.replacement`
>   handling) — this only removes the "which raylib key means what" literal
>   duplication, it doesn't add a new wire mechanism. The plan's "open design
>   question" about continuous movement axes not fitting the boolean keybind
>   registry is sidestepped entirely: movement never went through
>   `vb.register_keybind` and still doesn't.
> - **Known regression, deliberately accepted:** `client.break_progress()`
>   (6.16's HUD hook) now always returns `nil` — the old local timer it read
>   from is gone, and the real replacement (server-replicated damage
>   *value*, this section's original second bullet's prerequisite) is still
>   not implemented. `content/base/ui/hud.lua` simply draws no progress bar
>   until that lands. `BlockDamageSystem`/`C2S_BlockBreakBegin`/`Stop` (6.5)
>   are untouched and still unused by `content/base` (every shipped block
>   still has the implicit `max_damage = 0`) — `mechanics.lua`'s hold timer
>   is a Lua-side clock, not a `BlockDamageSystem` consumer, same one-flat-
>   duration-for-every-block posture the old C++ timer had.
> - Full `vb_tests` green (283/283, up from 282) on `build-net-lua`; all 4
>   CTest cases pass.
>
> Original plan (superseded, kept for context):

> User-requested (2026-09-18), after being surprised that (a) WASD/LMB-break/
> RMB-place are 100% hardcoded C++ with no pack involvement at all, and (b)
> the hold-to-break duration doesn't persist/heal across repeated attempts on
> the same block. Both trace back to the same root cause: `content/base`
> ships every block with `max_damage = 0`, so `src/client/main.cpp`'s
> original 5.2 hold-to-break timer — a fixed, local-only, non-authoritative
> `kBreakSeconds = 0.35` constant — is still what governs breaking for every
> block in the game today, and the real, already-built, already-overridable
> `vb::world::BlockDamageSystem` (6.5) never gets consulted at all. This is
> the exact gap 6.5's own last bullet already flagged ("the existing 5.2
> hold-to-break timer is untouched and still governs every `max_damage == 0`
> block, which is every block in `content/base` today") — this item is where
> that finally gets closed, plus the separate, never-before-tracked input-
> binding gap. Matches the project's stated core philosophy (`ARCHITECTURE_SPEC.md`
> §7/§17): engine ships a sane default, a pack can override as much of it as
> it wants, same shape as `vb.physics.set_params` (6.7) and
> `vb.daynight.set_curve` (6.8).

- [ ] **Give breaking a real, overridable default duration+retention+heal
      policy instead of nothing.** `block_damage.hpp` is explicit that the
      engine "ships zero built-in accrual/heal policy" — that's the actual
      bug the user hit ("I don't feel like the block is retaining the break
      value... duration is still the same across multiple attempts"): with
      no pack `block_break_tick`/`block_health_tick` handlers registered
      (true for `content/base`), and `max_damage == 0` on every block, there
      is no damage value at all to retain — every hold is an independent
      local timer, by design, not a bug in `BlockDamageSystem` itself.
      Two changes needed together:
    - Engine-level **default** `max_damage` (e.g. derived from a new
      `vb.blocks.set_break_defaults{seconds = 0.35, ...}` global, applied to
      any block that doesn't explicitly set its own `max_damage`) instead of
      today's implicit 0 — so out-of-the-box breaking already goes through
      `BlockDamageSystem`, not the parallel client timer.
    - Engine-level **default** `damage_tick_fn`/`health_tick_fn` policy
      (currently only ever pack-supplied): a straightforward "1/`seconds`
      damage per contributing-player-tick; after `heal_after_seconds` idle,
      heal back at `heal_rate` per tick" default, overridable exactly like
      today by registering `vb.on("block_break_tick"/"block_health_tick",
      ...)` (pack handler replaces the default entirely, same "no built-in
      policy once you opt in" semantics already documented for those hooks).
      `vb.blocks.set_break_defaults{...}` is the pack-facing knob for tuning
      the default without writing full tick handlers, mirroring
      `vb.physics.set_params`'s "override individual fields on top of a
      built-in default" shape.
- [ ] **Wire the client to the real system instead of the parallel timer.**
      `ClientSession::send_block_break_begin`/`send_block_break_stop`
      (6.5) already exist, tested, unused. Replace `src/client/main.cpp`'s
      local `breaking`/`break_target`/`break_progress`/`kBreakSeconds` block
      with: send `C2S_BlockBreakBegin` on first LMB-down over a voxel /
      `C2S_BlockBreakStop` on release-or-retarget, and read progress back
      from server-replicated damage state rather than a local clock — which
      needs the still-deferred "replicate the damage *value*, not just
      begin/stop/complete" half of 6.5 (`BlockDamageTickResult::changed`/
      `cleared`, currently computed and thrown away) finished as a
      prerequisite. `client.break_progress()` (6.16) keeps its exact same
      signature (nil | 0..1) so `content/base/ui/hud.lua` needs no changes.
      Placing (RMB, always instant) is unaffected. **Update (6.20,
      2026-09-19): placing was later decoupled the same way — see that
      section.**
- [ ] **Movement/action key bindings as a pack-overridable default**, not
      just a client-local rebind (5.3's still-"not attempted" keybindings
      screen is a *different*, complementary gap — physical-key-to-action
      storage/UI on one player's machine; this item is the pack/engine
      default those local rebinds would apply on top of). Today WASD
      (`sample_input_cmd`, `src/client/main.cpp:351-360`) and LMB-break/
      RMB-place (same file, the block-edit input block) are compiled-in
      constants with no pack seam at all — unlike literally every other
      Phase 6 system, a pack cannot change what triggers movement or
      breaking/placing. Proposed shape, following 6.3's existing
      `vb.register_keybind`/`S2C_KeybindRegistry` substrate rather than
      inventing a second mechanism: extend that registry to cover the
      engine's own built-in core actions (`move_forward`, `move_back`,
      `move_left`, `move_right`, `jump`, `sprint`, `sneak`, `break`,
      `place`) with their current hardcoded keys as the pre-registered
      defaults, so `vb.rebind_keybind("break", key)`-style pack overrides
      and (later, 5.3) a real settings-screen UI both write into the one
      registry instead of two separate ones. **Open design question, not
      resolved here:** whether core movement axes (continuous, analog-ish)
      fit the existing keybind registry's boolean-per-tick shape at all, or
      need their own parallel `vb.movement.set_bindings{...}` — decide
      during implementation, not speculatively here.
- [ ] Tests: extend `tests/unit/block_damage_test.cpp` for the new default
      policy (accrual without any registered handler, heal-after-idle
      without any registered handler, a pack override replacing just the
      default cleanly); a `pack_runtime_integration_test.cpp` case proving a
      `content/base`-equivalent pack (no handlers registered at all) now
      retains damage and heals over multiple attempts on one block; update
      `content_pack_test.cpp` if `content/base`'s shipped blocks' effective
      `max_damage` changes as a result.

**Resolved as 6.19 (2026-09-19):** Movement/action key bindings extended into
the 6.3 keybind registry — `move_forward`/`move_back`/`move_left`/
`move_right`/`jump`/`sprint`/`primary`/`secondary` are pre-registered by
every `PackRuntime` before any pack script runs, so a pack can read
`input.keybinds["jump"]` etc. exactly like a custom keybind, resolving the
open design question above (movement axes are already boolean, so they fit
the existing keybind shape with no format change). Physical-key rebinding
landed separately as Phase 5.3's keybindings screen — this item's own scope
(name registry, not physical keys) is fully closed.

### 6.18 Growtopia-style combat: discrete punching replaces hold-to-break  ✅ done (2026-09-18)

> User-requested pivot (2026-09-18), immediately after 6.17 landed: rather
> than Minecraft-style continuous holding (even the Lua-side hold timer
> 6.17 shipped in `content/base/mechanics.lua`), attack should be a discrete
> "punch" — one per click, Growtopia-style — that can land on either a block
> or a player (PvP), whichever is in front. Explicit design constraints from
> that discussion: (1) movement must **not** move to a raw-key-event model —
> it stays exactly as 6.17 left it (continuous `InputCmd.move`/`buttons`,
> already fully pack-overridable via `vb.on("player_input", ...)`, so
> client-side prediction is untouched); (2) the engine, not Lua, resolves
> *what* a punch hits — a pack only decides *when* to call `player:punch()`.
>
> **What shipped:**
> - New public `ServerSession::punch(NetId puncher) -> PunchResult`
>   (`inc/vb/net/session.hpp` + `src/net/session.cpp`) is the one new engine
>   primitive. It raycasts blocks (`world::raycast_voxel`, reused as-is) and
>   nearby players — modeled as a vertical cylinder (feet at their tracked
>   position, top at `MoveParams::height` above it, radius
>   `PunchParams::hit_radius`) that the puncher's look-ray passes through, not
>   a single point — along the puncher's own authoritative yaw/pitch
>   (`replication::EntityState::rot`, not anything client-reported), and
>   picks whichever candidate is closer along the ray. A player hit calls
>   the existing `damage_player()` (Phase 6.6, unchanged); a block hit
>   increments a new sparse `unordered_map<IVec3, uint16_t>
>   block_punch_counts_` and, once it reaches the target's
>   `BlockType::max_damage` (0 still means "break on the first punch", same
>   meaning it always had), commits the break through the `apply_script_
>   block_edit()` primitive 6.17 already added (reach check, hooks, drops,
>   relight, fan-out — unchanged, unaware punching exists at all). A veto'd
>   break (a pack's `block_break` hook returns false) leaves the punch count
>   at `max_damage` rather than resetting it, so the very next punch retries
>   instead of needing `max_damage + 1` hits.
> - `vb.combat.set_params{reach=, hit_radius=, player_damage=}` (new
>   `PackRuntime::effective_punch_params()`, mirrors `vb.physics.set_params`'s
>   exact "override individual fields on top of a built-in default" shape)
>   lets a pack tune the defaults (`reach = 5.5`, `hit_radius = 0.6`,
>   `player_damage = 1.0`) without touching engine code. Wired into both
>   `src/server/main.cpp` and `--singleplayer`'s `Singleplayer` constructor
>   (`src/client/main.cpp`), same two call sites `effective_move_params`
>   already has.
> - `player:punch()` (`PlayerHandle::punch`, `src/script/pack_runtime.cpp`)
>   is the Lua-facing wrapper — returns a table (`hit_player`, `target`,
>   `hit_block`, `x`/`y`/`z`, `punches`, `broken`) so a pack can react (swing
>   VFX, a hit-marker) without being required to.
> - `content/base/mechanics.lua` was rewritten (no longer a hold-timer at
>   all): a `vb.on("player_input", ...)` handler tracks the previous tick's
>   `buttons.primary` per player (same `was_down`/edge-detection idiom
>   `content/examples/kitchen_sink/keybinds.lua` already established) and
>   calls `player:punch()` exactly once per rising edge — click once, punch
>   once, regardless of how long the button stays held afterward. `BREAK_
>   SECONDS`/the per-position hold-time table from 6.17's version are gone;
>   there is no "holding" concept left in this file at all.
> - New shared math helper `core::forward_from_yaw_pitch(yaw_deg, pitch_deg)`
>   (`inc/vb/core/math.hpp`) — the same trig `FirstPersonController::
>   forward()` already had inline, now also used server-side (no camera
>   object exists there) to compute a puncher's look direction from their
>   authoritative yaw/pitch. `camera.hpp`'s `forward()` now delegates to it
>   instead of duplicating the formula.
> - Movement is untouched, as scoped: `MovementBindings`
>   (`src/client/main.cpp`, 6.17) and the underlying `InputCmd.move`/
>   `buttons` pipeline are exactly as they were.
> - New tests (`tests/unit/blockedit_test.cpp`, plain-`ServerSession` style,
>   no Lua/PackRuntime involved — this file already had that pattern for
>   block edits): a `max_damage == 0` block breaks on the first punch; a
>   custom `max_damage = 3` block (built via `BlockRegistry::add_or_get`, not
>   through Lua) takes exactly three punches, reporting an accurate running
>   count each time; punch() prefers a closer player over a block further
>   along the same ray; punch() hits nothing when both are out of reach.
>   6.17's own `pack_runtime_integration_test.cpp` case (proving
>   `buttons.primary` alone does nothing without a pack handler, and that a
>   minimal handler calling `player:break_block()` works end-to-end) is
>   untouched and still passes — `break_block()` itself wasn't removed, it's
>   just no longer what `content/base` calls directly.
> - **Follow-up, same day:** block self-heal. A block that stops taking
>   punches now heals back to full over time instead of an accumulated
>   count sitting there forever — `PunchParams` gained `heal_after_seconds`
>   (default 4.0: idle time since the last landed punch before healing
>   starts) and `heal_interval_seconds` (default 1.5: -1 punch every this
>   many seconds once eligible), both pack-overridable via
>   `vb.combat.set_params{heal_after_seconds=, heal_interval_seconds=}`
>   alongside `reach`/`hit_radius`/`player_damage`. Unlike 6.5's
>   `BlockDamageSystem` (which ships *zero* heal policy until a pack
>   supplies one), this is a real engine default — only the rate is a
>   pack-facing knob, not whether healing happens at all, since punching has
>   no begin/stop lifecycle for a pack to hang a policy off of the way
>   holding a target did. `block_punch_counts_`'s value type grew from a
>   bare `uint16_t` into `PunchDamageState{punches, idle_seconds,
>   heal_progress}`; a new per-tick `ServerSession::
>   update_block_punch_healing(dt)` (called from `tick()` right after the
>   unrelated `update_block_damage()`) decrements idle entries and erases
>   ones that fully heal. Landing a punch resets both timers on that block
>   (a fresh hit un-does any partial healing progress, it doesn't add to
>   it); a negative `heal_after_seconds` disables healing entirely. Three
>   new `blockedit_test.cpp` cases: an idle block heals fully back to 0 and
>   the next punch starts fresh from 1; a punch landing again resets the
>   idle clock instead of the heal continuing to count from before it; the
>   existing "N punches to break" case is unaffected (no idle gap, no
>   healing kicks in). Full `vb_tests` green (289/289, up from 287).
>
> **Known simplifications, not attempted:** no punch-rate cooldown enforced
> engine-side (a pack that doesn't edge-detect, or a macro, could call
> `punch()` every tick — left as the calling pack's responsibility, same
> "engine provides the primitive, doesn't guess at abuse policy" posture as
> everywhere else); no swing animation/cooldown-visual on the client; PvP
> damage has no armor/cooldown/knockback, just a flat `player_damage` per
> landed punch.

**Resolved: punch-rate cooldown, landed 2026-09-28** as
`PunchParams::punch_cooldown_seconds` (default `0.0`, disabled — every
existing direct-`punch()` test/call site is unaffected unless a pack opts
in). New `Conn::punch_cooldown_remaining`, ticked down once per server tick
in `system_network_io()`. `ServerSession::punch()` rejects a call still on
cooldown and **arms the next cooldown up front**, before target resolution
— a whiff costs the same swing time a landed hit would. Pack-facing via
`vb.combat.set_params{punch_cooldown_seconds=...}`, deliberately opt-in (no
engine default swing rate). Verified: full `vb_tests` 392/392 green (2 new
`blockedit_test.cpp` cases: a configured cooldown rejects an immediate
second swing but lets a later one land once real time has ticked past, and
the disabled-by-default case proves two back-to-back swings both land),
clean `-Werror` build. **Still open, unchanged by this pass:** no swing
animation; PvP has no armor/knockback — this item only closed the cooldown
half of its own original scope.

### 6.20 Right-click placing decoupled from the engine (last hardcoded block edit closed)  ✅ done (2026-09-19)

> User-flagged (2026-09-19): right-click placing was still 100% hardcoded
> C++ with no pack seam at all — 6.17/6.18 decoupled breaking, but their own
> writeups explicitly noted "Placing (RMB, always instant) is unaffected."
> `src/client/main.cpp` raycast on `MOUSE_BUTTON_RIGHT` press and sent a
> `C2S_BlockEdit{kPlace, base_block::stone}` directly, bypassing the
> `input.buttons.secondary` channel that was already being reported to Lua
> (6.17 wired it up, nothing ever read it). Closed the same way breaking was:
> the engine keeps only the validated primitive, a pack decides *when* and
> *what*.

- [x] New `player:place_block(x, y, z, block)` (`PlayerHandle::place_block`,
      `src/script/pack_runtime.cpp`) — same shape as `break_block()`, just
      calls the pre-existing `ServerSession::apply_script_block_edit()` with
      `BlockEditAction::kPlace` and a caller-chosen block id instead of a
      fixed one. No engine/session changes needed — `apply_script_block_edit`
      already took an arbitrary `action`/`block` pair, breaking was just the
      only caller.
- [x] `src/client/main.cpp`'s hardcoded RMB block-edit send (raycast +
      `client->push_block_edit(C2SBlockEdit{kPlace, base_block::stone})`) is
      gone outright — the client still runs the same raycast for the
      crosshair-highlight visual, but no longer acts on a right-click itself;
      `input.buttons.secondary` (already round-tripped since 6.17) is the
      only signal left. `edit_seq`/`client->push_block_edit()` are now dead
      in the windowed client path (the dead `edit_seq` member was removed;
      `push_block_edit` itself is left in place as a real, tested
      `ClientSession` API other callers/tests use).
- [x] `content/base/mechanics.lua` gained a second edge-detected handler
      alongside the existing punch one: on a rising edge of
      `input.buttons.secondary`, it raycasts from the player's authoritative
      `get_pos()` + a hardcoded `EYE_HEIGHT = 1.62` (mirrors
      `physics::MoveParams::eye_height`'s engine default — there's no
      `vb.physics.get_params()` to read an override back) along
      `input.yaw`/`input.pitch`, and calls `player:place_block()` with
      `base_stone_id` (the global `content/base/blocks/stone.lua` sets,
      loaded before this root-level file per `pack_loader.cpp`'s ordering) —
      i.e. it places exactly what the old hardcoded path placed. A pack that
      never loads this file now sees `buttons.secondary` do nothing, same
      "opt-in content, not an engine default" posture 6.17 established for
      breaking.
- [x] Full `vb_tests` green (290/290) on `build-net-lua`; no protocol/wire
      change needed (`apply_script_block_edit` was already generic).
- [x] Held item / hotbar selection — landed 2026-09-25 (entity-management
      follow-up, closes this gap). Placing used to always place a hardcoded
      `base_stone_id` regardless of what the player was carrying, because no
      primitive existed for a pack to ask "what slot is this player's hotbar
      on, and what's in it". Wire protocol bumped **21 -> 22**: `InputCmd`
      (within `C2S_InputBatch`) gains a `u8 selected_slot` (0-based), reported
      every cmd exactly like `buttons`/`keybinds` — the client reads number
      keys 1-9 into a persistent `selected_slot` local (gated on
      `mouse_captured`) and outlines the selected slot in the hotbar HUD.
      Server-side, `ecs::PlayerInput::selected_slot` mirrors the latest
      processed value, readable via `ServerSession::selected_slot(NetId)`; a
      pack's `vb.on("player_input", ...)` handler chain can also override it
      (1-based to match `player:get_inventory()`'s own 1-based array), same
      veto/replace shape `PlayerInputOverride` already gave `move`/`yaw`/
      `pitch`/`buttons`/`keybinds`. Two new Lua primitives on `PlayerHandle`
      close the actual gap: `player:get_selected_slot()` (1-based) and
      `player:get_held_item()` (resolves that slot against the player's real
      inventory — `nil` if empty/out-of-range). `content/base/mechanics.lua`'s
      right-click placing now reads `player:get_held_item()` instead of the
      hardcoded stone id — an empty/out-of-range slot places nothing, and a
      successful placement spends one unit via `player:take()`, so placing
      is a real inventory drain now rather than an infinite stone dispenser.
      **Gotcha hit while wiring the Lua bindings:** `pack_runtime.cpp`'s
      `PlayerHandle` usertype (20+ methods, all through sol2's template-heavy
      `new_usertype`) was already close enough to MSVC's object-file
      section-count ceiling that adding these two pushed it over (`fatal
      error C1128: number of sections exceeded object file format limit`) —
      fixed with a per-source `/bigobj` on `pack_runtime.cpp` only; a second
      gotcha on the way there: `set_source_files_properties()` is
      directory-scoped to where it's *called*, not where the consuming
      target is defined, so setting it from `src/script/CMakeLists.txt`
      silently never reached `vb_core`'s build rule until adding CMake
      3.18's `TARGET_DIRECTORY vb_core` argument. Verified: full `vb_tests`
      green, including a `netcode_test.cpp` round-trip case for
      `InputCmd::selected_slot`, a `pack_runtime_test.cpp` case for the
      no-session default, and a `pack_runtime_integration_test.cpp` case
      proving a real client's `InputCmd::selected_slot` reaches both
      accessors end-to-end. The actual hotbar-highlight rendering was **not**
      manually eyeballed — no GUI in this agent environment.

### 6.21 Interaction reach + eye-height: default + override + read-back  ✅ done (2026-09-22)

> Found while reviewing 6.20 (2026-09-19): `content/base/mechanics.lua`'s new
> placing raycast hardcodes `EYE_HEIGHT = 1.62` to mirror
> `physics::MoveParams::eye_height`'s engine default, and a `max_dist = 5.0`
> to mirror the old hardcoded C++ path — but the block-edit reach actually
> enforced server-side, `WorldReplicator::kMaxReachBlocks`
> (`src/net/world_replicator.cpp`), is a *different*, non-pack-overridable
> hardcoded constant (5.5), unlike `PunchParams::reach` (also 5.5 by default,
> but real pack-overridable via `vb.combat.set_params`). Two inconsistencies
> at once: (1) a Lua script has no way to read back an *effective*
> pack-overridden physics value it needs for its own raycast, so an override
> can silently desync Lua's target-selection from the engine's actual
> validation; (2) block-edit reach follows a different rule (hardcoded, no
> override) than combat reach (default + override) for what is conceptually
> the same "how far can this player interact" number.
>
> **User-directed resolution (2026-09-19):** stay consistent with the
> established Phase 6 shape — give it a real default, make it overridable,
> *unless* the runtime cost of allowing a live override is too high, in which
> case restrict the override to init time only. In practice that's a
> non-issue: every existing `set_params`-style override (`vb.physics`,
> `vb.combat`, `vb.daynight`) already only applies **before**
> `PackRuntime::freeze()` (`"registry already frozen"` once a pack tries
> after load) — reach/eye-height would use the exact same mechanism, so
> there's no new hot-path cost versus what gravity/walk-speed/punch-reach
> already pay (one extra struct field read per interaction, not a per-tick
> cost). A read-back accessor is also wanted regardless of the override
> question, specifically so Lua never has to hardcode/guess an engine
> constant that might not match a pack's own override.

- [x] **Sub-question resolved (2026-09-19), implemented (2026-09-22): new
      `vb.action` namespace, not `vb.combat`.** Block-edit reach and punch
      reach are unified into one value under a new
      `net::ActionParams` struct (`inc/vb/net/world_replicator.hpp`, sibling
      to `BlockEditHooks`) — `WorldReplicator::set_reach()`/`reach()` hold it,
      read by both `in_reach()`/`apply_block_edit()` and (via
      `world_replicator()->reach()`) `ServerSession::punch()`
      (`src/net/session.cpp`). `WorldReplicator`'s old hardcoded
      `kMaxReachBlocks` constant and `ServerSession::PunchParams::reach` are
      both gone as independent values. Exposed to Lua as
      `vb.action.set_params{reach=...}` (new `sol::table` in `vb["action"]`,
      `src/script/pack_runtime.cpp`, frozen-check same as every other
      `set_params`) / `PackRuntime::effective_action_params(base)` (same
      `effective_*_params(base)` pattern 6.7/6.18 already use) —
      deliberately its own namespace, not `vb.combat.set_params`, since a
      pack author hunting for "the block-mining-reach knob" has no reason to
      look inside something named `combat`. `vb.combat.set_params` keeps
      only what's genuinely combat-specific (`hit_radius`, `player_damage`,
      `heal_after_seconds`, `heal_interval_seconds`) — `reach` was removed
      from both `combat_tbl`'s handled fields and `effective_punch_params`.
      No shipped pack called `vb.combat.set_params{reach=...}` before this
      (verified 2026-09-19), so there was no real migration to carry.
      `vb.action` is left open as the natural home for any *other* future
      cross-cutting "player action" primitive.
- [x] Read-back accessors added — `vb.physics.get_params()` (full effective
      `physics::MoveParams` as a plain table, including `eye_height`) and
      `vb.action.get_params()` (effective `reach`) — both in
      `src/script/pack_runtime.cpp`, returning the *effective* (post
      pack-override) values by applying the pack's captured override table
      over an engine-default-constructed base, the same base every real call
      site (`src/server/main.cpp`, `src/client/main.cpp`) starts from.
- [x] `content/base/mechanics.lua`'s placing handler now calls
      `vb.physics.get_params().eye_height` / `vb.action.get_params().reach`
      instead of its own hardcoded `EYE_HEIGHT = 1.62` / `max_dist = 5.0`
      constants (both removed).
- [x] Wired into both binaries' startup: `src/server/main.cpp` calls
      `session.world_replicator()->set_reach(pack_runtime
      .effective_action_params(vb::net::ActionParams{}).reach)` right after
      `set_punch_params`; `src/client/main.cpp`'s `--singleplayer` path calls
      the equivalent on its own `replicator` before handing it to
      `set_world_replicator`, mirroring `move_params`'s existing
      config-then-pack-override precedent.
- [x] Tests: a new `blockedit_test.cpp` case
      ("`WorldReplicator::set_reach` widens both block-edit reach and
      `punch()` reach from the same one value") proves a single
      `set_reach()` call moves both `in_reach()`'s and `punch()`'s
      accept/reject boundary together, with a real `EditWorld`/`ServerSession`
      (no Lua). New `pack_runtime_test.cpp` cases cover
      `vb.action.set_params` (override applied, rejected after `freeze()`),
      the built-in default with no override, and
      `vb.physics.get_params()`/`vb.action.get_params()` round-tripping the
      effective values both before and after `set_params`. Full `vb_tests`
      green (294/294) on `build-net-lua`; `voxel_browser`/
      `voxel_browser_server` also rebuild clean.

### 6.22 Fall damage, PvP proof, hunger, mob damage ✅ done (2026-09-27/28)

- [x] **6.22: fall damage.** Landed 2026-09-27. New `net::ServerSession::
      set_landed_hook(fn(NetId, double impact_speed))` — fires once per
      player exactly on the tick a fall is arrested by hitting ground
      (`handle_input_batch`'s per-cmd loop compares `on_ground` before/after
      each `physics::step_movement` call; `impact_speed` is the pre-step
      downward velocity, captured before that tick's own small gravity
      increment). Raw notification only, no built-in formula/threshold —
      same "mechanism, not policy" posture as `RegionHooks`/`BlockBreakHooks`;
      `vb.on("player_landed", function(player, impact_speed) ... end)` is the
      new Lua event (only installed when a pack actually registers one).
      `content/base/fall_damage.lua` (new) is the reference policy: no
      damage below an 8 m/s safe-speed threshold, then 1 HP per m/s above
      it, via the existing `player:damage(amount, "fall")` primitive (6.6) —
      no protocol change (server-local hook, nothing replicated). Verified:
      full `vb_tests` 371/371 green (1 new `pack_runtime_integration_test.cpp`
      case drives a real gravity fall over `LoopbackTransport` — spawn a
      player 5 blocks above real generated terrain with no jump input, pump
      real 0.05s ticks, confirm the hook fires exactly once at a plausible
      impact speed and never again while resting on the ground), clean
      `-Werror` build. Not manually eyeballed in a real window (no GUI in
      this agent environment) — though this item has no rendering
      component at all, so that caveat matters less here.
- [x] **PvP half confirmed already real, not stale text** — checked
      2026-09-28. `ServerSession::punch()` (Phase 6.18/6.20) already resolves
      the nearest other player along the puncher's look-ray (a forgiving
      vertical-cylinder hitbox, capped at the shared `reach`, closer of
      {player, block} wins) and calls `damage_player(target,
      punch_params_.player_damage, "pvp")` on a hit; `content/base/
      mechanics.lua` already dispatches `player:punch()` on every left-click
      rising edge — so a real pack has driven real PvP since those phases
      landed, this item's own text was just never updated. What was
      actually missing was proof, not code: the existing `blockedit_test.cpp`
      "punch() prefers a closer player over a block" case only checked
      `PunchResult::hit_player`/`target`, never that repeated hits actually
      reduce health and kill/respawn the victim. New `netcode_test.cpp` case
      ("PvP: repeated punches reduce health and kill + respawn the target,
      with cause 'pvp'") closes that proof gap: two plain `ServerSession`
      connections, a `set_respawn_handler` capturing the real `(player,
      cause, health_before)` tuple, 20 direct `punch()` calls (default
      health 20, default `player_damage` 1/punch) landing exactly on empty
      on the 20th, confirming `cause == "pvp"`, `health_before == 1.0` (the
      value *before* the killing blow), and the victim's position snapping
      back to their own spawn point. Verified: full `vb_tests` 416/416
      green (1 new case), clean `-Werror` build.
- [x] **Hunger primitive — landed 2026-09-28.** New `vb::ecs::Hunger`
      component (current/max, default 100/100, emplaced alongside `Health`
      at player join) plus `ServerSession::HungerParams`
      (`decay_per_second`/`starvation_damage_per_second`, both `0.0` by
      default — decay stays fully disabled until a pack opts in, same
      "engine ships the mechanism at a value that changes nothing" posture
      as `PunchParams::punch_cooldown_seconds`) and a new `"update_hunger"`
      `SystemRunner` phase, placed right before `check_respawns` so same-tick
      starvation damage is seen by that same tick's death check.
      `ServerSession::player_hunger()`/`add_player_hunger()` are the
      read/spend-or-restore accessors; a respawn always resets hunger to max
      alongside health. Pack-facing: `vb.hunger.set_params{decay_per_second=,
      starvation_damage_per_second=}`/`get_params()` (mirrors `vb.action`'s
      own shape) and `player:get_hunger()`/`add_hunger(amount)` (mirrors
      `player:damage()`'s shape). **Real pre-existing bug found and fixed in
      the same pass, unrelated to hunger itself:** `effective_action_params()`
      (Phase 6.21) had **no** stub at all in `pack_runtime.cpp`'s
      `!VB_WITH_LUA` block, even though `src/server/main.cpp`/
      `src/client/main.cpp` call it unconditionally — a real link error
      waiting for anyone who actually configures `-DVB_WITH_LUA=OFF`, never
      caught because no CI workflow does. Fixed alongside adding
      `effective_hunger_params()`'s own stub. **Deliberately out of scope:**
      no client HUD/protocol replication for hunger — server-side
      bookkeeping only, same posture the despawn-on-health item established
      for script-entity health. Verified: full `vb_tests` 420/420 green (5
      new cases: default-disabled decay plus a configured rate, starvation
      damage killing + respawning with cause `"hunger"` and hunger restored
      to max, `vb.hunger.set_params`/`get_params` round-trip + freeze
      rejection, and a `pack_runtime_integration_test.cpp` case proving
      `player:get_hunger()`/`add_hunger()` and a configured decay driving a
      real starvation death through a real session), clean `-Werror` build.
- [x] **Mob damage — landed 2026-09-28** as `content/base/entities/
      zombie.lua`: a real hostile mob (20 hp, `vb.register_entity{health=20}`)
      spawned via `/zombie`, that chases down and bites (2 dmg every 1s once
      in range) whichever player spawned it, using the existing
      `entity:damage()`/`player:damage()` primitives — no new engine-side
      mechanism was needed for the mob concept itself, confirming this
      item's own original text.
      **A real, previously-unknown engine bug was found and worked around
      while building this** (not fixed — see `remaining_tasks/cross_cutting.md`
      for the bug itself, kept separate since it's a real, still-open engine
      correctness gap in its own right, not specific to mob AI): a
      `PlayerHandle` stored across calls (in an entity's own field, a
      module-level Lua table, or even a bare upvalue — all three were
      tested) and then read back from *inside* `vb.on("tick", ...)` or a
      script entity's own `on_tick` returns silently corrupted data (wrong
      `rt`/`session` pointers, wrong `net_id` — confirmed with ASan, not
      just inferred from a crash). Calling a method on a **freshly
      passed-in** `PlayerHandle` from within the *same* handler call (the
      pattern every pre-existing content/base script already uses) is
      completely unaffected. `zombie.lua`'s chase/attack logic is therefore
      driven from `vb.on("player_input", ...)` instead of the zombie's own
      `on_tick` — that handler already hands over a fresh `PlayerHandle` on
      every real `InputCmd`, used immediately, never stored; only the
      zombie's own entity handle (`self:get_pos()`/`set_pos()`, which never
      touches a `PlayerHandle` at all) is stored across calls, keyed by the
      hunted player's *name* (a plain string, safe to snapshot).
      Verified: full `vb_tests` 421/421 green (1 new `content_pack_test.cpp`
      case driving a real `/zombie` command through 300 real
      `InputCmd`-pumped ticks to an actual kill, confirming
      `cause == "zombie"`), clean `-Werror` build.

---

### Moved from `REMAINING_TASKS.md`'s core (dream, 2026-10-06)

> Verbatim text of the core file's "Remaining" list for this section at the
> time of the move; the core now keeps a one-line summary.

- [x] **6.22: fall damage** — landed 2026-09-27 as `net::ServerSession::
      set_landed_hook(fn(NetId, impact_speed))`, firing once per player on
      the tick a fall is arrested. `vb.on("player_landed", ...)` is the new
      Lua event (opt-in, mechanism not policy); `content/base/
      fall_damage.lua` is the reference policy (8 m/s safe threshold, 1 HP
      per m/s above it).
- [x] **PvP confirmed already real, not stale text** — checked 2026-09-28.
      `ServerSession::punch()` already resolved player-vs-player hits and
      `content/base/mechanics.lua` already dispatched them; what was
      missing was proof, closed by a new `netcode_test.cpp` case driving 20
      punches to a confirmed kill+respawn with `cause == "pvp"`.
- [x] **Hunger primitive** — landed 2026-09-28. New `vb::ecs::Hunger`
      component + `ServerSession::HungerParams` (decay/starvation rates,
      both `0.0` by default), a new `"update_hunger"` `SystemRunner` phase,
      and `vb.hunger.set_params{}`/`player:get_hunger()`/`add_hunger()`.
      Server-side bookkeeping only, no client HUD/replication. Also fixed a
      real pre-existing link-error gap: `effective_action_params()` (6.21)
      had no `!VB_WITH_LUA` stub at all.
- [x] **Mob damage** — landed 2026-09-28 as `content/base/entities/
      zombie.lua`, a real hostile mob using existing damage primitives —
      no new engine mechanism needed. **Found and worked around, not fixed,
      a real engine bug while building this:** a `script::PlayerHandle`
      stored across calls and read back from inside `vb.on("tick", ...)` or
      an entity's own `on_tick` silently returns corrupted data (confirmed
      with ASan) — see Cross-Cutting's own entry below for the full repro.
      `zombie.lua` works around it by driving chase/attack from
      `vb.on("player_input", ...)` instead, which always hands over a fresh
      handle.
- [x] Automatic despawn-on-health trigger for generic script entities —
      landed 2026-09-25. Opt-in via `vb.register_entity{health=...}`;
      `entity:damage()` now decrements real HP and auto-despawns at 0
      instead of requiring the pack to track it and call `:remove()`.
- [x] Client-side kind-specific rendering for script entities — landed
      2026-09-25 (`EntityKind` id drives billboard width/height via
      `S2C_EntityKindRegistry`, protocol 19). Real per-kind sprite art
      landed the same day, see Phase 4's own entry.
- [x] Per-connection rate limiting on custom-keybind events — closed
      2026-09-27 as part of Phase 3's flood guard (same token bucket
      already covers keybind bits riding inside `InputCmd`).
- [x] Replicate block-damage *value* to nearby players — landed 2026-09-27.
      `S2C_BlockDamage` (protocol 25) fans out every live punch-count
      change; `client.break_progress()` (6.16) now reports a real fraction
      instead of always `nullopt`, with a default darkening crack overlay.
- [x] Real crack-stage texture art + `crack_texture` override — landed
      2026-09-27, closing 6.5 in full (protocol **25 -> 26**). New
      `vb::render::CrackAtlas`/`CrackOverlay` replace the flat overlay with
      real per-stage texture art. Also fixed a real pre-existing bug: three
      `BlockRegistryRecord` call sites were silently dropping `max_damage`
      on every hop, so `break_progress()` was permanently `nullopt`.
- [x] Movement/action key bindings extended into the 6.3 keybind registry
      (6.19) — movement/primary/secondary are pre-registered by every
      `PackRuntime`, so a pack reads them like any custom keybind.
      Physical-key rebinding is Phase 5.3's separate keybindings screen.
- [x] HUD widgets wired to `report_click`/`report_change`/`report_list_change`
      — landed 2026-09-27.
- [x] Player list / chat log / hotbar migrated off hardcoded C++ into
      `ui.define_hud` — landed 2026-09-27. New `WidgetType::kText` (colored,
      alignable) plus 6 new `client.*` read-only accessors; the chat input
      box itself deliberately stays a plain `GuiTextBox`, never a widget.
- [x] No punch-rate cooldown enforced engine-side — landed 2026-09-28 as
      `PunchParams::punch_cooldown_seconds` (default `0.0`, opt-in). A whiff
      arms the next cooldown just like a landed hit. Still open: no swing
      animation, no PvP armor/knockback.
- [x] Held item / hotbar selection — landed 2026-09-25, closing 6.20's own
      gap. Protocol **21 -> 22**: `InputCmd` gains `u8 selected_slot`.
      `player:get_selected_slot()`/`get_held_item()` let placing read what's
      actually in hand instead of an infinite hardcoded stone dispenser.
- [x] **6.21:** block-edit reach and punch reach unified into one
      pack-overridable `net::ActionParams::reach`, replacing
      `WorldReplicator`'s old hardcoded constant. New
      `vb.action.set_params{reach=}`/`get_params()` and
      `vb.physics.get_params()` (its own namespace, not `vb.combat`).

### Phase 6 leftovers closed (2026-10-17)

- **Swing animation:** `PunchParams::swing_clip` / `swing_clip_seconds`
  (`vb.combat.set_params`). Every `punch()` that clears the cooldown sets the
  puncher's replicated clip override (the existing `entity:set_clip` channel,
  so no protocol change) and clears it afterwards unless the pack replaced it.
  `content/base` turns it on; `textures/player.png` gained two `swing` columns.
- **PvP armor / knockback:** `player:set_armor(fraction)` / `get_armor()`
  (clamped to `[0, 0.95]`, PvP damage only, persists across respawn);
  `knockback_speed` / `knockback_lift` set the victim's velocity along the
  puncher's look direction (off by default, armor doesn't reduce it).
- **UI `sprite` widget + `client.my_appearance()`:** `sprite` draws a pack PNG
  (`texture`, optional `src = {x,y,w,h}`, `color` tint); `UiRenderer` loads it
  from the synced pack or, in singleplayer, off disk. `my_appearance()` returns
  the own `visual_override` (or nil) so a preview can stack one sprite per layer.
- Tests: three `blockedit_test.cpp` cases, two `ui_runtime_test.cpp` cases.
