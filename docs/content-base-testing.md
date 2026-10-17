# Testing `content/base` — design and phased plan

> Status: **C0–C5 all done** (this file). Phases C0 (fixture), C1
> (declarative surface), C2 (server-side behaviour), C3 (UI pack files) and
> C4 (e2e journeys) are done — see `tests/unit/content_base_fixture.hpp`,
> `content_base_data_test.cpp`, `content_base_behaviour_test.cpp`,
> `content_base_ui_test.cpp` and `tests/e2e/test_base_pack.py`. C5's CI/docs
> checks are done (this status line, CONTRIBUTING.md), and its optional
> cleanup (task 4) is also done: `content_pack_test.cpp`'s three cases that
> load the real `content/base` directory now use `BasePackFixture` instead
> of their own inline setup (the two that load a synthetic throwaway pack —
> `require()`, a broken-Lua directory — stayed as-is, since `BasePackFixture`
> is specifically about the real `content/base/` directory). The findings
> below are what's left.
>
> **Findings from implementing this:**
> - No read-back accessor exists for `vb.register_biome` entries (C1 task 4
>   anticipated this) — `content_base_data_test.cpp`'s biome test works
>   around it by spying on `vb.register_biome` via a prelude loaded before
>   `load_content_pack`, which only works because `blocks/*.lua` share one
>   Lua global namespace with `biomes/*.lua`.
> - ~~`PlayerHandle` has no `get_health()`~~ — resolved: `player:get_health()`
>   now exists (returns `{current, max}`), and `ServerSession::player_health()`
>   is available in every build, so the fall-damage HP-curve assertions in
>   `content_base_behaviour_test.cpp` no longer need a `VB_WITH_AUTOMATION` build.
> - `tests/e2e/vbtest/stack.py` never pinned `asset_cache_dir` in the
>   generated `client.toml`, so on macOS (where `user_cache_dir()` hardcodes
>   `~/Library/Caches`, ignoring `XDG_CACHE_HOME`) the harness's own
>   `client.cache_dir` attribute pointed at a directory the client never
>   actually wrote to — `test_multiplayer_basics.py`'s existing cache
>   assertion was silently checking the wrong path on that platform. Fixed
>   as part of adding `test_base_pack.py`'s asset-sync test.
> - The automation protocol's `entity_visible`/`entity_near` predicates only
>   match by player name (`src/client/automation_endpoint.cpp`'s `names`
>   map) — a script-spawned entity like `base:zombie` has no player name,
>   so C4 task 4 ("a second player sees the zombie") isn't expressible with
>   the current harness. Would need a by-kind or by-net_id entity query.
>
> Below this line is the original design and phased task list, kept as the
> record of what was planned and why — not updated to past tense.

## 1. Why

`content/base` is shipped content, and it is where most of the engine's
"mechanism, not policy" decisions actually live: punching, placing, fall
damage, crafting, the pause/inventory screens, block drops. The engine is
well covered; the pack that gives it behaviour is barely covered at all.

What exists today:

| Test | What it actually asserts about `content/base` |
|---|---|
| `tests/unit/content_pack_test.cpp` | the pack loads; `registry.size() == base + 2`; eight block names resolve; `/craft` happy path + three failure messages; `/zombie` spawns, chases, kills |
| `tests/e2e/test_multiplayer_basics.py` | incidentally: break/place replicate, placing spends an item, `/craft base:planks`, the inventory screen opens and closes |
| `tests/e2e/test_recorder.py` | incidentally: the inventory screen again, as recorder output |

Everything below is currently unasserted anywhere:

- **Block data.** `solid`/`opaque`/`liquid`/`light`/`region`/`texture` per
  block. A typo turning `base:leaves` opaque, or dropping `base:water`'s
  `region = true`, ships silently.
- **`on_break` drops.** Eight blocks spawn an item drop of themselves;
  `base:grass` deliberately drops `base:dirt`; `base:water` deliberately has
  no `on_break` at all. None of that is checked.
