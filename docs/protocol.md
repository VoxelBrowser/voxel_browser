# Voxel Browser — Wire Protocol Reference

> Normative reference for the wire format. Update this file in the **same commit**
> as any change to a struct in `inc/vb/protocol/`, and bump
> `kEngineProtocolVersion` in `cmake/version.hpp.in`.

Current `ENGINE_PROTOCOL_VERSION`: **32**.

- **32** — Entity labels, clips, attachments, layers and real on-ground flags.
  - New `S2C_EntityProps` (61, lane `kFeedback`, reliable ordered): `u32
    server_tick`, `varint n` (≤ 65536) + `n × (u32 net_id, u8 mask, fields)`.
    `mask` bit 0 = text, bit 1 = clip, bit 2 = attachment (other bits ⇒
    `kMalformed`); each present field is `bool has` + its value (`has =
    false` clears it), an absent field is unchanged.
    - text: `EntityText` = `string value` (UTF-8, ≤ 64 bytes,
      `kMaxEntityTextBytes`; longer ⇒ `kLengthExceeded`), `u8×4 color` (RGBA),
      `bool has_background` + `u8×4 background`, `f32 size` (line height,
      metres), `f32 offset_y` (metres above the entity's position), `f32
      max_distance` (0 = no limit), `bool through_walls`.
    - clip: `string` (≤ 64 bytes), the clip `entity:set_clip` forces.
    - attachment: `u32 parent`, `f32×3 offset`, `bool face_offset`, `bool
      has_layer` + `i8 layer`. The client draws the entity at the parent's
      interpolated position + offset (turned by the parent's yaw when
      `face_offset`); the server moves it there every tick as well.
    - visual (bit 3): an `EntityVisualOverride` (same encoding as in
      `EntityRecord`), replacing the entity's whole override. Since v32 the
      server sends a spawn-time `visual_override` this way too (reliable,
      and changeable later: `entity:/player:set_visual_override`), and never
      on `EntityRecord`. Works for players' net ids as well.
  - The server sends at most one per player per tick: every field of every
    entity (script entity or player) with any of them that entered that
    player's interest set this tick, plus each field that changed on an
    entity they already see, plus changes to the recipient's own player (so
    a client can draw its own outfit in third person). The properties live as long as the entity is in the client's
    interest set; because snapshots are unreliable and this is not, the
    client drops them on a snapshot `removed` entry only if they are older
    (`server_tick`) than the snapshot.
  - `EntityRecord.flags` bit 0 (`on_ground`) is now set on every record,
    not just `local`: players copy their collider; script entities without
    physics and item drops are always grounded. (Before, every remote entity
    looked airborne and resolved to the `jump`/`fall` clip.)
  - `EntityVisualDef` gains trailing `i8 layer` and `bool through_walls`;
    `EntityVisualOverride` gains trailing `bool has + i8 layer` and `bool has
    + bool through_walls`. Each layer step draws a billboard 0.02 blocks
    nearer the camera; `through_walls` skips the depth test.
  - `EntityVisualDef` gains trailing `varint n` (≤ 16) + `n ×
    EntityVisualLayer`; `EntityVisualOverride` gains trailing `bool has +
    layers` the same way. `EntityVisualLayer` = `string texture`, `bool
    below`, `u8 rows` (bit i = pose row i; 0 = all rows), `u8×4 tint`.
  - `S2C_ServerInfo` gains a trailing `bool third_person_allowed`
    (`vb.render.set_third_person`).
  - `EntityKindRegistryRecord` gains a trailing `bool hidden`
    (`vb.register_entity{visual = false}`): the client draws no sprite or
    placeholder for that kind.

- **31** — `S2C_BlockDamage` moves from lane `kWorld` to a new lane
  `kFeedback` (5, reliable ordered) and gains a trailing `u64 revision`: the
  server's revision of the chunk holding `pos` when it was sent. On
  `GnsTransport`, `kFeedback` is its own GNS connection lane (lane 1, sent
  ahead of lane 0) without Nagle batching, so a damage update no longer waits
  behind queued chunk data. Because it is no longer ordered against
  `S2C_ChunkDelta`/`ChunkAdd`, the client (`net::BlockDamageTracker`) drops a
  damage update whose `revision` is older than the last change to the block
  at `pos` (a delta's `new_revision`, or a re-sent chunk's `revision` for the
  voxels it changed).

- **30** — `S2C_ServerInfo` gains a trailing `string engine_version_req` (≤ 128
  bytes, `kMaxEngineVersionReqBytes`; longer ⇒ `kLengthExceeded`): the pack's
  `pack.toml` `engine_version_req` (Cargo-style comparators such as
  `>=0.6.0, <0.7.0`; empty = no requirement). The client parses it with
  `vb::core::VersionReq` and disconnects itself ("this server's pack needs Voxel
  Browser >=0.6.0; you have 0.5.2 -- run `vb update`") before any asset is
  downloaded; an unparseable value also disconnects (fail closed). Design:
  `architecture_spec/dev-experience.md` §3.7.

- **29** — External authentication plumbing (Phase 9.2; design:
  `architecture_spec/auth.md` §5.2). `AuthMode` gains `kExternal = 2`.
  New `S2C_AuthChallenge` (8, lane `kControl`), sent right after
  `S2C_ServerInfo` only when `auth_mode == kExternal`: `string provider`,
  `string display_name`, `string issuer`, `string client_id`, `varint n` +
  `n × string scope` (n ≤ 16), `varint n` + `n × (string key, string value)`
  params (n ≤ 16), `string nonce` (≤ 256 bytes, server-random, bound into the
  sign-in so a token cannot be replayed on another connection); every string
  except the nonce ≤ 2048 bytes. `S2C_AuthResult` gains a trailing `string
  resolved_name` (the name the server will use; for external auth, the
  verified `name_claim`). `C2S_Auth.token` is capped at 16 KiB
  (`kMaxAuthTokenBytes`; longer ⇒ `kLengthExceeded`). Wire format only, not yet
  sent: `S2C_ReauthRequest` (9: `string nonce`, `u16 grace_seconds`) and
  `C2S_Reauth` (10: `string token`), used from Phase 9.6. Server handshake
  gains `kVerifyingAuth` (token received, verdict pending; no other client
  message is accepted) and a separate `auth_timeout_seconds` (default 300)
  for the sign-in window. A rejected or unavailable verifier closes the
  connection before `S2C_AssetManifest`.

- **28** — `S2C_PlayerStatus` (108, lane `kControl`) payload defined:
  `f32 health`, `f32 max_health`, `f32 hunger`, `f32 max_hunger`. Sent to
  one player the tick after they join and again whenever any value changes
  (the server diffs each tick, so damage, healing, hunger decay and respawn
  all reach the client without per-source dirty flags). Full snapshot.
  `ClientSession::player_status()` keeps the latest copy; `nullopt` until
  the first arrives. Lets a Lua HUD draw health/hunger bars.

- **27** — `C2SHello` (`C2S_Hello`, 1) gains a trailing `u8 client_flags`
  (new constant `kClientFlagAutomation = 1 << 0`). Clients built with
  `VB_WITH_AUTOMATION` set it; a server built without automation
  (`HandshakeServerConfig::accept_automation_clients == false`, the default
  for such builds) refuses the connection in the very first handshake step,
  before `S2C_ServerInfo`, with `S2C_Disconnect{kBadHandshake, "automation
  clients are not accepted by this server"}`. Keeps dev/test clients off
  production servers (docs/e2e-automation.md §7.4). Not a security boundary:
  anyone can build a client that clears the bit. Side effect of appending a
  field to the first message: a pre-27 client now fails the server's Hello
  decode ("malformed Hello") rather than getting the friendlier
  protocol-mismatch reason, same as any earlier Hello change would have.

- **26** — `BlockRegistryRecord` (`S2C_BlockRegistry`, 40) gains `string
  crack_texture` -- mirrors `vb::world::BlockType::crack_texture` (spec
  §5.2/§10.7): an optional pack-relative crack-stage spritesheet path (`vb::
  render::CrackAtlas::kStages` equal-width square frames, left to right),
  empty = the client's own built-in generic crack overlay. Closes
  REMAINING_TASKS.md 6.5's last piece ("real crack-stage texture art +
  crack_texture override"). Set via `vb.register_block{crack_texture=...}`
  (`BlockRegistry::set_crack_texture()`, same "attach without disturbing
  other already-frozen fields" posture as `texture`/`set_texture()`).
  Alongside this, a real pre-existing bug was fixed in the same commit: all
  three sites that build/apply a `BlockRegistryRecord`
  (`src/server/main.cpp`'s and `src/client/main.cpp`'s `host.block_registry`
  callbacks, and `ClientSession::apply_block_registry()` in
  `src/net/session.cpp`) used an aggregate-init listing only the record's
  first 5-6 fields, silently dropping `max_damage` (and now `crack_texture`)
  the whole time -- no client ever actually received a nonzero `max_damage`
  for any block, so `client.break_progress()` (introduced in **25**, below)
  was permanently `nullopt` in practice until this fix, regardless of a
  block's real `max_damage`.
- **25** — New `S2CBlockDamage` (54, lane `kWorld`): `IVec3 pos`, `u16
  punches`. Closes the deferred half of Phase 6.5 ("no wire message
  replicates the damage value itself to nearby players yet") — `ServerSession`
  broadcasts it whenever a block's live punch count (`block_punch_counts_`,
  driven by `punch()`/6.18) changes, to every player who currently mirrors
  the chunk containing `pos` (`WorldReplicator::player_has_chunk`), not just
  the puncher. `punches == 0` means "no damage" (fully healed via
  `update_block_punch_healing()`, or the block just broke) — the client
  drops any crack-overlay state for that `pos` rather than treating 0 as a
  value to render. No `max_damage` field: the receiving client already
  knows the block's registered `BlockType::max_damage` from its own chunk
  mirror + block registry, so this stays a pure delta. `ClientSession::
  block_damage()` exposes the resulting `pos -> punches` map;
  `client.break_progress()` (Phase 6.16's HUD primitive, previously always
  `nullopt` awaiting exactly this) now reports the currently-looked-at
  block's fraction (`punches / max_damage`) when it has live damage.
- **24** — `S2CFogParams` (52) gains an optional underwater tint (Phase 7.5's
  override half, spec §7.5): `bool has_underwater_tint` followed by, only if
  true, `u8 underwater_tint_r`, `u8 underwater_tint_g`, `u8
  underwater_tint_b`. `has_underwater_tint = false` means "no pack override,
  the client keeps computing its own texture-average/placeholder default,"
  not "black" — same "absence is not a value" posture `fog_start`/`fog_end`
  already had relative to the client's own `view_distance` default. Set via
  `vb.render.set_fog{start=, ["end"]=, underwater_tint={r=, g=, b=}}` — the
  new `underwater_tint` field is itself optional, sibling to the already-
  required `start`/`end`. Above-water fog color is unaffected and still
  never independently settable (2026-09-19's decision, unchanged).
- **23** — `EntityVisualDef` (within `S2C_EntityKindRegistry`'s optional
  `visual`, 53) gains a `bool mirror` field, appended right after `origin_y`
  (before `clip_count`); `EntityVisualOverride` (within `S2C_EntitySnapshot`'s
  optional `visual_override`, 60) gains the matching optional `mirror`
  (its own presence bool, appended right after `facings`'s). User-requested:
  the debug player sprite made it obvious `select_pose()`'s mirroring (one
  authored "side" pose flipped horizontally for the opposite side) reads as
  "no real left-facing art," not just a cosmetic nit — `mirror = false` (a
  pack opts in per kind or per instance override; default `true` preserves
  every existing pack's behavior unchanged) requires a fully authored row per
  facing instead (`render::build_entity_visual_layout`'s row count becomes
  `mirror ? facings/2+1 : facings`) and `select_pose()` never flips anything
  for that kind. Landed alongside a real bug fix, unrelated to the wire
  format: `render::EntityRenderer`'s per-entity `EntityPresentationState` was
  permanently constructed with a hardcoded facings=8 (`kDefaultFacings`)
  regardless of the entity's actual registered kind — a facings=4 kind (e.g.
  `base:player`) had its pose picked using 8-sector bucket math against a
  3-row (or now 4-row) spritesheet, silently indexing rows outside its own
  texture. `EntityRenderer::sync()` now re-resolves the real facings/mirror
  from whichever `KindVisual` `draw()` will actually use (instance override,
  else kind default) every frame and rebuilds `state` only when that value
  changes.
- **22** — `InputCmd` (within `C2S_InputBatch`, 80) gains a `u8 selected_slot`
  field, appended after `keybinds`: which inventory slot (0-based) the player
  currently has selected, reported every cmd exactly like `buttons`/
  `keybinds` (entity-management follow-up, REMAINING_TASKS' "held item /
  hotbar selection" gap, Phase 6.20). The engine assigns no meaning to the
  index beyond "which slot of this player's inventory" — `ServerSession`
  just mirrors the latest value into `ecs::PlayerInput::selected_slot`
  (`net::ServerSession::selected_slot()` reads it back); a pack decides what
  "holding" that slot means via the new `player:get_selected_slot()`/
  `player:get_held_item()` Lua primitives (`content/base/mechanics.lua`'s
  right-click placing now reads the held item instead of a hardcoded
  `base_stone_id`). A pack's `vb.on("player_input", ...)` handler can also
  override it (the input table's `selected_slot`, 1-based to match
  `player:get_inventory()`'s own 1-based array), same override shape as
  `move`/`yaw`/`pitch`/`buttons`/`keybinds`.
- **21** — `EntityRecord` (within `S2C_EntitySnapshot`, 60) gains an optional
  `visual_override` field: a script entity's per-instance visual override
  (`vb.world.spawn(kind, pos, {visual_override = {...}})`, entity-management
  follow-up to version 20's kind-level `visual`, spec
  `architecture_spec/rendering.md` §11.3's "Per-instance override"). Layout,
  after the existing `flags`: `bool has_override`; if true, an
  `EntityVisualOverride` — every field independently optional, each prefixed
  by its own presence bool: `string texture`; `u16 frame_width, u16
  frame_height` (travel together, both or neither); `u8 facings`; `f32
  origin_x, f32 origin_y` (also together); `varint clip_count` + `clip_count ×
  {string clip, u16 frames, f32 fps}` (capped at 64, same shape as
  `EntityVisualDef.clips`). Client-side, `render::merge_visual_override`
  (`inc/vb/render/entity_visual_layout.hpp`) merges this field-by-field over
  the entity's kind-level `EntityVisualDef` (absent kind default = an
  all-default `EntityVisualDef{}`) before decoding — a pack overriding only
  `texture` (the "player skin" motivating case) keeps its kind's `facings`/
  `origin`/`clips` unchanged. **Only ever populated on an `entered` record**
  (`net::ServerSession::to_record`/`broadcast_snapshots`) — `updated`/`local`
  records always send `has_override = false`, which means "unchanged", not
  "cleared": `ClientSession` caches whatever it first learned
  (`entity_visual_overrides_`) for the rest of that NetId's replicated
  lifetime, the same "learned once, immutable" posture as `EntityRecord.kind`
  itself. There is no wire path yet to change or clear an override after a
  client has already seen the entity — deliberately out of scope for this
  pass, see `REMAINING_TASKS.md`.
- **20** — `EntityKindRegistryRecord` (within `S2C_EntityKindRegistry`, 53)
  gains an optional `visual` field: real per-kind spritesheet art
  (`vb.register_entity{visual = {...}}`, entity-management follow-up to
  version 19's width/height item, spec `architecture_spec/rendering.md`
  §11.3's 2026-09-17 schema). Layout, after the existing `name`/`width`/
  `height`: `bool has_visual`; if true, `string texture`, `u16 frame_width`,
  `u16 frame_height`, `u8 facings`, `f32 origin_x`, `f32 origin_y`, then
  `varint clip_count` + `clip_count × {string clip, u16 frames, f32 fps}`
  (capped at `kMaxEntityClips = 64`, `src/protocol/world.cpp`). Absent
  (`has_visual = false`) for a kind that never sets `visual = {...}` (e.g.
  `kitchen_sink:sentry`, which only sets `width`/`height`) — same
  "missing = default" posture as every other opt-in field here; that kind's
  billboard stays the flat placeholder exactly as version 19 shipped it.
  `frame_width`/`frame_height` are the *resolved* pixel size of the pack's
  chosen named variant (`small`/`tall`/`flat`/`medium`/.../`large_flat`,
  looked up server-side at registration in `PackRuntime`'s
  `parse_entity_visual` — see `docs/lua-api.md`) — only the resolved size
  travels the wire, never the variant name. The required sheet size
  (`frame_width * sum(clip frames)` by `frame_height * (facings/2+1)`) is
  validated against the real decoded PNG client-side
  (`render::build_entity_visual_layout`, `inc/vb/render/
  entity_visual_layout.hpp`), not on this struct — a mismatch there leaves
  that kind on the flat placeholder (logged `VB_WARN`), not a decode error.
- **19** — `S2C_EntityKindRegistry` (53) defined (entity-management follow-up
  to Phase 6.1/REMAINING_TASKS' "no client-side kind-specific rendering for
  script entities" item): `varint n` + `n × {string name, f32 width, f32
  height}`. `kinds[i]` describes `core::EntityKindId` value `i + 1`, matching
  `PackRuntime::Impl::entity_kinds`' own registration-order id assignment, so
  no id is sent per record. Sent between `C2S_Ready` and `S2C_JoinAccept`
  alongside `S2C_BlockRegistry`/`S2C_KeybindRegistry` only if
  `HandshakeServerHost::entity_kind_registry` returns a value; `nullopt`
  default (no pack ever called `vb.register_entity`) sends no frame, so a
  host/test that doesn't use this sees zero behavior change and every remote
  entity keeps rendering as the flat placeholder billboard it always has.
  `width`/`height` default to the placeholder's own `0.8`/`1.8` metre
  dimensions, overridable per kind via `vb.register_entity{width=, height=}`.
  `PackRuntime::install_entity_kind_registry(host)` wires the built vector in
  (mirrors `install_keybind_registry`'s shape exactly). A player's
  `EntityRecord::kind` is always `kInvalid` (0) and never indexes into this
  list — `render::EntityRenderer` keeps the placeholder default for those.
- **18** — `S2CServerInfo` (`kS2CServerInfo`) gains a `u32 view_distance`
  field: the operator's real `server.toml` `view_distance`, sent always (not
  an opt-in `nullopt`-default frame like `S2C_FogParams`/`S2C_MoveParams` —
  the server always knows this about itself). `HandshakeServerConfig::
  view_distance` (`inc/vb/net/handshake.hpp`) carries it from
  `ServerConfig::view_distance` (`src/server/main.cpp`) / the integrated
  singleplayer host's own `sp_server_config()` (`src/client/main.cpp`,
  echoing the local player's own `configured_view_distance` back, a no-op
  clamp). The client clamps its effective `view_distance` — which both
  `kLoading`'s expected-chunk-count estimate and the default fog distance
  (`fog_end = view_distance * kChunkDim`) are computed from — to
  `min(configured_view_distance, S2CServerInfo::view_distance)` in
  `enter_playing()` right after joining, so a real server advertising a
  smaller view distance than the player asked for can never leave fog
  reaching past where that server will actually stream chunks (closes the
  gap `client.toml.example`'s `render_distance` comment already claimed —
  "clamped to the server's view_distance" — but that was never actually
  implemented until now).
- **17** — Real texture/atlas system: `BlockRegistryRecord` (`S2C_BlockRegistry`,
  40) gains a `string texture` field (pack-relative path, e.g.
  `"textures/stone.png"`; empty = no texture, unchanged rendering), mirroring
  `vb::world::BlockType::texture`. The client resolves it against its own
  Asset Sync virtual FS (`vb::assetsync::ClientAssetCache`) and packs every
  referenced texture into one `vb::render::TextureAtlas` per session
  (`src/client/main.cpp`, right after the block registry is applied and
  before any chunk is meshed) — closes the long-standing "no real texture
  system in this engine at all" gap tracked across `REMAINING_TASKS.md`'s
  Phase 4/5/6.5/7.5 items. A block with no texture (or whose texture path
  isn't synced) keeps rendering its old flat placeholder color, unaffected.
- **16** — `S2C_FogParams` (52) payload defined (Phase 7.2, spec §7.2):
  `f32 fog_start`, `f32 fog_end`. Sent between `C2S_Ready` and
  `S2C_JoinAccept` alongside `S2C_MoveParams`/`S2C_DayNightCurve` only if
  `HandshakeServerHost::fog_params` returns a value; `nullopt` default = no
  frame, and unlike move_params/day_night_curve there's no universal engine
  default this replaces — the server doesn't know each client's own
  `view_distance`, so a client with no override computes its own default
  fog distance from it. No color field: fog always blends into whatever
  `vb::world::sky_color_for_time()` already returns for the current time of
  day, never an independently drifting tint (decided 2026-09-19).
  `PackRuntime::effective_fog_params()` returns the raw `{start, end}` a
  pack passed to `vb.render.set_fog{...}`, or `nullopt` if no pack ever
  called it.
- **15** — `S2C_DayNightCurve` (51) payload defined (Phase 6.8, spec §5.4):
  `varint n` + `n × {u32 tick, f64 brightness, u8 r, u8 g, u8 b}`, mirroring
  `vb::world::DayNightKeyframe` flat (protocol/ never depends on world/, same
  posture as `S2C_MoveParams`). Sent between `C2S_Ready` and `S2C_JoinAccept`
  alongside `S2C_BlockRegistry`/`S2C_MoveParams` only if
  `HandshakeServerHost::day_night_curve` returns a value; `nullopt` default =
  no frame, so a host/test that never opts in leaves the client on
  `vb::world::default_day_night_curve()`, unchanged. `PackRuntime::
  effective_day_night_curve()` returns the raw keyframe list a pack passed to
  `vb.daynight.set_curve{keyframes = {...}}`, or `nullopt` if no pack ever
  called it.
- **14** — `S2C_MoveParams` (50) payload defined (Phase 6.7, spec §7.3): a
  flat snapshot of `vb::physics::MoveParams` (`f64×13` tunables + `bool fly`)
  mirroring `vb::protocol::BlockRegistryRecord`'s posture of duplicating
  fields rather than protocol/ depending on physics/. Sent between
  `C2S_Ready` and `S2C_JoinAccept` alongside `S2C_BlockRegistry`/
  `S2C_KeybindRegistry` (`HandshakeServerHost::move_params`, `nullopt`
  default = no frame, zero behavior change) so client-side prediction uses
  the exact same tunables (gravity included) as the server's authoritative
  simulation instead of silently keeping `MoveParams`'s own hardcoded
  defaults. `PackRuntime::effective_move_params()` applies a pack's
  `vb.physics.set_params{...}` on top of the engine/operator default
  (`ServerConfig::gravity` folded in first) — only fields the pack actually
  sets are overridden.
- **13** — Phase 6.5 (shared block-damage breaking, spec §10.7):
  `BlockRegistryRecord` (`S2C_BlockRegistry`, 40) gains a `u16 max_damage`
  field (0 = today's instant break, no behavior change for any existing
  block). `C2S_BlockBreakBegin` (48) `{svarint×3 pos, svarint×3 face}` and
  `C2S_BlockBreakStop` (49) `{svarint×3 pos}` bracket a player holding a
  target with `max_damage > 0`; the shared damage pool itself and its
  completion into an actual break ride the *existing*
  `C2S_BlockEdit`/`S2C_ChunkDelta` pipeline server-side (`vb::world::
  BlockDamageSystem`) — no new replication channel for the damage value
  itself yet (no client renders cracks regardless, see 6.5's texture-atlas
  dependency note in `REMAINING_TASKS.md`).
- **12** — `S2C_KeybindRegistry` (47) payload defined (Phase 6.3, closed-schema
  custom keybinds): `varint n`, `n × string` (registered keybind names, index
  == bit position). Sent between `C2S_Ready` and `S2C_JoinAccept` alongside
  `S2C_BlockRegistry` when the host opts in
  (`HandshakeServerHost::keybind_registry`, wired by
  `PackRuntime::install_keybind_registry` from `vb.register_keybind` calls);
  `nullopt` default = no frame, zero behavior change. `InputCmd` (in
  `C2S_InputBatch`) also gains a `u32 keybinds` field (bit *i* = the keybind
  at index *i* held this cmd) — additive, capped at 32 registered names so
  the bitset always fits one `u32`.
- **11** — `S2C_Inventory` (107) payload defined (Phase 5.1, real inventory
  sync): `varint n`, `n × {u16 item, u16 count}`. Sent to one player whenever
  their inventory changes (currently: after `player:give()`); always a full
  snapshot, not a delta. `ClientSession::inventory()` keeps the latest copy.
  Closes the "no wire message syncing inventory contents to the client at
  all" gap `REMAINING_TASKS.md` 5.1 tracked — `player:get_inventory()`/
  `player:give()` (Phase 4.2) were server-Lua-only state until now.
- **10** — `S2C_TimeOfDay` (46) payload defined (Phase 5.4, day/night cycle):
  `u32 time_of_day`. The server advances a `time_of_day` clock every tick
  (`vb::world::advance_time_of_day`, `ServerSession::set_day_length_seconds`,
  default 1200s/day) and broadcasts it to every playing connection about once
  a second; `S2C_JoinAccept::time_of_day` already carried the initial value
  (since version 1) but nothing advanced it server-side or kept an
  already-connected client in sync until now.
- **9** — `S2C_PlayerJoin` (104), `S2C_PlayerLeave` (105), `S2C_PlayerList`
  (106) payloads defined (Phase 5.4, player list / join-leave messages).
  Broadcast to already-playing connections when a new player finishes the
  join handshake / disconnects; `S2C_PlayerList` is sent once to a newcomer
  listing everyone else already playing.
- **8** — `C2S_Chat` (100) payload defined (Phase 5.4, HUD chat box):
  `string text`, sent by a playing client. Routed server-side through
  `vb.on("chat", handler)` (veto); if allowed, broadcast to every playing
  connection as `S2C_Chat{"<name>: <text>"}`.
- **7** — `C2S_UiEvent` (102) payload defined (Phase 4.5, client UI VM):
  `ui_name`, `widget_id`, `event_kind`, `value_json`. Sent when a widget's
  `on_click`/`on_change`/`on_close` Lua callback calls
  `ui.send_event(...)`/`ui.close()`; routed server-side to
  `vb.on("ui_event", handler)`.
- **6** — Asset sync (Phase 4.4, spec §9): `C2S_AssetManifestRequest` (20),
  `S2C_AssetManifest` (21), `C2S_AssetRequest` (22), `S2C_AssetData` (23)
  payloads defined, AND the handshake sequence itself changes — three new
  states (`kAwaitingAssetManifestRequest`/`kAwaitingAssetRequest`/
  `kStreamingAssets` server-side; `kAwaitingAssetManifest`/`kSyncingAssets`
  client-side) sit between `S2C_AuthResult` and `C2S_Ready` unconditionally,
  not just an optional extra message — a structural, breaking change to the
  handshake, hence the version bump (not merely additive like 4/5).
- **5** — `S2C_BlockRegistry` (40) payload defined (Phase 4.3): sent between
  `C2S_Ready` and `S2C_JoinAccept` when the host opts in
  (`HandshakeServerHost::block_registry`); the dedicated server always opts
  in with its live (possibly Lua-extended) registry, `nullopt` (no frame)
  otherwise.
- **4** — `S2C_Chat` (101) + `S2C_OpenUi` (103) payloads defined (Phase 4.2,
  `player:send_message`/`player:open_ui`); no client handles them yet, but the
  wire format is real.
- **3** — `C2S_BlockEdit` (44) + `S2C_BlockEditResult` (45) payloads defined.
- **2** — `S2C_EntitySnapshot` gains `bool has_local` + trailing `EntityRecord local`
  (the recipient's own authoritative state, for client reconciliation);
  `C2S_InputBatch` (80) payload defined.
- **1** — initial shipped set.

## Conventions

- Little-endian for all fixed-width integers and IEEE-754 floats.
- `varint` = unsigned LEB128 (≤ 10 bytes). `svarint` = zig-zag + LEB128.
- `string` = `varint` length prefix + raw UTF-8 bytes (length capped at 64 MiB).
- `bool` = one byte, `0` or `1` (any other value is a decode error).
- Decoders are bounds-checked and return `Result<T, ProtocolError>`; a message
  that decodes but leaves trailing bytes is rejected (`kTrailingBytes`).

Primitives live in `inc/vb/protocol/byte_buffer.hpp` (`ByteWriter` / `ByteReader`).

## Envelope

`inc/vb/protocol/message.hpp` — every logical message is framed as:

| Field         | Type     | Notes                                        |
| ------------- | -------- | -------------------------------------------- |
| `type`        | `uint16` | `MessageType`                                |
| `flags`       | `uint16` | bitfield; `0x01` = payload is LZ4-compressed |
| `payload_len` | `uint32` | ≤ 16 MiB, else `kLengthExceeded`             |
| `payload`     | bytes    | `payload_len` bytes                          |

`read_frame()` reports `kShortBuffer` until the whole envelope+payload is
buffered, and yields `consumed` so a stream reader can advance.

## Lanes (GameNetworkingSockets)

`lane_for(MessageType)` maps each type to its lane:

| Lane | Name       | Reliability            | Message types                                  |
| ---- | ---------- | ---------------------- | --------------------------------------------- |
| 0    | `control`  | reliable ordered       | handshake, auth, chat, RPC, disconnect         |
| 1    | `world`    | reliable ordered       | block registry, chunk add/delta/remove, edits  |
| 2    | `snapshot` | unreliable (seq-gated)  | entity snapshots                               |
| 3    | `assets`   | reliable ordered       | asset manifest + file chunk transfer           |
| 4    | `input`    | unreliable (seq)        | `C2S_InputBatch`                               |
| 5    | `feedback` | reliable ordered       | `S2C_BlockDamage` (v31+), `S2C_EntityProps` (v32+); not ordered against `world` |

## Messages

### Handshake / control — `inc/vb/protocol/handshake.hpp` (implemented)

| Type (id)                | Fields                                                                 |
| ------------------------ | -------------------------------------------------------------------- |
| `C2S_Hello` (1)          | `u16 engine_protocol_version`, `u64 client_nonce`, `string client_version` `u8 client_flags` (v27+; bit 0 = `kClientFlagAutomation`, set by `VB_WITH_AUTOMATION` builds; a server not built with automation refuses it with `kBadHandshake`, see `docs/e2e-automation.md` §7.4; unknown bits ignored) |
| `S2C_ServerInfo` (2)     | `string pack_name`, `string pack_version`, `u16 engine_protocol_version`, `u16 tick_rate`, `string motd`, `u8 auth_mode`, `string engine_version_req` (v30+) |
| `C2S_Auth` (3)           | `string player_name`, `string token` (empty when `auth_mode = none`; ≤ 16 KiB) |
| `S2C_AuthResult` (4)     | `bool ok`, `string reason`, `string resolved_name` (v29+)              |
| `S2C_AuthChallenge` (8)  | v29+, only when `auth_mode = external`: `string provider, display_name, issuer, client_id`, `string[] scopes` (≤16), `(string,string)[] params` (≤16), `string nonce` |
| `S2C_ReauthRequest` (9)  | v29+, wire format only until Phase 9.6: `string nonce`, `u16 grace_seconds` |
| `C2S_Reauth` (10)        | v29+, wire format only until Phase 9.6: `string token` (≤ 16 KiB)      |
| `C2S_Ready` (5)          | *(empty)*                                                              |
| `S2C_JoinAccept` (6)     | `u32 net_id`, `f64×3 spawn_pos`, `u64 world_seed`, `u32 time_of_day`   |
| `S2C_Disconnect` (7)     | `u8 reason`, `string message`                                          |

`auth_mode`: `0 = none`, `1 = token`. `reason`: see `DisconnectReason` (0 unknown,
1 server full, 2 protocol mismatch, 3 auth failed, 4 shutdown, 5 kicked,
6 timeout, 7 protocol error, 8 bad handshake).

### Snapshot — `inc/vb/protocol/snapshot.hpp` (implemented)

| Type (id)               | Fields                                                                 |
| ----------------------- | --------------------------------------------------------------------- |
| `S2C_EntitySnapshot` (60) | `u32 server_tick`, `u32 last_acked_input_seq`, `varint n` + `n×EntityRecord entered`, `varint n` + `n×EntityRecord updated`, `varint n` + `n×u32 removed`, `bool has_local`, `EntityRecord local` (only if `has_local`) |
| `S2C_EntityProps` (61) | `u32 server_tick`, `varint n` + `n×(u32 net_id, u8 mask, fields)` — see the version-32 entry above |

`EntityRecord` = `u32 net_id`, `u16 kind`, `f64×3 pos`, `f32×2 rot` (yaw,pitch deg),
`f32×3 vel`, `u8 flags` (bit 0 = `on_ground`, set for every entity since v32), `bool has_override` + optional
`EntityVisualOverride visual_override` (version 21, see that entry above —
only ever set on an `entered` record). Interest culling excludes the
recipient, so their own authoritative state rides in `local` for
prediction/reconciliation (spec §8.4).

### Input — `inc/vb/protocol/input.hpp` (implemented)

| Type (id)            | Fields                                                        |
| -------------------- | ------------------------------------------------------------ |
| `C2S_InputBatch` (80) | `varint n` (≤64) + `n×InputCmd cmds` (ascending `seq`)       |

`InputCmd` = `u32 seq`, `f32 dt`, `f32×3 move` (x=strafe, y=up/fly, z=forward, [-1,1]),
`f32 yaw`, `f32 pitch`, `u8 buttons` (bit0 jump, bit1 sprint, bit2 primary,
bit3 secondary, bit4 fly-up, bit5 fly-down), `u32 keybinds` (bit *i* = the
keybind at index *i* of the most recent `S2C_KeybindRegistry` is held, Phase
6.3), `u8 selected_slot` (which inventory slot is selected, version 22). Sent
every client frame; each batch resends recent unacked commands. The server
simulates any `seq` above the last it has run and acks the highest via
`S2C_EntitySnapshot.last_acked_input_seq`.

### Asset sync — `inc/vb/protocol/assetsync.hpp` (implemented)

| Type (id)                     | Fields                                                        |
| ------------------------------ | ------------------------------------------------------------ |
| `C2S_AssetManifestRequest` (20) | `hash known_manifest_hash` ({0,0} = nothing cached)          |
| `S2C_AssetManifest` (21)        | `hash manifest_hash`, `u64 total_bytes`, `varint n` + `n×AssetEntryRecord entries` (empty if `known_manifest_hash` matched) |
| `C2S_AssetRequest` (22)         | `varint n` + `n×hash missing` (empty = "I have it all")     |
| `S2C_AssetData` (23)            | `hash hash`, `u32 seq`, `u32 total_chunks`, `varint len` + `len` bytes (≤ ~48 KiB target, decode caps at 1 MiB) |

`AssetEntryRecord` = `string path`, `hash hash`, `u64 size`, `u8 kind` (0
script / 1 texture / 2 model / 3 ui / 4 sound / 5 data). `hash` = two raw
`u64`s (`lo`, `hi` — xxHash3-128 of the file's bytes).

Sent between `S2C_AuthResult` and `C2S_Ready` (spec §9, see the handshake
sequence below): the server always sends exactly one `S2C_AssetManifest`
reply; if it has no manifest at all (opted out, or built without
`VB_WITH_COMPRESSION`), `manifest_hash` is `{0,0}` and `entries` is empty.
The client always replies with exactly one `C2S_AssetRequest`; an empty
`missing` list (nothing to fetch) skips straight past `kSyncingAssets`.
`missing` names each hash at most once, even when several manifest paths
share its bytes; the client writes the received file to every such path.
`S2C_AssetData` chunks are paced by the server (a small per-tick send budget,
not real flow-control windowing) and verified by hash on the client before
being committed to its content-addressed cache — a mismatch aborts the
connection (spec §9.4). No model/texture/asset-kind-specific handling exists
downstream of the cache yet (no Lua `require`, no texture loader) — the
assembled virtual pack filesystem (`path -> bytes`) is exposed but unread.

### Block registry — `inc/vb/protocol/world.hpp` (implemented)

| Type (id)              | Fields                                                        |
| ----------------------- | ----------------------------------------------------------- |
| `S2C_BlockRegistry` (40) | `varint n` + `n × {string name, bool solid, bool opaque, bool liquid, u8 light_emission, string texture, u16 max_damage, string crack_texture}` (index == `BlockId`) |

Sent between `C2S_Ready` and `S2C_JoinAccept` (Phase 4.3) only if
`HandshakeServerHost::block_registry` returns a value; `nullopt` (default)
sends nothing, so a host/test that never opts in is unaffected. The client
rebuilds a `world::BlockRegistry` from the records (in order, so ids match)
and swaps it into its `ClientChunkStore`. No model/texture/collision-shape
fields exist yet — those wait on asset sync (4.4) + the base pack (5.1).

### Physics parameters — `inc/vb/protocol/world.hpp` (implemented)

| Type (id)             | Fields                                                        |
| ---------------------- | ------------------------------------------------------------ |
| `S2C_MoveParams` (50) | `f64×13 {half_width, height, eye_height, walk_speed, sprint_speed, accel, air_accel, friction, gravity, jump_speed, terminal_velocity, step_height, fly_speed}`, `bool fly` |

Sent between `C2S_Ready` and `S2C_JoinAccept` (Phase 6.7) only if
`HandshakeServerHost::move_params` returns a value; `nullopt` (default) sends
nothing, so a host/test that never opts in leaves the client on
`vb::physics::MoveParams`'s own hardcoded defaults, unchanged. The client
swaps this straight into the same `physics::MoveParams` its local prediction
already runs (`ClientSession::set_move_params`), so a pack's
`vb.physics.set_params{...}` override (or the operator's `server.toml`
`gravity`) reaches client-side prediction exactly, not just server authority.

### Day/night curve — `inc/vb/protocol/world.hpp` (implemented)

| Type (id)                | Fields                                                        |
| ------------------------- | ------------------------------------------------------------ |
| `S2C_DayNightCurve` (51) | `varint n` + `n × {u32 tick, f64 brightness, u8 r, u8 g, u8 b}` |

Sent between `C2S_Ready` and `S2C_JoinAccept` (Phase 6.8) only if
`HandshakeServerHost::day_night_curve` returns a value; `nullopt` (default)
sends nothing, so a host/test that never opts in leaves the client on
`vb::world::default_day_night_curve()`, unchanged. The client rebuilds a
`world::DayNightCurve` from the records (in wire order) and uses it for both
`sky_brightness()`/`sky_color_for_time()` in place of the built-in default —
a pack's `vb.daynight.set_curve{keyframes = {...}}` reaches the actual
rendered sky, not just server-side bookkeeping.

### Fog parameters — `inc/vb/protocol/world.hpp` (implemented)

| Type (id)           | Fields                     |
| -------------------- | -------------------------- |
| `S2C_FogParams` (52) | `f32 fog_start`, `f32 fog_end`, `bool has_underwater_tint`, then if true `u8 underwater_tint_r`, `u8 underwater_tint_g`, `u8 underwater_tint_b` |

Sent between `C2S_Ready` and `S2C_JoinAccept` (Phase 7.2) only if
`HandshakeServerHost::fog_params` returns a value; `nullopt` (default) sends
nothing. Unlike `S2C_MoveParams`/`S2C_DayNightCurve` there is no universal
engine default this replaces — the server doesn't know each client's own
`view_distance`, so a client that never receives this frame computes its own
default fog distance from its own `view_distance` config
(`src/client/main.cpp`: `end = view_distance * kChunkDim`, `start = end *
0.6`). No color field: fog always blends into whatever
`sky_color_for_time()` already returns, never an independently drifting
tint. A pack's `vb.render.set_fog{start=, end=}` (`PackRuntime::
effective_fog_params()`) is the only source of this frame.

### World editing — `inc/vb/protocol/world.hpp` (implemented)

| Type (id)               | Fields                                                        |
| ----------------------- | ----------------------------------------------------------- |
| `C2S_BlockEdit` (44)      | `u32 predicted_seq`, `u8 action` (0 break / 1 place), `svarint×3 pos` (world voxel), `u16 block` (place only) |
| `S2C_BlockEditResult` (45) | `u32 predicted_seq`, `bool accepted`, `svarint×3 pos`        |

Client applies the edit optimistically to its chunk mirror keyed by
`predicted_seq`, then rolls back if `accepted` is false. The authoritative block +
light change fans out to every interested player as an `S2C_ChunkDelta`; the
server validates reach (≤ 5.5 blocks from the eye), target validity, and
non-floating placement (a Lua `block_break`/`block_place` veto slots in at
Phase 4.2).

### Shared block-damage breaking — `inc/vb/protocol/world.hpp` (implemented, mechanism only)

| Type (id)                    | Fields                                        |
| ----------------------------- | ---------------------------------------------- |
| `C2S_BlockBreakBegin` (48)    | `svarint×3 pos`, `svarint×3 face` (hit-normal) |
| `C2S_BlockBreakStop` (49)     | `svarint×3 pos`                                |

Sent by a client holding on a block whose registered `max_damage > 0`
(§5.2/§10.7); `ServerSession` gates entry (reach + `max_damage > 0` +
`vb.on("block_break_begin", ...)` veto), tracks contributors in a
`vb::world::BlockDamageSystem`, and drives `vb.on("block_break_tick", ...)`/
`vb.on("block_health_tick", ...)` once per server tick. Completion (summed
damage reaches `max_damage`) commits the break through the unchanged
`WorldReplicator::apply_block_edit` path — the exact same
`S2C_ChunkDelta`/`on_break` flow a manual `C2S_BlockEdit` triggers. No wire
message replicates the damage *value* itself to nearby players yet (no
client renders cracks regardless of the wire format — see
`REMAINING_TASKS.md` 6.5's texture-atlas dependency note), so a second
player currently can't *see* another's break progress, only feel its effect
once the block actually breaks.

### Day/night — `inc/vb/protocol/world.hpp` (implemented)

| Type (id)            | Fields                |
| --------------------- | --------------------- |
| `S2C_TimeOfDay` (46)  | `u32 time_of_day`     |

Periodic update of the clock `S2C_JoinAccept::time_of_day` already seeds at
join (spec §5.4). `ServerSession` advances its own `time_of_day` once per
tick (`vb::world::advance_time_of_day`, day length configurable via
`ServerSession::set_day_length_seconds`, default 1200s/day) and broadcasts
`S2C_TimeOfDay` to every playing connection roughly once a second — coarser
than snapshots since the clock only needs to look smooth, not be exact every
tick. The client folds it into `ClientSession::time_of_day()`, which returns
the join-time value until the first update lands. Tick convention: 0 =
sunrise, `kTicksPerDay/4` = noon, `kTicksPerDay/2` = sunset,
`3*kTicksPerDay/4` = midnight, wrapping at `kTicksPerDay` (24000) —
see `vb::world::daynight.hpp`. `src/client/main.cpp` derives a simple
4-keyframe sky gradient color from it (`sky_color_for_time`) for the
`ClearBackground` behind the 3D view, plus an "HH:MM" readout in the debug
overlay.

### Chat / UI RPC — `inc/vb/protocol/chat.hpp` (implemented)

| Type (id)           | Fields                                                        |
| -------------------- | ------------------------------------------------------------ |
| `C2S_Chat` (100)     | `string text`                                                 |
| `S2C_Chat` (101)     | `string text`                                                 |
| `C2S_UiEvent` (102)  | `string ui_name`, `string widget_id`, `string event_kind` ("click"\|"change"\|"close"), `string value_json` |
| `S2C_OpenUi` (103)   | `string ui_name`, `string ctx_json`                           |
| `S2C_PlayerJoin` (104) | `u32 net_id`, `string name`                                 |
| `S2C_PlayerLeave` (105) | `u32 net_id`                                               |
| `S2C_PlayerList` (106) | `varint n`, `n × {u32 net_id, string name}`                 |

### Inventory sync — `inc/vb/protocol/inventory.hpp` (implemented)

| Type (id)             | Fields                                                     |
| ---------------------- | ---------------------------------------------------------- |
| `S2C_Inventory` (107)  | `varint n`, `n × {u16 item, u16 count}`                     |

Sent to one player whenever their inventory changes (currently only
`player:give()`, Phase 4.2). Always a full snapshot of every slot, not a
delta — mirrors `S2C_PlayerList`'s "just resend the whole thing" posture.
`ClientSession::inventory()` holds the latest copy client-side; the HUD
hotbar (`src/client/main.cpp`) reads it directly.

`S2C_PlayerStatus` (108) — `f32 health`, `f32 max_health`, `f32 hunger`,
`f32 max_hunger`; see protocol 28 above. Read client-side through
`client.health()`/`client.hunger()` in `ui/*.lua`.

`S2C_Chat`/`S2C_OpenUi` sent by the server Lua runtime
(`player:send_message`/`player:open_ui`, Phase 4.2); `ctx_json` is the
pre-serialized JSON of the Lua `ctx` table. `C2S_UiEvent` (Phase 4.5) is sent
by the client's separate UI VM (`vb::script::UiRuntime`) when a widget's
`on_click`/`on_change`/`on_close` callback calls
`ui.send_event(...)`/`ui.close()`; the server routes it to
`vb.on("ui_event", handler)` (non-vetoable). `C2S_Chat` (Phase 5.4) is sent by
the client's HUD chat box; the server runs `vb.on("chat", handler)` as a veto
(default-allow when no pack/handler is attached — e.g. `--singleplayer`,
which has no `PackRuntime`) and, if not vetoed, broadcasts
`S2C_Chat{"<name>: <text>"}` (server-formatted, not the raw client `text`) to
every playing connection, sender included.

`S2C_PlayerJoin`/`S2C_PlayerLeave`/`S2C_PlayerList` (Phase 5.4) are
`ServerSession`-generated (no Lua involvement, same posture as chat's
server-side formatting): `S2C_PlayerList` is sent once to a client right when
it finishes joining, listing every other already-playing connection;
`S2C_PlayerJoin`/`S2C_PlayerLeave` are then broadcast to every other playing
connection as players come and go. The client folds join/leave into its chat
log as `"* <name> joined/left the game"` lines and keeps a live
`net_id -> name` map (`ClientSession::players()`) for a HUD player list.

## Handshake sequence

See `ARCHITECTURE_SPEC.md` §8.3 for the full diagram. Order:
`Hello → ServerInfo → Auth → AuthResult → AssetManifestRequest →
AssetManifest → AssetRequest → AssetData×N → Ready → BlockRegistry →
KeybindRegistry → MoveParams → JoinAccept → initial ChunkAdd + EntitySnapshot`
(the last three are each sent only if their respective
`HandshakeServerHost` hook opts in).

Implemented, transport-agnostic, in `inc/vb/net/handshake.hpp`:

- `ServerHandshake` — per-connection FSM: `AwaitingHello → AwaitingAuth →
  AwaitingAssetManifestRequest → AwaitingAssetRequest → StreamingAssets →
  AwaitingReady → Playing` (or `Closed`). `StreamingAssets` is unusual: it
  expects no incoming frame at all, paced instead by `pump_assets()` called
  once per tick from `ServerSession::tick()` (a small per-tick send budget,
  not literal byte-in-flight flow control) — every other state is purely
  reactive to `on_frame()`. Rejects out-of-order messages (`kBadHandshake`),
  protocol-version mismatch (`kProtocolMismatch`), a full server
  (`kServerFull`), and failed auth (`kAuthFailed`); `on_timeout()` →
  `kTimeout`.
- `ClientHandshake` — drives `Hello → Auth → AssetManifestRequest →
  AssetRequest → Ready` and exposes `ClientHandshakeStatus { Connecting,
  Authenticating, AwaitingAssetManifest, SyncingAssets, Syncing, Joined,
  Failed }` for the connect UI. Asset-sync mechanics (which hashes are
  missing, verifying + committing streamed chunks) are pushed out to a new
  `HandshakeClientHost` hook struct — `ClientHandshake` itself has no
  filesystem access; `ClientSession` wires it to a
  `vb::assetsync::ClientAssetCache` when one is supplied (optional 4th
  constructor parameter, `nullptr` by default — every pre-4.4 call site is
  unaffected and behaves as "already fully synced"). Any `S2C_Disconnect`
  fails the handshake with the server-provided message.

The `Transport` interface (`inc/vb/net/transport.hpp`) delivers whole framed
messages per lane. Backends: `LoopbackTransport` (in-process, tests +
integrated singleplayer) and `GnsTransport` (GameNetworkingSockets, real UDP,
`VB_WITH_NET`) — both implemented. `GnsTransport` maps each `Lane`'s
reliability (see `send_mode_for_lane`) onto GNS send flags. Every lane shares
GNS connection lane 0 (one reliable ordered stream, so `control`, `world` and
`assets` stay mutually ordered) except `feedback`, which is GNS lane 1 at a
higher send priority and skips Nagle (`gns_lane_index`, `lane_skips_nagle`;
lanes are configured on both ends with `ConfigureConnectionLanes`).
`LoopbackTransport` delivers everything in send order.

## Dependency pins

| Dependency            | Pinned tag | Notes                                    |
| --------------------- | ---------- | --------------------------------------- |
| raylib / raygui       | `5.5` / `4.0` | window/GL/input; version-matched      |
| EnTT                  | `v3.13.2`  | ECS                                     |
| tomlplusplus          | `v3.4.0`   | server.toml / client.toml loader        |
| doctest               | `v2.4.11`  | tests                                   |
| GameNetworkingSockets | `v1.6.0`   | Phase 1.2; needs a real protobuf install (vcpkg/apt/brew — not FetchContent-able, see `cmake/Dependencies.cmake`) + BCrypt (Windows) or OpenSSL (Linux/macOS) |
| zpl / librg           | `v18.1.4` / `v7.2.2` | Phase 1.4 spike — confirm API |
| FastNoise2            | `v0.10.0`  | Phase 2                                 |
| lz4 / xxHash          | `v1.9.4` / `v0.8.2` | Phase 4.4 manifest hashing (`XXH3_128bits`) is the first real consumer; both fetched under `VB_WITH_COMPRESSION`. xxHash is linked **before** lz4 in `src/core/CMakeLists.txt` on purpose — lz4 vendors its own private, older `xxhash.h` with no XXH3 API, and `#include <xxhash.h>` resolves against whichever `-I` entry comes first |
| Lua / sol2            | `v5.4.6` / `v3.3.0` | Phase 4                          |
