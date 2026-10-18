---@meta
-- Client UI VM: screens and the always-on HUD. Files in a pack's `ui/` folder run here, NOT in the
-- server VM: `vb` is nil in `ui/*.lua`, and `ui`/`client` are nil in server files.

---@alias WidgetType "label"|"panel"|"button"|"textbox"|"list"|"rect"|"text"|"icon"|"sprite"
---@alias Rgba integer[] `{r, g, b, a?}`, 0-255 each.

---@class Widget
---@field id string Unique within the frame.
---@field type WidgetType
---@field x number Absolute pixel position.
---@field y number
---@field w? number
---@field h? number
---@field text? string label/panel/button text, textbox value, or `text` widget content.
---@field items? string[] `list` only.
---@field list_index? integer `list` only (0-based selected row, -1 none).
---@field color? Rgba `rect` fill / `text` colour / `icon` tint.
---@field border? Rgba `rect` outline (default none).
---@field font_size? integer `text` only (default 16).
---@field align? "left"|"center"|"right" `text` only: `x` anchors this edge/point.
---@field item? BlockId `icon` only: block/item id drawn from the atlas.
---@field texture? string `sprite` only: pack-relative PNG path, e.g. `"textures/skins/red.png"`.
---@field src? number[] `sprite` only: `{x, y, w, h}` pixel crop of the image (default: the whole image). `color` tints it.
---@field on_click? fun() Called when a button is clicked.
---@field on_change? fun(value: string|integer) Called when a textbox/list changes.

---@class ScreenLayout
---@field widgets Widget[]
---@field on_close? fun() Local cosmetic cleanup when the screen closes.
---@field capture_mouse_on_close? boolean Recapture the mouse whenever this screen closes (default false).

---@class UiCloseOptions
---@field capture_mouse? boolean true: recapture the mouse once no screen or chat box is open.

---@vb context ui
---Client UI VM only. The UI API table (`vb` does not exist here).
---@class ui
ui = {}

---@vb context ui
---Client UI VM only. Registers a named screen; `render_fn(state)` runs every UI frame while it is open.
---`state` is one table that persists across frames, seeded from `player:open_ui(name, ctx)`.
---`state.login` is engine-owned: a read-only `{ name, subject }` when the server's pack has an auth.lua, else nil.
---```lua
---ui.define("mypack:hello", function(state)
---  return { widgets = {
---    { id = "title", type = "label", x = 20, y = 20, w = 200, h = 24, text = "Hello " .. (state.name or "?") },
---    { id = "ok", type = "button", x = 20, y = 60, w = 80, h = 28, text = "OK", on_click = function() ui.close() end },
---  } }
---end)
---```
---@param name string
---@param render_fn fun(state: table): ScreenLayout
function ui.define(name, render_fn) end

---@vb context ui
---Client UI VM only. Registers the single always-on HUD (evaluated every frame, never opened or closed).
---```lua
---ui.define_hud(function(state)
---  local s = client.screen_size()
---  return { widgets = { { id = "t", type = "text", x = s.width / 2, y = 8, align = "center", text = "hi" } } }
---end)
---```
---@param render_fn fun(state: table): ScreenLayout
function ui.define_hud(render_fn) end

---@vb context ui
---Client UI VM only. Sends a UI event to the server (`ui_event` handler); the screen and widget are filled in automatically.
---```lua
---ui.send_event("buy", { item = "base:wood" })
---```
---@param kind string
---@param value any JSON-compatible.
function ui.send_event(kind, value) end

---@vb context ui
---Client UI VM only. Closes the current screen (always notifies the server with a "close" event).
---By default the mouse stays free; `capture_mouse = true` drops the player back into the game,
---and the click that closed the screen is not sent as a punch. Calling it from the screen's own
---render function is fine: the screen closes right after that render returns.
---```lua
---ui.close()
---ui.close{ capture_mouse = true } -- "back to the game"
---```
---@param opts? UiCloseOptions
function ui.close(opts) end

---@vb context ui
---Client UI VM only. Read-only local client state.
---@class client
client = {}

---@vb context ui
---Client UI VM only. `nil`, or 0..1 while holding to break a block.
---```lua
---local p = client.break_progress()
---```
---@return number|nil
function client.break_progress() end

---@vb context ui
---Client UI VM only. Window size in pixels.
---```lua
---local s = client.screen_size(); print(s.width, s.height)
---```
---@return {width: number, height: number}
function client.screen_size() end

---@vb context ui
---Client UI VM only. Monotonic seconds, for presentation timing.
---```lua
---local pulse = math.sin(client.time() * 4)
---```
---@return number
function client.time() end

---@vb context ui
---Client UI VM only. Cursor position in window pixels.
---```lua
---local m = client.mouse_position()
---```
---@return {x: number, y: number}
function client.mouse_position() end

---@vb context ui
---Client UI VM only. Local player's name.
---```lua
---local me = client.player_name()
---```
---@return string
function client.player_name() end

---@vb context ui
---Client UI VM only. Names of the other connected players.
---```lua
---for _, n in ipairs(client.players()) do print(n) end
---```
---@return string[]
function client.players() end

---@vb context ui
---Client UI VM only. Chat lines, oldest first.
---```lua
---local lines = client.chat_log()
---```
---@return string[]
function client.chat_log() end

---@vb context ui
---Client UI VM only. Whether the chat input is open.
---```lua
---if client.chat_open() then end
---```
---@return boolean
function client.chat_open() end

---@vb context ui
---Client UI VM only. Asks the client to capture (`true`) or release (`false`) the mouse.
---A capture waits until no screen and no chat box are open; the last request wins.
---```lua
---client.capture_mouse(true)
---```
---@param on boolean
function client.capture_mouse(on) end

---@vb context ui
---Client UI VM only. Whether the mouse is captured (looking around) right now.
---```lua
---if not client.mouse_captured() then end
---```
---@return boolean
function client.mouse_captured() end

---@vb context ui
---Client UI VM only. Inventory slots (`item` 0 = empty); live.
---```lua
---for i, s in ipairs(client.inventory()) do print(i, s.name, s.count) end
---```
---@return {name: string, count: integer, item: BlockId}[]
function client.inventory() end

---@vb context ui
---Client UI VM only. 1-based selected hotbar slot.
---```lua
---local slot = client.selected_slot()
---```
---@return integer
function client.selected_slot() end

---@vb context ui
---Client UI VM only. The local player's own appearance as set by `player:set_visual_override`, or `nil` when the pack set none. Only the fields the override sets are present. Feed `texture` / `layers[i].texture` to `sprite` widgets for a wardrobe preview.
---```lua
---local me = client.my_appearance()
---if me and me.layers then print(#me.layers, "layers") end
---```
---@return {texture?: string, frame_width?: integer, frame_height?: integer, facings?: integer, layers?: {texture: string, below: boolean, rows: integer, tint: integer[]}[]}|nil
function client.my_appearance() end

---@vb context ui
---Client UI VM only. Local health, or `nil` before the first status arrives.
---```lua
---local h = client.health(); if h then print(h.current .. "/" .. h.max) end
---```
---@return {current: number, max: number}|nil
function client.health() end

---@vb context ui
---Client UI VM only. Local hunger, or `nil` before the first status arrives.
---```lua
---local h = client.hunger()
---```
---@return {current: number, max: number}|nil
function client.hunger() end
