-- kitchen_sink:status -- opened by keybinds.lua's player_input handler via
-- `player:open_ui("kitchen_sink:status", {})` (Phase 6.2/6.3, spec §10.4).
-- Loaded client-side only, over Asset Sync for a real connection or straight
-- off disk for --loopback (docs/lua-api.md's "Client wiring" note) --
-- never by src/script/pack_loader.cpp, which only ever sees this pack's
-- server-side files (blocks/entities/biomes/root *.lua/init.lua).
--
-- `render(state)` (Phase 6.2) runs once per UI frame while this screen is
-- open; `state` persists across those frames (seeded once from `open()`'s
-- ctx_json, `{}` here since keybinds.lua passes an empty context table).
ui.define("kitchen_sink:status", function(state)
	return {
		widgets = {
			{
				id = "title",
				type = "label",
				x = 24,
				y = 24,
				w = 260,
				h = 28,
				text = "Kitchen Sink Status",
			},
			{
				id = "body",
				type = "label",
				x = 24,
				y = 56,
				w = 260,
				h = 60,
				text = "This screen opened via a custom keybind\n(Phase 6.3 + player:open_ui, Phase 6.2).",
			},
			{
				id = "close",
				type = "button",
				x = 24,
				y = 124,
				w = 120,
				h = 32,
				text = "Close",
				on_click = function()
					ui.close()
				end,
			},
		},
	}
end)
