# Developer CLI (`vb`) — Design & Phased Plan

> Full detail for this topic; the backlog entry is `REMAINING_TASKS.md`
> Phase 8. Status: **8.1–8.3 implemented** (2026-10-04); 8.4+ not started. 8.1's CI wiring is unexercised until the next workflow run / tag.

## 1. Goals / non-goals

**Goals**

1. **Manage installed copies of Voxel Browser** — download a release, verify
   it, keep several versions side by side, pick a default, update, uninstall.
   Everything lives in the **current user's profile**: no admin/root, no
   system directories, no registry writes.
2. **Host a server easily** — one command for "just run a server here"
   (`vb host`), plus named, persistent server *instances* that can run in
   the background and be started, stopped, inspected and tailed.
3. **Developer loop** — point the CLI at a local `build/` directory and a
   local content pack so that pack/engine authors use the same commands
   against their own work-in-progress as players use against releases.

**Non-goals (for now)**

- A system-wide installer, OS package (`.deb`/`.msi`/brew), or OS service
  (systemd unit / Windows Service). `vb` can *print* a unit file later
  (8.6) but never installs one with elevated rights.
- A GUI launcher. The client's own main menu stays the player-facing UI.
- Remote server administration (RCON). Out of scope until the engine has an
  admin protocol; `vb` only manages processes on the local machine.
- Server browser / master list (already `Deferred` in `REMAINING_TASKS.md`).

## 2. What exists today (constraints the design has to respect)

| Fact | Where | Consequence for `vb` |
| --- | --- | --- |
| Two executables, `voxel_browser` + `voxel_browser_server` | `src/client`, `src/server` | `vb` is a third executable; it launches these, never links their `main`s. |
| `vb_core` already has `Args` (CLI parsing), TOML config (`tomlplusplus`), `sha256`, `user_cache_dir()`, `describe_build()` | `inc/vb/core/*` | `vb` links `vb_core` only (no render, no Lua); reuse instead of re-implementing. |
| Server resolves `content_pack` and `world_dir` **relative to its CWD**; config path defaults to `./server.toml` | `src/server/main.cpp`, `server.toml.example` | `vb` runs every server with CWD = its instance directory and passes an **absolute** `--content-pack`. No engine change needed to host. |
| Server stops cleanly on `SIGINT`/`SIGTERM` (`g_stop`) and already has `--ticks <n>` | `src/server/main.cpp` | POSIX stop works today. Windows has no SIGTERM for another process → needs a small engine addition (§6.3). `--ticks` gives cheap integration tests. |
| Client's `--singleplayer` uses a **CWD-relative** `content/base` and `world_singleplayer/`; `client.toml` defaults to `./client.toml` | `src/client/main.cpp` (`kSingleplayerContentPack`, `kSingleplayerWorldDir`) | Launching the client from an install dir would write the player's world *inside the versioned install*, where an uninstall/prune deletes it. Needs an engine change (§6.2). |
| Asset cache already per-user: `%LOCALAPPDATA%\voxel_browser\cache`, `~/Library/Caches/voxel_browser`, `$XDG_CACHE_HOME/voxel_browser` | `src/core/paths.cpp` | `vb`'s roots sit next to it under the same `voxel_browser` name. |
| CI stages `voxel_browser`, `voxel_browser_server`, `server.toml.example`, `content/` per platform/build type, then `bundle.yml` **merges** all `voxel_browser-*` artifacts into one flat artifact and `publish.yml` uploads `voxel_browser*` to the GitHub Release | `.github/workflows/*` | Same-named files from linux/macos/windows × release/debug land at the same paths in one flat artifact — there is no well-defined per-platform download. **Fixing the release layout is step 8.1, the prerequisite for everything else.** |
| Version is the git tag (`v0.0.1` exists), `describe_build()` prints it | `CMakeLists.txt`, `cmake/version.hpp.in` | Versions are tags: `v<major>.<minor>.<patch>`. |

## 3. Key decisions

### 3.1 Implementation language: C++, in this repo, linking `vb_core`

`vb` is `src/cli/` → target `vb_cli`, output name `vb` (`vb.exe`). Reasons:
one toolchain and one CI matrix (already cross-platform); free reuse of
`Args`, TOML, SHA-256, paths and build-info; the CLI can share *types* with
the engine (e.g. parse a `server.toml` with the real `ServerConfig` loader to
validate it, rather than a second schema that drifts).

The two capabilities `vb_core` lacks, and the chosen dependencies:

| Need | Choice | Notes |
| --- | --- | --- |
| HTTPS download | **libcurl** | Windows: `FetchContent` curl built with `CURL_USE_SCHANNEL=ON` (OS TLS, no OpenSSL). macOS: the SDK's system `libcurl`. Linux: system `libcurl` (`libcurl4-openssl-dev` in CI; `libcurl.so.4` is present on every mainstream distro). Gated behind `VB_BUILD_CLI` so engine-only builds don't need it. |
| Archive extraction | **miniz** (single-file, zip only) | Release archives are `.zip` on **all** platforms so there is exactly one extractor. Executable bits are set explicitly for the known binaries after extraction (zip external attrs are not trusted). |
| Machine-readable release metadata | **TOML** (existing `tomlplusplus`) | No JSON library needed for anything in 8.1–8.5 (see §4.2). |

Alternative considered: a Go/Rust standalone CLI (trivial static binaries,
built-in HTTP+zip). Rejected for now: a second toolchain in CI, and it would
duplicate config parsing and version logic that `vb_core` already owns.

### 3.2 Name: `vb`

Short, matches the `vb::` namespace and `vb_*` targets. If it ever collides
on a user's PATH the binary can be renamed without touching anything else
(no code depends on argv[0]).

### 3.3 Integrity and trust

- Every release publishes `release.toml` listing each archive's filename,
  size and **SHA-256**. `vb` refuses to extract anything whose hash doesn't
  match (hashing via the existing `vb::core::sha256`).
- Transport trust = HTTPS to `github.com`, plus a **signed manifest**: each
  release carries `release.toml.sig`, a base64 Ed25519 signature over the exact
  bytes of `release.toml` (see "Release signing" below). The manifest names every
  archive's SHA-256, so authenticating it authenticates the whole release.
- Zip extraction rejects absolute paths, `..` components and symlinks
  (zip-slip).

## 4. Distribution format (release side)

### 4.1 Archives

One zip per platform × build type, uploaded as individual release assets:

```
voxel_browser-v0.2.0-linux-x86_64.zip
voxel_browser-v0.2.0-linux-x86_64-debug.zip
voxel_browser-v0.2.0-macos-universal.zip
voxel_browser-v0.2.0-windows-x86_64.zip
vb-v0.2.0-linux-x86_64.zip      # just the CLI, for bootstrapping (§7)
...
release.toml
```

Archive contents (same layout every platform, `.exe` on Windows):

```
voxel_browser            # client
voxel_browser_server     # server
vb                       # the CLI itself (so any install can self-bootstrap)
server.toml.example
client.toml.example
content/base/...         # (storage.json / db stripped, as CI does today)
BUILD_INFO.toml          # version, commit, date, build_type, platform
```

### 4.2 `release.toml`

```toml
schema  = 1
version = "v0.2.0"
commit  = "a723d89…"
date    = "2026-10-04T12:00:00Z"
engine_protocol_version = 7          # lets `vb` warn on client/server mismatch

[[artifact]]
kind      = "game"                   # "game" | "cli"
platform  = "linux-x86_64"           # "<os>-<arch>"; macOS = "macos-universal"
build     = "release"                # "release" | "debug"
file      = "voxel_browser-v0.2.0-linux-x86_64.zip"
size      = 18734512
sha256    = "…"
# the signature is a separate file, release.toml.sig (see "Release signing")
```

Resolution without the GitHub API (no JSON, no rate limit):

- latest: `https://github.com/<repo>/releases/latest/download/release.toml`
- pinned: `https://github.com/<repo>/releases/download/<tag>/release.toml`
- archive: same prefix + `file`.

Only `vb list --remote` (enumerate all tags) needs the REST API; that's the
one place a JSON parser is added (8.5, `nlohmann_json` via FetchContent).

The repository is configurable (`source` in `cli.toml`, `VB_SOURCE` env) so
forks and mirrors work, and a `dir:` source (`dir:/path/to/folder` holding
`release.toml` + zips) is supported from day one — it's what the tests use,
and what an offline/LAN mirror uses.

## 5. Local layout (user-scoped)

### 5.1 Roots

New helpers in `inc/vb/core/paths.hpp`, alongside the existing
`user_cache_dir()`, so client/server/CLI agree on locations:

| Helper | Windows | macOS | Linux / other |
| --- | --- | --- | --- |
| `user_data_dir()` | `%LOCALAPPDATA%\voxel_browser` | `~/Library/Application Support/voxel_browser` | `$XDG_DATA_HOME/voxel_browser` or `~/.local/share/voxel_browser` |
| `user_config_dir()` | `%APPDATA%\voxel_browser` | `~/Library/Application Support/voxel_browser/config` | `$XDG_CONFIG_HOME/voxel_browser` or `~/.config/voxel_browser` |
| `user_cache_dir()` (exists) | `%LOCALAPPDATA%\voxel_browser\cache` | `~/Library/Caches/voxel_browser` | `$XDG_CACHE_HOME/voxel_browser` or `~/.cache/voxel_browser` |

