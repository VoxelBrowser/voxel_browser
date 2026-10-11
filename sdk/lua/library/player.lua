---@meta
-- Usertypes handed to scripts: `Player` (event handlers, `vb.on`) and `Entity` (`self` in
-- `vb.register_entity` callbacks). Server pack VM only.

---@vb context runtime
---Server pack VM only. A connected player. Obtained from event handlers; do not store across ticks
---unless you also handle `player_leave`. In `player_leave` the connection is already gone, but
---`get_name()`, `get_login()` and `get_pos()` (last known position) still answer.
---@class Player
local Player = {}

---@vb context runtime
---Server pack VM only. Current position.
---```lua
---local p = player:get_pos() -- {x=, y=, z=}
---```
---@return Vec3
function Player:get_pos() end

---@vb context runtime
---Server pack VM only. Teleports the player: feet position, velocity zeroed,
---look direction kept. The player's client snaps to it like any server
---correction. Errors on non-finite coordinates.
---```lua
---player:set_pos(0.5, 90, 0.5)
---```
---@param x number
---@param y number
---@param z number
function Player:set_pos(x, y, z) end

---@vb context runtime
---Server pack VM only. The feet position this player was granted when they
---joined (where the default respawn puts them).
---```lua
---local s = player:get_spawn_pos()
---player:set_pos(s.x, s.y, s.z) -- back to spawn
---```
---@return Vec3
function Player:get_spawn_pos() end

---@vb context runtime
---Server pack VM only. Adds to the player's velocity (m/s).
---```lua
---player:set_velocity(0, 8, 0) -- launch upward
---```
---@param x number
---@param y number
---@param z number
function Player:set_velocity(x, y, z) end

---@vb context runtime
---Server pack VM only. Logged no-op (nothing to remove a player from).
---```lua
---player:remove()
---```
function Player:remove() end

---@vb context runtime
---Server pack VM only. Inventory as a list of `{item=, count=}` (1-based slots).
---```lua
---for slot, stack in ipairs(player:get_inventory()) do print(slot, stack.item, stack.count) end
---```
---@return ItemStack[]
function Player:get_inventory() end

---@vb context runtime
---Server pack VM only. Sends a private chat line.
---```lua
---player:send_message("welcome!")
---```
---@param text string
function Player:send_message(text) end

---@vb context runtime
---Server pack VM only. Opens a client UI screen defined with `ui.define`.
---```lua
---player:open_ui("mypack:shop", { gold = 12 })
---```
---@param name string
---@param ctx? table Initial UI state (JSON-compatible).
function Player:open_ui(name, ctx) end

---@vb context runtime
---Server pack VM only. Adds items to the inventory.
---```lua
---player:give{ item = vb.register_block{ name = "mypack:gem" }, count = 3 }
---```
---@param stack ItemStack
function Player:give(stack) end

---@vb context runtime
---Server pack VM only. Removes `count` of `item` across slots, all-or-nothing.
---```lua
---if player:take{ item = 4, count = 2 } then player:send_message("paid") end
---```
---@param stack ItemStack
---@return boolean ok `false` (and no change) if the player lacks enough.
function Player:take(stack) end

---@vb context runtime
---Server pack VM only. In-game name.
---```lua
---print(player:get_name())
---```
---@return string
function Player:get_name() end

---@vb context runtime
---Server pack VM only. Verified login (frozen table), or `nil` when the server does not authenticate.
---```lua
---local login = player:get_login()
---if login then print(login.subject) end
---```
---@return Login|nil
function Player:get_login() end

---@vb context runtime
---Server pack VM only. Reduces health; `cause` is passed to `player_death` handlers.
---```lua
---player:damage(2, "lava")
---```
---@param amount number
---@param cause? string
function Player:damage(amount, cause) end

---@vb context runtime
---Server pack VM only. Current and maximum health, or `nil` if the player is gone.
---```lua
---local h = player:get_health(); print(h.current, h.max)
---```
---@return {current: number, max: number}|nil
function Player:get_health() end

---@vb context runtime
---Server pack VM only. Current and maximum hunger, or `nil` if the player is gone.
---```lua
---local h = player:get_hunger()
---```
---@return {current: number, max: number}|nil
function Player:get_hunger() end

---@vb context runtime
---Server pack VM only. Restores (positive) or spends (negative) hunger, clamped to `[0, max]`.
---```lua
---player:add_hunger(5) -- ate food
---```
---@param amount number
function Player:add_hunger(amount) end

---@vb context runtime
---Server pack VM only. Breaks the block through the full validated pipeline (reach check, hooks, drops).
---```lua
---local ok = player:break_block(10, 64, 10)
---```
---@param x integer
---@param y integer
---@param z integer
---@return boolean accepted
function Player:break_block(x, y, z) end

---@vb context runtime
---Server pack VM only. Places `block` through the validated pipeline.
---```lua
---player:place_block(10, 65, 10, 3)
---```
---@param x integer
---@param y integer
---@param z integer
---@param block BlockId
---@return boolean accepted
function Player:place_block(x, y, z, block) end

