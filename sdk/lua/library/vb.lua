---@meta
-- Voxel Browser server pack API: registration, events, timers, storage.
-- Environment: server pack VM only (init.lua, blocks/, entities/, biomes/, ...).

---@vb context load
---Server pack VM only. The engine API root table.
---@class vb
---@field storage table<string, any> Pack-global persistent store (see `vb.storage`).
vb = {}

---@alias BlockId integer Numeric block id. A held item and a placeable block share one id space.
---@alias EntityKindId integer Numeric entity kind id (registration order, starting at 1).
---@alias Vec3 {x: number, y: number, z: number}
---@alias ItemStack {item: BlockId, count: integer}

---@class BlockDef
---@field name string Namespaced name, e.g. `"mypack:ruby"`. Required. Re-registering a name returns the existing id.
---@field solid? boolean Collides with players (default true).
---@field opaque? boolean Blocks light and hides faces behind it (default true).
---@field liquid? boolean Liquid block (default false).
---@field region? boolean Fire `region_enter`/`region_exit` events (defaults to `liquid`).
---@field light? integer Light emission 0-15 (default 0).
---@field texture? string Pack-relative path, e.g. `"textures/ruby.png"`. Empty = flat placeholder colour. Clear texels are cut out (sprites); mostly half-transparent art is blended (glass).
---@field max_damage? integer Punches needed to break; 0 = instant (default 0).
---@field crack_texture? string Pack-relative crack-stage spritesheet; empty = engine default overlay.
---@field max_stack? integer Inventory stack cap (engine default).
---@field pickup_radius? number Dropped-item pickup radius in blocks (engine default 1.5).
---@field item_lifetime_seconds? number Dropped-item lifetime (engine default 120).
---@field replaceable? boolean A structure with `replace = "air_and_plants"` may overwrite it (default false).
---@field on_break? fun(player: Player, x: integer, y: integer, z: integer) Called after the block is broken.
---@field on_place? fun(player: Player, x: integer, y: integer, z: integer) Called after the block is placed.

---@vb context load
---@vb since 0.1.0
---Server pack VM only. Registers a block type and returns its id.
---Block ids follow registration order and saved worlds store ids, so keep the order stable.
---```lua
---local ruby = vb.register_block{ name = "mypack:ruby", texture = "textures/ruby.png", max_damage = 3 }
---```
---@param def BlockDef
---@return BlockId
function vb.register_block(def) end

---@class ItemDef
---@field name string Item name.

---@vb context load
---Server pack VM only. Registers an item (captured; items share the block id space).
---```lua
---vb.register_item{ name = "mypack:ruby_shard" }
---```
---@param def ItemDef
function vb.register_item(def) end

---@class EntityVisualClip
---@field clip string Clip name.
---@field frames integer Frame count (> 0).
---@field fps number Frames per second (> 0).

---@class EntityVisualLayer
---@field texture string Pack-relative sheet; must be exactly the size of the base `texture`, or it is skipped with a warning.
---@field below? boolean Draw before (behind) the base sheet instead of over it (default false).
---@field rows? integer[] Only draw on these facing rows of the sheet, 0-7 (default every row), e.g. a cape behind the body on front views and over it on back views.
---@field tint? integer[] `{r, g, b}` or `{r, g, b, a}`, 0-255, multiplied into the sheet (default white), so one greyscale sheet serves many dyes.

---@class EntityVisual
---@field variant string One of the frame-size presets, `small` ... `large_flat`.
---@field texture string Pack-relative spritesheet path.
---@field facings? integer 4 or 8 (default 8).
---@field mirror? boolean Flip the authored side pose for the opposite side (default true).
---@field origin? {x: number, y: number} Normalised anchor in a frame (default bottom-centre `{0.5, 1.0}`).
---@field clips EntityVisualClip[] Non-empty animation clip list.
---@field layer? integer Draw order among overlapping sprites, -8..8 (default 0): a higher layer at the same spot always draws in front.
---@field through_walls? boolean Draw over terrain instead of being hidden by it, e.g. markers (default false).
---@field layers? EntityVisualLayer[] Paper-doll sheets drawn in the same billboard as `texture`, in order, with the same frame, facing row and mirroring, so they never drift (max 16).

