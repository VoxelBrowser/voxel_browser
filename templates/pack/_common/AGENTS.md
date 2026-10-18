# Agent notes: {{name}} (Voxel Browser content pack)

**Rule 1 -- two Lua environments.** Files in `ui/` run in the *client UI VM*: they see `ui` and
`client`, and `vb` is nil. Every other `.lua` file runs in the *server pack VM*: it sees `vb`, and
`ui`/`client` are nil. Never call `vb.*` from `ui/`, never call `ui.*` from server files. Talk across
the boundary with `player:open_ui(name, ctx)` (server -> client) and `ui.send_event(kind, value)`
(client -> server, handled by `vb.on("ui_event", ...)`).

## Loop

1. Edit.
2. `vb pack check --json` -- exit 0 = clean. Each diagnostic has `severity`, `file`, `line`,
   `message`. Fix every error; use `--strict` to also fail on warnings. Warnings include definition
   keys the engine ignores (typos), blocks without a texture, and APIs newer than `engine_version_req`.
3. `vb pack dev` (or `vb pack dev --no-client`) -- hosts the pack and restarts when files change.

## Layout and load order

`blocks/*.lua`, `entities/*.lua`, `biomes/*.lua` (each sorted), then other root `*.lua`, then
`init.lua`. `ui/*.lua` loads separately on the client, sorted. `require("a.b")` loads `a/b.lua` from
this pack only. `pack.toml`'s `engine_version_req` is enforced.

## Sandbox

Lua 5.4 with `os`, `io`, `package`, `load`, `dofile`, `loadfile`, `collectgarbage` and `debug`
(except `traceback`) removed. Instruction budget and wall-clock limit per callback.

## Where the API is documented

- `.vb/lua/*.lua` -- LuaCATS stubs: signatures, field lists and one example per function.
- `vb docs lua-reference/README.md` -- the same as a one-line-per-function cheat sheet.
- `vb docs <name>` e.g. `vb docs vb.world.raycast` prints one function.

## Common mistakes

- Registration (`vb.register_*`, `vb.physics.set_params`, ...) is rejected after the pack has
  loaded: call it at file top level, not inside event handlers.
- Block ids follow registration order and saved worlds store ids: never reorder or delete existing
  `vb.register_block` calls in a pack with a live world; append instead.
- `vb.world.*` and player methods need a running game: not available at load time.
- `end` is a Lua keyword: `vb.render.set_fog{ start = 40, ["end"] = 120 }`.
- Textures are pack-relative paths (`"textures/x.png"`), not file system paths.
