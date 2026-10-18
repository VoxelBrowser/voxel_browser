---@meta
-- Event names and handler signatures for `vb.on` (server pack VM only).

---@alias VbEvent
---| "player_join"
---| "player_leave"
---| "login_changed"
---| "block_break"
---| "block_place"
---| "player_interact"
---| "chat"
---| "tick"
---| "ui_event"
---| "player_death"
---| "player_input"
---| "block_break_begin"
---| "block_break_tick"
---| "block_health_tick"
---| "region_enter"
---| "region_exit"
---| "player_landed"

---@class PlayerInput
---@field move Vec3 Requested movement.
---@field yaw number
---@field pitch number
---@field buttons {jump: boolean, sprint: boolean, primary: boolean, secondary: boolean, fly_up: boolean, fly_down: boolean}
---@field keybinds table<string, boolean> Only registered keybind names appear.
---@field dt number Seconds of play this input covers (read-only).

---@class DeathDecision
---@field heal? number New health after respawn.
---@field pos? Vec3 Respawn position.
---@field message? string Private chat line ("" = none).
---@field drop_inventory? boolean Drop every slot at the death position.

---@class Login
---@field provider "oidc"|"keycloak"|"firebase"
---@field subject string Stable account id; key persistent data on this, never on the name.
---@field name string
---@field claims table<string, any> Only the claims allowlisted in `auth.lua`.

-- The per-event overloads live on `vb.on` in vb.lua.
