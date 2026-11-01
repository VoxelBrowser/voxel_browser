# Gotchas (detail) — long-form toolchain/compiler/runtime traps

> Full write-ups for gotchas too long to keep inline in `STATE.md` §4. Short bullets there
> point here. Keep entries newest-relevant first; don't delete an entry just because it's old —
> these are landmines that still apply regardless of when they were found.

---

## e2e verification: concrete misses (2026-10-05)

> Moved verbatim from `STATE.md`'s "Standing priority: prefer e2e verification" section
> (dreamed 2026-10-31). The rule itself stays in `STATE.md`.

- A HUD scaling fix *looked* right by inspection but silently drew nothing in
  a real client — `build-e2e` was a stale binary built before a
  `client.health()` Lua binding it depended on had landed, so every HUD frame
  threw a Lua error. Only a real screenshot surfaced the empty HUD; nothing
  about the Lua or C++ diffs themselves would have shown it.
- A status-bar width fix (anchoring to the full hotbar row instead of however
  many slots are filled) introduced a new overflow at the engine's own
  enforced minimum window width — caught by screenshotting a fully-stocked
  9-slot hotbar at that floor, not by re-reading the arithmetic.
- `tests/unit/content_base_behaviour_test.cpp`'s fall-damage test drives
  `player_landed` by setting velocity directly (`BasePackFixture::land()`),
  which exercises the *policy curve* but not the actual jump-launch-speed /
  landing-speed round trip through real physics — the real bug (jumping on
  flat ground took damage because `SAFE_SPEED` sat below the engine's own
  `jump_speed`) needed a real `key.press("jump")` through a real client to
  reproduce and confirm fixed.
- That same e2e jump test raced its own assertion: checking health
  immediately after `on_ground` flips back to `true` missed the damage,
  which lands a tick or two later — confirmed by hand that an immediate
  check silently passed against the *known-buggy* code. Absence-of-an-event
  assertions need a real settle window; presence-of-an-event ones should use
  `wait_for`/`expect` as usual (no fixed sleeps there).
