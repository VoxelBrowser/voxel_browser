---@meta
-- Terrain generation pipeline and noise node builders. Server pack VM only, pack-load time.

---@alias NoiseNode table Plain tagged table built by `vb.noise.*` (not an opaque handle).

---@class vb.noise
vb.noise = {}

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Constant noise node.
---```lua
---local flat = vb.noise.constant(64)
---```
---@param value number
---@return NoiseNode
function vb.noise.constant(value) end

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Value noise. The frequency may also be given as `{ frequency = n }`.
---Inside `vb.noise.fbm` leave it at 1: the fbm node's own `frequency` multiplies it.
---```lua
---local n = vb.noise.value(0.01) -- same as vb.noise.value{ frequency = 0.01 }
---```
---@param frequency? number|{frequency?: number} Default 1.0.
---@return NoiseNode
function vb.noise.value(frequency) end

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Cellular (Worley) noise. The frequency may also be given as `{ frequency = n }`.
---```lua
---local cells = vb.noise.cellular(0.02)
---```
---@param frequency? number|{frequency?: number} Default 1.0.
---@return NoiseNode
function vb.noise.cellular(frequency) end

---@class FbmDef
---@field source NoiseNode
---@field frequency? number Default 1.0.
---@field octaves? integer Default 4.
---@field lacunarity? number Default 2.0.
---@field gain? number Default 0.5.

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Fractal Brownian motion over a source node.
---```lua
---local hills = vb.noise.fbm{ source = vb.noise.value(1), frequency = 0.01, octaves = 5 }
---```
---@param def FbmDef
---@return NoiseNode
function vb.noise.fbm(def) end

---@class RemapDef
---@field source NoiseNode
---@field in_min? number Default 0.
---@field in_max? number Default 1.
---@field out_min? number Default 0.
---@field out_max? number Default 1.

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Linearly remaps a node's range.
---```lua
---local height = vb.noise.remap{ source = hills, in_min = -1, in_max = 1, out_min = 50, out_max = 90 }
---```
---@param def RemapDef
---@return NoiseNode
function vb.noise.remap(def) end

---@class CombineDef
---@field a NoiseNode
---@field b NoiseNode
---@field op? "add"|"multiply"|"min"|"max" Default "add".

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Combines two nodes.
---```lua
---local both = vb.noise.combine{ a = hills, b = cells, op = "max" }
---```
---@param def CombineDef
---@return NoiseNode
function vb.noise.combine(def) end

---@class vb.worldgen
vb.worldgen = {}

---@class CarverDef
---@field noise NoiseNode
---@field threshold number
---@field y_min? integer
---@field y_max? integer

---@class VeinDef
---@field block string Ore block name.
---@field target_rock string Block it replaces.
---@field height_min integer
---@field height_max integer
---@field vein_size integer
---@field spawn_rate number

---@class PipelineDef
---@field height NoiseNode Required terrain height node.
---@field base_height? number
---@field amplitude? number
---@field sea_level? integer
---@field soil_depth? integer
---@field beach? string Optional beach block name.
---@field cell_size? integer Biome Voronoi cell size.
---@field carvers? CarverDef[]
---@field veins? VeinDef[]

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Replaces the default terrain with a pack-driven pipeline (compiled once after freeze).
---Registered biomes become Voronoi candidates.
---```lua
---vb.worldgen.set_pipeline{ height = vb.noise.constant(64), sea_level = 60, soil_depth = 4 }
---```
---@param def PipelineDef
function vb.worldgen.set_pipeline(def) end
