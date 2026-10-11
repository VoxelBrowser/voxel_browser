---@meta
-- Tunables and read-only config: physics, combat, action, hunger, daynight, render, config.
-- Server pack VM only. The `set_*` functions are pack-load-time only (rejected after freeze).

---@class vb.physics
vb.physics = {}

---@class PhysicsParams
---@field gravity? number
---@field walk_speed? number
---@field sprint_speed? number
---@field jump_speed? number
---@field accel? number
---@field air_accel? number
---@field friction? number
---@field step_height? number
---@field fly_speed? number
---@field fly? boolean
---@field half_width? number
---@field height? number
---@field eye_height? number
---@field terminal_velocity? number

---@vb context load
---Server pack VM only. Overrides player movement tunables (only the given fields; replaces any earlier call).
---Sent to clients so prediction matches the server.
---```lua
---vb.physics.set_params{ gravity = 20, jump_speed = 9 }
---```
---@param def PhysicsParams
function vb.physics.set_params(def) end

---@vb context runtime
---Server pack VM only. Effective movement tunables (defaults plus pack overrides).
---```lua
---print(vb.physics.get_params().gravity)
---```
---@return PhysicsParams
function vb.physics.get_params() end

---@class vb.combat
vb.combat = {}

---@class CombatParams
---@field hit_radius? number Punch hit radius for players.
---@field player_damage? number Damage per PvP punch.
---@field heal_after_seconds? number Seconds before a hit block starts healing.
---@field heal_interval_seconds? number
---@field punch_cooldown_seconds? number

---@vb context load
---Server pack VM only. Overrides punch/combat tunables.
---```lua
---vb.combat.set_params{ player_damage = 2, punch_cooldown_seconds = 0.4 }
---```
---@param def CombatParams
function vb.combat.set_params(def) end

---@class vb.action
vb.action = {}

---@class ActionParams
---@field reach? number Maximum block interaction distance in blocks.

---@vb context load
---Server pack VM only. Overrides action tunables (block reach).
---```lua
---vb.action.set_params{ reach = 6 }
---```
---@param def ActionParams
function vb.action.set_params(def) end

---@vb context runtime
---Server pack VM only. Effective action tunables.
---```lua
---print(vb.action.get_params().reach)
---```
---@return ActionParams
function vb.action.get_params() end

---@class vb.hunger
vb.hunger = {}

---@class HungerParams
---@field decay_per_second? number
---@field starvation_damage_per_second? number

---@vb context load
---Server pack VM only. Overrides hunger tunables.
---```lua
---vb.hunger.set_params{ decay_per_second = 0.02 }
---```
---@param def HungerParams
function vb.hunger.set_params(def) end

---@vb context runtime
---Server pack VM only. Effective hunger tunables.
---```lua
---print(vb.hunger.get_params().decay_per_second)
---```
---@return HungerParams
function vb.hunger.get_params() end

---@class vb.daynight
vb.daynight = {}

---@class DayNightKeyframe
---@field tick integer Time of day in ticks.
---@field brightness number 0..1.
---@field color {r: integer, g: integer, b: integer} Sky colour.

---@vb context load
---Server pack VM only. Replaces the sky gradient. `keyframes` must be non-empty.
---```lua
---vb.daynight.set_curve{ keyframes = {
---  { tick = 0,    brightness = 0.2, color = { r = 10, g = 10, b = 40 } },
---  { tick = 6000, brightness = 1.0, color = { r = 135, g = 206, b = 235 } },
---} }
---```
---@param def {keyframes: DayNightKeyframe[]}
function vb.daynight.set_curve(def) end

---@vb context load
---Server pack VM only. Real seconds one in-game day takes (> 0).
---```lua
---vb.daynight.set_day_length(600)
---```
---@param seconds number
function vb.daynight.set_day_length(seconds) end

---@class vb.render
vb.render = {}

---@class FogParams
---@field start number Fog start distance.
---@field ["end"] number Fog end distance (`end` is a Lua keyword; quote the key). Must be > start.
---@field underwater_tint? {r: integer, g: integer, b: integer} Optional 0-255 tint when submerged.

---@vb context load
---Server pack VM only. Overrides the distance fog sent to clients.
---```lua
---vb.render.set_fog{ start = 40, ["end"] = 120 }
---```
---@param def FogParams
function vb.render.set_fog(def) end

---@vb context load
---Server pack VM only. Allows (default) or forbids the client's third-person camera (F5), which lets a player see
---their own appearance (`player:set_visual_override`).
---```lua
---vb.render.set_third_person(false)
---```
---@param allowed boolean
function vb.render.set_third_person(allowed) end

---@class vb.config
vb.config = {}

---@vb context runtime
---Server pack VM only. Read-only operator setting from `server.toml`/CLI, or `nil` (unknown key, or singleplayer).
---Keys: bind_address, port, content_pack, max_players, view_distance, tick_rate, world_seed, gravity,
---void_kill_y, day_length_seconds, asset_max_file_mb, asset_max_total_mb, max_connections_per_ip,
---max_messages_per_second, auth_mode, motd.
---```lua
---local motd = vb.config.get("motd")
---```
---@param key string
---@return any
function vb.config.get(key) end