- Raw input automation (`key.press`/`key.hold`) does **not** auto-capture the
  mouse the way `walk_to`/`break_block`/`select_slot` do (by design — see
  `docs/automation-protocol.md`'s `mouse.capture` row) — a test driving
  movement/jump via raw `key.press` silently no-ops (zero position change,
  `on_ground` never changes) until `client.capture_mouse()` is called first,
  same as a real player who hasn't clicked the window.

None of these would have been caught by re-reading the diff more carefully or
by the existing unit-test suite; they needed a real client process, a real
screenshot, or a real multi-tick wait. Treat "I reasoned through the code and
it looks right" as a hypothesis, not a result, for anything in this category.

---

- **`std::erase`/`std::remove` on a `std::vector<ChunkCoord>` (or any small
  trivially-copyable struct of that size/alignment shape) fails to compile**
  with this repo's toolchain (clang targeting the MSVC STL): `<xutility>`'s
  `_Find_vectorized`/`_Remove_vectorized` hit `static_assert(false, "unexpected
  size")`. Seen 2026-09-11 in world_replicator.cpp. Workaround: a manual
  `for (it...) if (*it == x) { v.erase(it); break; }` loop instead of
  `std::erase(v, x)`. Haven't checked whether this is specific to 12-byte
  structs, this exact clang/MSVC-STL version pairing, or something else —
  just avoid `std::erase`/`std::remove` on small POD-struct vectors here.
- **A local `clang-format --dry-run --Werror` binary may not be trustworthy
  as-is against this repo's `.clang-format`** — see `STATE.md.local` for a
  machine where this was confirmed (a Homebrew clang-format flagging dozens
  of violations even on an untouched `HEAD` file) and the general
  workaround (diff against clang-format run on the unmodified file, don't
  trust a bare pass/fail on the edited one). Re-verify on whichever machine
  you're on before trusting either a pass or a wall of violations.
- **`doctest`'s `--test-case=` filter is a glob pattern, not a substring
  match** — `--test-case="GnsTransport: connect, exchange a message..."`
  (the literal, full test name) matches *zero* cases silently (`0 passed | N
  skipped`, no error) unless it's the exact full string with no drift at all;
  wrap it in `*...*` (e.g. `--test-case='*UDP*'`) to match by substring. Also
  quote it — zsh glob-expands an unquoted `*UDP*` itself before doctest ever
  sees it, and errors with "no matches found" if nothing in the CWD happens
  to match.
- **MSVC (`cl.exe`) rejects a ternary between two different instantiations
  of a templated smart-pointer type with converting constructors** — e.g.
  `FastNoise::SmartNode<Value>` vs. `FastNoise::SmartNode<Constant>` in
  `node.a ? compile_node(*node.a) : FastNoise::New<FastNoise::Constant>();`
  fails with `C2445: result type of conditional expression is ambiguous:
  ... can be converted to multiple common types` (seen 2026-09-18,
  `src/worldgen/fastnoise2_compile.cpp`, Phase 6.14) — MSVC can't pick
  between converting each side to the other's type. Clang/GCC may well
  accept the same ternary (not cross-checked here, MSVC is this project's
  only currently-verified toolchain per `STATE.md.local`); don't assume a
  ternary between two related-but-distinct template instantiations compiles
  portably. Workaround: plain `if`/`else` (or a small helper function that
  does the branching and returns one common type) instead of the ternary.
- **A sibling file's comment claiming something "isn't implemented yet" /
  "nothing calls this" can go stale the moment a later phase actually lands
  it, without that comment ever being updated** — this repo's convention is
  heavy in-line prose explaining *why*, which is valuable but rots exactly
  like this. Concretely hit 2026-09-18: `content/base/entities/
  dropped_item.lua`'s comment said `vb.world.spawn()` "just logs and
  returns nil" — true when written, false since Phase 6.1 (2026-09-17)
  actually wired real entity dispatch, but the comment was never touched
  when 6.1 landed. A newly-written file (`content/examples/kitchen_sink/
  entities/sentry.lua`, Phase 6.15) copied that same stale framing without
  checking, and had to be corrected afterward (see that phase's `STATE.md`
  entry and `REMAINING_TASKS.md` 6.15 for the fix). **Before reusing a
  sibling file's "this doesn't work yet" framing in new code, grep the
  actual current binding implementation** (e.g. `src/script/
  pack_runtime.cpp`'s `world_tbl["spawn"]`/`entity_methods[...]`) rather
  than trusting the comment — a comment describing a limitation is a claim
  about the state *when it was written*, not a live fact.
- **A client built without `VB_WITH_COMPRESSION` can never successfully
  asset-sync against a server built with it on, and fails with a deterministic,
  first-asset `"asset transfer failed (hash mismatch or size cap)"` on
  every single connect** — not intermittent, not a real corrupted transfer.
  Root cause: `hash_bytes()` (`src/assetsync/manifest.cpp`) is stubbed to
  always return an all-zero `AssetHash{}` in the `#if !VB_WITH_COMPRESSION`
  build, and `ClientAssetCache::ingest_chunk` (`src/assetsync/cache.cpp`)
  verifies every received file by comparing its own `hash_bytes(received
  bytes)` against the hash the server sent — a disabled-compression client
  always computes `{}` regardless of what it actually received, which can
  never equal a real server-computed hash. Hit 2026-09-18: server built
  fresh on a Mac with `VB_WITH_COMPRESSION=ON`, client run from this
  project's existing `build-net-lua` on Windows, which had never had
  compression turned on (see `STATE.md.local`) — reconfiguring that one
  build dir with `-DVB_WITH_COMPRESSION=ON` and rebuilding fixed it (280/280
  `vb_tests` still green). **Both the client and server binaries in a real
  (non-`--singleplayer`) connection need matching `VB_WITH_COMPRESSION`
  settings** — there's no runtime negotiation/fallback for this mismatch,
  the failure mode gives no hint that compression flags differ, and
  `content/base/ui/*.lua` (the HUD, inventory, pause screens) silently never
  loads for *any* real connection at all when compression is off on the
  server (see this same section's asset-sync note) — so a from-scratch
  build intended for real multiplayer likely wants
  `-DVB_WITH_COMPRESSION=ON` from the start on every machine involved, not
  just one side.
- **A second, independent bug produces this exact same
  `"asset transfer failed (hash mismatch or size cap)"` text even with
  matching `VB_WITH_COMPRESSION` on both ends** — found 2026-09-18
  immediately after the note above, when the same user hit it again
  connecting a Mac client to their own Mac server (same build, same
  machine, so the compression-flag theory above was already ruled out).
  Root cause: `src/server/main.cpp` called `build_manifest()` (hashes
  `storage.json` on disk) before `PackRuntime`'s `vb.storage` write from
  `load_content_pack`'s `init.lua` (both `content/base` and
  `content/examples/kitchen_sink` write `vb.storage.boot_count` at load) had
  actually reached disk — that write only flushes on the *first server
  tick* (`PackRuntime::Impl::dispatch_tick`,
  `storage_dirty_flag`/`flush_storage()`, Phase 4.2's deferred-write
  design), which always happens *after* the manifest is already built and
  handed to every connecting client. The manifest hashes the stale
  pre-write bytes; the first tick then silently rewrites the file to the
  post-write bytes before any real client's `asset_file_bytes()` fetch —
  guaranteed mismatch, on every connect, on every machine, regardless of
  build flags, for any pack that touches `vb.storage` at load time (which
  both shipped packs do). **Fixed** by calling
  `pack_runtime.flush_storage()` between `freeze()` and `build_manifest()`
  in `src/server/main.cpp` — forces the write to disk before the manifest
  hashes it. Regression coverage: `tests/unit/content_pack_test.cpp`'s two
  new `VB_WITH_COMPRESSION`-gated cases (one proving the fixed ordering
  keeps the hash consistent, one proving the old ordering really does
  reproduce the mismatch). See `REMAINING_TASKS.md` 4.4's added bullets for
  the still-open, lower-priority follow-on (a pack writing `vb.storage`
  again *after* startup, not just at load, still goes stale for that
  server's remaining lifetime — no shipped pack does this today).
  **Lesson for next time:** this exact error string has (at least) two
  unrelated root causes — don't stop investigating after ruling out the
  compression-flag mismatch above; check whether the content pack writes
  `vb.storage` at load time next.