`VB_HOME=<dir>` overrides all three (data = `<dir>`, config = `<dir>/config`,
cache = `<dir>/cache`) — for portable installs, CI, and tests.

### 5.2 Tree

```
<data>/
├── versions/
│   ├── v0.1.0/                 # an extracted archive, never modified after install
│   │   ├── voxel_browser  voxel_browser_server  vb  content/  …
│   │   └── .install.toml       # receipt: source, sha256, installed_at, build
│   └── v0.2.0/
├── links/
│   └── dev.toml                # `vb link dev ./build` → path = "/home/me/voxel_browser/build"
├── servers/
│   └── <name>/                 # a server instance (§6)
│       ├── instance.toml       # version pin, pack, autostart flags
│       ├── server.toml         # the real engine config, user-editable
│       ├── world/              # server's world_dir (relative, CWD = here); blocks.txt records its block ids
│       ├── worlds/<pack>-<hash>/ # `vb host --pack <other>` / `vb pack dev`: one world per other pack
│       ├── logs/server.log     # + rotated server.log.1 …
│       └── run/                # pid, stop sentinel (§6.3) — exists only while running
├── worlds/singleplayer/        # client's --singleplayer world (§6.2), outside versions/
├── worlds/singleplayer-<pack>-<hash>/ # `vb launch -- --content-pack <dir>`: that pack's own world
├── downloads/                  # *.part files; resumable, cleaned on success
├── bin/                        # optional shims (8.5)
└── lock                        # advisory lock for mutating commands
<config>/
├── cli.toml                    # default_version, source, default build type
└── client.toml                 # the client's settings (launched with --config this)
```

`current` default version is stored as `default_version = "v0.2.0"` in
`cli.toml` rather than a `current` symlink — symlinks need Developer Mode or
admin on Windows.

### 5.3 Install transaction

1. Take `<data>/lock` (exclusive; a second `vb install` waits or fails fast
   with `--no-wait`).
2. Fetch `release.toml`, select the artifact for this platform/build.
3. Download to `downloads/<file>.part` (HTTP `Range` resume if present),
   verify size + SHA-256, rename to `downloads/<file>`.
4. Extract into `versions/.staging-<version>-<random>/`, validate (expected
   binaries present, `vb … --version` output parses), write `.install.toml`.
5. Atomic `rename` → `versions/<version>/`. Delete the zip unless `--keep-download`.
6. On any failure: remove the staging dir; `versions/` is never half-written.

Uninstall/prune refuse to delete a version that a **running** server instance
uses, or that an instance pins (unless `--force`, which also tells you which
instances will break).

## 6. Hosting

### 6.1 Two levels

- **`vb host`** — zero-setup, foreground. Uses the default version, an
  instance named `default` (created on first use), port 27015. Ctrl+C stops
  it. Flags pass through: `vb host --port 27020 --pack ./my_pack --seed 42`.
  `vb host --pack ./my_pack` is the content-author loop: the pack directory
  is used *in place* (absolute path passed as `--content-pack`), nothing is
  copied.
- **`vb server …`** — named, persistent instances that can be detached.

`instance.toml`:

```toml
version = "v0.2.0"        # or "default" (follows `vb use`), or a link name like "dev"
pack    = "builtin:base"  # "builtin:<name>" → <version>/content/<name>; or an absolute path
```

Launching an instance = `cd <instance>` and exec
`<version>/voxel_browser_server --config server.toml --content-pack <abs pack>`,
stdout/stderr → `logs/server.log` when detached. Because the server already
resolves `world_dir` against its CWD, each instance's world is naturally
isolated in its own directory.

Before launch `vb` validates `server.toml` with the real
`vb::core::load_server_config()` and checks the UDP port is free (bind test),
so typos and port clashes fail with a clear message instead of a log line in
a detached process.

### 6.2 Engine change: client paths stop depending on CWD

Small, self-contained change to `src/client/main.cpp`:

- `--content-pack <dir>` for `--singleplayer` (default unchanged when
  `content/base` exists relative to CWD; otherwise fall back to
  `<exe_dir>/content/base`).
- `--world-dir <dir>` for `--singleplayer`. `vb launch` passes
  `<data>/worlds/singleplayer` so worlds survive version changes.
