# Developer Experience (pack authors & agents) — Design & Phased Plan

> Full detail for this topic; the backlog entry is `REMAINING_TASKS.md`
> Phase 10. Status: **implemented 2026-10-06** (10.A-10.G; see "As built" below for
> the deviations from this plan).

## 1. Problem

The engine has grown a large Lua surface and a capable `vb` CLI, but a person
(or an agent) who wants to *write a content pack* still has to reverse-engineer
it:

| # | Gap | Today |
| --- | --- | --- |
| 1 | No `npm init` equivalent | A new pack is made by copying `content/base` or `content/examples/kitchen_sink` by hand. `vb structure new` is the only scaffolding command. |
| 2 | No quick-reference for the Lua API | `docs/lua-api.md` (~750 lines) is an accurate but narrative design log: one bullet can run 60 lines and mix several functions, history and phase numbers. There is no "one function, signature, one example" lookup. |
| 3 | No editor support | No type stubs, so editors flag `vb` as an undefined global, offer no completion, and happily complete `io.open`/`os.time`, which the sandbox removes (`strip_sandbox`, `src/script/vm.cpp`). |
| 4 | Not agent-friendly | `vb --help` is a one-line-per-command list; there is no markdown CLI reference, no per-command help with examples, no machine-readable command list, and no headless "does my pack load?" check an agent can loop on. |

## 2. Goals / non-goals

**Goals**

1. `vb pack init [dir]` (the `npm init` equivalent) produces a pack
   that loads, runs (`vb pack dev`), has editor completion, and explains itself
   to a human or an agent in its own `README.md`/`AGENTS.md`.
2. **One source of truth for the Lua API** from which the stubs and the quick
   reference are generated, with a test that fails when a binding is added to
   C++ without being described (drift is the main risk of any API doc).
3. A fast, headless, scriptable validation loop: `vb pack check [--json]`
   reports load errors with `file:line`, exit code `0/1`.
4. A complete, generated CLI reference (`docs/cli.md`) and `vb help <command>`,
   both from the command table, plus an `--json` form for tools.
5. Everything works offline from an installed version: stubs and docs ship in
   the release archive.

**Non-goals (for now)**

- A pack registry / `vb pack publish` / dependency resolution (stacked packs
  are already `Deferred`).
- An editor plugin. We target the standard Lua Language Server (LuaLS,
  `sumneko.lua`), which every mainstream editor already runs; stubs follow its
  LuaCATS annotation format.
- Server-side hot reload (also `Deferred`); `vb pack dev` keeps using the
  existing `vb host --watch` restart loop.
- Rewriting `docs/lua-api.md`'s history away. It stays as the narrative /
  design guide; the new reference is the lookup.

## 3. Key decisions

### 3.1 Source of truth: hand-written LuaCATS stubs

Candidates considered:

| Option | Pros | Cons |
| --- | --- | --- |
| **A. Hand-written LuaCATS stubs** (`sdk/lua/library/*.lua`) are the source; a script renders markdown from them | Stubs are what editors consume, so they can never be "the generated thing nobody checked". LuaCATS is a known format, agents already read it. Examples live next to the signature. | Need a small parser in the generator (only our own subset of annotations). |
| B. A schema (TOML/JSON) generating both stubs and docs | Fully structured | A third format to learn; examples and prose are awkward in TOML; two generators. |
| C. Annotate the C++ binding sites and extract | Closest to code | sol2 lambdas are spread over 3 300 lines of `pack_runtime.cpp`; comment-extraction from C++ is fragile; still needs prose somewhere. |

**Decision: A.** Drift is caught by a runtime test (§3.2), not by generating
from C++.

Layout (new top-level `sdk/`, shipped in the release archive):

