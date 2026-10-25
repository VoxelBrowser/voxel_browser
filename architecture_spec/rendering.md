# Rendering (Client) — Full Reference

> Full detail for this topic; linked from `ARCHITECTURE_SPEC.md`. Ground truth — do not duplicate here.

## 11. Rendering (Client)

### 11.1 Stack

`raylib` owns the window, GL context, input, and 2D/UI draw. A hand-rolled
face-culled mesher (`vb::world::chunk_mesher` / `chunk_mesh_snapshot`) owns
voxel meshing and chunk mesh management — see §18 Q2 for why this is the
permanent choice rather than a placeholder for a greedy-mesh library.
`raygui` draws menus/HUD as an immediate-mode overlay.

### 11.2 Chunk meshing

- Client keeps a `ClientChunkStore` mirroring replicated chunk data.
- On `ChunkAdd`/`ChunkDelta`, mark mesh dirty; a **mesh worker thread pool**
  builds per-face-culled vertex buffers from `(block registry, block data,
  light volume, neighbor faces)`. Per-vertex light + AO baked in.
- Completed meshes are uploaded to GPU on the main thread (GL calls are
  single-threaded); a budget caps uploads per frame to avoid hitches.
- Frustum culling + distance culling per chunk. Optional simple occlusion via
  chunk visibility bitset from generation.
- Transparent blocks (water, leaves-as-cutout) drawn in a second pass, back to
  front.

### 11.3 Entity rendering — billboard sprites

**Decided (2026-09-11, §18 Q7): players and Lua entity kinds are 2D sprites, not
3D blocky models** — a *Don't Starve*-style presentation: a single flat billboard
per entity, with a handful of directional poses and per-state animation clips
standing in for full 3D animation, inside an otherwise fully 3D, free-look
first-person world (unlike Don't Starve's fixed isometric camera, so the
direction-selection math below has to do real work instead of being baked in at
authoring time).

**Billboarding.** Each entity draws as one `DrawBillboardPro(camera, atlas,
frame_rect, feet_position, up = {0,1,0}, size, origin, rotation = 0, tint)` call.
Passing a fixed world-up locks the quad vertical (it never leans with camera
pitch/roll); raylib still derives the quad's *right* edge from the camera's view
matrix, but that vector is always horizontal regardless of pitch (the cross
product of any forward vector with world-up has zero Y component) — so the quad
yaws to face the camera's bearing and nothing else. This is confirmed directly
from raylib 5.5's `rmodels.c`, not assumed; no custom quad math is needed. The
quad is anchored at the entity's feet (matching `Position`/`Collider`'s own
convention) and horizontally centered.

**Directional pose selection.** An entity kind declares `facings` (4 or 8,
default 8). Each frame, the client computes the bearing from the entity to the
camera in the world XZ plane, subtracts the entity's own facing yaw
(`Rotation.yaw`), normalizes to [0°, 360°), and buckets into `facings` equal
sectors to pick the pose — the classic "which side of you is the viewer standing
on" trick from isometric/2.5D games. A small hysteresis (the bucket must change
*and* persist a couple of frames, or a small angular deadzone at sector
boundaries) stops flicker when the viewer sits exactly between two sectors.
Packs only need to author the unique half of the poses — front, front-diagonal,
side, back-diagonal, back (5 sprites) for 8-way, or front/side/back (3) for
4-way — the engine mirrors the rest via `DrawBillboardPro`'s negative-`size.x`
horizontal flip, halving the art budget.

**Animation state.** Driven entirely by data already on the wire (§8.4's
`EntityRecord`), no extra round trip: horizontal speed from `vel` buckets
walk/run, and `flags` (currently only bit 0, `on_ground`) grows three more bits
— `dead`, `hurt_pulse` (edge-triggered: the client plays it once then the sender
clears it), `acting` — giving a fixed client-side priority resolution `dead >
hurt_pulse > acting > jump/fall > run > walk > idle`. Each clip is an ordered
list of atlas rects with an fps and a loop-or-hold-last-frame flag, defined per
entity kind, not hardcoded per clip name beyond that small base set. Wiring
the new `flags` bits bumps `kEngineProtocolVersion` and `docs/protocol.md` when
it happens (Phase 3.5) — this section records the *plan*, not a shipped wire
change.

