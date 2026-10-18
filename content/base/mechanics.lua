-- content/base/mechanics.lua -- Growtopia-style discrete punching, in Lua.
--
-- REMAINING_TASKS.md 6.17/6.18: combat used to be Minecraft-style
-- hold-to-break (a fixed 0.35s LMB-hold timer, 100% hardcoded C++, no pack
-- involvement, and no way to hit another player at all). The engine now has
-- zero built-in "holding a button does X" behavior -- src/client/main.cpp
-- only ever reports raw, level-triggered button state (input.buttons.primary
-- = "LMB is down right now", every tick, exactly like buttons.jump/sprint);
-- this file turns the *rising edge* of that into one discrete punch via
-- player:punch(), and the engine decides what the punch actually hit.
--
-- player:punch() (Phase 6.18, src/net/session.cpp's ServerSession::punch())
-- raycasts blocks and nearby players along this player's own authoritative
-- look direction and resolves to whichever is closer -- a block accumulates
-- one hit against BlockType::max_damage (0 = breaks on the first punch,
-- same as every pre-6.18 block); a player takes vb.combat.set_params's
-- default PvP damage. Neither of those policies lives here -- this file's
-- only job is deciding *when* a punch happens (once per click, not once per
-- tick a mouse button happens to still be down).

-- Per-player "was primary/secondary already down last tick" edge state,
-- keyed by player name -- same convention content/examples/kitchen_sink/
-- keybinds.lua already uses (input.keybinds/buttons are level-triggered;
-- PlayerHandle is a fresh wrapper object every call, not a stable table key,
-- so a NetId-or-name string is the only stable thing to index by).
local was_down = {}
local was_secondary_down = {}

-- Every swing, hit or whiff, plays base:player's "swing" clip (entities/
-- player.lua) on the puncher for every client that can see them.
vb.combat.set_params({ swing_clip = "swing" })

-- REMAINING_TASKS.md 6.20: right-click placing used to be the one remaining
-- hardcoded block edit -- src/client/main.cpp sent a C2S_BlockEdit{kPlace}
-- directly off MOUSE_BUTTON_RIGHT with a hardcoded stone block, no pack seam
-- at all (6.17's own writeup flagged this: "Placing (RMB, always instant) is
-- unaffected"). This closes that gap the same way 6.18 closed it for
-- breaking/punching: the client only reports raw input.buttons.secondary,
-- this handler decides *when* (rising edge) and *what* via
-- player:place_block(), and the engine only ever runs the validated edit
-- primitive (apply_script_block_edit -- reach check, hooks, fan-out), same
-- as break_block().
--
-- Entity-management follow-up (REMAINING_TASKS' "held item / hotbar
-- selection" gap): "what" used to be a hardcoded base_stone_id regardless of
-- what the player was even carrying. Now reads player:get_held_item() (the
-- new selected-slot primitive, Phase 6.20) -- an empty/out-of-range slot
-- places nothing at all, and a successful placement spends one unit of the
-- held stack via player:take() the same way crafting already spends
-- ingredients, so placing is a real inventory drain now instead of an
-- infinite stone dispenser.
--
vb.on("player_input", function(player, input)
	local name = player:get_name()
	local down = input.buttons and input.buttons.primary or false
	if down and not was_down[name] then
		player:punch()
	end
	was_down[name] = down

	local secondary = input.buttons and input.buttons.secondary or false
	if secondary and not was_secondary_down[name] then
		local pos = player:get_pos()
		local yaw = math.rad(input.yaw)
		local pitch = math.rad(input.pitch)
		local dir = {
			x = math.sin(yaw) * math.cos(pitch),
			y = math.sin(pitch),
			z = -math.cos(yaw) * math.cos(pitch),
		}
		-- Phase 6.21: reads the engine's *effective* (post pack-override)
		-- eye_height/reach instead of hardcoding a copy of them, so an
		-- override (vb.physics.set_params/vb.action.set_params) can never
		-- silently desync this raycast from the engine's own reach check.
		local eye_height = vb.physics.get_params().eye_height
		local reach = vb.action.get_params().reach --[[@as number]]
		local origin = { x = pos.x, y = pos.y + eye_height, z = pos.z }
		local held = player:get_held_item()
		local hit = held and vb.world.raycast(origin, dir, reach)
		if held and hit then
			local placed = player:place_block(
					hit.x + hit.nx, hit.y + hit.ny, hit.z + hit.nz, held.item)
			if placed then
				player:take({ item = held.item, count = 1 })
			end
		end
	end
	was_secondary_down[name] = secondary
	-- nil: pass the input through unmodified -- this handler only reacts to
	-- state, it never changes movement/look.
end)