```
sdk/lua/
  library/              # LuaCATS stubs, ---@meta files, never executed
    vb.lua              # vb.register_*, vb.on/after/every, vb.storage ...
    vb.world.lua        # one file per sub-table (world, worldgen, physics,
    vb.worldgen.lua     #   combat, action, hunger, daynight, render, noise,
    ...                 #   config, db, crypto, auth)
    player.lua          # usertypes: Player, Entity, Inventory, ...
    events.lua          # vb.on overloads: one ---@overload per event name,
                        #   so the handler's argument types are known
    ui.lua              # client UI VM (`ui` table, widget tree types)
    data_script.lua     # what a pure data script may return (block tables)
    sandbox.lua         # documents what is NOT available (io, os, load, ...)
  luarc.template.json   # copied to a pack's .luarc.json by `vb pack init`
```

Annotation conventions (checked by the generator, so the docs stay uniform):

- Every public function has a one-line summary, `---@param`/`---@return` for
  every argument, and **at least one fenced ```` ```lua ```` example** in its
  description. LuaLS shows that markdown on hover, so the example also reaches
  the editor.
- Definition tables (`register_block`'s `def`, `visual`, worldgen pipeline
  stages) are `---@class` with one `---@field` per key, including default and
  units (`---@field max_damage? integer Damage to break; 0 = instant (default 0)`).
- Context tags in a custom, LuaLS-ignored line: `---@vb context load|runtime|ui|data`
  (pack-load-only registration vs. runtime vs. client UI VM vs. data script)
  and `---@vb since <protocol or release>`. The generator turns these into
  badges in the reference.
- Event names are a `---@alias VbEvent "player_join"|...` plus one overload
  per event so `vb.on("block_break", function(e) ... end)` gets a typed `e`.

### 3.2 Drift test (runtime ⇆ stubs)

`tests/unit/lua_api_surface_test.cpp` (behind `VB_WITH_LUA`):

1. Build a `PackRuntime` over a loopback transport (as existing pack tests
   do) and a `UiRuntime`; walk `vb` / `ui` recursively, plus the usertype
   metatables (`Player`, ...), collecting dotted names of every function.
2. Read `sdk/lua/api_index.txt` — a sorted list of the same dotted names that
   the generator writes from the stubs (so the C++ test needs no Lua parser).
3. Fail with the symmetric difference: "bound but undocumented" and
   "documented but not bound". An explicit `sdk/lua/api_ignore.txt` lists
   internal helpers that are deliberately left out.

CI also runs `scripts/gen_lua_docs.py --check` (regenerates into a temp dir,
diffs against the committed output) so a stub edit without regenerated docs
fails the lint job.

### 3.3 Generated quick reference

`scripts/gen_lua_docs.py` (Python 3, stdlib only, like the other release
scripts) reads `sdk/lua/library/*.lua` and writes:

```
docs/lua-reference/
  README.md          # cheat sheet: every function as one line
                     #   `vb.world.get_block(pos) -> BlockId` — summary
                     #   grouped by table, each linked to its section
  vb.md, vb.world.md, ..., ui.md, events.md, types.md
sdk/lua/api_index.txt
```

Per-function section: signature, context badge, summary, parameter table,
returns, example(s), "see also" links. Examples are **syntax-checked** by a
doctest (`luaL_loadbuffer` without running; the generator also extracts them
into `build/doc_examples/*.lua`), so a typo in a doc example breaks CI.

`docs/lua-api.md` gets a header pointing to the reference for lookups and
keeps its narrative/design content; over time, per-function detail that the
reference now covers can be trimmed from it.

### 3.4 Headless pack validation: `voxel_browser_server --check-pack`

New server flag: `--check-pack <dir> [--json]` loads the pack into a
`PackRuntime` exactly like a real start (pack loader order, `require`,
`auth.lua` parsing, `freeze()`, worldgen pipeline construction, `ui/*.lua`
compiled in a `UiRuntime`, `structures/` validated with the existing editor
validator), generates **one** chunk around the origin to exercise the
worldgen pipeline, then exits without opening a socket. Output: diagnostics
`{severity, file, line, message}` (human or JSON), exit `0` clean / `1`
errors. Warnings (unknown fields in `def` tables, block without texture, …)
never fail unless `--strict`.

Living in the server binary (rather than in `vb`) keeps one loader and makes
the check match whatever engine version will host the pack. `vb pack check`
resolves the version (§3.5) and runs it.

### 3.4.1 Per-environment globals (decided 2026-10-06)

A pack's files run in four different environments, but LuaLS applies one set
of globals to the whole workspace:

| Files | Environment | Engine globals |
| --- | --- | --- |
| `init.lua`, `blocks/`, `entities/`, `biomes/`, other root `*.lua` | server pack VM (`PackRuntime`) | `vb` |
| `ui/*.lua` | client UI VM (`UiRuntime`) | `ui`, `client` |
| `data/*.lua` data scripts | bare state (`eval_data_script`) | none (`vb.*` raises) |
| `auth.lua` | auth VM (`load_auth_lua`) | none; returns a table |

So stubs that declare both `vb` and `ui` let `vb.world.get_block` complete
silently in `ui/hud.lua` (nil at runtime — and inside an `on_click` handler
only when clicked), and `ui.define` in `init.lua`. Pack-defined cross-file
globals (`base_ui` in `content/base/ui/_style.lua`, `base_*_id` on the server
side) leak across the boundary the same way.

**Decision:** the editor is best-effort, `vb pack check` is the enforcement.

1. Stubs declare every engine global; each carries `---@vb context` and its
   description starts with the environment ("Server pack VM only." /
   "Client UI VM only."), so hover makes the boundary visible.
2. `--check-pack` runs a **static global-access check**: compile every file
   (full Lua library, outside the sandbox), walk the function prototypes'
   bytecode for `_ENV` reads/writes (what `luac -l` lists as
   `GETTABUP/SETTABUP _ENV "name"`) and compare against the file's
   environment:
   - allowed = Lua builtins left by the sandbox + that environment's engine
     globals + globals *assigned* by any file of the same environment
     (so `base_ui` is fine in `ui/`, an error in `init.lua`);
   - a read of another environment's engine global is an **error**
     (`ui/hud.lua:42: 'vb' is not available in the client UI VM (server-only)`);
   - a read of a sandbox-removed builtin (`os`, `io`, `load`, …) is an error;
   - any other unknown global read is a **warning** (likely typo; `--strict`
     fails on it).
   Line numbers come from the prototype's line info, so diagnostics are
   `file:line` like every other check.
3. Optional, only if it proves reliable in 10.B: a generated VS Code
   multi-root `.code-workspace` with a nested `ui/.luarc.json` that drops the
   server stubs, giving real editor warnings in `ui/`. Not generated for
   editors where it doesn't work; no LuaLS plugin (too LuaLS-specific to
   maintain).
4. The template `AGENTS.md` states the rule in its first screen.

### 3.5 `vb pack` command group

```
vb pack init [dir] [--template minimal|ui|worldgen|base] [--name id]
             [--engine-req <range>] [--force] [--json]
                                  # dir defaults to "."; created if missing;
                                  #   refuses a dir that already has pack.toml
                                  #   (or other files) unless --force
vb pack check [dir] [--json] [--strict] [--version v]
vb pack dev [dir] [--version v] [--port n] [--no-client]
                                  # vb host --pack dir --watch + vb launch --connect
vb pack types [dir] [--version v] # (re)write .vb/lua stubs + .luarc.json
vb pack info [dir] [--json]       # name/version/engine req, files, block/entity counts
```

- **Non-interactive by default** (agents and CI): every value has a flag and
  a default; no prompts. `--json` prints what was created.
- **One name, decided 2026-10-06:** `vb pack init` is the only scaffolding
  command — no top-level `vb init`, no separate `vb pack new`. It works like
  `npm init`/`cargo init` on the current directory, or on `dir` (created if
  missing), which covers both cases.
- **Version pinning:** `pack.toml`'s `engine_version_req` is filled with
  `">=<selected version>"` (override with `--engine-req`). The engine
  enforces it (§3.7); `vb pack check/dev/types` pick the newest installed
  version satisfying it (or `--version`, which must also satisfy it), and
  print the `vb install <v>` to run if none does.
- **Templates** live in `templates/pack/<name>/` in the repo and are embedded
  into `vb` at build time (a CMake step generating a `.cpp` byte table, no new
  dependency), so `vb pack init` works before any version is installed and
  can't fail on a missing file. `{{name}}`, `{{version}}`, `{{engine_req}}`
  are the only substitutions. `--template base` instead copies
  `content/base` from the selected installed version (a full real pack).
  Every template is loaded by `--check-pack` in CI.

Generated pack (template `minimal`):

```
my_pack/
  pack.toml            # name, version 0.1.0, engine_version_req, entry
  init.lua             # vb.on("player_join") greeting + one chat command
  blocks/example.lua   # one textured block, registration commented
  ui/hello.lua         # one ui.define screen opened from init.lua
  textures/example.png # 16x16 placeholder
  README.md            # how to run/check/edit, links to the reference
  AGENTS.md            # §3.6
  .luarc.json          # LuaLS: Lua 5.4, library ./.vb/lua, sandbox builtins off
  .vb/lua/             # stubs copied from the version (gitignored; `vb pack types`)
  .gitignore           # .vb/, storage.json, db/, world/
```

`.luarc.json` disables the builtins the sandbox removes (`io`, `os`,
`package`, `debug` except `traceback` via a stub override), so the editor
reports `os.time()` as undefined exactly as the engine would. Stubs are
*copied* (not referenced in the version directory) so the pack's editor setup
survives `vb prune` and works when the pack is opened on another machine.

`vb pack types` is also how an existing pack (including `content/base` and
`kitchen_sink` in this repo) opts in.

### 3.6 Agent friendliness

1. **Command table becomes the CLI's documentation source.** `Command` gains
   `details` (paragraphs), `examples` (`{cmd, explanation}` pairs), per-flag
   help and `json` (does it support `--json`). Subcommand groups (`server`,
   `structure`, `pack`) get their own tables instead of one usage string.
   From it:
   - `vb help <command> [<sub>]` and `vb <command> --help`: full help with
     examples.
   - `vb help --markdown` → committed `docs/cli.md`, CI `--check` diff like the
     Lua docs. `vb help --json` → the whole table for tools.
   - Completions keep being generated from the same table (already true).
2. **Consistent machine contract**, written down once in `docs/cli.md`'s
   header: exit codes `0/1/2`, errors on stderr prefixed `error:` with a
   `hint:` line where one exists, `--json` on every command that reports
   state (adding it to `install`, `doctor`, `pack *`), no prompts unless a
   TTY *and* the action is destructive (and `--yes` always skips them).
3. **`AGENTS.md` files.**
   - Repo root `AGENTS.md`: a short pointer with the same content as
     `CLAUDE.md` (the convention other agent tools read), so non-Claude agents
     find `STATE.md`/`REMAINING_TASKS.md`/`CONTRIBUTING.md`.
   - Template `AGENTS.md` for packs (the high-value one): the pack layout and
     load order, the two VMs and which files run where, sandbox limits, the
     edit → `vb pack check --json` → `vb pack dev` loop, where the stubs and
     reference are (`.vb/lua/`, `vb docs`), and common mistakes (registration
     after freeze, block id order is saved in worlds, and — stated up front —
     files in `ui/` can't touch `vb` and server files can't touch `ui`/`client`,
     §3.4.1).
4. **Offline docs: `vb docs`.** The release archive ships `docs/` (at least
   `lua-reference/`, `cli.md`, `lua-api.md`, `structure-editor.md`, `auth.md`).
   `vb docs` lists topics, `vb docs vb.world.raycast` prints that section,
   `vb docs --path` prints the directory. An agent without web access can
   read the exact docs of the version it targets.
5. **`docs/llms.txt`**: a one-screen index of the docs (title + one line +
   path each), following the llms.txt convention, linked from `README.md`.

### 3.7 Enforcing `engine_version_req` (decided 2026-10-06)

Today `pack.toml`'s `engine_version_req` is informational: nothing parses
it (`content/base/pack.toml` says so). Decision: **the engine enforces it**,
so a pack that needs a newer engine fails up front with a clear message
instead of `attempt to call a nil value (field 'raycast')` mid-game.

**Syntax** — a small Cargo-style subset, parsed by one function in `vb_core`
(`vb::core::VersionReq`, shared by the engine and `vb`):

| Form | Meaning |
| --- | --- |
| `*` | any version |
| `>=0.5.0`, `>0.5.0`, `<=0.7`, `<0.7`, `=0.5.2` | comparator; missing minor/patch = 0 |
| `>=0.5.0, <0.7.0` | comma = AND |
| `^0.5.1` | `>=0.5.1, <0.6.0` (pre-1.0: minor is the breaking digit); `^1.2.3` = `>=1.2.3, <2.0.0` |
| `~0.5.1` | `>=0.5.1, <0.6.0` |

No OR, no pre-release identifiers. An unparseable value is an **error**
(fail closed), never treated as `*`. A missing field is treated as `*` with a
warning (existing packs keep loading); `--check-pack --strict` makes it an
error.

**Which version is "the engine's"** — `kVersionNumeric` (the newest
reachable tag, `cmake/version.hpp.in`). A dev build some commits past `v0.5.0`
counts as `0.5.0`; a build outside git counts as `0.0.0`. So an engine
developer whose pack needs an untagged feature uses the dev-only override
below rather than a special rule for dev builds.

**Where it's checked**

1. **Server, at pack load** (`load_content_pack`, before any Lua runs):
   refuse to start, like the `auth.lua` fail-closed path:
   `error: pack 'my_pack' requires engine >=0.6.0 (pack.toml engine_version_req); this server is 0.5.2` + `hint: vb install 0.6.0` when run
   through `vb`.
2. **Singleplayer** loads the pack through the same path, so it gets the
   same check and shows the message in the main menu instead of crashing.
3. **Client, on connect.** The pack's `ui/*.lua` runs on the *client's*
   engine, and two releases can share a protocol version while having
   different UI APIs. The server sends the requirement in the handshake
   (new field on the existing server-info/handshake reply — a wire change:
   protocol bump + `docs/protocol.md` + round-trip/fuzz test, per the
   cross-cutting rules) and the client disconnects itself with
   "this server's pack needs Voxel Browser >=0.6.0; you have 0.5.2 — run
   `vb update`" before downloading assets.
4. **`--check-pack`** reports a mismatch as an error diagnostic on
   `pack.toml`'s line; **`vb`** uses the same `VersionReq` to choose a
   version (§3.5) and to warn in `vb server start` before spawning.

**Override** — `--ignore-engine-req` on the server (and for singleplayer on
the client), dev-only and compiled out under `VB_DISTRIBUTION`, exactly
like `--insecure-skip-auth`; logs a warning.

**Bundled packs** (`content/base`, `content/examples/*`) ship with the engine
and keep `"*"`; `vb pack init --template base` rewrites the copy's field to
`">=<selected version>"`. The pack.toml comment and `content-pack-format.md`
are updated to say the field is enforced.

**Later, optional:** with `---@vb since <version>` on every stub (§3.1),
`--check-pack` can warn when a pack calls an API newer than the lower bound
of its own `engine_version_req` (`vb.world.raycast` is since 0.6.0, but the
pack says `>=0.5.0`). Needs the static check to follow constant field chains
(`vb.world.raycast`), not just the `vb` global; tracked in 10.D as a stretch.

## 4. Testing strategy

| What | How |
| --- | --- |
| Stubs ⇆ runtime | `lua_api_surface_test.cpp` (§3.2) |
| Docs up to date | `gen_lua_docs.py --check`, `vb help --markdown` diff, in `lint.yml` |
| Doc examples compile | doctest loading every extracted example with `luaL_loadbuffer` |
| Stubs are valid LuaCATS | CI runs `lua-language-server --check sdk/lua content/base` (pinned release download); zero warnings in `sdk/`, warnings in packs reported but not gating until §5 phase F |
| Templates work | integration test: `vb pack init` each template into a temp dir, then `voxel_browser_server --check-pack` it |
| `--check-pack` | unit/integration cases: clean pack, syntax error (file:line), runtime error in `init.lua`, bad `register_block` def, bad `ui/*.lua`, bad structure, `--json` shape |
| Global-access check | `vb` read in `ui/` → error with line; `ui`/`client` in `init.lua` → error; `os.time` → error; `base_ui` shared across `ui/` files → clean, same global read in `init.lua` → error; unknown global → warning, error under `--strict`; `content/base` and `kitchen_sink` → clean |
| `engine_version_req` | `VersionReq` parse/match table (every form above, bad syntax, missing field); server refuses a too-new pack and starts with `--ignore-engine-req`; client disconnects with the message on a mismatched handshake field; handshake round-trip + fuzz test |
| `vb pack` / `vb help` | CLI tests in the existing `vb` test style (`VB_HOME` temp root, no network) |

## 5. Phased plan

Order follows dependencies (stubs before scaffolding, which copies them;
`--check-pack` before templates, which CI validates with it). The agent-docs
phase (F) depends only on the command table and can run in parallel with B–D.

### 10.A — API inventory + drift test (S–M)

- [x] Surface walker test (§3.2) that dumps every bound name; commit the first
      dump as the to-document checklist.
- [x] `sdk/lua/` skeleton, `api_index.txt`, `api_ignore.txt`; the test runs
      in "report only" mode until 10.B finishes, then gates.
- **Exit:** the list of everything that needs a stub exists and is enforced.

### 10.B — LuaCATS stubs (L)

- [x] Stubs for the server VM (`vb.*`, sub-tables, usertypes, definition
      classes, `vb.on` overloads per event), the UI VM (`ui`, `client`, widget
      tree, `state`), data scripts and `auth.lua`'s returned table.
- [x] One example per function; context tags plus an environment line at the
      start of every engine global's description (§3.4.1); `sandbox.lua`.
- [x] Try the multi-root / nested `ui/.luarc.json` setup in VS Code and
      Neovim; keep it for 10.E only where it gives correct warnings.
- [x] `luarc.template.json`; `content/base` and `kitchen_sink` get a
      `.luarc.json` so the repo dogfoods it.
- [x] CI: LuaLS `--check` on `sdk/lua`; drift test gates.
- **Exit:** opening `content/base` in VS Code with LuaLS gives completion and
  hover docs for every `vb.*` call and no false "undefined global" warnings.

### 10.C — Generated quick reference (M)

- [x] `scripts/gen_lua_docs.py` → `docs/lua-reference/` + cheat sheet + `--check`.
- [x] Example extraction + syntax-check doctest.
- [x] `docs/lua-api.md` header → reference; README links the cheat sheet.
- **Exit:** every bound function has a reference entry with an example, and
  CI fails if a stub change isn't regenerated.

### 10.D — Headless pack validation + engine version enforcement (M–L)

- [x] `voxel_browser_server --check-pack <dir> [--json] [--strict]` (§3.4),
      including UI VM compile, structure validation and one-chunk worldgen.
- [x] Static global-access check per environment (§3.4.1).
- [x] Diagnostics carry `file:line` (parse Lua error prefixes; registration
      errors report the calling chunk via `debug.traceback` level).
- [x] `vb pack check` + version resolution from `engine_version_req`.
- [x] Enforce `engine_version_req` (§3.7): `vb::core::VersionReq` in `vb_core`;
      server/singleplayer refuse a mismatched pack; handshake carries the
      requirement and the client checks it (protocol bump, `docs/protocol.md`,
      round-trip + fuzz test); `--ignore-engine-req` (dev-only);
      `vb server start` warns; update `content/base/pack.toml`'s comment and
      `content-pack-format.md`.
- [x] Stretch: warn when a pack uses an API whose `since` is newer than its
      requirement's lower bound (§3.7, "Later").
- **Exit:** a broken pack, or one that needs a newer engine, yields a
  precise, machine-readable error without starting a game.

### 10.E — Scaffolding: `vb pack init` (M–L)

- [x] `templates/pack/{minimal,ui,worldgen}` + build-time embedding into `vb`.
- [x] `vb pack init`/`vb pack types`/`vb pack info`; `--template base`;
      `--engine-req` (default `>=<selected version>`).
- [x] Generated `README.md`, `AGENTS.md`, `.luarc.json`, `.gitignore` (+ the
      `ui/` multi-root setup if 10.B kept it).
- [x] `vb pack dev` (host `--watch` + client connect; `--no-client`).
- [x] Integration test: new → check for every template.
- **Exit:** `vb pack init && vb pack dev` puts a player into a world running the
  new pack in under a minute, with editor completion working.

### 10.F — CLI reference & agent docs (M)

- [x] Extend `Command` (details, examples, flags, json); split subcommand
      tables; `vb help <cmd>`, `--help` everywhere.
- [x] `vb help --markdown` → `docs/cli.md` (+ CI check); `vb help --json`.
- [x] Machine contract audit: `error:`/`hint:` format, `--json` on
      `install`/`doctor`/`pack *`, `--yes` on every prompt.
- [x] Root `AGENTS.md`; `docs/llms.txt`.
- **Exit:** an agent given only `vb help --json` (or `docs/cli.md`) can
  install a version, scaffold, check and host a pack without guessing flags.

### 10.G — Ship it (S)

- [x] `package_release.py`: include `sdk/` and `docs/` in the game archive.
- [x] `vb docs [topic] [--path]`.
- [x] README: "Make your first pack" section (`vb pack init` → `vb pack dev`),
      `CONTRIBUTING.md`: how to add a binding (C++ + stub + regenerate).
- **Exit:** everything above works from a fresh `install.sh` with no repo
  checkout and no network after install.

## 6. Open questions

1. ~~**Per-directory globals.**~~ Decided 2026-10-06: labelled stubs +
   static check in `vb pack check`; nested editor config only if it works
   (§3.4.1).
2. ~~**`vb init` vs `vb pack init`.**~~ Decided 2026-10-06: `vb pack init`
   only (§3.5).
3. ~~**Should the engine enforce `engine_version_req`?**~~ Decided
   2026-10-06: yes — server, singleplayer and client (§3.7).
4. **Example execution.** Syntax-checking is cheap; actually running examples
   needs a fixture world per example. Revisit after 10.C if doc examples rot.

## 7. As built (2026-10-06)

Deviations and decisions made while implementing, in the order a reader will hit them:

- **Surface walker** (`vb::script::describe_lua_surface`, `PackRuntime/UiRuntime::describe_api`): walks
  `vb`/`ui`/`client` plus sol2 usertype metatables from the Lua registry. `Player` is the Lua name of the C++
  `PlayerHandle`; `Entity` (the `self` of `vb.register_entity` callbacks) is not reachable from globals, so
  `describe_api` lists its shared method table explicitly. A table with a metatable (`vb.storage`) counts as a
  member (stub tag `---@vb member`).
- **Stub parser** is a small line-based parser in `scripts/gen_lua_docs.py`, not a LuaCATS implementation:
  `function A.b.c(args) end` / `function T:m(args) end` declarations preceded by `---` blocks. The generator
  fails on a missing summary, example, `---@vb context` or a `---@param` that does not match the arguments.
  Doc examples are syntax-checked by extracting the fenced blocks from the generated markdown (no
  `build/doc_examples/`).
- **LuaLS in CI** (since 2026-10-18): `scripts/luals_check.sh` downloads a pinned release (3.18.2) and
  runs `--check` on `sdk/lua`, `content/base`, `content/examples/kitchen_sink` and the scaffolded
  templates; any warning fails the `luals` lint job (packs gate too, not just `sdk/`). Its first run found
  stub signatures that disagreed with the engine (block `on_break`/`on_place` ctx, entity `on_hit`
  arguments, missing `PlayerInput.dt`) -- the drift test only compares names.
- **`--check-pack` warnings** (since 2026-10-18): the server-side load runs with `vb.register_*` and
  `vb.worldgen.set_pipeline` wrapped, recording each definition table's keys, block textures and the
  calling `file:line` (`debug.traceback`; read back with `PackRuntime::eval_string`). Keys are compared
  with the stub `---@class` fields and `since` versions with `engine_version_req`'s lower bound, both
  from `src/server/check_pack_data.inc`, which `gen_lua_docs.py` generates (and `--check` verifies). The
  `since` check scans code (comments and strings skipped) for `vb.`/`ui.`/`client.` field chains;
  `Player:`/`Entity:` methods are not covered (the receiver type is not known statically).
- **Nested `ui/.luarc.json`** (since 2026-10-18): lists only `ui.lua` + `sandbox.lua`, so with `ui/` added
  as its own workspace folder (multi-root) LuaLS flags `vb` there; `vb pack init/types` write it when the
  pack has a `ui/` folder, and CI checks each pack's `ui/` with it. No generated `.code-workspace`: the
  template README says to add the folder.
- **`--check-pack`** lives in `src/server/check_pack.cpp` (library `vb_pack_check`, linked by the server and the
  tests). Order: `pack.toml` (+ `engine_version_req`) -> compile every file and scan its bytecode for global
  accesses -> `auth.lua` -> load the server files one by one (stops at the first failing file) -> `freeze()`,
  `validate_worldgen()`, one generated chunk -> `ui/*.lua` in a `UiRuntime`. The bytecode scan is
  `src/script/lua_global_scan.c` (needs Lua's internal headers, so it is C, `-w`). Globals defined at run time
  (`_G[name] = ...`, as `content/base/blocks/register.lua` does) are learned from the VM after the load
  (`global_names()`), so only reads that nothing defines warn. Pack `print` is silenced so `--json` stays clean.
  Structure files are validated through `parse_structure` (registration) and `validate_worldgen`; the editor's
  own validator is not run. "Block without a texture" and "unknown field in a def table" warnings are not
  implemented.
- **`engine_version_req` enforcement**: `vb::core::VersionReq` (`inc/vb/core/version_req.hpp`); bare versions
  (no comparator) are rejected. Server: refuses at startup (`--ignore-engine-req`, compiled out under
  `VB_DISTRIBUTION`). Singleplayer: falls back to the hardcoded base set with a message on stderr (the main
  menu does not show it yet). Client: `S2CServerInfo.engine_version_req` (protocol 30), checked in the
  handshake before any asset is downloaded; no client-side override. `vb pack check/dev` pick the newest
  installed version that satisfies the requirement.
- **Templates**: `minimal` (init + block + texture), `ui` (adds `ui/hello.lua` opened from chat) and
  `worldgen` (adds `worldgen.lua`) instead of the plan's single `minimal` that already had a UI. They live in
  `templates/pack/<name>/` over `_common/` and are baked into `vb` by `cmake/EmbedFiles.cmake`; the Lua stubs
  are embedded the same way, so `pack init`/`types` work with no installed version (an installed version's
  `sdk/` wins when present). `--template base` copies the installed `content/base`.
- **Asset manifest**: dot-files/dot-directories (`.vb/`, `.luarc.json`), the pack-root `README.md`/`AGENTS.md`
  and the server's runtime state (`storage.json`, `db/`) are never advertised to clients; the pack loader skips
  dot-directories.
- **CLI**: help text lives in `src/cli/help.cpp` beside (not inside) the command table; `docs/cli.md` is
  `vb help --markdown`, checked by `dev_cli_help_test`. `vb help --json` exports the table. `error:`/`hint:`
  replaced the old `vb:` prefix. `--json` added to `install`, `update`, `doctor`, `pack *`. Nothing prompts;
  the only confirmation-style guard (`server rm`) already needed `--yes`.
- **Release archive** gains `sdk/` and `docs/*.md|txt`; `vb docs [topic] [--path]` reads them from the selected
  version (`vb docs vb.world.raycast` prints one reference section).