---@class EntityText
---@field value? string Label text: UTF-8, at most 64 bytes, `\n` starts a new line. Empty hides the label.
---@field color? integer[] Text colour `{r, g, b}` or `{r, g, b, a}`, 0-255 (default white).
---@field background? integer[]|false Rounded panel colour behind the text, RGB(A); without one the text gets a dark outline.
---@field size? number Height of one text line in blocks (default 0.3).
---@field offset_y? number Height of the label's bottom above the entity's position (default the kind's `height` + 0.25; 0 for `visual = false`).
---@field max_distance? number Hide the label beyond this many blocks (default 0 = never).
---@field through_walls? boolean Draw through walls instead of being hidden by them, e.g. name tags (default false).

---@class EntityDef
---@field name string Kind name. Required; idempotent by name.
---@field health? number Max health (> 0); enables `entity:get_health()` and auto-despawn at 0.
---@field width? number Footprint width (default 0.8).
---@field height? number Footprint height (default 1.8).
---@field visual? EntityVisual|false Sprite description; `false` draws no sprite at all (a text-only entity).
---@field text? EntityText Default label style (and optional default `value`) for every instance.
---@field represents? "player"|"item_drop" Render players or dropped items with this kind.
---@field on_spawn? fun(self: Entity)
---@field on_tick? fun(self: Entity, dt: number)
---@field on_hit? fun(self: Entity, attacker: Player|nil, damage: number)
---@field on_death? fun(self: Entity, cause: string|nil)

---@vb context load
---Server pack VM only. Registers an entity kind and returns its id.
---```lua
---vb.register_entity{ name = "mypack:slime", health = 10, on_tick = function(self, dt) end }
---```
---@param def EntityDef
---@return EntityKindId
function vb.register_entity(def) end

---@class BiomeDef
---@field name string Biome name.
---@field surface? string Surface block name.
---@field filler? string Block name under the surface.
---@field stone? string Deep block name.
---@field probability? number Voronoi draw weight (default 1.0).
---@field adjacency? table<string, number> Soft multiplier per neighbouring biome name.
---@field decoration? table[] List of `{structure=, spawn_rate=, ...placement}` or `{spawn_rate=, blocks={{x,y,z,block}}}`.

---@vb context load
---Server pack VM only. Registers a biome (only used once `vb.worldgen.set_pipeline` is called).
---```lua
---vb.register_biome{ name = "mypack:desert", surface = "base:sand", filler = "base:sand", stone = "base:stone" }
---```
---@param def BiomeDef
function vb.register_biome(def) end

---@class StructurePlacement
---@field on? string[] Ground block names the anchor may sit on (empty = any solid).
---@field replace? "air"|"air_and_plants"|"all" What it may overwrite (default "air").
---@field rotate? boolean Allow 90-degree rotation.
---@field mirror? boolean Allow mirroring.
---@field min_spacing? integer Minimum gap between anchors.
---@field max_slope? integer Maximum terrain variation under the footprint.
---@field y_min? integer
---@field y_max? integer
---@field cluster? number 0..1 low-frequency clustering gate.

---@class StructureVariant
---@field weight number Selection weight.
---@field layers string[][] `layers[y][z]` is one row of `size.x` palette characters, bottom layer first.

---@class StructureDef
---@field name string Unique name (a repeat is an error).
---@field size {x: integer, y: integer, z: integer} Each 1..64.
---@field anchor? {x: integer, y: integer, z: integer} Cell that sits on the ground block (default 0,0,0).
---@field palette table<string, string|false> Character -> block name, or `false` to keep the terrain.
---@field variants StructureVariant[]
---@field placement? StructurePlacement Defaults for decoration rules.

---@vb context load
---Server pack VM only. Registers a decorative structure for biome decoration.
---```lua
---vb.register_structure(require("structures.oak_tree"))
---```
---@param def StructureDef
function vb.register_structure(def) end

---@class CraftDef
---@field name? string Recipe name.
---@field ingredients? ItemStack[]
---@field result? ItemStack

---@vb context load
---Server pack VM only. Captures a crafting recipe (the engine does not read it back; `content/base/crafting.lua` is an example consumer).
---```lua
---vb.register_craft{ ingredients = { { item = 1, count = 1 } }, result = { item = 2, count = 4 } }
---```
---@param def CraftDef
function vb.register_craft(def) end

---@vb context load
---Server pack VM only. Declares a custom input slot and returns its bit index (max 32).
---Idempotent by name. The engine names (`jump`, `sprint`, ...) are pre-registered.
---```lua
---vb.register_keybind("dash")
---vb.on("player_input", function(player, input)
---  if input.keybinds.dash then return { move = { z = 2.0 } } end
---end)
---```
---@param name string
---@return integer index
function vb.register_keybind(name) end

---@vb context runtime
---Server pack VM only. Subscribes `handler` to an engine event. Handler arguments depend on the event; see `events.lua`.
---```lua
---vb.on("chat", function(player, text) if text == "boo" then return false end end)
---```
---@param event VbEvent
---@param handler function
---@overload fun(event: "player_join", handler: fun(name: string, login: Login|nil): boolean|nil)
---@overload fun(event: "player_leave", handler: fun(player: Player))
---@overload fun(event: "login_changed", handler: fun(player: Player, login: Login))
---@overload fun(event: "block_break", handler: fun(player: Player, pos: Vec3): boolean|nil)
---@overload fun(event: "block_place", handler: fun(player: Player, pos: Vec3): boolean|nil)
---@overload fun(event: "player_interact", handler: fun(player: Player, target: Vec3): boolean|nil)
---@overload fun(event: "chat", handler: fun(player: Player, text: string): boolean|string|nil)
---@overload fun(event: "tick", handler: fun(dt: number))
---@overload fun(event: "ui_event", handler: fun(player: Player, ui_name: string, widget_id: string, event_kind: string, value: any))
---@overload fun(event: "player_death", handler: fun(player: Player, cause: string|nil, health_before: number): DeathDecision|nil)
---@overload fun(event: "player_input", handler: fun(player: Player, input: PlayerInput): false|PlayerInput|nil)
---@overload fun(event: "block_break_begin", handler: fun(player: Player, pos: Vec3): boolean|nil)
---@overload fun(event: "block_break_tick", handler: fun(player: Player, pos: Vec3, max_damage: integer): number)
---@overload fun(event: "block_health_tick", handler: fun(pos: Vec3, damage: number, max_damage: integer, ticks_since_last_hit: integer): number|nil)
---@overload fun(event: "region_enter", handler: fun(player: Player, pos: Vec3, block_name: string))
---@overload fun(event: "region_exit", handler: fun(player: Player, pos: Vec3, block_name: string))
---@overload fun(event: "player_landed", handler: fun(player: Player, impact_speed: number))
function vb.on(event, handler) end

---@vb context runtime
---Server pack VM only. Runs `fn` once after `seconds`.
---```lua
---vb.after(5, function() print("five seconds later") end)
---```
---@param seconds number
---@param fn fun()
function vb.after(seconds, fn) end

---@vb context runtime
---Server pack VM only. Runs `fn` every `seconds` (catches up on a stalled tick, capped at 8 fires per dispatch).
---```lua
---vb.every(60, function() print("a minute passed") end)
---```
---@param seconds number
---@param fn fun()
function vb.every(seconds, fn) end

---@vb context runtime
---@vb member
---Server pack VM only. Persistent pack-global key/value store, saved to `<pack>/storage.json`.
---Assigning `nil` is allowed; values are JSON-compatible (numbers, strings, booleans, tables).
---```lua
---vb.storage.visits = (vb.storage.visits or 0) + 1
---```
vb.storage = {}
