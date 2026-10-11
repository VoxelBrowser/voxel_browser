# Voxel Browser — Lua Content API Reference

> **Looking something up?** Use the generated quick reference,
> [`lua-reference/README.md`](lua-reference/README.md): every function with its
> signature, one example and a context badge. It is generated from the LuaCATS
> stubs in `sdk/lua/library/` (the source of truth, also what editors read), and
> `lua_api_surface_test` fails when a binding and its stub disagree. This file is
> the **narrative/design guide**: why things work the way they do, load order,
> phase history.

> **Status: implemented and demonstrated.** The server content API
> (`ARCHITECTURE_SPEC.md` §10.3) and the client UI API (§10.4) are both real,
> and `content/base` (`content/base/`) is the worked, runnable example this
> doc keeps referring back to — it's the pack `voxel_browser_server` loads by
> default (`server.toml`'s `content_pack = "content/base"`), not sample code
> that lives only here. A few registration calls remain write-only until a
> later phase gives them a consumer (`vb.register_biome`, and parts of
> `vb.register_entity`) — each is called out below, in place, rather than
> implied by a blanket disclaimer at the top of this file.

## Binding layer

`sol2` (v3.5.0), header-only, over PUC-Lua 5.4 — decision recorded in
`ARCHITECTURE_SPEC.md` §18 Q1. (v3.3.0's bundled optional does not compile under
Clang ≥ 18.)

## Runtime — `vb::script::Vm` (`inc/vb/script/vm.hpp`, implemented, `VB_WITH_LUA`)

pImpl wrapper around one `sol::state`; the rest of the engine never includes
sol2, and a build without `VB_WITH_LUA` links a stub whose every call returns
`core::ScriptError::kDisabled`.

- `Vm(VmLimits{ memory_bytes, instruction_budget })`
- `ScriptResult do_string(code, chunk_name)` — compiles **source only**
  (`sol::load_mode::text`; bytecode is rejected) and runs it under the per-call
  hook. `ScriptResult` carries `{ok, core::ScriptError, message}` where `message`
  is the Lua error text + `debug.traceback`.
- `begin_call_budget()` — re-arms the instruction counter; the tick loop calls it
  before each pack callback (Phase 4.2).
- `memory_used()` / `memory_limit()` / `sandbox_intact()`.

Error classes: `kSyntax`, `kRuntime`, `kBudgetExceeded` (hook fired),
`kOutOfMemory` (allocator ceiling — recoverable, the VM stays usable).

## Server API (pack VM) — `vb::script::PackRuntime` (`inc/vb/script/pack_runtime.hpp`, implemented, `VB_WITH_LUA`)

pImpl'd the same way as `Vm` (a disabled build links a no-op stub); owns one
`Vm`, the content registries, the event bus, and `vb.after`/`vb.every`
timers. Construction is split because a `player_join` veto must wrap
`HandshakeServerHost::authenticate` **before** the `ServerSession` that owns
it exists:

```cpp
PackRuntime rt(transport, registry, storage_path);
rt.load_pack_file(pack_source);
rt.freeze();
rt.install_join_veto(host);           // before constructing ServerSession
ServerSession session(transport, cfg, host);
// ... construct WorldReplicator ...
rt.attach_world(*session.world_replicator());
rt.attach_session(session);
// main loop, after session.tick(dt):
for (auto &j : session.take_joins())  rt.dispatch_player_join_completed(j);
for (auto &l : session.take_leaves()) rt.dispatch_player_leave(l);
rt.dispatch_tick(dt);
```

- **Block data scripts (recommended convention).** A pack can keep its block
  tables in a pure-data Lua file (`data/blocks.lua`) that `return`s a list of
  the tables `vb.register_block` takes, and register them from pack code
  (`content/base` does this in `blocks/register.lua`, which the loader runs
  first so root modules like `crafting.lua` still see `base_*_id` globals at
  load time). Tools such as the structure editor evaluate the data script alone
  (`vb::script::eval_data_script`) in a bare Lua state: any `vb.*` access raises
  "block data scripts must only return data; register blocks from pack code",
  and `require` only reaches the pack's own `.lua` files. Fields the engine
  doesn't know (`on_break`, a pack's own `drops`) are ignored. Keep the list in
  registration order — block ids follow it and saved worlds store ids. A pack
  that registers some blocks directly still works; the editor just won't see
  those. See `docs/structure-editor.md` §G.
- `register_block{replaceable = true}` (default `false`): a structure whose rule
  says `replace = "air_and_plants"` may overwrite this block (`base:leaves`).
  Like `texture`, it lands on an already-registered built-in block too.