**Where it's defined.** `vb.register_entity{ visual = {...} }` (§10.3, Phase
4.2) is the single source of truth per entity *kind*; the atlas PNG travels
through Asset Sync (§9, Phase 4.4) like any other texture, with no separate
synced manifest. Phase 3 ships first against a **hardcoded single-frame
placeholder** (`facings = 1`, a flat tint) so remote players are visible
before Lua/asset-sync exist, the same way Phase 2 shipped a hand-rolled mesher
ahead of Cellulose — real art (5.1) is a local swap once 4.2/4.4 land.

**Decided (2026-09-17): frame-size variants, spritesheet grid schema, and
per-instance override.** The atlas is a spritesheet, not one loose sprite per
pose, so the engine needs a declared frame size to slice it into a grid —
rather than accept arbitrary pixel dimensions, packs pick from a small closed
set of named variants (multiples of 128px, matching typical small/medium/large
entity silhouettes at a fixed pixels-per-metre ratio):

| variant | frame size (px) |
|---|---|
| `small` | 128×128 |
| `tall` | 128×256 |
| `flat` | 256×128 |
| `medium` | 256×256 |
| `medium_tall` | 256×512 |
| `medium_flat` | 512×256 |
| `large` | 512×512 |
| `large_tall` | 512×1024 |
| `large_flat` | 1024×512 |

These are an authoring/validation convenience, not an engine type — the
variant name just looks up a `{frame_width, frame_height}` pair used below.

**Kind-level default**, set via `vb.register_entity{ visual = {...} }`:

```lua
visual = {
  variant = "tall",              -- looked up -> frame_width/frame_height
  texture = "textures/entities/player.png",
  facings = 8,                   -- rows = floor(facings/2)+1 unique poses
                                  -- (§11.3 pose mirroring), engine mirrors rest
  mirror = true,                 -- default; false = author a full `facings`
                                  -- rows instead (a real left pose, not a
                                  -- flipped right one) and nothing is mirrored
  origin = { x = 0.5, y = 1.0 }, -- normalized anchor within a frame (feet point)
  clips = {                      -- column layout, shared across every pose row
    { clip = "idle",   frames = 4, fps = 6  },
    { clip = "walk",   frames = 6, fps = 10 },
    { clip = "run",    frames = 6, fps = 14 },
    { clip = "jump",   frames = 1, fps = 1  },
    { clip = "fall",   frames = 1, fps = 1  },
    { clip = "acting", frames = 4, fps = 8  },
    { clip = "hurt",   frames = 2, fps = 10 },
    { clip = "dead",   frames = 1, fps = 1  },
  },
}
```

Row = pose index (the same index `select_pose()` already produces). Column =
a pose-row-local pixel offset, computed by the engine as a running sum over
`clips` in declared order — so `clip` names and per-clip frame counts/fps are
entirely pack-defined (no positional row/column convention to memorize), while
built-in kinds (`base:player`, `base:dropped_item`) simply ship their own
`visual` table as sensible defaults in `content/base`, the same mechanism a
third-party pack would use, not a special-cased engine path. The required
sheet size is derived and validated exactly at load time: `width ==
frame_width * sum(frames)`, `height == frame_height * rows` where `rows =
mirror and (floor(facings/2)+1) or facings` — a mismatch is a pack load
error, not a silent misdraw. A kind that omits a
clip `resolve_anim_clip()` can still resolve to (e.g. `base:dropped_item`
never running or jumping) falls back to the first declared clip (conventionally
`idle`) rather than erroring at runtime.

**Per-instance override.** Each spawned instance may carry its own partial
`visual` table in its `ScriptState` under a reserved key (`entity.
visual_override = {...}`), merged field-by-field over its kind's default —
this is the *only* per-instance case (e.g. a player skin overriding just
`texture` while inheriting `variant`/`clips`/`facings`/`mirror`/`origin`
unchanged);
every other entity kind is expected to look the same across all its
instances and only needs the kind-level default. No new storage mechanism:
`ScriptState` already exists as arbitrary per-entity Lua data (§4's entity
component table above).

**Client-side.** `vb/render/entity_renderer` (sibling to `vb/render/chunk_renderer`)
keeps one `EntityRenderState` (clip, elapsed time, direction bucket + hysteresis
counter) per replicated `NetId`, updated from `ClientSession::remote_entities()`
+ `interpolated_pos()` (already built for §8.4). The local player is not drawn
as a billboard in first person (no viewmodel in scope).

**Explicitly out of scope for v1:** skeletal/vertex animation, per-limb
equipment layering, blob shadows, and dynamic per-entity lighting sampled from
the block underfoot (chunks already compute the light value that would need —
cheap follow-up, not core).