- **Biomes.** `base:plains`/`base:forest` and their surface/filler/stone/
  decoration fields are write-only today — but the registration call itself
  can still regress.
- **Entities.** `base:player` and `base:dropped_item` exist only to supply a
  `visual` and a `represents` tag; nothing asserts the tag reaches
  `ServerSession::set_player_visual_kind()`/`set_item_drop_visual_kind()`.
  `base:zombie`'s `health = 20`, attack damage, 1 s cooldown and `on_death`
  are only covered by one broad "it kills the caller" case.
- **`mechanics.lua`.** The rising-edge contract is the whole point of the
  file: holding LMB must punch *once*, not once per tick. Placing must read
  `get_held_item()`, must consume exactly one unit, must do nothing with an
  empty slot, and must use the engine's *effective* `eye_height`/`reach`.
  None of it is tested directly.
- **`keybinds.lua`.** `base:pause` has no test at all (only `base:inventory`,
  via e2e). Neither keybind is tested for the hold-doesn't-reopen edge.
- **`fall_damage.lua`.** The `SAFE_SPEED = 8.0` threshold, the linear
  1 HP/(m/s) curve above it, and the `"fall"` cause string are untested.
- **`init.lua`.** `vb.storage.boot_count` persisting across a restart is the
  pack's one demonstration that `vb.storage` works; nothing checks it.
- **`ui/*.lua`.** **No test loads these files at all.** A syntax error in
  `ui/hud.lua` would pass the entire suite and ship. The hotbar, chat log,
  player list, break-progress bar, inventory slot grid, empty-inventory
  label and both close buttons are unverified.
- **Textures.** `textures/{stone,water,dirt,grass,sand,wood,planks,leaves,sticks,player,dropped_item}.png` are
  referenced by name from Lua; nothing checks the referenced files exist, or
  that they reach a client through asset sync.

## 2. Design

### 2.1 Three layers, chosen by what each check actually needs

**Layer 1 — in-process pack tests (doctest, `VB_WITH_LUA`).** A real
`PackRuntime` + `ServerSession` over `LoopbackNetwork`, loading the real
`content/base` directory through `vb::script::load_content_pack`, exactly as
`content_pack_test.cpp` already does. This is where the bulk of the coverage
goes: it is deterministic, millisecond-fast, needs no window, no sockets and
no Python, and it can drive `player_input`/`player_landed`/`chat` hooks
directly with exact values — which is the only practical way to assert a
`SAFE_SPEED` threshold or a rising-edge contract.

**Layer 2 — in-process UI-pack tests (doctest, `VB_WITH_LUA`).** The same
idea for the client-side VM: load the real `content/base/ui/*.lua` into a
`UiRuntime` and assert the widget trees it renders. `ui_runtime_test.cpp`
already exercises `UiRuntime` with inline Lua; this adds the "load the files
we actually ship" half, mirroring what `content_pack_test.cpp` is to
`pack_runtime_test.cpp`.

**Layer 3 — e2e pack journeys (pytest, `ctest -L e2e`).** Real
`voxel_browser` + `voxel_browser_server` processes via the existing `vbtest`
harness. Reserved for what genuinely needs two processes and a cold cache:
textures arriving over asset sync, a dropped item being walked into and
picked up, a keybind reaching the server and pushing a screen back, the HUD
drawing in a real window.

### 2.2 The routing rule

Put a check in the **lowest layer that can hold it.** A behaviour expressible
as "this hook, these arguments, this result" belongs in Layer 1, where it is
fast and exact. Promote to Layer 3 only when the thing under test *is* the
round trip — asset sync, input→server→UI push, rendering. Duplicating a
Layer 1 case in Layer 3 buys nothing and costs seconds per run.

Practical consequence: Layer 3 gains roughly six tests, not thirty.

### 2.3 A shared fixture, written once

Every Layer 1 case needs the same ~40 lines: `LoopbackNetwork`, a `base()`
registry, a `PackRuntime` with a temp storage path, `load_content_pack`,
`freeze`, a `ServerSession`, a connected `ClientSession`, a `pump(n)` lambda,
and inventory/chat readers. `content_pack_test.cpp` spells that out inline in
each of its cases today.

