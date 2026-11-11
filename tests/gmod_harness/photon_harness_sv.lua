-- photon_harness_sv.lua: server half of the headless test driver (P3e). Active only with data/photon_harness.txt, like the
-- client half. Spawns addon content from STEPS: "photon_sv_give <weapon>", "photon_sv_ent <class> <distance> <side> [white]"
-- (simfphys vehicles go through their own spawner).
if not file.Exists("photon_harness.txt", "DATA") then return end

concommand.Add("photon_sv_give", function(ply, _, a)
	ply = IsValid(ply) and ply or Entity(1)
	local w = ply:Give(a[1])
	ply:SelectWeapon(a[1])
	print(string.format("PHOTON_SV give %s valid=%s", a[1], tostring(IsValid(w))))
end)

-- ARC9's customization screen: its own shared toggle, as its key bind ends up calling
concommand.Add("photon_sv_custom", function(ply, _, a)
	ply = IsValid(ply) and ply or Entity(1)
	local w = ply:GetActiveWeapon()
	local on = a[1] == "1"
	if IsValid(w) and w.ToggleCustomize then w:ToggleCustomize(on) end
	ply:SendLua("local w = LocalPlayer():GetActiveWeapon() if IsValid(w) and w.ToggleCustomize then w:ToggleCustomize(" .. tostring(on) .. ") end")
	print(string.format("PHOTON_SV customize %s on %s", tostring(on), IsValid(w) and w:GetClass() or "-"))
end)

concommand.Add("photon_sv_ent", function(ply, _, a)
	ply = IsValid(ply) and ply or Entity(1)
	local fwd = Angle(0, ply:EyeAngles().y, 0):Forward()
	local pos = ply:GetPos() + fwd * (tonumber(a[2]) or 400) + fwd:Cross(Vector(0, 0, 1)) * (tonumber(a[3]) or 0) + Vector(0, 0, 60)
	local ang = Angle(0, ply:EyeAngles().y + 180 + 30, 0)
	local e
	if simfphys and list.Get("simfphys_vehicles")[a[1]] then
		e = simfphys.SpawnVehicleSimple(a[1], pos, ang)
	else
		e = ents.Create(a[1])
		if IsValid(e) then e:SetPos(pos); e:SetAngles(ang); e:Spawn(); e:Activate() end
	end
	-- "white": a fixed colour for image comparisons (Glide picks a random spawn colour, base_glide/init.lua:843)
	if a[4] == "white" and IsValid(e) then e:SetColor(Color(255, 255, 255)) end
	print(string.format("PHOTON_SV ent %s valid=%s model=%s", a[1], tostring(IsValid(e)), IsValid(e) and tostring(e:GetModel()) or "-"))
end)

-- T4.2 geometry check: "photon_sv_prop <model> x y z": one prop_physics at exactly x y z, upright, frozen
concommand.Add("photon_sv_prop", function(_, _, a)
	local e = ents.Create("prop_physics")
	if not IsValid(e) then return end
	e:SetModel(a[1] or "models/props_junk/wood_crate001a.mdl")
	e:SetPos(Vector(tonumber(a[2]) or 0, tonumber(a[3]) or 0, tonumber(a[4]) or 0))
	e:SetAngles(Angle(0, 0, 0))
	e:Spawn()
	local p = e:GetPhysicsObject()
	if IsValid(p) then p:EnableMotion(false) end
	local c = e:LocalToWorld(e:OBBCenter())
	print(string.format("PHOTON_SV prop %s at %.1f %.1f %.1f obb centre %.1f %.1f %.1f", e:GetModel(), e:GetPos().x, e:GetPos().y, e:GetPos().z, c.x, c.y, c.z))
end)

-- P3h: a prop with a lighting-origin override: "photon_sv_lo <model> x y z ox oy oz": a static-sitting prop_dynamic at x y z lit from a second one at ox oy oz (which must reach the client: same PVS)
concommand.Add("photon_sv_lo", function(_, _, a)
	local function make(x, y, z)
		local e = ents.Create("prop_dynamic")
		e:SetModel(a[1]); e:SetPos(Vector(tonumber(x), tonumber(y), tonumber(z))); e:Spawn()
		return e
	end
	local e = make(a[2], a[3], a[4])
	local src = make(a[5], a[6], a[7])
	e:SetLightingOriginEntity(src)
	print(string.format("PHOTON_SV lighting origin %s at %s %s %s from %s %s %s", tostring(e), a[2], a[3], a[4], a[5], a[6], a[7]))
end)
concommand.Add("photon_sv_plain", function(_, _, a)
	local e = ents.Create("prop_dynamic")
	e:SetModel(a[1]); e:SetPos(Vector(tonumber(a[2]), tonumber(a[3]), tonumber(a[4]))); e:Spawn()
end)

