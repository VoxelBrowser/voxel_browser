# Automation protocol (development builds only)

JSON-lines RPC between a test harness and a `voxel_browser` /
`voxel_browser_server` built with `-DVB_WITH_AUTOMATION=ON`. Design and safety
rules: `docs/e2e-automation.md` (§5 protocol, §7 keeping it out of production).
Production binaries contain none of this and exit non-zero on `--automation`.

**Protocol version: 1** (`kAutomationProtocolVersion`, `inc/vb/automation/protocol.hpp`).
Implemented so far (through phase E6): phase E2 (host, `hello/state/step/quit/wait_for`, predicate
engine, read-only state) and phase E3 (client input / UI / chat / multi-frame
actions, server admin commands, deferred replies). Windowed-only commands
(`menu.*`, `screenshot`, typing into the raygui chat box) are not implemented;
windowed clients are driven the same way (see "Windowed clients" below).

## Transport

`--automation stdio` on either binary. One JSON object per line.

- **stdin** → requests. **stdout** → responses and events. Closing stdin makes
  the process exit (no orphaned servers).
- At startup fd 1 is duplicated for protocol frames and then redirected to
  **stderr**, so `std::cout`, `printf` and Lua's `print` can never corrupt a
  frame. Read logs from stderr.
- The client runs headless (`--headless`) or in a real window (no `--headless`; needs a
  display, e.g. `xvfb-run -a`). `--automation-clock real|manual` (client): `real`
  (default) runs frames at 60 Hz wall clock; `manual` only advances on `step`.
- `--net-sim <spec>` (both binaries, E5): fake network conditions, see "Network
  simulation" below.
- `--automation tcp[:PORT]` is the other transport (E6), see "TCP attach" below. Any other
  `--automation` value is an error; a build without the flag prints `built without
  VB_WITH_AUTOMATION` and exits 1 (also for `--automation-record`, `--net-sim`).

All commands run on the main thread between frames/ticks; a reader thread only
enqueues parsed requests.

**Deferred replies (E3).** A command that takes frames to finish (`key.hold`,
`walk_to`, `break_block`, ...) is answered when it completes, not when it is
accepted, like `wait_for`/`step`. Several can be in flight at once; match
replies by `id`. Timeouts for client actions are counted in frames (60 per
second), so they behave the same under `--automation-clock manual`.

## Frames

```jsonc
→ {"id":1,"cmd":"hello","proto":1}                        // `proto` may also be in args
→ {"id":2,"cmd":"wait_for","args":{"pred":{"joined":true},"timeout_ms":5000}}
← {"id":1,"ok":true,"result":{...}}
← {"id":2,"ok":false,"error":{"code":"timeout","message":"...","last":{...}}}
← {"event":"log", ...}                                    // unsolicited, no id
```

`id` is a number or string, echoed verbatim. Responses can arrive out of order
(`wait_for`, `step` answer later).

### Error codes