- Registration (pack load only, rejected once `freeze()` has run):
  `vb.register_block(def) -> BlockId` (idempotent by `name`; **the real
  texture/atlas system**: `texture` is a pack-relative path (e.g.
  `"textures/stone.png"`), synced to the client over the existing Asset Sync
  virtual FS and packed into one atlas texture per session
  (`vb::render::TextureAtlas`) — a block with no `texture` (the default,
  empty string) keeps rendering a flat placeholder color. Texture alpha is
  honoured: fully clear texels are cut out (a plant sprite on
  `opaque = false` shows the terrain behind it), and a texture whose
  visible texels are mostly half-transparent (glass) is drawn blended.
  `model` is still
  accepted but not stored — no wire-visible model field exists on
  `BlockType` yet; `max_damage` — Phase 6.5, default `0` = today's instant
  break — opts the block into the shared block-damage breaking system below,
  replicated to clients as part of `S2C_BlockRegistry`. **Idempotent
  registration doesn't update an already-registered block's other
  properties** — re-registering an existing `name` (e.g. one of the base 8)
  just returns its id unchanged; `max_damage`/`solid`/`opaque`/etc. only take
  effect the first time a name is registered. `texture` is the one
  exception: re-registering an existing `name` with a non-empty `texture`
  still attaches it (`BlockRegistry::set_texture`) — this is how
  `content/base/blocks/stone.lua`/`water.lua` give an already-hardcoded
  `BlockRegistry::base()` block a real texture without needing to touch
  `src/world/block.cpp`; `crack_texture` — Phase 6.5's last piece, same
  re-declare-to-attach exception as `texture` (`BlockRegistry::
  set_crack_texture`) — an optional pack-relative crack-stage spritesheet
  path (`vb::render::CrackAtlas::kStages`, currently 8, equal-width square
  frames laid out left to right; a wrong-shaped or undecodable image is
  logged and the block falls back to the shared default row, exactly like a
  bad `texture` falls back to a flat placeholder color). Unset (empty
  string, the default) means the client draws its own built-in, procedurally
  generated generic crack overlay for that block instead — no art pipeline
  needed to get *some* progressive damage cue; `region` — Phase 7.3, generic per-tick
  occupancy tracking opt-in, independent of `liquid` (see `region_enter`/
  `region_exit` below) — defaults to `liquid`'s own value (a liquid block
  opts in automatically, matching `base:water`) unless given explicitly,
  `vb.register_item(def)`, `vb.register_entity(def)`
  (`name` (idempotent-by-name), `on_spawn`/`on_tick`/`on_hit`/`on_death`
  callbacks, opt-in `health=` (`entity:get_health()`/`set_health()`,
  auto-despawn at 0), `width=`/`height=` billboard footprint (default
  `0.8`/`1.8`, the placeholder's own dimensions) — real dispatch is a small
  hardcoded system (`PackRuntime::Impl::entities`), not an EnTT registry
  (Phase 3.1 still doesn't exist server-side for this). `visual = {...}`
  (entity-management follow-up, schema finalized 2026-09-17 in
  `architecture_spec/rendering.md` §11.3) is now real: `variant` (one of the
  9 named frame-size presets, `small` through `large_flat` — see the table
  in `rendering.md`), `texture` (pack-relative path, synced like any asset),
  `facings` (4 or 8, default 8), `mirror` (default `true` — one authored
  "side" pose is flipped horizontally for the opposite side, so a pack
  authors `facings/2+1` rows; `false` requires a fully authored row per
  facing instead, `facings` rows total, e.g. a real left-facing pose distinct
  from the right one, and the engine never flips anything), `origin = {x=,
  y=}` (normalized anchor within a frame, default `{0.5, 1.0}` =
  bottom-centre feet point, each component in `[0, 1]`), and `clips` (a
  non-empty array of
  `{clip=, frames=, fps=}`, `frames`/`fps` both positive) — all validated at
  registration (`PackRuntime`'s `parse_entity_visual`); the sheet's real
  pixel dimensions are validated separately, client-side, once the texture is
  actually decoded (`render::build_entity_visual_layout`) — a mismatch there
  logs a warning and that kind keeps the flat `width`/`height` placeholder
  rather than failing pack load. A kind that never sets `visual` is
  unaffected either way. Per-instance override (spec's
  `ScriptState.visual_override`, e.g. skins) is now real too:
  `vb.world.spawn(kind, pos, { visual_override = {...} })` takes the same
  shape as `register_entity`'s `visual` table but with every field optional
  (`variant`/`texture`/`facings`/`mirror`/`origin`/`clips`) — an omitted field
  inherits the kind's own `visual` unchanged (`render::merge_visual_override`,
  client-side), so a pack can override just `texture` (a player-skin variant)
  while keeping the kind's `facings`/`clips`/`origin`. Validated the same way
  as `register_entity`'s `visual` whenever a field is given
  (`PackRuntime`'s `parse_entity_visual_override`); the merged, decoded
  result falls back to the kind's own visual (or the flat placeholder if the
  kind has none) on any validation/decode failure, same posture as the
  kind-level path. Set once at spawn time only — there's no
  `entity:set_visual_override()`/live-update path yet, and no wire mechanism
  to change or clear it for a client that has already seen the entity (spec
  architecture_spec/rendering.md §11.3, `docs/protocol.md`'s version-21
  entry); the reserved `self.visual_override` table is kept for pack
  introspection only, nothing engine-side reads it back. **Text labels**
  (protocol 32): `vb.world.spawn(kind, pos, { text = "27s" })` or `{ text =
  { value=, color=, background=, size=, offset_y=, max_distance=,
  through_walls= } }` attaches a camera-facing world-space label, drawn with
  the engine's UI font; `entity:set_text(...)` changes it at any time and is
  replicated as a small `S2C_EntityProps` delta (the entity keeps its net id,
  so a countdown that ticks every second costs a few bytes per viewer, not a
  respawn), `entity:set_text(nil)` removes it, `entity:get_text()` reads the
  value back. `vb.register_entity{ text = {...} }` sets a kind's default
  style (and optionally a default `value`): a string passed to `set_text`
  changes only the value, a table re-applies the kind style with its own
  fields on top. `visual = false` makes a text-only kind (no sprite, no
  placeholder quad). The value is capped at 64 bytes of UTF-8 and colours
  are `{r, g, b}`/`{r, g, b, a}` with 0-255 integers; both are checked in
  the binding and fail with a Lua error. Labels are depth-tested (walls hide
  them) unless `through_walls = true`, and `max_distance` hides one beyond
  that many blocks. **Clips, layers and attachments** (protocol 32): the
  client picks a clip from the entity's velocity and on-ground flag (a
  stationary script entity plays `idle`, a moving one `walk`/`run`);
  `entity:set_clip(name)` forces any clip the sheet declares (`"open"`,
  `"glow"`, a padding trick to show one frame) for every client, including
  late joiners, until `set_clip(nil)`. A clip the sheet doesn't have falls
  back to its first clip and logs one warning per sheet and clip name.
  Overlapping sprites are drawn back to front with clear texels cut out (a
  sprite's empty margin never hides what's behind it); `visual.layer`
  (-8..8, also in `visual_override`) decides which of two sprites at the
  same spot draws in front, and `visual.through_walls = true` draws over
  terrain. `entity:attach_to(parent, { offset =, face_offset =, layer = })`
  glues an entity to another entity or a player: it follows the parent on
  the server every tick and on clients every frame (no trailing), and is
  despawned with cause `"parent_removed"` when the parent goes;
  `entity:detach()` lets go. A name tag is `visual = false` + `text =
  {through_walls = true}` + `attach_to(player, {offset = {y = 2}})`. `vb.register_biome(def)` (Phase 6.14: `name`
  (idempotent-by-name, mirrors every other registration function),
  `surface`/`filler`/`stone` (block *names*, resolved to `BlockId`s via the
  registry when a pipeline is compiled), `probability` (base Voronoi-cell
  draw weight, default `1.0`), `adjacency = {["other:biome"] = weight, ...}`
  (soft multiplier vs. an already-resolved neighbor cell, default `1.0` for
  an unlisted pair, engine-floor-clamped so it's never a hard exclusion —
  see `vb.worldgen.set_pipeline` below), and `decoration` — a *table* of
  `{spawn_rate=, blocks={{x=,y=,z=,block=},...}}` schematic entries is a real
  consumer (Phase 6.14); a plain *string* (`content/base`'s pre-6.14 usage,
  e.g. `decoration = "trees"`) stays captured-but-inert, unchanged behavior.
  Every `vb.register_biome` call becomes one Voronoi-cell candidate only once
  a pack also calls `vb.worldgen.set_pipeline` — with no such call, biomes
  stay captured exactly like before this phase, since `WorldGenerator` keeps
  running its fixed single-biome-shaped default. / `vb.register_craft(def)` (captured;
  the engine itself doesn't read it back, but `content/base/crafting.lua`
  is a real, working example built on top of it — see below).
  `vb.register_keybind(name) -> index` (Phase 6.3, idempotent by `name`)
  declares a closed-schema custom input slot — `index` (registration order)
  becomes bit *index* of every `InputCmd.keybinds` going forward, capped at
  32 registrations (`S2CKeybindRegistry::kMaxKeybinds`) so the bitset always
  fits one `u32`; an unregistered key literally cannot be represented on the
  wire. Reaches joining clients as `S2C_KeybindRegistry` when
  `PackRuntime::install_keybind_registry(host)` is called (`src/server/
  main.cpp` does, right alongside `install_join_veto`), same opt-in shape as
  `block_registry`. Phase 6.19: every `PackRuntime` pre-registers 8 engine
  names — `move_forward`, `move_back`, `move_left`, `move_right`, `jump`,
  `sprint`, `primary`, `secondary` — before any pack script runs, so they're
  always present in `S2C_KeybindRegistry` and readable via
  `input.keybinds["jump"]` etc. just like a custom keybind. This is purely
  additive: `input.buttons`/`input.move` (and the client's hardcoded WASD/
  Space/Shift/mouse physical keys) are unchanged — it just makes these
  actions *discoverable* the same way a custom keybind is, for a future
  rebind-UI to enumerate. Calling `vb.register_keybind` with one of these
  names returns the same pre-assigned index (idempotent-by-name already
  covers it); it does not let a pack change which physical key drives it.
  `vb.worldgen.set_pipeline{height=, base_height=, amplitude=, sea_level=,
  soil_depth=, beach=, cell_size=, carvers={{noise=, threshold=, y_min=, y_max=},
  ...}, veins={{block=, target_rock=, height_min=, height_max=, vein_size=,
  spawn_rate=}, ...}}` (Phase 6.14, pack-load-time only) replaces
  `WorldGenerator`'s fixed fBm-heightmap default with a pack-driven pipeline
  — `height` (required, a `vb.noise.*`-built node) plus every registered
  biome are compiled *once*, on the main thread, into an immutable
  `worldgen::PackWorldGenPipeline` (`PackRuntime::build_worldgen_pipeline`,
  called right after `freeze()`, before constructing `WorldGenerator`/
  `WorldGenWorkerPool` — same call-site shape as `effective_move_params`).
  **Deliberate deviation from this item's original `set_pipeline(fn)`
  phrasing:** Lua/sol2 is strictly single-threaded and
  `WorldGenWorkerPool` calls `WorldGenerator::generate()` from N worker
  threads with no locking, so the pipeline can't literally be "a Lua
  callback run per chunk" — it's data, compiled once into plain C++ (and,
  when `VB_WITH_WORLDGEN` links real FastNoise2, into an actual FastNoise2
  `SmartNode` graph — see `vb/worldgen/fastnoise2_compile.hpp`; the
  dependency-free `vb/core/noise.hpp` evaluator is what runs otherwise, same
  "zero-dependency default, optional backend on top" posture as every other
  `VB_WITH_*` flag). No call at all (the common case) leaves
  `WorldGenerator` on its exact pre-6.14 fixed path — byte-identical output,
  `tests/unit/worldgen_test.cpp`'s golden-hash gate for the default path is
  unaffected.
  `beach = "base:sand"` (optional block name, default off) makes every
  column whose surface is at or below `sea_level + 1` use that block for
  surface and filler, like the fixed default path's sand beaches; without it
  each biome's own surface block runs down to the sea floor, and a structure
  rule with `on = {grass}` never anchors on a beach.
  `vb.noise.*` builds the node-graph description `set_pipeline`'s `height`
  and each carver's `noise` field expect — plain tagged Lua tables, not
  opaque handles: `vb.noise.constant(v)`, `vb.noise.value{frequency=}`,
  `vb.noise.cellular{frequency=}`, `vb.noise.fbm{source=, octaves=,
  lacunarity=, gain=, frequency=}`, `vb.noise.remap{source=, in_min=,
  in_max=, out_min=, out_max=}`, `vb.noise.combine{a=, b=,
  op="add"|"multiply"|"min"|"max"}`.
  `vb.register_structure(def)` (structure editor S1, pack-load-time only,
  not idempotent: a repeated `name` is an error) registers a decorative
  structure: `name`, `size = {x=,y=,z=}` (each 1..64), optional `anchor`
  (default `{0,0,0}`; the cell that sits on the ground block), `palette`
  (single character -> block *name*, or `false` for "keep the terrain
  there"), `variants = {{weight=, layers={...}}, ...}` (`layers[y][z]` is one
  row of `size.x` palette characters, bottom layer first) and an optional
  `placement = {on=, replace=, rotate=, mirror=, min_spacing=, max_slope=,
  y_min=, y_max=, cluster=}` of defaults. Unknown keys are errors. The table
  is plain data, so the usual home is `structures/<name>.lua` returning it,
  registered with `vb.register_structure(require("structures.oak_tree"))`
  (the loader does not walk `structures/`). Block names are resolved after
  the whole pack has loaded: `PackRuntime::validate_worldgen()` (called by
  the server and `--singleplayer` right after `freeze()`) reports an unknown
  block or structure name, naming it. A biome's `decoration` entry can now
  be `{structure = "name", spawn_rate = n, <any placement field>}`; the
  inline `{blocks = ...}` form still works and becomes an anonymous
  one-variant structure (`replace = "all"`, no rotation).
  **Decoration is data, not callbacks** (same threading reasoning as above):
  a per-site Lua callback can't run on a worker thread either, so decoration
  is a list of structures (`vb.register_structure`, above) plus declarative
  placement rules, not a function. Placement is rule-based and crosses chunk
  borders (structure editor S2): for each rule, anchors come from a global
  jittered grid keyed by `(world seed, grid cell, rule)` — `spawn_rate` is the
  expected number of placements per 32×32 column, `min_spacing` the minimum
  gap between anchors on some axis, `cluster` (0..1) gates them with
  low-frequency noise without changing the mean. An anchor is kept only when
  the biome *at the anchor* is the rule's biome, the ground block is in `on`
  (any solid block when empty), the anchor is not underwater, the ground
  height is inside `y_min..y_max`, and the terrain under the (rotated)
  footprint varies by at most `max_slope`. All of that reads only
  pre-decoration terrain (`WorldGenerator::block_at_pregen`, the height
  field), so it never depends on a neighbor chunk. Each chunk pulls the
  placements whose footprint reaches it and stamps its own cells in a fixed
  order (anchor x, z, then rule), so a tree on a border is identical from
  either side and from every vertical chunk. `replace` decides what a
  structure may overwrite — `"air"` (default), `"air_and_plants"` (air or a
  block registered with `replaceable = true`) or `"all"`; an explicit
  `base:air` palette cell always carves. The anchor cell sits one block above
  the ground block. Still not attempted: runtime/callback decoration that
  reacts to its surroundings (`REMAINING_TASKS.md`'s Deferred section) and
  Voronoi cell resolution result caching.
  A pack registering blocks beyond the Phase 2 `base()` set logs an info
  line; those ids reach the client if (and only if) the host wires
  `HandshakeServerHost::block_registry` from this same registry
  (§4.3/`S2C_BlockRegistry` — `src/server/main.cpp` does). The server-side
  registry starts from `base()` so the well-known 8 ids still match the
  client's fallback `base()` call when no registry frame is sent at all.
- Runtime world (needs `attach_world()`): `vb.world.get_block(x,y,z)`,
  `vb.world.set_block(x,y,z,id)` (**known gap:** doesn't run the relight
  cascade `C2S_BlockEdit` does — can desync lighting until something else
  touches the chunk), `vb.world.raycast(origin, dir, max) -> {hit,x,y,z,nx,ny,nz}|nil`,
  `vb.world.spawn(kind, pos)` (logs and returns `nil` — no entity system to
  spawn into yet, Phase 3.1). `vb.world.spawn_item_drop(pos, item, count)`
  (needs `attach_session()`, not just `attach_world()`) is a separate,
  hardcoded-but-real path: spawns a dropped-item entity backed by
  `vb::world::ItemDropSystem` (`src/net/session.cpp`), replicated through the
  same interest-grid/`S2C_EntitySnapshot` machinery a player uses, no new
  wire message — a nearby player auto-collects it into their inventory. Not
  routed through `vb.register_entity`/`vb.world.spawn` at all; see
  `content/base/blocks/*.lua`'s `on_break` handlers for the intended usage.
  Phase 6.11: `vb.register_block{pickup_radius=..., item_lifetime_seconds=...}`
  overrides `ItemDropSystem`'s own construction-time defaults (1.5 blocks /
  120s) for drops of that specific block — e.g. a magnet-radius power-up or a
  rare item that never despawns. Unset (or negative) means "use the engine
  default"; same idempotent-registration caveat as `max_damage`/`max_stack`
  applies — re-registering an existing block name doesn't update it.
- Entity / player Lua object (needs `attach_session()`; one merged usertype
  today since no non-player entity exists): `:get_pos() -> {x,y,z}`,
  `:set_pos(x,y,z)` (teleport: feet position, velocity zeroed, the client
  snaps to it), `:get_spawn_pos() -> {x,y,z}` (the join spawn point),
  `:set_velocity(x,y,z)`, `:remove()` (no-op, logged — nothing to remove
  from), `:get_inventory() -> {{item,count}, ...}`, `:give({item,count})`,
  `:take({item,count}) -> bool` (removes up to `count` of `item` across
  however many slots hold it; `false` and no change at all if the player
  doesn't have enough — all-or-nothing, not partial), `:send_message(text)`,
  `:open_ui(name, ctx?)`, `:get_name()`, `:damage(amount, cause?)` (Phase
  6.6 — the one way to reduce a player's health from Lua; `cause` is an
  opaque string, e.g. `"fall"`/`"pvp"`, threaded through unchanged to a
  `player_death` handler below), `:get_health()` (`{current=, max=}`, or `nil`
  if the player is gone — read-only; lower it with `:damage()`).
  `send_message`/`open_ui` are real, framed messages (`S2C_Chat`/`S2C_OpenUi`,
  see `docs/protocol.md`) that a real client actually handles: `send_message`
  lands in the HUD chat log (`src/client/main.cpp`, Phase 5.4) exactly like a
  normal chat line, and `open_ui` is drawn by `UiRenderer` (Phase 4.5/5.1).
  **`vb.register_item` never allocates its own id space** — a held item and a
  placeable block still share one `BlockId`, so every `item` field above
  (`give`/`take`/`get_inventory`) is really a `BlockId`; anything a pack
  wants a player to be able to hold has to go through `vb.register_block`
  today, crafted-only materials included (see `content/base/blocks/
  planks.lua`/`sticks.lua`).
- Events: `vb.on("player_join"|"player_leave"|"block_break"|"block_place"|
  "player_interact"|"chat"|"tick"|"ui_event"|"player_death"|"player_input"|
  "block_break_begin"|"block_break_tick"|"block_health_tick"|
  "region_enter"|"region_exit"|"player_landed", handler)`,
  (`player_leave` runs after the connection is gone, but its `player` still
  answers `get_name()`, `get_login()` and `get_pos()` — the last known
  position — so cleanup keyed by name works; health/hunger read `nil`),
  vetoable via `return false` (except `tick`/`ui_event`, which have
  no veto semantics; `player_death` is a *decision* hook, not a veto —
  see below; `player_input`/`chat` may veto *or* replace, see below;
  `block_break_tick`/`block_health_tick` return numbers, not booleans —
  see below; `region_enter`/`region_exit`/`player_landed` are pure
  notifications, any return value is ignored, see below).
  `player_join` fires from `install_join_veto`'s `authenticate` wrapper (a
  real pre-join veto — note it hands the handler a plain player *name*
  string, not a `Player` handle, since no session/connection exists yet at
  that point) and, since Phase 9.4, a second argument `login`: `function(name,
  login)` where `login` is the frozen login table below, or `nil` when the
  server isn't authenticating (see "Authentication" below). `block_break`/`block_place` fire from `WorldReplicator::
  apply_block_edit`'s `BlockEditHooks` seam with a real `Player` handle +
  position, and a block's own `on_break`/`on_place` callback (from
  `register_block`) fires separately, after the edit is applied. `chat`
  fires from a real `C2S_Chat` (Phase 5.4) as `function(player, text) ->
  boolean|string?` before `ServerSession` broadcasts `S2C_Chat`: `return
  false` vetoes the line outright, `return "new text"` (Phase 6.10) rewrites
  it for the next handler in registration order and, if it's the last one to
  touch it, for the broadcast itself, and `true`/`nil`/anything else passes
  the current text through unchanged — the same veto-or-replace chaining
  shape as `player_input` below, just for one string field instead of a
  table. `content/base/crafting.lua` is the reference example of building a
  whole feature (recipe parsing, ingredient checks) entirely on top of this
  one event (veto-only, no text rewriting). `ui_event` fires from `C2S_UiEvent`
  (Phase 4.5). `player_interact` is still wired generically
  (`PackRuntime::dispatch_player_interact`) with nothing calling it — no
  interact wire message exists yet. `player_death` (Phase 6.6) fires once
  per respawn — health reaching 0, for any cause, void-kill included — as
  `function(player, cause, health_before) -> table?`; the *first* registered
  handler that returns a table wins (not a veto chain), and that table may
  set `heal` (new health), `pos` (`{x,y,z}`, the new position), `message`
  (a private chat line, `""`/omitted = none), and `drop_inventory` (spawns
  every inventory slot as a real dropped-item entity **at the player's
  death position**, not wherever they're about to respawn, and empties
  their inventory). No handler registered at all (or none returns a table)
  falls back to the engine's built-in behavior — full heal, teleport to the
  join spawn point, `"* you died and respawned"` — unchanged from every
  pre-6.6 build. `player:get_pos()` inside the handler still reads the
  death position (the engine hasn't moved the player yet at this point),
  useful for anything beyond the built-in `drop_inventory` flag.
  `ServerSession::spawn_point(net_id)` (not a Lua binding, a host-side
  accessor) exposes the original join spawn point if a handler wants to
  fall back to it deliberately.
  `player_input` (Phase 6.3) fires once per `InputCmd`, before movement
  integration, as `function(player, input) -> false | table | nil`. `input`
  is `{ move = {x,y,z}, yaw, pitch, buttons = {jump,sprint,primary,
  secondary,fly_up,fly_down}, keybinds = {[name] = bool, ...} }` (only
  *registered* keybind names ever appear as `keybinds` keys). Returning
  `false` vetoes — the cmd's effect on movement/rotation is dropped entirely
  for that tick (its seq is still consumed/acked, so the client doesn't
  replay it forever); returning a table overrides only the fields present in
  it (omitted fields keep the previous value) — e.g. `return { move = {
  z = 2.0 } }` to reinterpret input as a dash. Multiple handlers chain in
  registration order, each seeing the prior one's output; the first `false`
  short-circuits the rest. Movement/look fields (`PlayerInput`'s existing
  wire shape) are otherwise untouched by this channel — it's additive.
  Only installed (`ServerSession::set_input_handler`) when a pack actually
  registers a `player_input` handler, so packs that don't use it pay zero
  extra cost in the per-tick input path.
  Shared block-damage breaking (Phase 6.5, spec §10.7) — only for a block
  with `max_damage > 0` (§`register_block` above); a `max_damage == 0` target
  never reaches any of these three, staying today's instant break.
  `C2S_BlockBreakBegin{pos, face}`/`C2S_BlockBreakStop{pos}` bracket a client
  holding on a target (`ClientSession::send_block_break_begin`/
  `send_block_break_stop` — no base-pack/client UI wires these yet, same
  "mechanism before content" posture as every other Phase 6 primitive).
  `block_break_begin(player, pos) -> bool?` gates entry — vetoable, on top of
  the engine's own reach + `max_damage > 0` checks. `block_break_tick(player,
  pos, max_damage) -> number` fires once per tick per contributing player
  (multiple concurrent players "breaking together" sum their own return
  values; multiple registered handlers for the same call also sum, an
  orthogonal case). `block_health_tick(pos, damage, max_damage,
  ticks_since_last_hit) -> number?` fires once per tick per currently-damaged
  block regardless of contributors — return a replacement damage value, or
  nothing for "unchanged" (the last handler to return a number wins if
  several are registered). No handler for either tick event = zero built-in
  policy: `block_break_tick` absent means damage never accrues at all;
  `block_health_tick` absent means permanent damage, no healing. Completion
  (summed damage reaching `max_damage`) drives the *existing*, unchanged
  `C2S_BlockEdit`/`block_break`/`on_break` pipeline — this system only gates
  *when* that fires. The live punch count itself replicates to every player
  currently mirroring the chunk (not just the puncher) via `S2C_BlockDamage`
  (protocol 25) — `client.break_progress()` reports a real
  `punches / max_damage` fraction for the looked-at block, and the client
  renders a real crack-stage overlay for it (`vb::render::CrackAtlas`,
  `crack_texture` above) — closing `REMAINING_TASKS.md` 6.5 in full.
  Generic region occupancy (Phase 7.3): `region_enter(player, pos,
  block_name)` / `region_exit(player, pos, block_name)` fire once per
  crossing (not once per tick spent inside), server-side, whenever a
  player's own position moves into/out of a block whose `BlockType.region`
  flag is set (`register_block{region=...}` above) — `pos` is the voxel
  they crossed into/out of, `block_name` its registered name (e.g.
  `"base:water"`, the only block that opts in by default). Purely a
  notification, no veto — a pack builds its own policy on top (e.g. slow
  movement in water) from this plus `player:set_velocity`/its own state, the
  engine hardcodes no swim-speed or liquid-specific behavior at all. Water
  is just the first user of a generic mechanism; a future lava/gas/
  poison-cloud block reuses it with zero engine changes. Only installed
  (`ServerSession::set_region_hooks`) when a pack registers at least one of
  the two events, same zero-extra-per-tick-cost posture as the block-damage
  hooks above.
  Fall damage primitive (Phase 6.22): `player_landed(player, impact_speed)`
  fires once per player exactly on the tick a fall is arrested by hitting
  ground (never per-tick while airborne or already grounded) — `impact_speed`
  is the player's downward speed in m/s the instant before landing. Pure
  notification, no veto/return value, same posture as `region_enter`/
  `region_exit` — the engine computes and reports the raw speed only, it
  ships zero fall-damage formula or threshold of its own. `content/base/
  fall_damage.lua` is the reference policy: no damage below a flat
  safe-speed threshold, then 1 HP per m/s above it, via the existing
  `player:damage(amount, cause)` primitive (Phase 6.6) with `cause =
  "fall"`. Only installed (`ServerSession::set_landed_hook`) when a pack
  registers a `player_landed` handler, same zero-extra-cost-when-unused
  posture as every other opt-in hook above.
- Scheduling: `vb.after(seconds, fn)` (one-shot), `vb.every(seconds, fn)`
  (repeating; catches up on a stalled tick, capped at 8 fires/dispatch).
  Storage: `vb.storage.key = value` — a metatable-backed proxy over a
  `nlohmann::json` object, persisted to `<content_pack>/storage.json`
  (flushed once per tick when dirty, and in `flush_storage()`).
- Generic per-key storage (Phase 6.4): `vb.db.get(key)` / `vb.db.set(key,
  value)` / `vb.db.delete(key)` — distinct from `vb.storage` above, `key` is
  whatever the script chooses (`"user:" .. name`, `"session:" .. token`, ...)
  and `value` round-trips through the same JSON conversion as `vb.storage`
  (tables/numbers/strings/booleans, not just strings). Written immediately
  (no dirty-flag/flush step, unlike `vb.storage`). Backed by
  `vb::script::ScriptDb` (`inc/vb/script/db.hpp`): each key's file lives at
  `<content_pack>/db/<sha256(key) 2-hex-prefix>/<sha256(key)>`, the same
  content-addressed shard layout as `vb::assetsync::ClientAssetCache`.
  Neither `db/` nor `storage.json` is ever sent to clients (both are left out
  of the asset manifest), so both are safe for server-private data. No
  enumeration API — get/set/delete by an already-known key only. The engine
  has no notion of "logged in": a connection stays just a connection until a
  pack's own login flow (built on `vb.db`) looks up a record and decides to
  recognize it. `vb.crypto.hash(data)` — SHA-256 hex digest
  (`vb::core::sha256_hex`, `inc/vb/core/sha256.hpp`) — so a pack implementing
  its own login doesn't have to roll credential hashing in pure Lua (the
  sandbox strips `os`/`io`, §10.2). The engine still takes no position on
  auth as a concept (spec §18 Q6).
- Physics tunables (Phase 6.7): `vb.physics.set_params{gravity=..,
  walk_speed=.., sprint_speed=.., jump_speed=.., accel=.., air_accel=..,
  friction=.., step_height=.., fly_speed=.., fly=.., half_width=..,
  height=.., eye_height=.., terminal_velocity=..}` overrides
  `physics::MoveParams` (pack-load time only, rejected once frozen) — only
  the fields the table sets are replaced; everything else keeps the
  operator's `server.toml` default (`gravity` there is the base a pack
  override wins over, not the other way round). One global override, not
  per-entity-kind — no entity kind besides the player runs physics today.
  Reaches joining clients as `S2C_MoveParams` so client-side prediction uses
  the exact same tunables as the server's authoritative simulation, same
  opt-in-hook shape as `block_registry`/`keybind_registry`.
- Day/night curve (Phase 6.8): `vb.daynight.set_curve{keyframes = {{tick=,
  brightness=, color={r,g,b}}, ...}}` overrides the engine's default
  4-keyframe sky gradient (`vb::world::default_day_night_curve()`); rejects
  an empty/missing `keyframes` table and any call after `freeze()`. Reaches
  joining clients as `S2C_DayNightCurve`, same "no call, no frame, client
  keeps the default" opt-in shape as `vb.physics.set_params`/`S2C_MoveParams`
  above — the client actually renders with the overridden curve
  (`ClientSession::day_night_curve()`), not just server-side bookkeeping.
  `vb.daynight.set_day_length(seconds)` (rejects `seconds <= 0`) overrides
  the real seconds one in-game day takes, the same config-then-pack-override
  shape as `gravity` (`server.toml`'s `day_length_seconds` is the base a pack
  override wins over).
- Distance fog (Phase 7.2): `vb.render.set_fog{start=, ["end"]=}` (`end` is a
  Lua keyword, so it must be a quoted key) overrides the
  engine's default fog distance (rejects a call missing either field, or
  `end <= start`, and any call after `freeze()`). Absent an override, each
  client computes its own default from its own `view_distance` config
  (`end = view_distance * kChunkDim`, `start = end * 0.6`) — the server
  doesn't know each client's `view_distance`, so unlike
  `vb.physics.set_params`/`vb.daynight.set_curve` there's no server-side
  universal default this replaces, only a per-client fallback. Reaches
  joining clients as `S2C_FogParams`. Deliberately **no color field for
  above-water fog** — it always blends into whatever `vb.daynight`'s current
  sky color already is (`sky_color_for_time()`), never an independently
  drifting tint. **Underwater is the one exception** (Phase 7.5): an
  optional `underwater_tint = {r=, g=, b=}` (each `0-255`) on the same
  `set_fog{...}` table overrides the client's own default underwater tint
  (the submerged liquid block's own texture, averaged) — omitting it leaves
  that per-client default alone; giving it with a missing channel or an
  out-of-range value is rejected the same way a bad `start`/`end` is.
- Read-only server config (Phase 6.13): `vb.config.get(key)` returns the
  operator's `server.toml`/CLI value for `key` — `bind_address`, `port`,
  `content_pack`, `max_players`, `view_distance`, `tick_rate`, `world_seed`,
  `gravity`, `void_kill_y`, `day_length_seconds`, `asset_max_file_mb`,
  `asset_max_total_mb`, `max_connections_per_ip` (§8.3 hardening, Phase 1.3
  polish — `0` = unlimited), `max_messages_per_second` (§8.3 hardening,
  Phase 3.2/6.3's per-connection flood guard — `0` = unlimited), `auth_mode`
  (`"none"`/`"token"`), or `motd`;
  `nil` for any other key. Deliberately **not** an override surface like
  `vb.physics.set_params`/`vb.daynight.set_curve` above — a pack can react to
  these values (e.g. tune spawn density to `view_distance`) but can't change
  what the operator running the server configured. `--singleplayer`'s
  in-process `PackRuntime` has no `ServerConfig`/`server.toml`, so every key
  returns `nil` there.

### Authentication (Phase 9.4; `architecture_spec/auth.md` §6)

When the pack ships `auth.lua`, the engine verifies an external ID token during
the handshake, before any asset or chunk is sent. Scripts only ever see *who
the player is*:

- `player:get_login()` → `nil` when the server isn't authenticating, else a
  **frozen** table `{ provider, subject, name, claims = { ...allowlisted... } }`.
  `provider` is `"oidc"|"keycloak"|"firebase"`; `claims` holds only the claims
  listed in `auth.lua`'s `claims`. Writes raise an error and the metatable is
  locked. No token, issuer or expiry is ever exposed.
- `vb.on("player_join", function(name, login) ... end)` — `login` is the same
  table (or `nil`); `return false` vetoes, *after* verification (e.g. an
  allowlist on `login.claims.email_verified` or a group).
- `vb.on("login_changed", function(player, login) ... end)` — fires after a
  periodic re-auth (default every 15 min) found a changed allowlisted claim
  (e.g. a group was removed); `login` is the new frozen table and
  `player:get_login()` now returns it. Notification only (no veto); the
  in-game name never changes mid-session. Players whose IdP session was
  revoked are kicked by the engine, so no handler is needed for that.
- `vb.auth.required()` → `true` iff `auth.lua` is active.

**Guarantee:** when `auth.lua` is active, `get_login()` is non-nil for every
`Player` a script can obtain (including in `player_leave`); no connection
reaches the game without a verified login. When it is not active, it is `nil`
for everyone, so a `nil` always means "no auth".

Rules to persist by: identity is `(issuer, subject)`, so **key your data on
`login.subject`, never on the name**. Two different accounts that want the
same name get `alex` and `alex#2`. A second sign-in of the same account kicks
the older session ("signed in elsewhere"); the newcomer is never refused.

```lua
vb.on("player_join", function(name, login)
	if login and login.claims.email_verified == false then
		return false -- unverified e-mail: keep them out
	end
end)
```

## Client UI API — `vb::script::UiRuntime` (`inc/vb/script/ui_runtime.hpp`, implemented, `VB_WITH_LUA`)

A second, separate `Vm` from the server's `PackRuntime` — pImpl'd the same
way, disabled-stub when `VB_WITH_LUA` is off. Drawing is split across the
core/render boundary: `UiRuntime` (`vb_core/script`) evaluates layout into a
plain `Widget` list (no raygui), and `vb::render::UiRenderer`
(`inc/vb/render/ui_renderer.hpp`) draws that list with raygui and reports
back which widgets fired an interaction — no sol2 in the render half.

- `ui.define(name, render_fn)` — `render_fn(state)` returns
  `{ widgets = { ... }, on_close = fn? }` and is called **once per UI frame**
  for as long as the screen is open (Phase 6.2; `raygui` is itself
  immediate-mode, so no diffing is needed). `state` is the *same* Lua table
  across every one of those frames — seeded once from `open()`'s `ctx_json`
  — so a widget callback (`on_click`/`on_change`) mutating `state` directly
  is naturally visible on the next frame with no extra plumbing. Use this
  for purely cosmetic, client-local state (selection highlight, scroll
  position); anything server-authoritative still goes over `ui.send_event`.
  A callback that wants a different screen should still `ui.close()` + have
  the server `open_ui()` again.
- Widget types implemented: `label`, `panel`, `button`, `textbox`, `list`
  (spec's named set minus **item grid**, deferred to 5.1 — needs real
  items), plus `rect` (Phase 6.16 — see below). Each widget table: `id`,
  `type`, `x`/`y`/`w`/`h`, `text` (label/panel/button/textbox), `items`/
  `list_index` (list only), and optional `on_click`/`on_change` callback
  fields.
- **`rect` — a raw filled/outlined rectangle, no baked-in meaning (Phase
  6.16).** `color = {r,g,b,a?}` (fill, default opaque white) and an optional
  `border = {r,g,b,a?}` (default: no border drawn). Unlike every other
  widget type, this one carries no semantic ("this is a progress bar", "this
  is a health bar") at all — it's the one primitive deliberately left
  meaning-free so a pack can compose *any* purely-visual element (a progress
  bar, a divider, a health bar segment, a colored swatch) out of one or more
  of them, entirely in Lua, rather than the engine shipping a
  `progress_bar`/`health_bar`/... widget type per use case.
- **HUD (Phase 6.16): `ui.define_hud(render_fn)`** — registers a single
  always-on overlay, separate from `ui.define`'s named-screen registry above.
  Unlike a modal screen, it's never `open()`/`close()`'d: `render_fn(state)`
  is evaluated every UI frame unconditionally (its own persistent `state`
  table, untouched by any modal screen opening/closing alongside it) and
  drawn regardless of whether a modal screen is also up. `content/base/ui/
  hud.lua` uses this + two `rect` widgets (a background/border, and a fill
  sized by the raw fraction below) to render the hold-to-break progress bar
  — "engine provides raw state, Lua deals with presentation": the engine
  still owns the actual hold-timer/reach/target-tracking logic (that's
  gameplay input handling), and offers only the meaning-free `rect`
  primitive, not a "progress bar" concept of its own. HUD widgets can be
  interactive too: a `button`/`textbox`/`list` widget's `on_click`/`on_change`
  fires exactly like a modal screen's own, via a separate HUD-only
  `report_hud_click`/`report_hud_change`/`report_hud_list_change` path
  (`src/client/main.cpp`'s HUD draw call) — `ui.send_event(...)` called from
  inside one of those carries `ui_name = "hud"` (there's no modal screen name
  to use).
- **Raw client-local state: the `client` table** (Phase 6.16, sibling to
  `ui` above) — read-only engine state a HUD (or any UI script) can query;
  nothing here draws a pixel, callers decide whether/how to show it:
  - `client.break_progress()` — `nil`, or `0..1` while the player is holding
    to break a targeted block.
  - `client.inventory()` — list of `{name=.., count=.., item=..}` (`item` is
    the raw block/item id, `0` = empty slot; what an `icon` widget's `item`
    takes). Live: follows `S2C_Inventory`. `client.selected_slot()` is the
    1-based selected hotbar slot.
  - `client.time()` — monotonic seconds, for presentation timing (fades).
    `client.mouse_position()` — `{x=.., y=..}` in window pixels, the same
    space as widget x/y (meaningful while the cursor is free, i.e. a modal
    screen is open).
  - `client.health()` / `client.hunger()` — `{current=.., max=..}` for the
    local player (`S2C_PlayerStatus`), or `nil` before the server's first
    status arrives. Updated whenever the value changes server-side.
  - `client.screen_size()` — `{width=.., height=..}`; widgets take absolute
    pixel positions like everywhere else in this API, so centering something
    in a HUD needs the real window size rather than a hardcoded guess.
- `ui.send_event(kind, value)` — sends a `C2S_UiEvent` to the server
  (`current_name`/the widget whose callback is currently running are filled
  in automatically). `ui.close()` — always sends one `"close"` event, then
  runs the layout's own `on_close` (if any) for local cosmetic cleanup, then
  clears state. The mouse stays released afterwards (the player clicks back
  into the world) unless the pack asks otherwise: `ui.close{ capture_mouse =
  true }`, a layout field `capture_mouse_on_close = true` (the default for
  every close of that screen, including server-side ones), or
  `client.capture_mouse(true)` from any callback. A capture request waits
  until no screen or chat is open, and the click that closed the screen is
  not sent as an attack/break. `client.capture_mouse(false)` releases it;
  `client.mouse_captured()` reports the current state. `ui.close()` may be
  called from the screen's own render function (e.g. "close myself when the
  server reopens me with `done = true`"): the render finishes normally and
  the close happens right after it, running the `on_close` that render
  returned.
- Client wiring (`src/client/main.cpp`): every synced `ui/*.lua` file
  (`ClientSession::virtual_pack_fs()`, Asset Sync/Phase 4.4) is loaded into
  `UiRuntime` right after join; `ClientSession::take_open_ui()` then drains a
  pending `S2C_OpenUi`, `UiRuntime::open()` evaluates the named layout, and
  `UiRenderer::draw()` runs once per frame while open, between the 3D pass
  and `window.end_frame()`; interactions route back through
  `report_click`/`report_change`/`report_list_change`. `content/base/ui/
  pause.lua` (`base:pause`) and `ui/inventory.lua` (`base:inventory`) are
  real, loadable screens — **known gap:** nothing in real gameplay currently
  *opens* either one (no client gesture or `C2S` message requesting "open my
  inventory"/"pause" exists; `player:open_ui` is server-push-only), and
  `ui/inventory.lua` renders raw numeric item ids (a `list` widget, not the
  spec's item-grid, §10.4's own still-missing widget type) rather than
  names, for the same `BlockId`-as-item-space reason noted above. The
  mechanism itself is exercised by `tests/unit/ui_runtime_test.cpp` and an
  end-to-end `player:open_ui` → click → `vb.on("ui_event", ...)` round trip
  in `tests/unit/pack_runtime_integration_test.cpp`. `ui/hud.lua` (`Phase
  6.16`) is the always-on HUD, loaded the same way; `--singleplayer` reads
  every `ui/*.lua` file straight off disk (`kSingleplayerContentPack`)
  rather than through Asset Sync, since it never asset-syncs at all — a
  real multiplayer connection still uses `virtual_pack_fs()`.

## Worked example — `content/base`

`content/base` is the pack `voxel_browser_server` loads by default and the
target of `tests/unit/content_pack_test.cpp`'s regression coverage (it loads
the real files, not inline Lua strings). Read it alongside this doc rather
than as a black box — every file is commented explaining *why*, not just
*what*. Load order (`src/script/pack_loader.cpp`): `blocks/*.lua` →
`entities/*.lua` → `biomes/*.lua` → any other root-level `*.lua` file
(sorted) → `init.lua` last.

| File(s)                              | Demonstrates                                            |
| ------------------------------------- | -------------------------------------------------------- |
| `blocks/dirt.lua`, `grass.lua`, etc.  | `vb.register_block`, idempotent re-declaration of the Phase 2 base set, `on_break` calling `vb.world.spawn_item_drop` |
| `blocks/planks.lua`, `sticks.lua`     | Registering genuinely new, crafted-only blocks (not a re-declaration) |
| `crafting.lua`                        | A full, working game system (recipes, ingredient checks, a `/craft` chat command) built entirely in content on top of `vb.register_craft` + `player:give`/`take` — **the reference example of "game rules belong in a pack, not the engine"** |
| `entities/dropped_item.lua`           | `vb.register_entity` — real dispatch since Phase 6.1 (`vb.world.spawn`/`on_spawn`/`on_tick`/`on_hit`/`on_death` all fire, no EnTT registry involved), but nothing in `content/base` itself ever calls `vb.world.spawn("base:dropped_item", ...)` — real block drops still go through the separate, already-working `vb.world.spawn_item_drop` hardcoded path above instead. `content/examples/kitchen_sink/entities/sentry.lua` (Phase 6.15) is the worked example of a pack actually spawning/hitting/killing one of these |
| `biomes/plains.lua`, `forest.lua`, `worldgen.lua`, `structures/*.lua`, `data/blocks.lua` | `vb.register_biome` with `decoration = {{structure=, spawn_rate=}, ...}`, `vb.worldgen.set_pipeline` (`worldgen.lua`, which also registers every structure from `structures/all.lua`), and the data files behind them: structures written by the structure editor and the block data script it reads. Terrain keeps the fixed default's range and gets sand beaches through `beach = "base:sand"`. `content/examples/kitchen_sink` is the other worldgen example |
| `ui/inventory.lua`, `ui/pause.lua`    | `ui.define`, real screens loaded by every connecting client |
| `ui/_style.lua`                       | A shared `base_ui` style table; the client sorts `ui/*.lua` before loading so `_style.lua` is always first |
| `ui/death.lua`, `death.lua`           | A `player_death` handler (returns nothing, so default respawn stands) that opens a "you died" notice via `player:open_ui` |
| `ui/hud.lua`                          | `ui.define_hud` + the `client.*` raw-state table (Phase 6.16) — the always-on hold-to-break progress bar |
| `init.lua`                            | Pack-wide setup that isn't a single registration — `vb.storage` persisting a boot counter across restarts |

If you're writing a new pack: copy `content/base`'s directory layout, keep
`pack.toml`'s `entry = "init.lua"` (not yet read — `load_content_pack` is the
real entry point until `require` exists, §10.2), and remember the loader
only auto-loads `blocks/`, `entities/`, `biomes/`, and root-level `*.lua`
files — anything under `ui/`/`textures/` is loaded client-side over Asset
Sync instead, never by the server's `load_content_pack`.

## Worked example — `content/examples/kitchen_sink` (Phase 6.15)

A second, sibling worked example — **not** loaded by default by anything,
point an operator's `--content-pack`/`server.toml` at
`content/examples/kitchen_sink` to run it. Where `content/base` stays
minimal/production-shaped (spec §5.1) and each Phase 6 feature it touches is
demonstrated only incidentally, this pack's entire purpose is the opposite:
exercise every Phase 6 "default + override" API at least once, each with a
short comment naming the exact phase/section it demonstrates (see its
`init.lua` for the full file-by-file index) — a working reference that
`tests/unit/kitchen_sink_pack_test.cpp` regression-tests the same way
`content_pack_test.cpp` protects `content/base`. Covers: `register_block`'s
`max_damage`/`max_stack`/`pickup_radius`/`item_lifetime_seconds` together on
one block (`blocks/unstable_ore.lua`), `register_entity` +
`vb.world.spawn` — real and dispatched since Phase 6.1 (2026-09-17), a
`/sentry` chat command actually spawns one and `on_tick`/`on_hit`/`on_death`
all really fire (`entities/sentry.lua`; `content/base/entities/
dropped_item.lua`'s own "nothing calls them" comment predates 6.1 and is now
stale — not this pack's file to fix), `register_biome` with real
`probability`/`adjacency`
(`biomes/savanna.lua`/`tundra.lua`) feeding a real `vb.worldgen.set_pipeline`
+ `vb.noise.*` graph with a carver and a vein (`worldgen.lua`) — the worked
pipeline example `content/base`'s own biome files point to —
`vb.physics.set_params` (`physics.lua`), `vb.daynight.set_curve`/
`set_day_length` (`daynight.lua`), `vb.db`/`vb.crypto.hash` plus a
`block_break_tick` handler (`mechanics.lua`), `player_death` with custom
`heal`/`message`/`drop_inventory` (`death.lua`), chat text-rewriting
(`chat.lua` — contrast with `content/base/crafting.lua`'s veto-only use of
the same event), and `register_keybind` + `player_input` opening a
`ui.define` screen via `player:open_ui` (`keybinds.lua` + `ui/status.lua`).

## Audio / sfx — not implemented (v0 has no audio subsystem)

There is no sound/music API, client-side or server-side, and no engine code
plays audio anywhere. This is a deliberate v0 scope cut, not an oversight:

- `cmake/Dependencies.cmake` builds raylib with
  `SUPPORT_MODULE_RAUDIO OFF` — raylib's audio module (`InitAudioDevice`,
  `PlaySound`, `LoadMusicStream`, ...) is compiled out entirely, so it isn't
  even linkable from `vb_render` today, let alone exposed to Lua.
- `ARCHITECTURE_SPEC.md` §10.5's block-break event-flow diagram mentions
  "sfx trigger" as an example of what a pack's `on_break` callback might
  *eventually* do (illustrative, alongside "drops") — it was never a real
  hook and nothing dispatches it. `register_block`'s `on_break`/`on_place`
  callbacks (Phase 4.2, `PackRuntime`) exist and fire for real, but a pack
  has no `vb.`/`ui.` call it could make from inside one to actually produce
  a sound.
- There is no `S2C_PlaySound`-shaped wire message, and no client-side sound
  cache/loader analogous to `ClientAssetCache` for textures/scripts (Phase
  4.4) — packaging sound assets for asset sync would need one.
- Tracked as a first-class deferred item, not folded into any phase's
  backlog: `REMAINING_TASKS.md`'s "Deferred (post first-playable)" list has
  "Audio subsystem + Lua sfx/music API" as its own line.

When this lands, the natural shape (unconfirmed, not designed) would mirror
`player:send_message`/`open_ui`: a server-authoritative
`entity:play_sound(name, opts?)` / `vb.world.play_sound_at(pos, name)` call
that sends a small S2C message, with sound files traveling over the existing
Asset Sync pipeline (Phase 4.4) like any other pack asset — but none of that
exists yet.

## Sandbox

Implemented in `Vm` construction (`strip_sandbox`): opens only `base`, `string`,
`table`, `math`, `coroutine`, `utf8`, then nils `os`, `io`, `dofile`, `loadfile`,
`load`, `loadstring`, `collectgarbage`, `require`, `package`, and every `debug.*`
except `traceback`. Instruction-count hook (`lua_sethook` / `LUA_MASKCOUNT`) and a
ceiling allocator are always on.

Still to do (Phase 4.4): reimplemented `require` over the synced virtual pack FS;
per-callback wall-clock budget enforced by the tick loop; the separate restricted
client UI VM. See §10.2.
