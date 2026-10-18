-- REMAINING_TASKS.md 6.15's own worked-example gap: `docs/lua-api.md`'s
-- `biomes/plains.lua`/`forest.lua` row explicitly says "a worked pipeline
-- example belongs to 6.15's separate demo pack" -- this is that example.
-- `vb.worldgen.set_pipeline` (Phase 6.14) replaces `WorldGenerator`'s fixed
-- fBm-heightmap default; loaded after `biomes/savanna.lua`/`tundra.lua`
-- above (root-level files load after `biomes/*.lua`, `src/script/
-- pack_loader.cpp`), so both are already registered when `PackRuntime::
-- build_worldgen_pipeline` reads them back at server startup.
--
-- `vb.noise.*` (Phase 6.14) builds the node-graph description `height`/each
-- carver's `noise` field expects -- plain tagged tables, not opaque handles;
-- see `docs/lua-api.md` for the full node type list.
-- Structure editor S2: a structure is a data file that returns one table (see
-- structures/acacia_tree.lua); biomes/savanna.lua places it by name. Block
-- names inside it are resolved after the whole pack has loaded.
vb.register_structure(require("structures.acacia_tree"))

local base_terrain = vb.noise.fbm({
	-- The source stays at frequency 1: the fbm's own `frequency` below
	-- multiplies it (a source at 1/96 too would mean features ~9000 blocks wide).
	source = vb.noise.value(1),
	octaves = 5,
	lacunarity = 2.0,
	gain = 0.5,
	frequency = 1 / 96,
})

-- A second, higher-frequency cellular layer combined on top, purely to make
-- the pipeline's output visibly different from the plain fBm heightmap
-- content/base's biomes (which never call set_pipeline at all) still use --
-- see docs/lua-api.md's vb.noise.combine entry.
local detail = vb.noise.cellular(1 / 24)
local height_noise = vb.noise.combine({ a = base_terrain, b = detail, op = "add" })

vb.worldgen.set_pipeline({
	height = height_noise,
	base_height = 66,
	amplitude = 24,
	sea_level = 60,
	soil_depth = 4,
	-- Voronoi biome cell size in blocks -- much larger than a chunk (32
	-- blocks) so each biome reads as a real region, not a per-chunk
	-- checkerboard.
	cell_size = 192,
	-- Carvers (spec §6 stage 4): a 3D density threshold that hollows out
	-- rock below sea level into small caves -- `noise` is any vb.noise.*
	-- node, evaluated in 3D (eval3) rather than the 2D height field above.
	carvers = {
		{
			noise = vb.noise.fbm({
				source = vb.noise.value(1),
				octaves = 3,
				frequency = 1 / 16,
			}),
			threshold = 0.78,
			y_min = 0,
			y_max = 55,
		},
	},
	-- Vein/scatter (spec §6 stage 5): kitchen_sink:unstable_ore (blocks/
	-- unstable_ore.lua) replaces base:stone in small clusters underground --
	-- `{block, target_rock, height_min, height_max, vein_size, spawn_rate}`,
	-- the exact shape the spec's own stage-5 description uses.
	veins = {
		{
			block = "kitchen_sink:unstable_ore",
			target_rock = "base:stone",
			height_min = 10,
			height_max = 50,
			vein_size = 6,
			spawn_rate = 1.5, -- expected veins per chunk column
		},
	},
})