Extract it to `tests/unit/content_base_fixture.hpp` — a header-only
`BasePackFixture` with `pump(n)`, `chat(text)`, `last_message()`,
`count_of(name)`, `input(...)`, `land(speed)` and access to the registry,
runtime, session and client. Build this *first*: it is what makes the rest
of the phases cheap, and it keeps new cases short enough to read.

`content_pack_test.cpp`'s existing cases are deliberately **not** rewritten
onto it in the same change — behaviour-preserving churn in a file whose job
is catching content regressions is the wrong risk to take while adding
coverage. Phase C5 revisits that as an optional cleanup.

### 2.4 Test-only Lua, and its limits

`content_pack_test.cpp`'s crafting case already registers an extra
`vb.on("chat", ...)` handler after the pack to give itself wood. That seam
stays available (and the fixture exposes it as `fixture.lua(code)`), but it
is for *acquisition shortcuts only* — never for reimplementing the behaviour
under test. A test that needs a drop to exist should break a real block; a
test that needs five stone may inject them.

### 2.5 What this plan deliberately does not do

- **No Lua-side test framework inside `content/base`.** Shipping a test
  runner in the pack means shipping test code to players. Assertions live in
  `tests/`.
- **No golden-image screenshot diffing.** The existing `vbtest.png.stats`
  (size, distinct colours, mean colour) is the agreed ceiling; pixel goldens
  are brittle across GL stacks and platforms.
- **No new engine features.** If a check turns out to need a primitive that
  doesn't exist, it gets written down in Phase C5's findings list and skipped,
  not built here.

## 3. Phases

Each phase is independently landable and leaves the suite green.

---

### Phase C0 — Fixture and gap baseline

1. Add `tests/unit/content_base_fixture.hpp`: header-only `BasePackFixture`
   as described in §2.3, guarded by `#if VB_WITH_LUA`.
2. Give it the helpers the later phases need:
   `pump(n)`, `lua(code)`, `chat(text)`, `drain_messages()`, `last_message()`,
   `count_of(block_name)`, `give(name, n)`, `send_input(InputCmd)`,
   `press(button)`/`release(button)` (rising-edge helpers over `send_input`),
   `land(impact_speed)`, `player_health()`, plus `registry()`, `runtime()`,
   `server()`, `client()`.
3. Add `tests/unit/content_base_data_test.cpp` with one smoke case using the
   fixture, and register it in `tests/CMakeLists.txt`'s `add_executable` list.

**Done when:** `./build-lua/vb_tests --test-case='*content/base*'` passes and
the new file is in the build. No behaviour coverage yet — this phase is the
scaffolding.

---

### Phase C1 — Declarative surface: blocks, biomes, entity registrations

All in `tests/unit/content_base_data_test.cpp`, all reading the frozen
registry after a real pack load.

1. **Block properties table.** One data-driven case over all nine blocks
   asserting `solid`/`opaque`/`liquid`/`light` against an expected table
   written out in the test — including the three deliberate oddities:
   `base:leaves` is solid but *not* opaque, `base:sticks` is neither, and
   `base:water` is liquid, non-solid, non-opaque.
2. **`base:water` keeps `region = true`.** It is the only region-tracked
   block and the first user of the per-tick occupancy hook; a lost flag
   silently disables liquid regions.
3. **Textures resolve.** For every block carrying a `texture`, assert the
   path is pack-relative and the file exists on disk under
   `content/base/`. Four files today (`stone`, `water`, plus the two entity
   textures in task 5).
4. **Biomes register.** `base:plains` and `base:forest` are present in
   whatever the runtime captured, with the right `surface`/`filler`/`stone`,
   and `base:forest` carries `decoration = "trees"`. If `PackRuntime` exposes
   no read-back for biomes today, assert via `run`-level Lua instead and note
   the missing accessor for C5.