- `vb launch` always passes `--config <config>/client.toml`, so settings
  (keybinds, recent servers) are shared across installed versions.

Running the client directly from a build dir keeps today's behavior exactly.

### 6.3 Engine change: portable graceful stop

POSIX: `vb server stop` sends `SIGTERM` (already handled). Windows: a detached
process has no console we can send Ctrl+C to, and `TerminateProcess` would
skip the world save. Add `--stop-file <path>` to the server: the main loop
checks for the file's existence once per second (cheap `stat`) and sets
`g_stop`. `vb` uses it on every OS (signal as a backup on POSIX), then waits
up to `--timeout` (default 30 s) for exit before offering `--kill`.

### 6.4 Process tracking

`run/server.pid` holds `pid` + a platform start token (process start time) +
the resolved version. A pid is "ours" only if the token still matches (guards
against pid reuse after a reboot). Stale `run/` files are cleaned on the next
`status`/`start`/`stop`.

### 6.5 Status (later)

8.5 adds `--status-file <path>` to the server: rewritten every few
seconds with uptime, tick rate achieved, player count/names, world seed.
`vb server status` reads it; until then status = "running (pid N, up X)"
from the pid file plus the last log lines.

## 7. Command surface

```
vb --version | --help

# versions
vb install [<version>|latest] [--build debug] [--force] [--keep-download]
vb uninstall <version> [--force]
vb list [--remote]                   # installed (default marked *), or available
vb use <version>                     # set default_version
vb update                            # install latest; `use` it if default was latest
vb prune [--keep N]                  # remove old unpinned, unused versions
vb link <name> <build-dir>           # register a local build as a pseudo-version
vb unlink <name>
vb which [client|server] [--version v]
vb self update [--check] [--force]    # update vb itself
vb shim install|remove                # voxel_browser / voxel_browser_server launchers in <data>/bin
vb completions bash|zsh|fish|powershell

# run
vb launch [--version v] [-- client args…]        # start the client
vb host [--version v] [--port n] [--pack dir] [--watch] [-- server args…]

# instances
vb server new <name> [--version v] [--pack dir|builtin:base] [--port n]
vb server list
vb server start <name> [--foreground]
vb server stop <name> [--timeout s] [--kill]
vb server restart <name>
vb server status [<name>]
vb server logs <name> [-f] [-n lines]
vb server config <name> [get [key] | set key value | unset key | edit]
vb server config <name> [get <key> | set <key> <value> | edit]
vb server rm <name> [--keep-world]

# maintenance
vb doctor                            # paths, permissions, versions, port checks, stale pids
vb paths                             # print data/config/cache roots
vb self update                       # 8.5
```

Conventions: exit code `0` success, `1` operational failure, `2` usage
error; human output by default, `--json` on `list`, `status`, `paths`,
`which` (8.5) for scripting; `--quiet`/`--verbose`; progress bars only
when stdout is a TTY. `Args` in `vb_core` handles flags; a thin subcommand
dispatcher (`std::span<std::string>` → handler) is added in `src/cli/`.

## 8. Bootstrapping `vb` itself

- `scripts/install.sh` (Linux/macOS) and `scripts/install.ps1` (Windows):
  download `vb-<latest>-<platform>.zip`, verify against `release.toml`, place
  `vb` in `<data>/bin/`, add that dir to the user's PATH (shell rc file /
  user-level `Path` env var — never system-wide), then run `vb install latest`.
- Every game archive also contains `vb`, so an unzipped release can manage
  itself: `./vb install` from inside it works.
- `vb self update` (8.5) replaces its own binary: write `vb.new`, then
  rename over (POSIX) or rename the running `vb.exe` → `vb.exe.old` first
  and delete it on the next run (Windows).

## 9. Code layout

```
inc/vb/cli/            # only what tests need to see
  version.hpp          # Version parse/compare ("v1.2.3", "latest", link names)
  layout.hpp           # Layout{data,config,cache} + versions/servers/... accessors
  release_manifest.hpp # parse/select from release.toml
  installer.hpp        # the §5.3 transaction over a Source interface
  source.hpp           # Source: HttpSource (libcurl), DirSource (dir:)
  instance.hpp         # instance.toml, launch command construction, pid files
  process.hpp          # spawn/detach/signal/is_alive — POSIX + Win32 impls
src/cli/
  main.cpp  commands_*.cpp  (above .cpp files)  process_posix.cpp  process_win32.cpp
```

`vb_cli_lib` (STATIC, links `vb_core`, curl, miniz) + `vb` (EXE). `vb_tests`
links `vb_cli_lib` so the logic is unit-tested without spawning the binary.

