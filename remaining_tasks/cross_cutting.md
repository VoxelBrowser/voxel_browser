## Cross-Cutting / Continuous

> Full history for this section; linked from `REMAINING_TASKS.md`. Ground truth for [x] items — do not duplicate here.

- [~] End-to-end multiplayer automation (dev-only) — designed 2026-10-02,
      see `docs/e2e-automation.md`. Phases: E0 input seam (landed), E1
      `ClientApp` extraction (landed 2026-10-02: `src/client/client_app.{hpp,cpp}`,
      `session_host.hpp`; `run_headless` now = `ClientApp(render=false)` +
      `connect_blocking()`; verified: `vb_tests` + 3 smoke tests green in a
      `VB_WITH_LUA/WORLDGEN/COMPRESSION` build, windowed path NOT run — no GPU
      in the CI container), E2 `VB_WITH_AUTOMATION`/`VB_DISTRIBUTION` +
      stdio host + predicates (landed 2026-10-03: `src/automation/`,
      `inc/vb/automation/`, `{client,server}/automation_endpoint.hpp`,
      `docs/automation-protocol.md`, CI `VB_DISTRIBUTION` legs + `--version`
      check; verified: `vb_tests` 11 new `automation*` cases and `ctest` 4/4 in a
      `VB_WITH_AUTOMATION=ON` + LUA/WORLDGEN/COMPRESSION build, ctest incl. the
      new `*_rejects_automation` in the default build; manual stdio round trips
      against the singleplayer client and the server; NOT run: `VB_WITH_NET`
      builds, Windows/macOS, real CI), E2b `C2S_Hello.client_flags` rejection (landed
      2026-10-03, protocol 27; 4 `net_test` cases + codec round-trip), E3
      actions + server admin commands (landed 2026-10-04; hand-verified with
      two real clients against a real dedicated server, 35 checks; windowed-only
      `menu.*`/`screenshot`/chat typing moved to E5), E4 pytest harness + CI `e2e` leg
      (landed 2026-10-04: 10 tests, `ctest -L e2e` 10/10 locally x5; CI job not yet
      run on a runner; also fixed a real `ui.close()` segfault found by it), E5
      `--net-sim`/windowed clients/screenshots/trace viewer (landed 2026-10-04: 18 e2e
      tests, 3x under Xvfb green; CI job not yet run on a runner), E6 TCP attach + session
      recorder (landed 2026-10-04: 24 e2e tests; a recorded session replays green on a fresh
      server; Windows Winsock path unbuilt). Constraint: automation
      code must never ship — see §7 of that doc. Each phase's doc updates:
      §11 of that doc.

