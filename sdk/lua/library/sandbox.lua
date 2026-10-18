---@meta
-- What is NOT available in pack Lua. The engine opens only `base`, `string`, `table`, `math`,
-- `coroutine` and `utf8`, then removes the functions below. `require` is replaced by a sandboxed
-- version that resolves only the pack's own `.lua` files (`require("structures.oak_tree")`).
-- Each call also has an instruction budget (20M), a wall-clock budget (250 ms) and a 64 MB heap.
--
-- Removed (undefined in the engine; the pack `.luarc.json` marks them undefined in the editor too):
--   os, io, package, dofile, loadfile, load, loadstring, collectgarbage,
--   debug.* except debug.traceback

---@diagnostic disable: lowercase-global

---The engine's `require`: loads one of the pack's own `.lua` files (dots are directories) once and
---returns its result; nothing outside the pack can be loaded.
---@param modname string e.g. `"structures.oak_tree"` for `structures/oak_tree.lua`.
---@return any
function require(modname) end