5. **Entity registrations.** `base:player` (0.8 × 1.8, `represents =
   "player"`, `mirror = false`, two clips), `base:dropped_item` (0.4 × 0.4,
   `represents = "item_drop"`, one clip), `base:zombie` (0.6 × 1.8,
   `health = 20`).
6. **`represents` reaches the session.** After `attach_session`, assert
   `ServerSession`'s player-visual and item-drop-visual kind ids are set to
   the two registered kinds — the thing those two files exist for.

**Done when:** a one-character edit to any block's `opaque`, to water's
`region`, or to either `represents` string, fails a test.

---

### Phase C2 — Server-side behaviour: drops, crafting, mechanics, falling, keybinds, storage

New file `tests/unit/content_base_behaviour_test.cpp`.

1. **Every block drops itself.** For each of the eight blocks with an
   `on_break`: place it, break it through the real block-edit path, assert an
   item drop entity of that block id exists at the block centre.
2. **`base:grass` drops `base:dirt`.** The one deliberate exception, asserted
   explicitly rather than folded into task 1's loop.
3. **`base:water` has no `on_break`.** Breaking it spawns no drop.
4. **Crafting failure paths that the existing case misses:** the `/craft `
   prefix is vetoed (never broadcast as chat) *even when the recipe fails*;
   a bare `/craft` with no argument is **not** vetoed and broadcasts
   normally (the pattern requires an argument); inventory is unchanged after
   every failure.
5. **Crafting is all-or-nothing.** With exactly one plank (sticks needs two),
   no take happens and the plank survives.
6. **Punch fires on the rising edge only.** Send `primary = true` for ten
   consecutive input frames at a block with `max_damage` high enough to
   survive one hit; assert exactly one punch landed. Then release, press
   again, assert a second.
7. **Place fires on the rising edge only**, same shape, and places at
   `hit + normal`.
8. **Placing spends exactly one unit** of the held stack, and **an empty or
   out-of-range selected slot places nothing** (no block change, no take).
9. **Placing respects the effective reach.** Override
   `vb.action.set_params{reach = ...}` through the test seam and assert the
   placement that was in range before is now refused — the regression
   `mechanics.lua`'s comment explicitly worries about.
10. **Fall damage curve.** `land(7.9)` → no damage; `land(8.0)` → no damage
    (`excess > 0` is strict); `land(12.0)` → exactly 4 HP; and the damage
    cause reaching the engine is `"fall"`.
11. **Both keybinds open their screen on the rising edge**, and holding the
    key does not reopen it. `base:pause` opens with an empty payload;
    `base:inventory`'s payload carries the player's current slots.
12. **`vb.storage.boot_count` persists.** Load the pack twice against the
    *same* storage file, assert the counter reads 1 then 2, and that it
    round-trips as an integer (`init.lua` formats it explicitly because JSON
    hands back floats).

**Done when:** each of `mechanics.lua`, `fall_damage.lua`, `keybinds.lua`,
`crafting.lua` and the `on_break` handlers has at least one test that fails
when its core constant or its edge-detection is changed.

---

### Phase C3 — The UI pack files

New file `tests/unit/content_base_ui_test.cpp`, Layer 2. Closes the "no test
loads these files at all" gap.

1. **A loader for `content/base/ui/`.** The client loads these into
   `UiRuntime`, not `PackRuntime`; mirror whatever `src/client/` does to walk
   that directory. If the walk lives inline in client code, lift the minimum
   needed into the test rather than refactoring the client in this phase.
2. **All three files parse and register.** `base:pause`, `base:inventory` and
   the HUD definition exist after loading. This single case is the one that
   stops a syntax error from shipping.
3. **`base:pause` renders** a `title` label reading "Paused" and a `resume`
   button; clicking `resume` closes the screen.
4. **`base:inventory` renders from state:** given three slots, there are
   three `slot_bg_*` rects, three `slot_icon_*` icons and three `slot_count_*`
   texts, and no `empty` label; given zero slots, the `empty` label is present
   and no slot widgets are. The `close` button closes the screen.
