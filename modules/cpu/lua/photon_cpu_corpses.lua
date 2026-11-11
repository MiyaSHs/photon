-- photon_cpu_corpses.lua (Photon CPU, R): corpse and dropped-loot settings and cleanup (user, 2026-11-09: corpses that are easier to
-- walk over, easily pushed or not colliding with players; not colliding with each other or props for performance; removing old corpses
-- past a count or after a time, without breaking mods; dropped loot following the corpse settings; a choice to skip drawing them).
-- AI1 (analysis/16): server corpses piled under fire stay awake, with the rifles the dead dropped among them, and their physics grows
-- until the game spirals down to a few frames a second.
-- What counts: a ragdoll the game made for a death (CreateEntityRagdoll, which ragdoll mods use too; on the server with keep corpses /
-- ai_serverragdolls, on the client otherwise: CreateClientsideRagdoll), and the weapon an NPC drops when it dies (its active weapon at
-- OnNPCKilled: Source drops that same entity). Ragdolls spawned from the menu or by tools are never touched; nor is anything held with
-- the physgun or with constraints (ropes, welds) on it; a dropped weapon someone picks up gets its own collision group back and leaves.
-- Settings (data/photon/settings.txt "key value", Photon's settings window; re-read every 5 s, so changes apply at once):
--   photon_cpu_corpse_collide  0 normal | 1 not with each other (default) | 2 not with players or vehicles | 3 only with the world
--                              (corpses and dropped loot alike: "not with each other" also keeps loot from rattling in the pile)
--   photon_cpu_corpse_modfix   1 (default) | 0: finish what ragdoll mods mean to end: Fedhoria's active_ragdoll_controller is meant to
--                              remove itself when the stumble is over (fedhoria_stumble_time), but its check never runs again then, so
--                              the controllers piled up (41 for 23 corpses) and kept every corpse awake forever (laptop, 2026-11-10);
--                              they are removed here 2 s after the stumble time, through the mod's own Remove/OnRemove
--   photon_cpu_corpse_light    0 (default) | 1: corpses at a quarter of the mass (players push them aside; shots move them more)
--   photon_cpu_corpse_max      0 = no limit | N (default 64): past N corpses (and past N dropped weapons) the oldest goes, one a tick
--   photon_cpu_corpse_life     0 = forever (default) | seconds: corpses and dropped loot go after this long
--   photon_cpu_corpse_draw     0 drawn (default) | 1 only near (within 1500 units) | 2 not drawn; per player (client), visual only
-- DYN steps (Photon's frame-rate controller, engaged only under load, always cheaper than the settings above): loot (all dropped
-- loot goes), debris (gameplay-spawned physics props go), corpses64/32/16 (a stricter corpse limit), corpsedraw (only nearby).
-- Tests: the same keys in data/photon_harness.txt (as key=value) win over the settings file. PHOTON_CPU_CORPSES status lines.

if SERVER then AddCSLuaFile() end
local defaults = { collide = 1, light = 0, max = 64, life = 0, draw = 0, modfix = 1 }   -- on by default (user, 2026-11-09): not with each other, 64
local cfg = table.Copy(defaults)
local render_base   -- Photon's "render scale" setting (photon_render_scale, written by Photon Render's menu; nil when absent)
local function read_settings()
	local s = "\n" .. (file.Read("photon/settings.txt", "DATA") or "")
	local h = file.Exists("photon_harness.txt", "DATA") and (file.Read("photon_harness.txt", "DATA") or "") or ""
	local rb = tonumber(h:match("photon_render_scale=([%d%.]+)") or s:match("\nphoton_render_scale[ \t]+([%d%.]+)"))
	render_base = rb and math.Clamp(rb, 0.5, 1) or nil
	for k in pairs(cfg) do
		local key = "photon_cpu_corpse_" .. k
		local v = h:match(key .. "=(%-?%d+)") or s:match("\n" .. key .. "[ \t]+(%-?%d+)")
		cfg[k] = tonumber(v) or defaults[k]
	end
end
read_settings()
timer.Create("photon_cpu_corpses_settings", 5, 0, read_settings)