| code | meaning |
|---|---|
| `bad_request` | unparseable line, missing/invalid fields, malformed predicate (extra `message`) |
| `unknown_command` | `cmd` not implemented by this role |
| `proto_mismatch` | `hello.proto` ≠ host's protocol version |
| `unsupported` | e.g. `step` without `--automation-clock manual`; `screenshot` on a headless client |
| `not_in_menu` | `menu.*` while the client isn't on the main menu (extra `app_state`) |
| `unavailable` | `signin.*`: that button is not on the sign-in screen (no sign-in in progress, the trust prompt is not up, or no browser method) |
| `timeout` | `wait_for` or a client action ran out of time; `error.last` holds what was observed |
| `not_playing` | client action needs `app_state` = `playing` (and a joined session) |
| `out_of_reach` | `break_block`/`place_block` target is farther than the reach (5.0); extra `distance`, `reach` |
| `not_loaded` | the target chunk isn't streamed in (client) / loaded (server); server edits never create chunks |
| `no_ui` / `no_widget` / `bad_widget` | `ui.*`: no screen open / no such widget id (extra `widgets` lists the ids) / widget is the wrong type |
| `failed` | action ran but didn't take effect (e.g. `select_slot` while a UI or the chat box is open) |
| `no_player` / `unknown_item` / `lua_error` | server admin: unknown player (extra `players`), unknown block name, `run_lua` failed (`message` is Lua's) |

## Commands (E2)

| Command | Roles | Args → result |
|---|---|---|
| `hello` | both | `{proto}` → `{role, proto, engine, build, clock}`; server adds `port` (the port it was told to listen on) |
| `state` | both | → snapshot (below) |
| `step` | client, manual clock | `{frames: 1..1000000}` → `{frames}` after that many frames ran |
| `wait_for` | both | `{pred, timeout_ms=5000}` → `{observed}` once true (evaluated every frame/tick in-process) |
| `quit` | both | → `{}`, then the process shuts down normally |

## Client commands (E3)

All take effect through the real input path: they feed a `SyntheticInput` that
`ClientApp::frame()` consumes, or call the same `UiRuntime::report_*` functions
the renderer calls. Positions are world coordinates; `pos` for blocks is floored.
Yaw/pitch use the camera's convention (degrees; **positive pitch looks up**,
yaw 0 faces -Z).

