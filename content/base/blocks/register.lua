-- Registers every block in data/blocks.lua. The only file in blocks/, so
-- src/script/pack_loader.cpp runs it first: crafting.lua and the other root
-- modules read the base_*_id globals set here at load time.
--
-- `vb.register_block` is idempotent by name (BlockRegistry::add_or_get), so
-- blocks BlockRegistry::base() already holds keep their ids.

BLOCK_IDS = {}

for _, def in ipairs(require("data.blocks")) do
	-- `on_break` runs after the block is already gone server-side
	-- ({pos = {x,y,z}, player = <Player>}) and drops the item as a real world
	-- entity. It looks the id up at break time, so `drops` may name a block
	-- later in the list.
	if def.drops ~= false then
		def.on_break = def.on_break or function(ctx)
			vb.world.spawn_item_drop(
				{ x = ctx.pos.x + 0.5, y = ctx.pos.y + 0.5, z = ctx.pos.z + 0.5 },
				BLOCK_IDS[def.drops or def.name], 1)
		end
	end
	-- `drops` is this pack's own key (see above), not a BlockDef field: keep it out of the
	-- engine's table (`vb pack check` warns about keys the engine would ignore).
	local block = {}
	for k, v in pairs(def) do
		if k ~= "drops" then
			block[k] = v
		end
	end
	local id = vb.register_block(block)
	BLOCK_IDS[def.name] = id
	-- "base:wood" -> base_wood_id: plain globals, shared by every pack file
	-- (one Lua state), for crafting.lua and others that need an id at load time.
	_G[(def.name:gsub("^base:", "base_")) .. "_id"] = id
end
