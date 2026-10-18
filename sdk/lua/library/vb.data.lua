---@meta
-- Persistent per-key storage, hashing and auth state. Server pack VM only.

---@class vb.db
vb.db = {}

---@vb context runtime
---@vb since 0.1.0
---Server pack VM only. Reads a value written by `vb.db.set` (JSON round-trip), or `nil`.
---```lua
---local rec = vb.db.get("user:" .. player:get_login().subject)
---```
---@param key string
---@return any
function vb.db.get(key) end

---@vb context runtime
---@vb since 0.1.0
---Server pack VM only. Writes a value immediately (no flush step).
---```lua
---vb.db.set("user:alex", { coins = 10 })
---```
---@param key string
---@param value any JSON-compatible.
function vb.db.set(key, value) end

---@vb context runtime
---@vb since 0.1.0
---Server pack VM only. Deletes a key. There is no enumeration API.
---```lua
---vb.db.delete("user:alex")
---```
---@param key string
function vb.db.delete(key) end

---@class vb.crypto
vb.crypto = {}

---@vb context runtime
---@vb since 0.1.0
---Server pack VM only. SHA-256 hex digest.
---```lua
---local digest = vb.crypto.hash("hello")
---```
---@param data string
---@return string hex
function vb.crypto.hash(data) end

---@class vb.auth
vb.auth = {}

---@vb context runtime
---@vb since 0.1.0
---Server pack VM only. `true` iff the pack's `auth.lua` is active (every `Player` then has a login).
---```lua
---if vb.auth.required() then print("authenticated server") end
---```
---@return boolean
function vb.auth.required() end