5. **The HUD renders** its hotbar (nine `hotbar_bg_*`/`hotbar_label_*`
   pairs), the chat log, the player list (self row plus one row per other
   player) and the break-progress bar, each from a state table the test
   supplies. Assert the break-progress *fill* width tracks the fraction.

**Done when:** `rm content/base/ui/hud.lua` (or breaking its syntax) fails a
test.

---

### Phase C4 — End-to-end content journeys

New file `tests/e2e/test_base_pack.py`, using the existing `server`/`clients`
fixtures. Six tests, no more — everything cheaper lives in C1–C3.

1. **Textures arrive over asset sync.** A cold-cache client ends up with the
   pack's four PNGs in its cache directory (extend
   `test_clients_join_and_see_each_other`'s existing cache assertion into a
   real per-file check).
2. **Break → drop → pick up.** Alice breaks a `base:stone` she is standing
   next to, walks onto the drop, and `expect(alice).to_have_inventory(
   "base:stone", 1)`. The full `on_break` → `ItemDropSystem` → pickup chain
   that Layer 1 can only see the first step of.
3. **The pause screen opens from the real key.** `key_press("pause")` →
   `to_have_ui_open("base:pause")` → click `resume` → closed. The
   `base:inventory` twin already exists in `test_multiplayer_basics.py`;
   this is `base:pause`'s missing half, and it is the only check that the
   `kCustomKeybinds` Escape binding is actually wired.
4. **A second player sees the zombie.** Alice runs `/zombie`; Bob
   `to_see_entity` it. Covers the `represents`/visual replication path that
   the in-process test cannot reach.
5. **Held-item placing through a real client.** Give Alice `base:planks`,
   `select_slot(1)`, place, and assert the placed block is `base:planks` —
   not stone. The pre-6.20 hardcoded-stone regression, caught end to end.
6. **A windowed client draws the HUD.** `clients(1, windowed=True)`,
   screenshot, and `vbtest.png.stats` shows a non-blank frame with more than
   a handful of distinct colours. Skips without a display, like the other
   windowed tests.

**Done when:** `ctest --test-dir build-e2e -L e2e` passes with the new file,
and each new test fails if its content file is broken.

---

### Phase C5 — CI, docs, and findings

1. **Confirm the Layer 1/2 files run in CI's Lua leg** — they are plain
   `vb_tests` sources, so this is a check, not a change; make sure the leg
   that has `VB_WITH_LUA=ON` is the one running them.
2. **Document the layering** in `tests/e2e/README.md` ("content checks go in
   `content_base_*_test.cpp` unless they need two processes") and add a
   `content/base` line to `CONTRIBUTING.md` next to the existing
   `content_pack_test.cpp` bullet.
3. **Write up the findings.** Any real bug the new tests expose, and any
   check that had to be skipped for a missing primitive (likely candidates:
   a biome read-back accessor, a way to observe `max_damage` accumulation
   from Lua), goes to `remaining_tasks/` as its own item — not fixed inline
   in a coverage change.
4. **Optional cleanup:** migrate `content_pack_test.cpp`'s four existing
   cases onto `BasePackFixture`. Only worth doing once C1–C4 have proven the
   fixture's shape; skip it if the fixture needed per-phase special-casing.

**Done when:** a contributor editing `content/base` can tell from the docs
where their test goes, and every deferred check has a backlog entry.

## 4. Cost and sequencing

C0 is a prerequisite for C1 and C2; C3 and C4 are independent of both and of
each other. C1 → C2 is the highest value per hour (most of the untested
surface, fastest tests). C3 is the highest value per *test* — it is the only
phase covering files that no test opens today, so a single case there removes
a whole class of ship-breaking failure. C4 is the slowest to run and the most
likely to be flaky; keeping it at six tests is deliberate.

Suggested order: **C0 → C3 → C1 → C2 → C4 → C5.** C3 moves early because the
"a syntax error in `ui/hud.lua` ships" hole is the worst one open, and
closing it is a single test.
