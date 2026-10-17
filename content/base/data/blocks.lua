-- content/base's block data: pure data, no `vb.*` calls (see
-- docs/structure-editor.md §G). Registered by blocks/register.lua; the
-- structure editor reads this same file to build its palette.
--
-- ORDER MATTERS: block ids are assigned in registration order and saved
-- worlds store ids. This is the order the old per-block files registered in
-- (sorted blocks/*.lua). base:stone, dirt, grass, sand, water, wood and
-- leaves already hold fixed ids from BlockRegistry::base() (src/world/block.cpp)
-- -- registering them again by name is idempotent and only attaches the extra
-- fields (texture, replaceable); planks and sticks are the new ids.
--
-- `drops` is read by blocks/register.lua, not the engine: the block that
-- spawns when this one breaks (default: itself). `drops = false` attaches no
-- on_break at all (water). `texture` is a pack-relative path synced to the
-- client over Asset Sync and packed into its texture atlas.
return {
	-- Terrain fill.
	{ name = "base:dirt", solid = true, opaque = true, texture = "textures/dirt.png" },
	-- Breaking grass drops dirt, the classic block-game convention.
	{ name = "base:grass", solid = true, opaque = true, texture = "textures/grass.png", drops = "base:dirt" },
	-- Solid but non-opaque so light passes and the mesher doesn't cull faces
	-- against it. Structures may overwrite it (`replace = "air_and_plants"`).
	{ name = "base:leaves", solid = true, opaque = false, replaceable = true, texture = "textures/leaves.png" },
	-- Crafted from base:wood (crafting.lua); never from worldgen directly.
	{ name = "base:planks", solid = true, opaque = true, texture = "textures/planks.png" },
	{ name = "base:sand", solid = true, opaque = true, texture = "textures/sand.png" },
	-- Crafted from planks. Non-solid and non-opaque: there is no held-item
	-- concept apart from placeable blocks, so this is the closest to "not a
	-- normal building block".
	{ name = "base:sticks", solid = false, opaque = false, texture = "textures/sticks.png" },
	{ name = "base:stone", solid = true, opaque = true, texture = "textures/stone.png" },
	-- Already hardcoded by BlockRegistry::base(); re-declared only to attach a
	-- texture the C++ default can't carry.
	{ name = "base:water", solid = false, opaque = false, liquid = true, region = true,
		texture = "textures/water.png", drops = false },
	{ name = "base:wood", solid = true, opaque = true, texture = "textures/wood.png" },
}