---@class PunchResult
---@field hit_player boolean
---@field target Player|nil
---@field hit_block boolean
---@field x integer
---@field y integer
---@field z integer
---@field punches integer
---@field broken boolean

---@vb context runtime
---Server pack VM only. Throws one punch along the player's look direction (players or blocks, whichever is closer).
---```lua
---local r = player:punch(2) -- counts as 2 punches against max_damage
---if r.broken then print("broke", r.x, r.y, r.z) end
---```
---@param block_damage? integer 1..65535 (default 1).
---@return PunchResult
function Player:punch(block_damage) end

---@vb context runtime
---Server pack VM only. 1-based selected hotbar slot.
---```lua
---local slot = player:get_selected_slot()
---```
---@return integer
function Player:get_selected_slot() end

---@vb context runtime
---Server pack VM only. The stack in the selected slot, or `nil` if empty.
---```lua
---local held = player:get_held_item()
---```
---@return ItemStack|nil
function Player:get_held_item() end

---@vb context runtime
---Server pack VM only. `self` in `vb.register_entity` callbacks: a table (free for your own fields)
---whose metatable provides these methods.
---@class Entity
local Entity = {}

---@vb context runtime
---Server pack VM only. Current position.
---```lua
---local p = self:get_pos()
---```
---@return Vec3
function Entity:get_pos() end

---@vb context runtime
---Server pack VM only. Teleports the entity.
---```lua
---self:set_pos(0, 70, 0)
---```
---@param x number
---@param y number
---@param z number
function Entity:set_pos(x, y, z) end

---@vb context runtime
---Server pack VM only. Registered kind name.
---```lua
---print(self:get_kind())
---```
---@return string
function Entity:get_kind() end

---@vb context runtime
---Server pack VM only. Reduces health (needs `health` in the kind's def); despawns at 0.
---```lua
---self:damage(3, "fire")
---```
---@param amount number
---@param cause? string
function Entity:damage(amount, cause) end

---@vb context runtime
---Server pack VM only. Despawns the entity.
---```lua
---self:remove("expired")
---```
---@param cause? string
function Entity:remove(cause) end

---@vb context runtime
---Server pack VM only. Current and maximum health, or `nil` for a kind without `health`.
---```lua
---local h = self:get_health()
---```
---@return {current: number, max: number}|nil
function Entity:get_health() end

---@vb context runtime
---Server pack VM only. Sets health (clamped; reaching 0 despawns).
---```lua
---self:set_health(5)
---```
---@param value number
function Entity:set_health(value) end

---@vb context runtime
---Server pack VM only. Sets, changes or removes the entity's world-space label; replicated as a small update, the entity is not respawned.
---A string changes only the text and keeps the current style. A table applies the kind's `text` style with its own fields on top
---(an omitted `value` keeps the current text). `nil` or `""` removes the label. Text over 64 bytes or a bad colour is an error.
---```lua
---self:set_text("26s")
---self:set_text({ value = "Ripe!", color = { 120, 235, 110 } })
---self:set_text(nil)
---```
---@param text string|EntityText|nil
function Entity:set_text(text) end

---@vb context runtime
---Server pack VM only. The label's current text, or `nil` if the entity has none.
---```lua
---if self:get_text() == "Ripe!" then return end
---```
---@return string|nil
function Entity:get_text() end

---@vb context runtime
---Server pack VM only. Forces the animation clip every client plays for this entity (any clip its sheet declares,
---e.g. `"open"`), instead of the one picked from its velocity and on-ground state. Sticky and replicated, including to
---players who join later; `nil` goes back to the automatic choice.
---```lua
---chest:set_clip("open")
---chest:set_clip(nil)
---```
---@param clip string|nil Clip name (1-64 bytes) or nil.
function Entity:set_clip(clip) end

---@class AttachOptions
---@field offset? Vec3 Position relative to the parent's position (default `{x=0, y=0, z=0}`).
---@field face_offset? boolean Turn `offset` with the parent's facing: x = right, z = forward (default false).
---@field layer? integer Overrides the visual's `layer` while attached, -8..8 (e.g. 1 to always draw in front of the parent).

---@vb context runtime
---Server pack VM only. Attaches this entity to another entity or a player: from then on it moves with the parent
---(every tick on the server, every frame on clients, so it never trails behind) until `detach()`. If the parent
---despawns or leaves, this entity is removed too (`on_death` cause `"parent_removed"`). `set_pos` has no lasting
---effect while attached.
---```lua
---local hat = vb.world.spawn("mypack:hat", player:get_pos())
---hat:attach_to(player, { offset = { x = 0, y = 1.9, z = 0 }, layer = 1 })
---```
---@param parent Entity|Player
---@param opts? AttachOptions
function Entity:attach_to(parent, opts) end

---@vb context runtime
---Server pack VM only. Undoes `attach_to`; the entity stays where it is. No-op when not attached.
---```lua
---hat:detach()
---```
function Entity:detach() end