New CMake option: `VB_BUILD_CLI` (default `ON`).

## 10. Testing strategy

- **Unit** (`tests/unit/cli_*_test.cpp`): version parse/sort; `release.toml`
  parse + platform selection; layout resolution incl. `VB_HOME` and XDG
  overrides; zip-slip rejection; install transaction against a `DirSource`
  built in a temp dir (good archive, bad hash, truncated zip, crash
  mid-extract leaves no `versions/<v>`); instance launch-command
  construction; pid-file staleness logic.
- **Integration** (ctest, `VB_BUILD_CLI` + `VB_BUILD_SERVER`): with
  `VB_HOME=<tmp>` and a `dir:` source built from *this* build's own
  binaries: `vb install`, `vb host -- --ticks 40`, `vb server new/start/stop`
  (stop must produce a saved `world/`), `vb uninstall`.
- **No network in tests.** `HttpSource` is covered by one opt-in test
  (`VB_TEST_NETWORK=1`) that fetches the real latest `release.toml`.
- CI runs all of the above on Linux, macOS, Windows (process/stop handling
  is the most platform-specific code — it must be exercised on all three).

## 11. Phased plan

Steps 8.1–8.6 match `REMAINING_TASKS.md` Phase 8. Each step is independently shippable and leaves `main` green. Sizes are
rough (S ≈ ≤1 day, M ≈ 2–3 days, L ≈ a week) for one contributor.

### 8.1 — Release pipeline produces installable artifacts (M) — *prerequisite*

Nothing can be downloaded reliably until this lands.

- [x] Per-platform staging produces `voxel_browser-<ver>-<os>-<arch>[-debug].zip`
      with the §4.1 layout (+ `client.toml.example`, `BUILD_INFO.toml`).
- [x] Replace `bundle.yml`'s flat merge with per-file release assets (or
      `merge` with `separate-directories: true` and upload each zip).
- [x] New script `scripts/make_release_manifest.py` (stdlib only) → `release.toml`
      with sizes + SHA-256; `publish.yml` uploads it alongside the zips.
- [x] Release-type builds only in the release (asan/tsan legs never staged —
      already the case on Linux; keep it so).
- Implemented by `scripts/package_release.py` (per-platform zips, also a CLI-only `vb-<ver>-<platform>.zip`), `scripts/make_release_manifest.py`, and the `bundle.yml` hash verification. Zips are named from the latest tag (`setup_metadata.version`); `BUILD_INFO.toml` records the exact `git describe`.
- **Exit:** tagging `v0.1.0` yields a release with ≥3 zips + a
  `release.toml` whose hashes match (`sha256sum -c` in the workflow).

### 8.2 — `vb` skeleton + local version management (M)

- [x] `paths.hpp`: `user_data_dir()`, `user_config_dir()`, `VB_HOME` override
      (+ tests in `core_test.cpp`).
- [x] `VB_BUILD_CLI`, `src/cli/`, subcommand dispatcher, `--help`/`--version`,
      exit-code conventions.
- [x] `Version`, `Layout`, `cli.toml` read/write. (The `lock` file moves to 8.3: nothing mutates shared state concurrently until `install` exists.)
- [x] `vb paths`, `vb list` (installed), `vb use`, `vb which`, `vb uninstall`,
      `vb link`/`unlink` (the dev loop works before any downloading exists:
      `vb link dev ./build && vb use dev`).
- [x] `vb launch` (client with `--config <config>/client.toml`).
- **Exit:** a developer can `vb link dev build/ && vb launch` and get the
  client; `vb uninstall` of a linked version only removes the link.

### 8.3 — Download & install (L)

- [x] Dependencies: curl (Schannel on Windows / system elsewhere), miniz,
      behind `VB_BUILD_CLI`; CI dependency installs updated.
- [x] `Source` interface; `DirSource`, `HttpSource` (redirects, `Range`
      resume, timeouts, proxy from env, progress callback).
- [x] `release.toml` parser + platform selection (`linux-x86_64`,
      `macos-universal`, `windows-x86_64`; clear error on unsupported).
- [x] Safe zip extraction + exec bits; §5.3 transaction.
- [x] `vb install`, `vb update`, `vb prune`, `vb doctor` (first cut).
- [x] Unit + integration tests per §10 (DirSource only in CI).
- Notes: sources are `owner/repo`, `owner/repo@<mirror-base-url>` or `dir:/path` (`VB_SOURCE` env or `source` in cli.toml). The `lock` file (flock/LockFileEx) landed here. Verified on Linux, including resume of a partial download against a Range-capable local HTTP server; the Windows (curl/Schannel, Win32 lock) and macOS paths are compiled by CI only. `HttpSource` has no automated test yet (needs `VB_TEST_NETWORK`).
- **Exit:** on a clean machine (`VB_HOME` empty), `vb install latest && vb launch`
  works on all three OSes; corrupt download is rejected with no partial install.