- [x] **Real engine bug found 2026-09-28 while building
      `content/base/entities/zombie.lua` (Phase 6's "mob damage" item):
      a `script::PlayerHandle` stored across calls and read back from
      inside `vb.on("tick", ...)` or a script entity's own `on_tick`
      silently returns corrupted data** — confirmed with ASan (Windows
      MSVC + `clang_rt.asan_dynamic`, `-DVB_ENABLE_ASAN=ON`), not just
      inferred from a crash: `PlayerHandle::get_name()`/`get_pos()` read a
      garbage `rt` (`PackRuntime::Impl*`), a `nullptr`/garbage `session`,
      and a garbage `net_id` (observed `0xFFFFFFFE`) when called this way,
      instead of the real stored values. **Repro (minimal, reproduced
      multiple times with a guaranteed-fresh rebuild each time — see
      `content_pack_test.cpp`'s zombie test's own comment for the summary):**
      `vb.on("chat", function(player, text) stashed = player end)` then,
      from `vb.on("tick", function(dt) print(stashed:get_name()) end)`,
      the printed name is wrong/empty on every subsequent tick, forever —
      not a one-time race. **What's proven NOT the cause:** storage
      location (module-level global, entity-table field, and a bare Lua
      upvalue closure all reproduce it identically); it is not a same-tick
      timing race (reproduces across many ticks after the store, not just
      the first). **What IS the distinguishing factor:** reading a
      *stored* `PlayerHandle`'s methods from within the `dispatch_tick`
      call path (`PackRuntime::Impl::dispatch_tick` → `fire("tick", ...)`
      *or* → `dispatch_entity_tick` → a kind's `on_tick`) is broken;
      reading a **freshly passed-in** `PlayerHandle` argument from within
      *any* handler (`chat`, `player_input`, `player_join`, `player_death`,
      ...) — even storing *that* one and reading it back from a **later,
      separate `chat`/`player_input`-style dispatch** — works perfectly
      (also confirmed with ASan, `rt`/`session`/`net_id` all correct).
      `run_veto()` and `fire()` (`src/script/pack_runtime.cpp`) are
      structurally identical templates, so the bug isn't in either of
      those directly — it's somewhere in how `dispatch_tick` specifically
      reaches Lua (`ServerSession::on_script_tick_` → `PackRuntime::
      attach_session`'s hook lambda → `PackRuntime::dispatch_tick` →
      `Impl::dispatch_tick`), not yet root-caused further. **Worked
      around, not fixed:** `zombie.lua`'s own chase/attack logic is driven
      from `vb.on("player_input", ...)` instead (a fresh `PlayerHandle`
      every real `InputCmd`, used immediately, never stored) — see Phase
      6's own "mob damage" entry for the full design. **Impact if unfixed:**
      any *future* pack that tries the natural "capture a player in a
      hook, act on them later from `on_tick`" pattern will hit this
      silently (no crash in a non-ASan build — it just reads wrong data,
      e.g. `player:damage()` becomes a no-op since `session` reads as
      `nullptr`) — a real correctness trap for content authors, not just
      an inconvenience. Whoever picks this up next: start from the exact
      repro above (it's a ~15-line pack script), build `build-net-lua`
      with `-DVB_ENABLE_ASAN=ON` (needs `clang_rt.asan_dynamic-x86_64.dll`
      on `PATH`, found under the MSVC toolchain's `bin\Hostx64\x64`), and
      get a fresh ASan stack trace before theorizing — several plausible-
      sounding theories (GC collecting a live-referenced value, a `fire()`
      vs `run_veto()` implementation difference, a moved/stale
      `PackRuntime` pointer) were all checked and ruled out empirically
      during this investigation.
    - **2026-09-29 follow-up: reproduced end-to-end with a real ASan build
      (`-DVB_WITH_NET=OFF -DVB_WITH_LUA=ON -DVB_ENABLE_ASAN=ON`, plain
      `-DCMAKE_BUILD_TYPE=RelWithDebInfo` — `VB_WITH_NET` can stay OFF for
      this repro since `LoopbackNetwork`/`ServerSession` don't need real
      GNS; a fresh `VB_WITH_NET=ON` configure on this box hits the
      pre-existing "Could NOT find Protobuf" gap noted in
      `STATE.md.local`).** Wrote a minimal `pack_runtime_integration_test.cpp`-
      style case: `vb.on("chat", ...)` stashes `player` into a Lua global on
      `text == "stash"`, `vb.on("tick", ...)` does
      `tick_name = stashed:get_name()` every tick, a later
      `vb.on("chat", ...)` on `text == "report"` returns `tick_name` so the
      C++ side can observe what the tick handler actually saw (chat's own
      dispatch path is the one proven unaffected, so it's a safe way to read
      the result out). Result: **not a silent bad-data read here — a real
      ASan-caught fault inside `PlayerHandle::get_name()` itself
      (`src/script/pack_runtime.cpp` — the `rt->session` dereference),
      every single time**, both as a plain `access-violation` (`rt` holding
      a small/near-null garbage value) and, in one run with extra
      `std::fprintf` debug args added inline, as a `stack-buffer-overflow`
      report pointing at `get_name()`'s own stack frame (with ASan's own
      "may be a false positive... setjmp/longjmp" hint attached, since
      Lua's C core (`ldo.c`) always uses raw `setjmp`/`longjmp` for
      `lua_pcall`'s protected-call boundary — `LUAI_TRY`/`LUAI_THROW` only
      switch to C++ `throw`/`catch` when the *including TU* is compiled as
      C++, which `ldo.c` never is). **Ruled out that longjmp/ASan
      interaction as the actual cause, though**: the fault reproduces
      identically with no `pcall` anywhere in the repro script (a bare
      `stashed:get_name()` inside `vb.on("tick", ...)`, no protected-call
      boundary below `fire()`'s own `sol::protected_function` at all) — same
      crash, same line, same "`rt` reads as garbage" shape. So this is a
      genuine memory bug, not an ASan/setjmp artifact; the "false positive"
      hint in one run's output was a red herring from that run's own added
      instrumentation, not a property of the underlying fault. **Not
      re-root-caused further** (this pass's budget went into getting a
      clean, deterministic, pcall-free repro plus a real ASan trace rather
      than a fix) — full sol2/call.hpp template chain from the trace:
      `fire<double&>` → `sol::basic_protected_function::call` → Lua calls
      into `sol::u_detail::binding<...>::call_with_` →
      `sol::call_detail::call_wrapped<PlayerHandle, ...>` →
      `lua_call_wrapper<PlayerHandle, std::string(PlayerHandle::*)()const,...>::call`
      → `stack::call_into_lua` → `stack::stack_detail::eval<0,
      argument_handler<...>&, ..., PlayerHandle&>` →
      `member_function_wrapper<...>::caller::operator()` → `get_name()`.
      Whoever picks this up next: don't re-derive the repro from scratch —
      the exact Lua snippet above plus that build command reproduces it in
      under a minute; step through `stack::stack_detail::eval`/
      `member_function_wrapper::caller` in a real debugger (cdb/WinDbg on
      this box) at the point it extracts `self` from the Lua stack for a
      *zero-argument* `const` member function returning `std::string` by
      value — that specific combination (no `sol::this_state`, no extra
      args, non-trivial return type) is the one thing every broken call site
      (`get_name()`, `get_pos()`) shares that the working ones (methods
      taking a fresh argument every call, e.g. `player_input`'s handle)
      don't obviously differ on at the Lua-script level, so the bug is most
      likely in how sol2 resolves `self` for exactly that call shape when
      the target function pointer is stored in a `sol::protected_function`
      invoked with a *single scalar argument unrelated to the stashed
      value* (`fire("tick", dt)` — `dt` alone) versus one invoked with the
      handle itself as a fresh argument. A fresh ASan run with a debugger
      attached (`cdb -g -G build-asan-repro\vb_tests.exe --test-case=...`)
      to catch the access-violation live and inspect the Lua stack slot at
      the crash point, rather than only reading the post-mortem trace,
      is the natural next step.
    - **2026-09-30: root-caused and fixed.** The 2026-09-29 "zero-argument
      const member function" theory was a red herring — confirmed by adding a
      second, structurally different repro (`vb.every(...)`'s timer, whose
      handler is called with **literally zero Lua arguments at all**, not
      just zero *scalar* args) alongside the original `vb.on("tick", ...)`
      one; both crashed identically, and a GC-stop experiment
      (`lua_gc(L, LUA_GCSTOP, 0)` right after opening libraries) did **not**
      prevent the crash, ruling out premature garbage collection of the
      userdata too. The real cause, found by temporarily instrumenting
      `PlayerHandle::get_name()` and `Impl::run_chat()` to print raw
      pointers: **the extracted `self` inside `get_name()` was byte-for-byte
      identical to `&p` of `run_chat()`'s own local `PlayerHandle p{...}`** —
      i.e. the Lua value being called wasn't a copy of the handle at all, it
      was a **raw pointer into `run_chat()`'s C++ stack frame**, already
      popped and reused by the time anything read it back. Cause: sol2
      (`stack_core.hpp`'s `stack_detail::push_reference<T>`) pushes a
      **non-const lvalue reference** to a registered usertype argument as
      `as_reference_tag` (a pointer into existing memory, no copy) by
      default — a documented perf optimization for the common case where a
      Lua handler only uses an argument during the call itself — unless the
      `SOL_FUNCTION_CALL_VALUE_SEMANTICS` config macro is defined on. Every
      dispatch call site in `pack_runtime.cpp` (`run_chat`, `run_player_input`,
      `run_respawn_handler`, `fire()`/`run_veto()`'s callers, ...) constructs
      a named local (`PlayerHandle p{...}`) and passes it straight into the
      Lua call — an lvalue every time — so any pack script that stores that
      argument beyond the call it was received in (a global, a table field,
      a closure upvalue — storage location never mattered, exactly as
      empirically found on 2026-09-28) ends up holding a dangling pointer
      into a stack frame that's already gone. It "worked" when read back from
      a *later, separate* `chat`/`player_input` dispatch purely by luck: that
      call path re-enters `run_chat`/`run_player_input` with the same C++
      call shape, so the next call's own `p` local happens to get allocated
      at the same stack address with correct-looking values; `dispatch_tick`'s
      entirely different call tree (`script_tick` phase vs. `network_io`
      phase in `ServerSession::build_systems()`) reuses that address for
      unrelated locals instead, producing the "always wrong from tick,
      always right from chat" split the original investigation correctly
      observed but explained one level too shallow. **Fix**
      (`cmake/Dependencies.cmake`, right after the `sol2` `vb_fetch()`
      call): `target_compile_definitions(sol2 INTERFACE
      SOL_FUNCTION_CALL_VALUE_SEMANTICS=1)` (resolved off the `sol2::sol2`
      alias via `ALIASED_TARGET`, since `target_compile_definitions()`
      rejects alias targets directly) — forces sol2 to push every
      registered-usertype function-call argument as an owned copy regardless
      of value category, restoring the invariant `PlayerHandle`'s own doc
      comment already assumed ("constructed fresh per dispatch call, so it
      can never dangle"). Applied to the sol2 target itself (not a
      `pack_runtime.cpp`-local `#define`) so it's consistent across every
      translation unit. Verified safe project-wide: `PlayerHandle` is the
      *only* usertype this project registers (`grep -n "new_usertype<"` across
      `src/script/`), and it's a stateless proxy — no method ever mutates
      `net_id`/`rt` in place — so forcing copy semantics changes nothing
      observable, only removes the unsafe aliasing. Confirmed with a
      permanent regression test (`pack_runtime_integration_test.cpp`, "a
      PlayerHandle stashed from a chat handler survives being read back from
      a later tick/timer handler") exercising both the `tick`-event and
      `vb.every` timer shapes. `content/base/entities/zombie.lua`'s
      `player_input`-driven workaround was left as-is (its comment updated to
      stop describing a now-fixed bug) — it never actually stored a
      `PlayerHandle` in the first place, only the zombie's own entity handle
      keyed by player name, so there was nothing to migrate. Verified: full
      `vb_tests` 425/425 green on `build-net-lua` (plain rebuild) and
      403/403 green on `build-asan-repro` (ASan on, exit 0 — no new leaks/UB),
      clean `-Werror` build of `vb_tests`/`voxel_browser`/
      `voxel_browser_server` (temporarily reconfigured `build-net-lua` with
      `-DVB_WARNINGS_AS_ERRORS=ON`, confirmed clean, reconfigured back to
      this dir's OFF default afterward).
- [x] Sanitizer (ASan/UBSan) debug CI job; TSan job for the threaded
      subsystems — landed 2026-09-28. `cmake/Sanitizers.cmake`'s
      `vb_sanitizers` INTERFACE target (and the `VB_ENABLE_ASAN`/
      `VB_ENABLE_UBSAN`/`VB_ENABLE_TSAN` options) already existed and were
      already linked into every first-party target, but no workflow ever
      turned them on — this closes that gap on the Linux matrix only
      (`.github/workflows/build_linux.yml`): two new `build_type` entries,
      `asan` (`-DVB_ENABLE_ASAN=ON -DVB_ENABLE_UBSAN=ON`, combinable per
      `Sanitizers.cmake`'s own mutual-exclusion check) and `tsan`
      (`-DVB_ENABLE_TSAN=ON`), both `RelWithDebInfo` so symbols/line info
      survive in sanitizer output. Both run the exact same `ctest
      --output-on-failure` the existing release/debug jobs already run — no
      new test list, the point is running *existing* tests under
      instrumentation. Binary staging/artifact upload is skipped for these
      two (`if: matrix.build_type == 'release' || matrix.build_type ==
      'debug'`) since a sanitizer build isn't a real distributable. Windows
      (MSVC) and macOS were deliberately left alone: `Sanitizers.cmake`
      itself already documents MSVC has no UBSan/TSan support at all (ASan
      only, and even that needed the `_DISABLE_*_ANNOTATION` workaround
      already in that file for third-party static-lib linking), and macOS CI
      doesn't even build `VB_WITH_NET` yet (Phase 1's own still-open item)
      — Linux is the one platform where all three sanitizers and the
      full net-enabled build are simultaneously supported. **Not verified by
      an actual GitHub Actions run** — this agent environment has no way to
      trigger/observe a real Actions run; the change is a straightforward
      extension of the existing, already-green release/debug matrix entries
      (same install/configure/build/test steps, just extra `-DVB_ENABLE_*`
      flags plumbed through `matrix.sanitizer_cmake_flags`), but the first
      real run may still surface a genuine sanitizer finding (a real race or
      UB) or an environment quirk (e.g. a third-party FetchContent'd lib not
      tolerating being linked against instrumented code) that a local
      Windows-only agent session can't preempt — same category of caveat as
      every GUI-adjacent item in this file, just for CI infrastructure
      instead of rendering.
- [x] Soak test target (N simulated clients, random walk + edits) — landed
      2026-09-28 as `tests/unit/soak_test.cpp`, folded into the normal
      `vb_tests` run rather than a separate nightly/pre-release job. 4
      simulated `ClientSession`s over `LoopbackNetwork` (real `ServerSession`
      + `WorldReplicator` + `World`, `WorldGenWorkerPool::kSynchronous`)
      random-walk (fixed-seed `std::mt19937`, reproducible) and
      break/place-edit for 150 ticks, then all disconnect. Asserts three
      things ARCHITECTURE_SPEC.md's Testing Strategy calls out: every client
      stays joined and no pending connection is left stuck
      (`player_count()`/`pending_count()`), `pending_edit_count()`/
      `unacked_input_count()` stay bounded rather than growing across the
      whole run (a real queue-growth bug — an ack that stops being sent, an
      edit result that never arrives — would blow well past the generous
      bound), and `World::loaded_coords()` stays bounded rather than growing
      roughly linearly with tick count (chunk unload actually keeps working
      under sustained random-walk churn). A final round confirms
      `player_count()`/`pending_count()` both return to 0 after every client
      disconnects — no leaked `Conn` entry. Real ASan/LSan leak detection
      comes for free from this simply being part of `vb_tests`, which the
      Linux CI matrix already runs under `-DVB_ENABLE_ASAN` (this same day's
      earlier CI pass) — no separate sanitizer wiring needed for that half.
      **Deliberately kept small** (4 clients, close together, 150 ticks):
      real fBm terrain generation dominates this test's cost in an
      unoptimized Debug build far more than the sim logic actually being
      soaked — an earlier draft (6 clients spread far enough apart to force
      a distinct set of chunk columns each, 400 ticks) measured ~55s of real
      CPU time standalone; this version measured ~14s against a ~70s
      baseline for the other 412 tests combined, a proportionate addition
      rather than a suite-doubling one. Verified: full `vb_tests` 413/413
      green, clean `-Werror` build of `vb_tests`/`voxel_browser`/
      `voxel_browser_server`.
- [x] Server capacity test — landed 2026-11-01 as `tests/unit/capacity_test.cpp`
      (loopback, in `vb_tests`): sequential fill to `max_players` + refusal +
      slot reuse, a `max_players + 8` simultaneous burst that must never
      overfill (it did: fixed in `ServerSession`'s `current_player_count`),
      and a 16-player median tick < 50 ms gate (measured ~0.12 ms).
- [ ] Real-UDP capacity benchmark: spawn N automation clients against a
      running `voxel_browser_server` and report tick time / bandwidth per
      client as N grows (needs `VB_WITH_NET` + `VB_WITH_AUTOMATION`).
- [x] Perf budget checks: chunk mesh time, snapshot size — landed 2026-09-28
      as `tests/unit/perf_budget_test.cpp`, two budget-gate `TEST_CASE`s
      folded into the normal `vb_tests` run (no historical tracking, no
      graphs, nothing scheduled separately — matching this item's own
      "simple benchmark harness" framing). Chunk mesh time: generates a
      real chunk at the generator's own reported surface height (a fixed
      chunk-y guess turned out unreliable — an earlier draft hit an entirely
      open-sky chunk for this seed/column and failed its own "not empty"
      sanity check), meshes it 3x via `mesh_chunk()` and keeps the fastest
      run, asserts `< 100ms` (measured ~21ms in this environment's
      unoptimized Debug build — real headroom, not a hair's-width pass).
      Snapshot size: encodes a 50-entity `S2CEntitySnapshot` (deliberately
      busier than any single player's interest radius would realistically
      ever surface at once) and asserts the encoded size stays under a
      per-entity byte budget (measured ~52 bytes/entity; budget is 90
      bytes/entity + a fixed 256-byte allowance) — would catch a real
      regression such as `visual_override` starting to ride on every
      record instead of only `entered` ones. **Frame time was deliberately
      left out**, not attempted as a stand-in: real rasterization/GPU cost
      needs a live GL context to mean anything, and this agent environment
      has no GUI. Verified: full `vb_tests` 415/415 green (2 new cases),
      clean `-Werror` build of `vb_tests`/`voxel_browser`/
      `voxel_browser_server`.

---

### Moved from `REMAINING_TASKS.md`'s core (dream, 2026-10-06)

> Verbatim text of the core file's "Remaining" list for this section at the
> time of the move; the core now keeps a one-line summary.

- [~] **End-to-end multiplayer automation (dev-only)** — design in
      `docs/e2e-automation.md`; phases E0–E6 below it. E0 (input seam) and
      E1 (`ClientApp`) and E2 (flags, stdio host, predicate engine) E2b (handshake `client_flags`, protocol v27) E3 (client actions + server admin commands) E4 (pytest harness, `e2e` CTest/CI job) E5 (`--net-sim`, windowed clients, screenshots, trace viewer) and E6 (TCP attach, session recorder) have landed (all phases); still open: the CI job has never run on a runner, and Windows/macOS are unbuilt; the rest is open. Must stay compiled out of production builds
      (`VB_WITH_AUTOMATION`, `VB_DISTRIBUTION`). Doc-upkeep checklist per phase:
      `docs/e2e-automation.md` §11.

- [x] **`script::PlayerHandle` stashed across ticks returning/crashing on
      corrupted data** (found 2026-09-28 building
      `content/base/entities/zombie.lua`, root-caused and fixed 2026-09-30).
      Real cause: sol2 pushes a non-const lvalue reference to a registered
      usertype as a raw pointer into the caller's own C++ stack frame, not a
      copy, unless `SOL_FUNCTION_CALL_VALUE_SEMANTICS` is defined on — every
      `PlayerHandle` dispatch call site constructs a named local and passes
      it straight into the Lua call, so a pack script that stores that
      argument beyond the call (a global, table field, upvalue — storage
      location never mattered) holds a dangling stack pointer the instant a
      *different* C++ call path reuses that address. Fixed by defining
      `SOL_FUNCTION_CALL_VALUE_SEMANTICS=1` on the `sol2` target
      (`cmake/Dependencies.cmake`) — `PlayerHandle` is the only usertype this
      project registers and is a stateless proxy, so forcing copy semantics
      is free. Full root-cause writeup, the empirical pointer-identity proof,
      and verification detail in `remaining_tasks/cross_cutting.md`.
- [ ] Keep `ENGINE_PROTOCOL_VERSION` + `docs/protocol.md` in lockstep with every
      wire change.
- [ ] Every new `vb/protocol` struct gets a round-trip + fuzz test.
- [x] Sanitizer (ASan/UBSan) debug CI job; TSan job for the threaded
      subsystems — landed 2026-09-28 on the Linux matrix only (MSVC has no
      UBSan/TSan support; macOS CI doesn't build `VB_WITH_NET` yet). Not
      verified by an actual GitHub Actions run — this agent environment
      can't trigger one.
- [ ] Determinism golden-value CI gate stays green across platforms.
- [x] Soak test target (N simulated clients, random walk + edits) — landed
      2026-09-28 as `tests/unit/soak_test.cpp`, folded into the normal
      `vb_tests` run. 4 simulated clients over `LoopbackNetwork`
      random-walk + break/place-edit for 150 ticks; asserts connection/edit-
      queue/loaded-chunk counts all stay bounded rather than growing, and
      that everything cleans up to 0 after disconnect. Deliberately kept
      small (real fBm terrain generation dominates cost far more than the
      sim logic being soaked).
- [x] Perf budget checks: chunk mesh time, snapshot size — landed 2026-09-28
      as `tests/unit/perf_budget_test.cpp`. Chunk mesh time asserts
      `< 100ms` (measured ~21ms); snapshot size asserts a per-entity byte
      budget (measured ~52 bytes/entity vs. a 90-byte budget). Frame time
      deliberately left out — needs a live GL context this environment
      doesn't have.
- [ ] `--headless` stays functional for both binaries (CI + integration tests).
- [ ] Address the remaining open item(s) in `ARCHITECTURE_SPEC.md` §18 as
      their blocking phase arrives (renumbered from §19; every row is now
      resolved or has a noted direction — Q4 chunk compression resolved
      2026-09-28 (LZ4 wired generically into `frame_message()`, see that
      row); Q5 persistence has region-file LZ4 framing resolved 2026-09-28
      too (`RegionStore` format version 1 -> 2, see that row), and
      `--singleplayer`'s `RegionStore` wiring is also now resolved
      (2026-09-28, see `STATE.md` history (`state/changelog-*.md`)) — Q5 has no open sub-
      item left; Q6 auth has a direction set but isn't implemented yet);
      record decisions in that section.