| Command | Args → result |
|---|---|
| `key.press` / `key.hold` | `{key}` / `{key, frames}` → `{frames}` once the key has been down that many frames. `key` is a logical name (`forward back left right jump sprint` follow the player's bindings; `inventory` `pause` `chat`), a letter/digit, `slot1..9`, `space enter escape tab shift ctrl` |
| `mouse.press` / `mouse.hold` | `{button=left\|right\|middle}` / `+ {frames}` → `{frames}` |
| `mouse.capture` | `{on=true}` → `{mouse_captured}`. Movement/look/break/place input is only read while the mouse is captured (as for a player); actions below capture it automatically |
| `look` | `{yaw, pitch}` → `{yaw, pitch}` (absolute) |
| `look_at` | `{pos}` → `{yaw, pitch}` (aim the eye at a point) |
| `select_slot` | `{n: 1..9}` → `{slot}` (presses the number key) |
| `walk_to` | `{pos: [x, y\|null, z], tolerance=0.5, timeout_ms=15000}` → `{feet}`; holds forward facing the target, hops when stuck. `timeout`'s `last.feet` shows where it stopped |
| `break_block` | `{pos, timeout_ms=5000}` → `{block_before, block_after, confirm_ms}`; `confirm_ms` is the wall time from each click to the client seeing the server's answer (the punch count changing, or for the last click the block changing); aims at the block centre, holds the aim steady for 8 frames (the server applies a command's look after running the pack's input hook), then punches once and waits for `S2C_BlockDamage`/the block change before the next punch, so it never over-punches. `timeout`'s `last` has `punches`. Note a punch hits the nearest *player* before any block |
| `place_block` | `{pos, face: [nx,ny,nz], timeout_ms=5000}` → `{placed, at}`; same aim settling, then exactly one right-click (no retry) on face `face` of the existing block `pos`, placing at `pos+face` with the selected slot's item. `timeout`'s `last` has `looking_at`/`face` |
| `chat.send` | `{text}` → `{}`; sends over `ClientSession::send_chat`, what Enter in the chat box does |
| `ui.click` / `ui.fill` / `ui.select` | `{id}` / `{id, text}` / `{id, index}` → `{}` on the open modal screen (button / textbox / list) |
| `hud.click` / `hud.fill` / `hud.select` | same, on the always-on HUD widgets |

## Server commands (E3)

Setup and verification. The server is authoritative, so edits go to the real
world and replicate to clients like any other change.

| Command | Args → result |
|---|---|
| `block_at` | `{pos}` → `{block: name\|null}` |
| `set_block` | `{pos, block}` (registry name, e.g. `base:stone`) → `{changed}`; `not_loaded` if no chunk is loaded there |
| `fill` | `{from, to, block}` (inclusive box, ≤ 100000 voxels) → `{changed, unchanged, not_loaded}`; error `not_loaded` if the whole box is outside loaded chunks |
| `teleport` | `{player, pos}` (`player` = name or net id) |
| `give` | `{player, item, count=1}` → inventory is synced to the player like `player:give{}` |
| `set_time` | `{ticks: 0..23999}` → all clients are told immediately |
| `set_health` | `{player, value}`; lowering goes through the damage path (0 kills + respawns, cause `automation`) |
| `kick` | `{player, reason?}` |
| `advance_reauth` | `{player, seconds}`: simulates `seconds` of tick time for the player's periodic re-auth (auth.md §5.6): subtracts it from the timer until the next request or, while a request is outstanding, from its grace countdown; `not_signed_in` if the player has no external login. The next tick runs the normal re-auth path; the reply must still arrive in real time |
| `run_lua` | `{code}` → `{}` or `lua_error`; runs in the pack VM after load: `vb.register_*` fails with `registry already frozen`; other APIs (e.g. `vb.world.*`) are callable, but world edits only stick in chunks that stay loaded (a player nearby) |

Build the scene in loaded chunks (e.g. near spawn, or after teleporting a
player and waiting for `chunks_loaded`): an edit outside them is refused rather
than creating an empty chunk.

## Windowed clients (E5)

Without `--headless` the client opens a real window and starts on the main menu (or
connects straight away with `--server`/`--singleplayer`), driven by the same synthetic
input. raygui reads raylib's real mouse state, which a script can't produce, so the menu
commands inject the *result* of a click and fill the fields a player would type into.

| Command | Args → result |
|---|---|
| `menu.set_name` | `{name}` → `{}` |
| `menu.connect` | `{host, port, name?}` → `{}`; the transition shows up in `app_state` (`connecting` → `loading` → `playing`; the loading screen needs frames to mesh the first chunks) |
| `menu.singleplayer` | `{name?}` → `{}` |
| `signin.trust` / `signin.browser` / `signin.cancel` | `{}` → `{}`; the engine-drawn sign-in screen's *Continue* (first-use trust prompt), *Sign in with your browser* (or *with Google*) and *Cancel*, acted on by the next frame like the click. `unavailable` when that button is not on screen (read `auth` first). Builds with `VB_WITH_AUTH`; a headless client answers the trust prompt and starts the browser flow by itself |
| `screenshot` | `{path}` → `{path, width, height}`; saves the framebuffer as a PNG at the end of the next frame (works on the menu and in game). `unsupported` on a headless client |
| `type` | `{text}` → `{}`; appends to the chat box as keyboard characters would, opening it first; `key.press enter` then sends it. Works headless too (the box's buffer and Enter handling are shared; only raygui's drawing of it is windowed) |

## Network simulation (`--net-sim`, E5)

`--net-sim lag_ms=120,jitter_ms=30,loss_pct=5[,reorder_pct=..][,dup_pct=..]` on either
binary, mapped onto GameNetworkingSockets' global fake-packet settings. It acts on
packets **this process sends**: give the server and the clients the same spec for a
symmetric link (so `lag_ms=100` shows up as roughly 200 ms of round-trip time).
`lag_ms`/`jitter_ms` are 0..5000, the percentages 0..100; jitter averages `jitter_ms` and is
capped at twice that. Unknown keys, bad numbers and trailing commas are errors. It needs a
`VB_WITH_NET` build, applies to real (GNS) connections only (`--singleplayer` is refused:
it uses an in-process loopback), and a build without `VB_WITH_AUTOMATION` refuses the flag
like `--automation`. The client snapshot's `rtt_ms` (and the `rtt_ms` predicate) is the
measured round-trip time, so a test can prove the simulation took effect.

## TCP attach (`--automation tcp[:PORT]`, E6)

For attaching to a game that is *already running*, typically a client a human is playing, and
for tools that outlive a process's stdin.

- **Loopback only, by construction.** The listener binds `127.0.0.1` and there is no option to
  bind anything else. `PORT` 0 (the default) picks a free one (unlike the game's UDP port, TCP can).
- **Token.** The first line of every connection must be
  `{"cmd":"auth","args":{"token":"..."}}`. The token is random (128 bits) unless
  `--automation-token <t>` is given, and is compared in constant time. Reply `{"ok":true,
  "result":{"authenticated":true}}`, or `{"ok":false,"error":{"code":"unauthorized",...}}` and the
  connection is closed. A connection that sends no auth line within 5 s is dropped.
- **Finding the endpoint.** `--automation-info <file>` writes `{"host","port","token","pid"}`
  there (permissions `0600`, set before the token is written); without it the endpoint is printed
  to stderr as `automation: listening on 127.0.0.1:PORT token=...`.
- **One connection at a time.** A second one waits in the listen backlog until the first drops.
  When a client disconnects the **game keeps running** and waits for the next attach (stdio
  mode, by contrast, exits when stdin closes). `quit` still shuts the process down. Replies owed
  to a client that left are dropped.
- After authentication the protocol is exactly the stdio protocol (same commands, frames,
  deferred replies). Game output stays on stdout/stderr (nothing is redirected).
- Windows uses Winsock; that code path is written but has never been compiled or run.

## Recording a session (`--automation-record FILE.py`, E6)

`voxel_browser --automation-record session.py` (windowed, or any `--automation` client) watches
the same input and game state a player produces and writes what they *meant* as a vbtest script,
rewritten after every step so a crash loses nothing. Dev builds only, like all of this.

| What happened | What is written |
|---|---|
| movement keys, then stopping | `p.walk_to((x, None, z), tolerance=0.7)` to where you stopped (shuffling < 0.5 blocks is dropped) |
| left click with the crosshair on a block | `p.break_block((x, y, z))` (several clicks on one block within 1.5 s are one break), then `expect(p).to_see_block(pos, "<name now>")` once the block changed |
| right click with the crosshair on a block | `p.place_block((x, y, z), face=(nx, ny, nz))`, then `expect(...).to_see_block(dest, ...)` |
| number keys 1-9 | `p.select_slot(n)` |
| jump, E, Esc | `p.key_press("jump" / "inventory" / "pause")` |
| a chat line sent | `p.chat("...")` and `expect(p).to_have_chat("Name: ...")` |
| a pack screen opens / closes | `expect(p).to_have_ui_open("name")` / `.not_.to_have_ui_open("name")` |
| pack UI clicks / edits | `p.ui("id").click()` / `.fill("text")` / `.select(i)` (`hud(...)` for the HUD) |
| main-menu choices | a comment line (the script itself connects through `clients()`) |

It is a **starting point**: scene setup (`set_block`, `give`, `teleport`) isn't recorded, coordinates
are absolute so it replays against the same world (same seed), looking around isn't recorded, and
a click at the sky is ignored. Steps whose result it couldn't see get no assertion. The
generated file is runnable as is (`def test_recorded_session(server, clients)`); `tests/e2e/
test_recorder.py` records a session and replays the script on a fresh server.
`--automation-record` with `--headless` and no `--automation` is refused (nothing to watch).

## Not implemented

- Clicking raygui widgets by pixel (menu *results* are injected instead), settings and
  keybindings screens.
- `--net-sim` for `--singleplayer` (loopback transport has no delay/drop queue).
- Recording: mouse look, scene setup, multi-client sessions, and replaying menu-driven starts.
- TCP: more than one simultaneous connection, non-loopback binding (deliberately).

### State snapshots

Client: `app_state` (`menu|settings|keybindings|connecting|loading|playing|error`),
`joined`, `net_id`, `feet [x,y,z]`, `on_ground`, `yaw`, `pitch`, `chat [str]` (the HUD's last 8
lines), `chat_open`, `mouse_captured`, `selected_slot` (1-based),
`health`/`max_health`/`hunger`/`max_hunger` (the player's own status, once the server's first
`S2C_PlayerStatus` arrives; absent before), `inventory [{item,count}]` (item = block name), `entities [{net_id,name,pos}]`,
`chunks_loaded`, `auth {phase: idle|choosing|working|finished, needs_trust, reauth_prompt, error}` (once a sign-in coordinator exists, i.e. after connecting; `reauth_prompt` is the "sign in again" banner), `rtt_ms` (once measured; real connections only), `target_block {pos,normal,block}` (what the crosshair is on,
within reach; absent if nothing), `ui {name, widgets}` (only while a modal screen
is open), `hud {widgets}`, `busy_actions` (in-flight multi-frame commands).
Widgets are `{id, type, text, x, y, w, h}` (+ `items`, `list_index` for lists); `type` is
`label|panel|button|textbox|list|rect|text|icon`.

Server: `tick`, `time_of_day`, `player_count`,
`players [{name,net_id,pos,health,max_health,login?}]` (`login = {provider,subject,name}` only
when the server authenticates players -- `auth.lua`; never a token).

## Predicates (`wait_for.pred`)

An object with exactly one key. Combinators: `all:[..]`, `any:[..]`, `not:{..}`
(nesting ≤ 16). A scalar argument is shorthand for the primary field.
Missing state reads as "not true", never as an error; an unknown predicate or
bad arguments is `bad_request`.

| Predicate | Args | Reads |
|---|---|---|
| `joined` | – | `joined` |
| `app_state` | `{is}` or `"playing"` | `app_state` |
| `chat_contains` | `{text}` / `{regex}` or `"text"` | `chat` |
| `entity_visible` | `{name}` | `entities` |
| `entity_near` / `player_near` | `{name,pos,radius}` | `entities` / `players` |
| `player_login` | `{subject, name?, provider?}` | `players[].login` (server): a connected player whose verified login has this subject |
| `auth` | `{phase?, needs_trust?, reauth_prompt?, error_contains?}` (all given fields must match) | `auth` (client, builds with `VB_WITH_AUTH`): the sign-in coordinator's view |
| `block_is` | `{pos,block}` (registry name, e.g. `"base:air"`) | the role's own block view; `false` if the chunk isn't loaded (both client and server, so an unloaded area never reads as air) |
| `pos_near` | `{pos,radius}` | `feet` |
| `health` | `{op,value}` (`< <= > >= == !=`) or a number | top-level `health` (client: the player's own, from `S2C_PlayerStatus`; the server's snapshot has no top-level `health`, use `players[].health`) |
| `chunks_loaded` | `{min}` or a number | `chunks_loaded` |
| `chunk_loaded` | `{pos}` | whether the chunk containing `pos` exists in the process's world (client: its mirror; server: its `World`). Wait for this before building a scene on the server |
| `on_ground` | `true` | `on_ground` (client: landed after the spawn drop) |
| `rtt_ms` | `{op,value}` (use `{"op":">=","value":150}`; a bare number means `==`) | `rtt_ms` (client, real connections) |
| `player_count` | `{op,value}` or a number | `player_count` (server) |
| `ui_open` | `{name}` or `"name"` | `ui.name` |
| `widget` | `{id, text?}` | `ui.widgets` (modal screen only) |
| `hud_widget` | `{id, text?}` | `hud.widgets` (the always-on HUD) |
| `inventory_has` | `{item, count=1}` | `inventory` (client) |

## Known limitation: no `--port 0`

The design called for `--port 0` with the bound port reported by `hello`.
GameNetworkingSockets' direct-UDP listen has no ephemeral-port allocation
(`GnsTransport::listen(0)` fails), so the harness (E4) must pick a free UDP port
itself. `hello.port` currently echoes the configured port.
