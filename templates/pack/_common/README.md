# {{name}}

A Voxel Browser content pack, made with `vb pack init`.

## Run it

```sh
vb pack check          # load the pack headless; prints file:line errors (add --json for tools)
vb pack dev            # host it with auto-restart on save, and open the client
```

`vb pack dev --no-client` only hosts (connect with `vb launch --connect localhost`).

## What is in here

| Path | Runs in | Purpose |
| --- | --- | --- |
| `pack.toml` | -- | name, version and `engine_version_req` (enforced) |
| `init.lua` | server VM | event handlers (`vb.on`), timers; loaded last |
| `blocks/*.lua` | server VM | `vb.register_block{...}`; loaded first, in file order |
| `entities/`, `biomes/` | server VM | `vb.register_entity`, `vb.register_biome` |
| `ui/*.lua` | **client UI VM** | `ui.define` screens and the HUD; cannot touch `vb` |
| `textures/` | -- | PNG files referenced by blocks (`texture = "textures/x.png"`) |

Server files see the global `vb`; files in `ui/` see `ui` and `client` instead. Mixing them up is
the most common mistake, and `vb pack check` reports it.

## Editor support

`.luarc.json` and `.vb/lua/` give the Lua Language Server (VS Code "Lua" by sumneko, Neovim, ...)
completion and hover docs for the whole API. Refresh the stubs with `vb pack types`.
If the pack has a `ui/` folder, `ui/.luarc.json` knows only the client UI API: add `ui/` as a second
folder of your editor workspace (VS Code: *File → Add Folder to Workspace*) and using `vb` there is
flagged, as it would fail at run time.

## Docs

- Quick reference (one line per function): `vb docs lua-reference/README.md`
- Everything else: `vb docs` lists topics, `vb docs --path` prints where they live.
