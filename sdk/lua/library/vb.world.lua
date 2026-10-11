---@meta
-- `vb.world`: runtime world access. Server pack VM only; needs an attached world (runtime, not load time).

---@vb context runtime
---Server pack VM only. World access table.
---@class vb.world
vb.world = {}

---@vb context runtime
---Server pack VM only. Block id at a voxel.
---```lua
---if vb.world.get_block(0, 64, 0) == 0 then print("air") end
---```
---@param x integer
---@param y integer
---@param z integer
---@return BlockId
function vb.world.get_block(x, y, z) end

---@vb context runtime
---Server pack VM only. Sets a block and replicates it. Known gap: does not run the relight cascade.
---```lua
---vb.world.set_block(0, 64, 0, 1)
---```
---@param x integer
---@param y integer
---@param z integer
---@param id BlockId
function vb.world.set_block(x, y, z, id) end

---@class RayHit
---@field hit true
---@field x integer Voxel hit.
---@field y integer
---@field z integer
---@field nx integer Face normal.
---@field ny integer
---@field nz integer

---@vb context runtime
---Server pack VM only. Casts a ray through the voxel world; `nil` when nothing is hit within `max`.
---```lua
---local hit = vb.world.raycast({x=0,y=70,z=0}, {x=0,y=-1,z=0}, 32)
---if hit then print(hit.x, hit.y, hit.z) end
---```
---@param origin Vec3
---@param dir Vec3
---@param max number Maximum distance in blocks.
---@return RayHit|nil
function vb.world.raycast(origin, dir, max) end

---@vb context runtime
---Server pack VM only. Spawns a dropped item that nearby players auto-collect.
---```lua
---vb.world.spawn_item_drop({x=0,y=65,z=0}, 3, 1)
---```
---@param pos Vec3
---@param item BlockId
---@param count integer
function vb.world.spawn_item_drop(pos, item, count) end

---@class SpawnOptions
---@field visual_override? table Per-instance override of the kind's `visual` (every field optional, including `layer`, `through_walls` and `layers`). Change it later with `entity:set_visual_override`.
---@field text? string|EntityText World-space label; fields not given come from the kind's `text`. Change it later with `entity:set_text`.

---@vb context runtime
---Server pack VM only. Spawns an entity of a registered kind and returns its `self` table.
---```lua
---local slime = vb.world.spawn("mypack:slime", {x=0,y=70,z=0})
---local sign = vb.world.spawn("mypack:label", {x=0,y=70,z=0}, { text = { value = "27s", size = 0.4 } })
---```
---@param kind string Registered kind name.
---@param pos Vec3
---@param opts? SpawnOptions
---@return Entity
function vb.world.spawn(kind, pos, opts) end