### 8.4 — Hosting (L)

- [x] Engine: server `--stop-file` (§6.3), with an integration test that a
      detached server stops cleanly on the file alone (no signal) and runs its
      shutdown/save path.
- [x] Engine: client `--content-pack` / `--world-dir` for `--singleplayer`
      and exe-relative fallback (§6.2); `vb launch` passes
      `<data>/worlds/singleplayer` and the version's own `content/base`.
- [x] `process.hpp` POSIX + Win32: spawn foreground, spawn detached with log
      redirect, graceful stop, liveness with pid-reuse guard.
- [x] `instance.toml`; `vb server new/list/start/stop/restart/status/logs/rm`
      (`-f` follow), `vb host`.
- [x] Pre-launch checks: config validation via `load_server_config`, UDP
      port free, pinned version installed (suggests `vb install <v>`).
- [x] Uninstall/prune respect pinned + running instances (`--force` on
      `uninstall`/`unlink` overrides).
- [x] Integration tests per §10 (Linux verified locally).
- Notes / deviations from the design above:
  - The run record (`run/server.pid`) stores pid + **start token** + start
    time + resolved version, not the executable path: the start token (Linux
    `/proc/<pid>/stat` starttime, macOS `kp_proc.p_starttime`, Windows process
    creation time) is what actually defeats pid reuse.
  - `vb server stop` writes the stop file *and* sends SIGTERM on POSIX, so a
    server built before `--stop-file` existed still stops gracefully there; on
    Windows such an old server only stops via `--kill`.
  - `vb` forwards SIGINT/SIGTERM/SIGHUP to a foreground server while it waits,
    so `kill <vb>` or a closing terminal still saves the world.
  - Detached servers are started with a double fork + `setsid`; a per-instance
    `run/start.lock` serialises concurrent `start`s.
  - `builtin:<name>` packs resolve to `<version>/content/<name>`, then
    `<version>/../content/<name>` so a linked in-tree `build/` finds the source
    tree's `content/`.
  - `vb server rm` refuses to delete a saved world without `--yes`
    (`--keep-world` keeps `world/` and everything else goes).
  - The server now flushes stdout after every write (`std::unitbuf`);
    otherwise a detached server's log only filled at exit.
  - Only Linux was exercised locally. The Windows (`CreateProcessW`
    DETACHED_PROCESS, `GetProcessTimes`, Winsock port probe) and macOS
    (`sysctl` start time) code is compiled by CI only.
- **Exit:** `vb host` is a one-command server; `vb server start foo` survives
  the terminal closing; `vb server stop foo` produces a saved world on
  Linux, macOS and Windows.

### 8.5 — Polish & ecosystem (M)

- [x] `vb list --remote` via the GitHub REST API (`nlohmann_json`).
- [x] `--json` output on `list`/`paths`/`which`/`server list`/`server status`.
- [x] `vb self update`; `scripts/install.sh` / `install.ps1` bootstrap; `bin/` shims.
- [x] Server `--status-file` + rich `vb server status` (§6.5).
- [x] `vb server config get/set/unset/edit`; log rotation.
- [x] Shell completions (bash/zsh/fish/PowerShell) generated from the
      dispatcher's command table.
- [x] `vb host --pack dir --watch`: restart the server when files under the
      pack change (debounced) — the content-author hot loop until
      server-side hot reload exists.