### 11.4 Frame loop

```
poll input ─▶ sample InputCmd, push to history, send on lane 4
           ─▶ predict local player (integrate + voxel collision, shared code)
recv network ─▶ apply snapshots, reconcile local player, apply chunk deltas
interpolate remote entities at (server_time_est - interp_delay)
update dirty meshes (bounded)
render: sky ─▶ opaque chunks ─▶ billboard entities (§11.3) ─▶ transparent chunks
        ─▶ particles ─▶ raygui HUD ─▶ open Lua-defined UI ─▶ debug overlay
present
```

Target 60 FPS decoupled from the 20 Hz server tick.

### 11.5 Camera & input

First-person camera driven by predicted local player transform. Mouse-look with
capture toggle. Keybindings configurable via a local (non-synced) settings file.

Input is read through an `InputSource` seam (`inc/vb/render/input.hpp`): the
client polls one `InputFrame` per frame (`RaylibInput` in production) and
`sample_input_cmd`, chat/hotbar/mouse-capture logic and the keybindings screen
read that snapshot rather than calling raylib. `SyntheticInput` is the
window-less, script-fed implementation used by unit tests and, later, the
dev-only e2e automation (`docs/e2e-automation.md`). The seam is ordinary
engine code and ships in production; only the automation driver is gated.


### 11.6 Engine menu screens (planned 2026-10-25, Phase 12)

The non-gameplay screens (`vb::render::MainMenu`, `src/render/main_menu.cpp`)
are engine-drawn, never pack content. Phase 12 redesigns them as a
search-engine-style home page; the approved mockup is the design canvas
linked from `remaining_tasks/phase12.md`.

**Backdrop.** Every non-gameplay screen (main, settings, keybindings,
connecting, sign-in, error, loading) draws the same backdrop first: an
engine-owned 16×16 dirt texture, tiled at 4× with point filtering, under a
flat black scrim at 55% opacity. The texture is a copy of
`content/base/textures/dirt.png` baked into the client binary
(`vb_embed_files`, `cmake/EmbedFiles.cmake`), so the menu never depends on a
pack being present. With the scrim, the brightest dirt pixel (156,116,86)
lands near (70,52,39), so white text is ~11:1. Panels drawn over the backdrop
are ~85% opaque dark brown (`#140E0A`) with a 1 px 14%-white border, so
contrast never depends on which texels sit behind a widget.

**Type.** Two OFL fonts embedded the same way: Pixelify Sans (title only,
"Voxel Browser") and Inter (all other text), loaded once with `LoadFontFromMemory`
and set as raygui's font. Licence texts ship next to the binary.

**Main screen** (top to bottom, centred):
- "Voxel Browser" title (92 px at 1280×720, scaled down with the window).
- One address field (`host`, `host:port`, `[ipv6]:port`; port defaults to
  27015) on an opaque light pill (`#F4F1EC`, dark text ~15:1) with a
  **Continue** button beside it (`#7BC35A`, dark text ~8:1). Enter in the
  field = Continue. Replaces the separate address/port boxes.
- "Recent servers" panel: the 3 most recent entries of `recent_servers`, plus
  an "All history in Settings" link. Clicking an entry (or the link) opens
  Settings › History with that entry highlighted and scrolled into view; it
  does not connect or fill the field. Hidden when the list is empty.
- Bottom-left: **Settings**. Top-right: the Phase 9.6 "Signed in to …" chip
  with Sign out (bottom-right at the minimum window size, where it would hit
  the title).
- No Singleplayer button (Phase 11.2: loopback is a CLI-only diagnostic) and
  no Quit button (closing the window exits).

**Settings** is one panel with a section list on the left and Back / Save at
the bottom:
- *General*: **Default player name** (`client.toml` `player_name`; moved off
  the main screen), asset cache size.
- *Video*: window size, V-Sync, FOV, render distance.
- *Controls*: mouse sensitivity, "Keybindings…" (the existing screen).
- *History*: every `recent_servers` entry, newest first, each with Connect
  and Remove, plus Clear history. Connect from here is the only one-click
  way to join a past server.

**Connecting** keeps its status text, optional asset-sync progress bar and
Cancel, now in a backdrop panel under a smaller title. Error, sign-in and
loading screens get the backdrop and panel styling without layout changes.

All layouts must fit the enforced 720×360 minimum window
(`kMinWindowWidth`/`kMinWindowHeight`) without clipping.