-- DYN: the steps Photon's frame-rate controller has engaged (photon_cpu.dll, through the gmsv_/gmcl_photon_cpu binary module), read
-- once a tick. Bits as in dyn.c: 0 loot, 1 debris, 2 corpses64, 3 corpses32, 4 corpses16, 5 corpsedraw, 6 tickcap4, 7 tickcap2
-- (photon_cpu.dll applies those two), 8 gpu:renderscale1, 9 gpu:renderscale2. Engaged steps only ever make
-- things cheaper than the user's own settings. Without the module (or photon_cpu.dll) the mask is 0.
-- only when the binary module is installed: a missing one still prints an include error through require, pcall or not
if file.Exists("bin/" .. (SERVER and "gmsv" or "gmcl") .. "_photon_cpu_win64.dll", "LUA") then pcall(require, "photon_cpu") end
local dyn = 0
local function dyn_on(bit) return math.floor(dyn / 2 ^ bit) % 2 == 1 end
local function read_dyn() dyn = (photon_cpu and photon_cpu.DynMask and photon_cpu.DynMask()) or 0 end

if CLIENT then
	-- drawing: corpses and loot the server flagged, and the client's own ragdolls when corpses are client-side
	local NEAR2 = 1500 * 1500
	local function override(self, flags)
		local d = cfg.draw
		if d == 0 and dyn_on(5) then d = 1 end   -- DYN corpsedraw: only nearby, unless the user chose "never"
		if d == 2 then return end
		if d == 1 and self:GetPos():DistToSqr(EyePos()) > NEAR2 then return end
		self:DrawModel(flags)
	end
	-- the override only while a draw-distance choice needs it (the setting, or DYN's corpsedraw step engaged): otherwise every corpse
	-- and dropped weapon would be drawn through Lua for nothing (CPROF, laptop Combine battle: ~250 DrawModel calls a frame from Lua)
	local function need() return cfg.draw ~= 0 or dyn_on(5) end
	local function mark(e)
		if not need() then return end
		if IsValid(e) and not e.photon_corpse and e.RenderOverride == nil then   -- a mod's own override stays
			e.photon_corpse = true
			e.RenderOverride = override
		end
	end
	hook.Add("CreateClientsideRagdoll", "photon_cpu_corpses", function(_, rag) mark(rag) end)
	-- C3e animation stepping (photon_cpu_anim_step, settable by shader packs through Photon.SetFeature): NPCs' and other players'
	-- poses update every N frames (1..8) or at a fixed rate (12..30 poses a second) and hold in between: their animation cycle is held
	-- at the value it had at the last pose tick (set before the frame's bone setup; the engine sets the real cycle again next frame).
	-- Visual only (no bone work saved yet); the local player, its viewmodel and ragdolls are never touched. A sequence change is shown
	-- at once.
	local anim_step, held, frame_n = 0, setmetatable({}, { __mode = "k" }), 0
	timer.Create("photon_cpu_anim_step", 0.5, 0, function()
		anim_step = (photon_cpu and photon_cpu.Setting and photon_cpu.Setting("photon_cpu_anim_step")) or 0
		if anim_step <= 0 and next(held) then held = setmetatable({}, { __mode = "k" }) end
	end)
	local function hold(e, tick)
		if not IsValid(e) or e:IsDormant() then return end
		local seq, h = e:GetSequence(), held[e]
		if not h or h.tick ~= tick or h.seq ~= seq then
			held[e] = { tick = tick, seq = seq, cycle = e:GetCycle() }
		else
			e:SetCycle(h.cycle)
		end
	end
	hook.Add("PreRender", "photon_cpu_anim_step", function()
		frame_n = frame_n + 1
		local n = anim_step
		if n <= 0 then return end
		local tick = n <= 8 and math.floor(frame_n / n) or math.floor(RealTime() * n)
		for _, e in ipairs(ents.FindByClass("npc_*")) do hold(e, tick) end
		local lp = LocalPlayer()
		for _, p in ipairs(player.GetAll()) do if p ~= lp then hold(p, tick) end end
	end)
	-- The 3D view's scale (mat_viewportscale), applied here only (Photon Render's menu writes the setting, never the ConVar):
	-- - nothing engaged: Photon's render scale setting (photon_render_scale) when set, else the user's own mat_viewportscale (a console
	--   change is kept);
	-- - DYN gpu:renderscale1 / 2 (bits 8, 9: Photon Render's GPU steps): 80% / 65% of that base; released, the base comes back.
	-- Set through the binary module (GMod blocks these ConVars from RunConsoleCommand). Below 1, Photon's temporal upscale takes over.
	local cv_s, cv_u = GetConVar("mat_viewportscale"), GetConVar("mat_viewportupscale")
	local user_s, user_u, applied
	local function renderscale()
		if not (cv_s and cv_u and photon_cpu and photon_cpu.SetViewportScale) then return end
		local f = dyn_on(9) and 0.65 or dyn_on(8) and 0.8 or nil
		if not f then
			if applied then photon_cpu.SetViewportScale(user_s, user_u); applied = nil end
			if render_base and math.abs(cv_s:GetFloat() - render_base) > 0.001 then photon_cpu.SetViewportScale(render_base, cv_u:GetFloat()) end
			user_s, user_u = render_base or cv_s:GetFloat(), cv_u:GetFloat()
			return
		end
		if not user_s then user_s, user_u = render_base or cv_s:GetFloat(), cv_u:GetFloat() end   -- engaged from the first frame
		local s = math.max(0.25, user_s * f)
		if applied ~= s then
			local ok = photon_cpu.SetViewportScale(s, 1)
			if ok then applied = s end
			if file.Exists("photon_harness.txt", "DATA") then
				Msg(string.format("PHOTON_CPU_RENDERSCALE set %.2f: %s, mat_viewportscale now %.2f (user %.2f)\n", s, tostring(ok), cv_s:GetFloat(), user_s or -1))
			end
		end
	end
	hook.Add("Think", "photon_cpu_corpses_dyn", function() read_dyn(); renderscale() end)
	-- mat_viewportscale is FCVAR_ARCHIVE: the engine saves it to config.cfg on exit. The user's own value must be back before that
	-- (Photon Render found a 0.65 left in a VM config.cfg): at the client's ShutDown (game exit, map change); and, for a run that
	-- never reached ShutDown, the base is kept in data/photon/renderscale_user.txt while a step is engaged and put back at the next
	-- start if the ConVar still holds one of the steps' exact values (80% / 65% of it).
	local USER_FILE = "photon/renderscale_user.txt"
	local function restore_now()
		if applied and user_s and photon_cpu and photon_cpu.SetViewportScale then photon_cpu.SetViewportScale(user_s, user_u or 1) end
		applied = nil
		if file.Exists(USER_FILE, "DATA") then file.Delete(USER_FILE) end
	end
	hook.Add("ShutDown", "photon_cpu_corpses_renderscale", restore_now)
	local orig_renderscale = renderscale
	renderscale = function()
		local was = applied
		orig_renderscale()
		if applied and not was and user_s then file.CreateDir("photon"); file.Write(USER_FILE, string.format("%.4f %.4f", user_s, user_u or 1)) end
		if was and not applied and file.Exists(USER_FILE, "DATA") then file.Delete(USER_FILE) end
	end
	timer.Simple(2, function()   -- a leftover from a run that did not shut down cleanly
		local t = file.Read(USER_FILE, "DATA")
		if not t or not cv_s or applied then return end
		local us, uu = t:match("^(%S+) (%S+)")
		us, uu = tonumber(us), tonumber(uu)
		local cur = cv_s:GetFloat()
		if us and (math.abs(cur - us * 0.8) < 0.001 or math.abs(cur - us * 0.65) < 0.001) and photon_cpu and photon_cpu.SetViewportScale then
			photon_cpu.SetViewportScale(us, uu or 1)
		end
		file.Delete(USER_FILE)
	end)
	if file.Exists("photon_harness.txt", "DATA") then
		timer.Simple(8, function()
			Msg(string.format("PHOTON_CPU_DYN client: module %s, photon_cpu %s, Loaded %s, mask %s, SetViewportScale %s, ConVars %s/%s, mat_viewportscale %.2f, render scale setting %s\n",
				tostring(file.Exists("bin/gmcl_photon_cpu_win64.dll", "LUA")), tostring(photon_cpu ~= nil),
				tostring(photon_cpu and photon_cpu.Loaded and photon_cpu.Loaded()), tostring(dyn),
				tostring(photon_cpu and photon_cpu.SetViewportScale ~= nil), tostring(cv_s ~= nil), tostring(cv_u ~= nil),
				cv_s and cv_s:GetFloat() or -1, tostring(render_base)))
		end)
	end
	timer.Create("photon_cpu_corpses_mark", 1, 0, function()
		local nd = need()
		for _, e in ipairs(ents.GetAll()) do
			local flagged = e:GetNW2Bool("photon_corpse")
			if nd and flagged then mark(e)
			elseif e.photon_corpse and e.RenderOverride == override and (not nd or not flagged and not e:IsRagdoll()) then
				e.RenderOverride, e.photon_corpse = nil, nil   -- no draw choice in effect, or loot picked up: drawn normally again
			end
		end
	end)
	return
end

local corpses = {}   -- ragdoll -> { t = creation time, n = serial }
local drops = {}     -- dropped weapon -> { t, n, group = its own collision group }
local debris = {}    -- gameplay-spawned physics prop candidates -> { t, n } (DYN "debris")
local serial = 0
local removed = 0
local groups = { [1] = COLLISION_GROUP_INTERACTIVE_DEBRIS, [2] = COLLISION_GROUP_WEAPON, [3] = COLLISION_GROUP_DEBRIS }

local function apply(e, is_corpse)
	if not IsValid(e) then return end
	local g = groups[cfg.collide]
	if g then e:SetCollisionGroup(g) end
	if is_corpse and cfg.light == 1 then
		for i = 0, e:GetPhysicsObjectCount() - 1 do
			local p = e:GetPhysicsObjectNum(i)
			if IsValid(p) then p:SetMass(math.max(p:GetMass() * 0.25, 1)) end
		end
	end
	e:SetNW2Bool("photon_corpse", true)
end

hook.Add("CreateEntityRagdoll", "photon_cpu_corpses", function(owner, rag)
	if not IsValid(rag) or rag:GetClass() ~= "prop_ragdoll" then return end
	serial = serial + 1
	corpses[rag] = { t = CurTime(), n = serial }
	-- next tick: after the game and other mods have set the ragdoll up (a mod that changes the group later keeps its own choice)
	timer.Simple(0, function() apply(rag, true) end)
end)
hook.Add("OnNPCKilled", "photon_cpu_corpses", function(npc)
	local w = IsValid(npc) and npc.GetActiveWeapon and npc:GetActiveWeapon()
	if not IsValid(w) then return end
	serial = serial + 1
	drops[w] = { t = CurTime(), n = serial }
	timer.Simple(0, function()   -- once dropped (no owner)
		if IsValid(w) and drops[w] and not IsValid(w:GetOwner()) then drops[w].group = w:GetCollisionGroup(); apply(w, false) end
	end)
end)
hook.Add("EntityRemoved", "photon_cpu_corpses", function(e) corpses[e] = nil; drops[e] = nil; debris[e] = nil end)

-- debris for the DYN "debris" step: physics props the game spawned during play (gibs, breakable pieces, gore mods' bits). Checked when
-- removed, not when created (a creator is set after spawning): never map-placed (MapCreationID), never with a creator or a
-- prop-protection owner (player-spawned, duplicated or tool-made), never held or constrained, never a tracked corpse.
hook.Add("OnEntityCreated", "photon_cpu_corpses_debris", function(e)
	if not IsValid(e) then return end
	local c = e:GetClass()
	if c ~= "prop_physics" and c ~= "prop_physics_multiplayer" then return end
	serial = serial + 1
	debris[e] = { t = CurTime(), n = serial }
end)
local function gameplay_spawned(e)
	if e:MapCreationID() ~= -1 or IsValid(e:GetCreator()) or corpses[e] then return false end
	if e.CPPIGetOwner then local o = e:CPPIGetOwner(); if o ~= nil and o ~= false then return false end end
	return true
end

local function removable(e)
	if e:IsPlayerHolding() then return false end
	if constraint and constraint.HasConstraints and constraint.HasConstraints(e) then return false end
	return true
end

-- cleanup: at most one corpse, one dropped weapon and one piece of debris a tick, the oldest removable one first. `limit` is the most
-- kept (0 = no limit); `life` the most seconds kept (0 = forever); `check` an extra condition (debris).
local function sweep(set, is_drops, limit, life, check)
	local n, oldest, ot = 0, nil, math.huge
	local now = CurTime()
	for e, c in pairs(set) do
		if not IsValid(e) then
			set[e] = nil
		elseif is_drops and IsValid(e:GetOwner()) then   -- picked up: its own collision group back, no longer tracked
			if c.group then e:SetCollisionGroup(c.group) end
			e:SetNW2Bool("photon_corpse", false)
			set[e] = nil
		elseif check and not check(e) then
			set[e] = nil   -- not debris after all (a player's prop, a map prop)
		else
			n = n + 1
			if c.n < ot and removable(e) then oldest, ot = e, c.n end
			if life > 0 and now - c.t > life and removable(e) then
				e:Remove(); removed = removed + 1
				return
			end
		end
	end
	if limit >= 0 and n > limit and IsValid(oldest) then oldest:Remove(); removed = removed + 1 end
end
local function stricter(user, n) return (user <= 0 or n < user) and n or user end
hook.Add("Tick", "photon_cpu_corpses", function()
	read_dyn()
	local max = cfg.max
	if dyn_on(4) then max = stricter(max, 16) elseif dyn_on(3) then max = stricter(max, 32) elseif dyn_on(2) then max = stricter(max, 64) end
	-- dropped loot: the corpse limits, or none at all while DYN "loot" is engaged; picked-up weapons leave every tick
	sweep(drops, true, dyn_on(0) and 0 or (max > 0 and max or -1), cfg.life)
	if max > 0 or cfg.life > 0 then sweep(corpses, false, max > 0 and max or -1, cfg.life) end
	if dyn_on(1) then sweep(debris, false, 0, 0, gameplay_spawned) end
end)

-- what the DYN steps could act on, once a second, so Photon's controller skips a step with nothing to do (host contract v4)
timer.Create("photon_cpu_corpses_counts", 1, 0, function()
	if not (photon_cpu and photon_cpu.ReportCounts) then return end
	local c, l, d = 0, 0, 0
	for e in pairs(corpses) do if IsValid(e) then c = c + 1 end end
	for e in pairs(drops) do if IsValid(e) and not IsValid(e:GetOwner()) then l = l + 1 end end
	for e in pairs(debris) do if IsValid(e) and gameplay_spawned(e) and removable(e) then d = d + 1 end end
	photon_cpu.ReportCounts(l, d, c)
	-- awake physics objects (what the server's physics time goes to): all, in loot, in debris, and in the corpses each corpse step
	-- would remove (the oldest beyond 64 / 32 / 16), for the controller's expected savings
	if photon_cpu.ReportAwake then
		local function awake(e)
			local n = 0
			for i = 0, e:GetPhysicsObjectCount() - 1 do
				local p = e:GetPhysicsObjectNum(i)
				if IsValid(p) and p:IsMotionEnabled() and not p:IsAsleep() then n = n + 1 end
			end
			return n
		end
		local total = 0
		for _, e in ipairs(ents.GetAll()) do if e:GetPhysicsObjectCount() > 0 then total = total + awake(e) end end
		local al, ad = 0, 0
		for e in pairs(drops) do if IsValid(e) and not IsValid(e:GetOwner()) then al = al + awake(e) end end
		for e in pairs(debris) do if IsValid(e) and gameplay_spawned(e) and removable(e) then ad = ad + awake(e) end end
		local list = {}
		for e, info in pairs(corpses) do if IsValid(e) then list[#list + 1] = { n = info.n, a = awake(e) } end end
		table.sort(list, function(x, y) return x.n > y.n end)   -- newest first
		local function beyond(k) local s = 0; for i = k + 1, #list do s = s + list[i].a end; return s end
		photon_cpu.ReportAwake(total, al, ad, beyond(64), beyond(32), beyond(16))
	end
end)

local fixed = 0
timer.Create("photon_cpu_corpses_modfix", 1, 0, function()
	if cfg.modfix ~= 1 then return end
	local cv = GetConVar("fedhoria_stumble_time")
	local limit = (cv and cv:GetFloat() or 2) + 2
	local now = CurTime()
	for _, c in ipairs(ents.FindByClass("active_ragdoll_controller")) do
		if c.Created and now - c.Created > limit then c:Remove(); fixed = fixed + 1 end
	end
end)

timer.Create("photon_cpu_corpses_agediag", 10, 0, function()   -- test runs: are old corpses still awake, and do ragdoll mods' controllers stay?
	if not file.Exists("photon_harness.txt", "DATA") then return end
	local now, young, yb, old, ob, oc = CurTime(), 0, 0, 0, 0, 0
	for rag, info in pairs(corpses) do
		if IsValid(rag) then
			local aw = 0
			for i = 0, rag:GetPhysicsObjectCount() - 1 do
				local p = rag:GetPhysicsObjectNum(i)
				if IsValid(p) and p:IsMotionEnabled() and not p:IsAsleep() then aw = aw + 1 end
			end
			if now - info.t < 5 then young, yb = young + 1, yb + aw else old, ob = old + 1, ob + aw; if aw > 0 then oc = oc + 1 end end
		end
	end
	print(string.format("PHOTON_CPU_AGEDIAG corpses under 5 s %d (awake bones %d), older %d (awake bones %d, %d of them awake); active_ragdoll_controller %d (ended by modfix %d), puppetmaster %d",
		young, yb, old, ob, oc, #ents.FindByClass("active_ragdoll_controller"), fixed, #ents.FindByClass("puppetmaster")))
end)

timer.Create("photon_cpu_corpses_status", 10, 0, function()
	if not file.Exists("photon_harness.txt", "DATA") then return end   -- status lines only in test runs
	local n, w = 0, 0
	for rag in pairs(corpses) do if IsValid(rag) then n = n + 1 end end
	for e in pairs(drops) do if IsValid(e) then w = w + 1 end end
	print(string.format("PHOTON_CPU_CORPSES corpses %d dropped weapons %d removed %d (collide %d light %d max %d life %d)", n, w,
		removed, cfg.collide, cfg.light, cfg.max, cfg.life))
end)
