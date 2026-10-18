-- base:pause -- a Lua-defined pause screen (spec §16/§10.4/§6.2).
--
-- Opened via the Escape key: `content/base/keybinds.lua` registers
-- "base:pause" and calls `player:open_ui("base:pause", {})` on its rising
-- edge (Phase 7.4) -- `src/client/main.cpp`'s `kCustomKeybinds` table binds
-- that name to `KEY_ESCAPE` client-side.
--
-- `render(state)` (Phase 6.2) is called once per UI frame while this screen
-- is open. The only thing it reads from `state` is the engine-owned
-- `state.login` (`{ name, subject }` on a server whose pack has an auth.lua,
-- nil otherwise), for a "Signed in as ..." line. The panel is centered on
-- `client.screen_size()`.
local kPanelW = 240
local kPanelH = 128
local kPad = 16
local kButtonW = 140
local kButtonH = 32

ui.define("base:pause", function(state)
	local screen = client.screen_size()
	local panel_x = math.floor((screen.width - kPanelW) / 2)
	local panel_y = math.floor((screen.height - kPanelH) / 2)

	local widgets = {
		{
			id = "backdrop",
			type = "rect",
			x = 0,
			y = 0,
			w = screen.width,
			h = screen.height,
			color = base_ui.backdrop,
		},
		{
			id = "panel",
			type = "rect",
			x = panel_x,
			y = panel_y,
			w = kPanelW,
			h = kPanelH,
			color = base_ui.panel_bg,
			border = base_ui.panel_border,
		},
		{
			id = "title",
			type = "text",
			x = panel_x + math.floor(kPanelW / 2),
			y = panel_y + kPad,
			align = "center",
			text = "Paused",
			font_size = 20,
			color = base_ui.title,
		},
		{
			id = "resume",
			type = "button",
			x = panel_x + math.floor((kPanelW - kButtonW) / 2),
			y = panel_y + kPanelH - kPad - kButtonH,
			w = kButtonW,
			h = kButtonH,
			text = "Resume",
			on_click = function()
				ui.close()
			end,
		},
	}
	if state.login then
		table.insert(widgets, {
			id = "signed_in",
			type = "text",
			x = panel_x + math.floor(kPanelW / 2),
			y = panel_y + kPad + 30,
			align = "center",
			text = "Signed in as " .. state.login.name,
			font_size = 14,
			color = base_ui.text_muted,
		})
	end
	return { widgets = widgets }
end)