-- Busy scene (laptop frame times, analysis/19): "photon_sv_busy N" drops N awake physics props in a stacked grid on the field west of the
-- regression's first pose (704 132 -143, looking west), a mix of crates, oil drums, barrels and melons, so they fall, collide and settle in view.
local busy_models = { "models/props_junk/wood_crate001a.mdl", "models/props_c17/oildrum001.mdl",
	"models/props_borealis/bluebarrel001.mdl", "models/props_junk/watermelon01.mdl" }
-- "photon_sv_busy N env": props whose materials use $envmap env_cubemap (fridge, stove, metal bucket), so P6 replay's per-copy cubemap pick runs
local busy_models_env = { "models/props_c17/furniturefridge001a.mdl", "models/props_c17/furniturestove001a.mdl", "models/props_junk/metalbucket01a.mdl" }
-- "photon_sv_busy N rag": ragdolls (skinned: more than one bone), which P6 replay must leave to the engine
local busy_models_rag = { "models/headcrabclassic.mdl", "models/gibs/fast_zombie_legs.mdl", "models/zombie/classic_legs.mdl", "models/humans/group01/male_07.mdl" }
-- "photon_sv_busy N npc": NPCs standing in a grid (citizens, zombies, metrocops), animating every frame (skinned, bones change each frame);
-- run with ai_disabled 1 so they stay put and do not fight
local busy_npc = { "npc_citizen", "npc_zombie", "npc_metropolice" }
local busy_n = 0
concommand.Add("photon_sv_busy", function(_, _, a)
	local n = tonumber(a[1]) or 100
	local models = a[2] == "env" and busy_models_env or a[2] == "rag" and busy_models_rag or busy_models
	-- optional base "x y z" after the kind (any kind word, e.g. "prop"): the grid's first corner instead of the field's (L1: a lit room)
	local bx, by, bz = tonumber(a[3]), tonumber(a[4]), tonumber(a[5])
	for i = 1, n do
		local k = busy_n + i - 1
		local col, row, layer = k % 12, math.floor(k / 12) % 12, math.floor(k / 144)
		if a[2] == "npc" then
			local e = ents.Create(busy_npc[k % #busy_npc + 1])
			if IsValid(e) then
				e:SetPos(bx and Vector(bx - row * 45, by + col * 45, bz + layer * 2) or Vector(560 - row * 45, -120 + col * 45, -140 + layer * 2))
				e:SetAngles(Angle(0, 0, 0))
				e:Spawn()
			end
			continue
		end
		local e = ents.Create(a[2] == "rag" and "prop_ragdoll" or "prop_physics")
		e:SetModel(models[k % #models + 1])
		e:SetPos(bx and Vector(bx - row * 45, by + col * 45, bz + layer * 70) or Vector(560 - row * 45, -120 + col * 45, -60 + layer * 70))
		e:SetAngles(Angle(0, (k * 37) % 360, 0))
		e:Spawn()
		local p = e:GetPhysicsObject()
		if IsValid(p) then p:Wake() end
	end
	busy_n = busy_n + n
	print(string.format("PHOTON_BUSY %d props", busy_n))
end)

-- T4.5 study (analysis/21): "photon_sv_hookcount start|stop", server Lua hook calls per tick by name while it runs (see the client half).
local sv_orig, sv_counts, sv_ticks
concommand.Add("photon_sv_hookcount", function(_, _, a)
	if a[1] == "start" and not sv_orig then
		sv_orig, sv_counts, sv_ticks = hook.Call, {}, engine.TickCount()
		hook.Call = function(name, gm, ...)
			sv_counts[name] = (sv_counts[name] or 0) + 1
			return sv_orig(name, gm, ...)
		end
	elseif a[1] == "stop" and sv_orig then
		hook.Call = sv_orig; sv_orig = nil
		local ticks = math.max(1, engine.TickCount() - sv_ticks)
		print(string.format("PHOTON_HOOKS server ticks %d", ticks))
		local names = {}
		for k in pairs(sv_counts) do names[#names + 1] = k end
		table.sort(names, function(x, y) return sv_counts[x] > sv_counts[y] end)
		for _, k in ipairs(names) do print(string.format("PHOTON_HOOKS server %s %.2f per tick", k, sv_counts[k] / ticks)) end
	end
end)

-- "photon_sv_freeze": every prop_physics frozen in place (physics motion off, still drawn), to split the busy scene's physics cost from its
-- draw cost (laptop, analysis/21)
concommand.Add("photon_sv_freeze", function()
	local n = 0
	for _, e in ipairs(ents.FindByClass("prop_physics")) do
		local p = e:GetPhysicsObject()
		if IsValid(p) then p:EnableMotion(false); n = n + 1 end
	end
	print(string.format("PHOTON_FREEZE %d props frozen", n))
end)
