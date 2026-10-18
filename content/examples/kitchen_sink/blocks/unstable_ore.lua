-- kitchen_sink:unstable_ore -- REMAINING_TASKS.md 6.15's "a max_damage block
-- with a block_break_tick handler" item, plus three more `vb.register_block`
-- fields this same one block can demonstrate at once:
--
-- - `max_damage` (Phase 6.5, docs/lua-api.md's "Shared block-damage
--   breaking"): opts this block into the hold-to-break system instead of
--   today's instant break. The actual damage-per-tick policy lives in
--   `vb.on("block_break_tick", ...)`, a global event, not a per-block
--   field -- see mechanics.lua for that handler.
-- - `max_stack` (Phase 6.9): a wide stack cap, well past the engine's
--   `world::kDefaultMaxStackSize` (64) default, so picking up a pile of
--   these visibly behaves differently from every content/base block.
-- - `pickup_radius`/`item_lifetime_seconds` (Phase 6.11): a dropped
--   unstable_ore is both easier to walk into (wide pickup radius) and much
--   more urgent to grab (short lifetime) than the engine's own
--   1.5/120.0 defaults -- flavor text for "unstable".
kitchen_sink_unstable_ore_id = vb.register_block({
	name = "kitchen_sink:unstable_ore",
	-- Grey rock with glowing specks (16x16, made once with a few lines of Pillow).
	texture = "textures/unstable_ore.png",
	solid = true,
	opaque = true,
	liquid = false,
	light = 2, -- faint glow, distinguishes it from base:stone at a glance
	max_damage = 6,
	max_stack = 999,
	pickup_radius = 4.0,
	item_lifetime_seconds = 20.0,
	on_break = function(ctx)
		vb.world.spawn_item_drop(
			{ x = ctx.pos.x + 0.5, y = ctx.pos.y + 0.5, z = ctx.pos.z + 0.5 },
			kitchen_sink_unstable_ore_id, 1)
	end,
})