- [x] README "Getting Started" rewritten around `vb`.
- Notes / decisions:
  - `list --remote` asks `api.github.com/repos/<repo>/releases` (published,
    non-draft, non-prerelease, clean `vX.Y.Z` tags only). A non-GitHub base URL
    is assumed to be GitHub Enterprise-shaped (`<base>/api/v3`); a plain
    static mirror cannot be listed. Unauthenticated API calls are rate
    limited and say so.
  - `self update` takes the newest release's `vb` archive (verified exactly
    like an install) and replaces the running binary: rename-over on POSIX;
    on Windows the running `vb.exe` is renamed to `vb.exe.old` and deleted by
    the next `vb` start. A development build (version not a clean tag) is only
    replaced with `--force`; `--check` reports without changing anything.
  - Shims (`vb shim install|remove`) are small scripts in `<data>/bin` that
    call back into `vb` (`vb launch -- "$@"` / `vb which server`), so they
    follow `vb use` with no PATH edits. They embed `vb`'s absolute path:
    re-run `vb shim install` if `vb` moves.
  - The server writes `run/status.toml` (`--status-file`) at start, every 5 s
    and at shutdown, atomically. `seed` is a string because a u64 can exceed
    TOML's signed 64-bit integers. `vb` shows it only while fresh (< 20 s old
    and `running = true`); an older server without the flag just shows no
    player list.
  - `auth.issuer`, `auth.client_id`, `auth.project_id`, `auth.api_key` set the
    `[auth]` overrides for a pack's `auth.lua` (so one pack works against
    staging and production realms): `vb server config <name> set auth.issuer
    https://id.example/realms/prod`. They live in an `[auth]` table (root keys
    are always kept above the first table); `get` never prints the API key.
  - `server config` edits are textual: one `key = value` line is replaced,
    appended or removed, so comments and layout survive; values are type
    checked (the engine's loader silently ignores wrong-typed keys) and the
    whole file is re-validated with the engine's loader before it replaces
    the old one. `edit` runs `$VISUAL`/`$EDITOR` and, if the result is invalid,
    restores the previous file and keeps the bad one as `server.toml.rejected`.
  - Log rotation: before each detached start, `logs/server.log` over 10 MiB
    becomes `server.log.1` (keeping 3). Foreground runs write to the terminal.
  - `host --watch` ignores what a running pack writes itself (`storage.json`,
    `db/`) and dot-files, or it would restart in a loop; it waits for the
    files to stop changing for 0.7 s before restarting; a server that dies
    with an error waits for the next edit instead of exiting; Ctrl+C,
    `vb server stop` and a normal exit end the watch.
  - Completion scripts are generated from the command table and call the
    hidden `vb __complete instances|versions` for live names.
  - The server now installs its SIGINT/SIGTERM handlers first thing in
    `main`. Before, a stop that arrived while a server was still loading its
    world killed it by signal (exit 143) instead of stopping it cleanly.
  - Not verified here: `install.ps1` (no PowerShell available — a direct
    port of the tested `install.sh`), and the zsh/fish/PowerShell completion
    scripts (only bash was exercised). The Windows self-update rename dance
    and macOS paths are compiled by CI only.

### 8.6 — Hardening (S–M, as needed)

- [x] Signed `release.toml` (Ed25519), verification in `vb` and `install.sh`.
- [x] `vb server service print <name>` → a systemd user unit / launchd agent /
      Task Scheduler XML the user can install themselves (still no elevation).
- [x] Protocol-mismatch warning: `vb launch --connect host:port` compares the
      client's protocol with a locally managed server's.
- [~] Linux arm64 / Windows arm64 artifacts: `vb`, both install scripts and the
      packaging scripts understand `linux-arm64` / `windows-arm64`; the CI legs
      that build them were added 2026-10-17 but have not run yet (see below).

#### Release signing

- **Scheme.** Ed25519 (RFC 8032, pure mode) over the raw bytes of `release.toml`;
  `release.toml.sig` is the base64 of the 64-byte signature. It is made with
  plain `openssl pkeyutl -sign -rawin` (`scripts/sign_release.sh`), so a
  maintainer needs nothing but OpenSSL. `vb` verifies with
  [orlp/ed25519](https://github.com/orlp/ed25519) (public domain / zlib, pinned
  by commit in `cmake/Dependencies.cmake`, built as its own target outside the
  project's `-Werror` flags). OpenSSL-made signatures verifying under it is
  pinned by a test vector (`dev_cli_hardening_test.cpp`).
  Minisign/Sigstore were not used: minisign pre-hashes with BLAKE2b (more code
  to ship) and Sigstore needs network services at install time.
- **Trust anchors.** Public keys compiled into `vb` from `release_keys.txt`, plus
  `trusted_keys = ["<hex>", ...]` in `cli.toml`. With no key at all nothing is
  enforced (so existing setups keep working). With any key, a missing signature
  is refused (`require_signature = false` in `cli.toml` relaxes that); a
  present-but-wrong signature is always refused. Applies to `install`, `update`
  and `self update`; `vb doctor` shows the policy.
- **Turning it on (maintainer, once).**
  1. `scripts/gen_release_key.sh private.pem` — creates the key pair and prints
     the public key in the two encodings below.
  2. Add the hex line to `release_keys.txt` and set `DEFAULT_PUBLIC_KEY_B64` in
     `scripts/install.sh`.
  3. Store the private key as the repository secret `RELEASE_SIGNING_KEY`.
  From then on `publish.yml` signs every release, and **fails** if a key is
  listed but the secret is missing (otherwise every `vb` would refuse that
  release). Until step 2 the workflow only warns.
- **Rotation.** Add the new hex line *next to* the old one, ship a `vb` release
  signed by the old key, then sign later releases with the new key; remove the
  old line once nothing signed by it needs installing.
- **`install.sh`** verifies the manifest itself with the user's `openssl` before
  trusting anything it says (the `vb` it installs cannot vouch for itself). It
  fails closed when `openssl` cannot verify raw Ed25519 (macOS's stock LibreSSL
  cannot: `brew install openssl`), unless `VB_ALLOW_UNSIGNED=1`.
- **`install.ps1` does not verify signatures.** Windows PowerShell has no
  Ed25519 primitive, and shelling out to the downloaded `vb` would be circular.
  Its trust is HTTPS + the manifest's SHA-256. Every later download (the game,
  `vb self update`) is signature-checked by `vb` itself.
- Not covered: a compromised signing key, and rollback to an older (validly
  signed) release. Neither is something `vb` can detect today.

#### Service definitions

`vb server service print <name> [--platform systemd|launchd|task]` writes the
definition to stdout (instructions go to stderr, so `> file` is clean). All three
run `vb server start <name> --foreground`, so `status`, `stop` and `logs` keep
working. systemd: a *user* unit (`KillMode=mixed`, `TimeoutStopSec=60`, restart on
failure, logs appended to the instance log). launchd: a login agent
(`KeepAlive/SuccessfulExit=false` — restart after a crash, not after
`vb server stop`). Windows: a per-user logon task at `LeastPrivilege`; ending the
task from Task Scheduler terminates the server **without saving**, so stop it with
`vb server stop`. `VB_HOME` is passed through when set. Generated XML is checked
to be well-formed (no `--` in comments, names escaped).

#### Protocol warning

`vb launch --connect host:port` passes `--server/--port` to the client and, when
the target is this machine and a server vb manages is running on that port,
compares the two protocol numbers (from the install receipt, else `<binary>
--version`) and prints a warning with the fix. It only warns: the client's own
handshake stays the authority. A server on another machine is invisible to
`vb`, so nothing is claimed about it.

#### arm64

Platform strings are `linux-arm64` and `windows-arm64` (macOS stays
`macos-universal`). Everything that reads them is ready.

- **linux-arm64: shipped (2026-10-17).** `build_linux.yml` builds
  `arch: [x86_64, arm64]`, the arm64 leg on `ubuntu-24.04-arm` (release and
  debug only, no asan/tsan). Built, tested and packaged green in CI.
  `fail-fast: false` keeps one leg's failure from cancelling the others.
  Still untested: `vb install` on a real arm64 machine after a release.
- **windows-arm64: not built.** It was tried on `windows-11-arm` (vcpkg
  `arm64-windows`, MSVC `arm64`). Fixed along the way, all guarded to MSVC
  ARM64: GameNetworkingSockets' endianness check
  (`VALVE_LITTLE_ENDIAN`), librg/zpl's x86-only `__rdtsc`
  (`src/replication/librg_impl.c`), and GNS's donna crypto needing `__emulu`
  (`cmake/shims/arm64_msvc_emulu.c`). It now compiles, links and runs, with 806
  of 812 unit tests passing, but the six real-UDP `GnsTransport` tests never
  connect, so it can't be released. The leg was removed from
  `build_windows.yml` (its comment has the entry to put back) because
  `bundle` needs every leg green. Next step is debugging GNS's connection
  setup on ARM64 Windows (suspect the bundled ed25519/curve25519 donna
  code paths chosen for an unrecognised architecture).

## 12. Open questions

1. **macOS Gatekeeper.** Unsigned binaries downloaded by `vb` (via libcurl)
   don't get the quarantine xattr, so they run; but the bootstrap script's
   download through a browser would. Notarization is a separate, paid
   decision — tracked here, not blocking.
2. **Linux binary portability.** CI builds on `ubuntu-latest`; the resulting
   glibc floor may be too new for older distros. Options: build on an older
   image, or document the floor. Decide during 8.1.
3. **Debug builds in releases.** Ship `-debug` zips (useful for bug reports,
   doubles release size) or not? Default plan: ship them, `vb install --build
   debug` opt-in.
4. **Instance location override.** Should `vb server new --dir <path>` allow
   instances outside `<data>/servers/` (e.g. a git checkout of a server
   config)? Cheap to add in 8.4 if wanted.
