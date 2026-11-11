-- photon_cpu_probe.lua (Photon CPU, C2): Lua cost by hook, client and server. Shared autorun (lua/autorun/, both realms).
-- Inert unless data/photon_harness.txt (written per run by the harness runner) holds the line "cpuprobe=1", so normal play and the other
-- session's runs are unaffected.
--   photon_cpu_lua start|stop      (client)   photon_cpu_lua_sv start|stop   (server)
-- start wraps every function in hook.GetTable() and every function of GAMEMODE with a counter and a SysTime timer; stop puts the
-- originals back (only where our wrapper is still in place) and prints per frame (client) or per tick (server):
--   PHOTON_CPU_LUA <realm> window <frames|ticks> <n> seconds <s> gc_kb_per_unit <kb> gc_cycles <n>
--   PHOTON_CPU_LUA <realm> <event> <id> calls <per unit> ms <per unit>      (id "GM" = the gamemode's own function)
-- Times are inclusive: a hook that runs other hooks counts their time too. A wrapper costs about two SysTime calls per call.
-- This wraps the functions, not hook.Call, so hooks the engine dispatches through its cached hook.Call are counted as well.
if not file.Exists("photon_harness.txt", "DATA") then return end
if not (file.Read("photon_harness.txt", "DATA") or ""):match("cpuprobe=1") then return end

local realm = SERVER and "server" or "client"
local SysTime, pairs, isfunction = SysTime, pairs, isfunction
local unit = SERVER and engine.TickCount or FrameNumber
local active, recs, orig_hooks, orig_gm, t0, u0, gc_last, gc_kb, gc_cycles

local function done(r, t, ...)
	r.t = r.t + (SysTime() - t)
	return ...
end
local function wrap(r, f)
	return function(...)
		r.n = r.n + 1
		return done(r, SysTime(), f(...))
	end
end

-- garbage: KB allocated per unit, from the rise of collectgarbage("count") between samples; a drop means a cycle finished
local function gc_sample()
	if not active then return end
	local c = collectgarbage("count")
	if c >= gc_last then gc_kb = gc_kb + (c - gc_last) else gc_cycles = gc_cycles + 1 end
	gc_last = c
end

local function start()
	if active then return end
	active, recs, orig_hooks, orig_gm = true, {}, {}, {}
	if hook.GetULibTable then
		-- ULib replaces the hook library: Hooks[event][priority][name] = { fn = f }; hook.GetTable() is only a backwards copy there
		for ev, pri in pairs(hook.GetULibTable()) do
			for _, tab in pairs(pri) do
				for id, ent in pairs(tab) do
					if istable(ent) and isfunction(ent.fn) then
						local r = { ev = ev, id = tostring(id), n = 0, t = 0 }
						recs[#recs + 1] = r
						local f = ent.fn
						local w = wrap(r, f)
						orig_hooks[#orig_hooks + 1] = { ent, "fn", f, w }
						ent.fn = w
					end
				end
			end
		end
	else
		for ev, tab in pairs(hook.GetTable()) do
			for id, f in pairs(tab) do
				if isfunction(f) then
					local r = { ev = ev, id = tostring(id), n = 0, t = 0 }
					recs[#recs + 1] = r
					local w = wrap(r, f)
					orig_hooks[#orig_hooks + 1] = { tab, id, f, w }
					tab[id] = w
				end
			end
		end
	end
	local gm = GAMEMODE or GM
	if gm then
		for k, f in pairs(gm) do
			if isfunction(f) and isstring(k) then
				local r = { ev = k, id = "GM", n = 0, t = 0 }
				recs[#recs + 1] = r
				local w = wrap(r, f)
				orig_gm[#orig_gm + 1] = { gm, k, f, w }
				gm[k] = w
			end
		end
	end
	-- scripted entities and weapons (C6): the engine calls their methods directly (ENT:Think, SWEP:DrawWorldModel, ...), not through
	-- hooks. Wrap every function of each registered class table; live instances find them through their metatable, so the wrappers
	-- apply to existing entities too. Classes named after their addon folder are easy to attribute.
	local function wrap_class(kind, class, tab)
		if not istable(tab) then return end
		for k, f in pairs(tab) do
			if isfunction(f) and isstring(k) then
				local r = { ev = kind .. ":" .. k, id = class, n = 0, t = 0 }
				recs[#recs + 1] = r
				local w = wrap(r, f)
				orig_gm[#orig_gm + 1] = { tab, k, f, w }
				tab[k] = w
			end
		end
	end
	for _, d in pairs(scripted_ents.GetList()) do
		if d.t then wrap_class("ENT", d.t.ClassName or "?", d.t) end
	end
	for _, d in ipairs(weapons.GetList()) do
		local st = weapons.GetStored(d.ClassName)
		if st then wrap_class("SWEP", d.ClassName, st) end
	end
	-- live entities: GMod copies a class's functions into each entity's own table when it spawns, so entities that exist already call
	-- those copies. Wrap them too, one record per class and method (shared by all instances of a class).
	local inst = {}
	for _, e in ipairs(ents.GetAll()) do
		local t = IsValid(e) and e:GetTable() or nil
		if istable(t) then
			local class = e:GetClass()
			for k, f in pairs(t) do
				if isfunction(f) and isstring(k) then
					local key = class .. "\0" .. k
					local r = inst[key]
					if not r then
						r = { ev = (e:IsWeapon() and "SWEP:" or "ENT:") .. k, id = class, n = 0, t = 0 }
						inst[key] = r
						recs[#recs + 1] = r
					end
					local w = wrap(r, f)
					orig_gm[#orig_gm + 1] = { t, k, f, w }
					t[k] = w
				end
			end
		end
	end
	t0, u0 = SysTime(), unit()
	gc_last, gc_kb, gc_cycles = collectgarbage("count"), 0, 0
	print(string.format("PHOTON_CPU_LUA %s start: %d hook functions (%s hook library), %d gamemode functions wrapped", realm, #orig_hooks,
		hook.GetULibTable and "ULib" or "GMod", #orig_gm))
end

local function stop()
	if not active then return end
	gc_sample()
	active = false
	for _, o in ipairs(orig_hooks) do if o[1][o[2]] == o[4] then o[1][o[2]] = o[3] end end
	for _, o in ipairs(orig_gm) do if o[1][o[2]] == o[4] then o[1][o[2]] = o[3] end end
	local n = math.max(1, unit() - u0)
	print(string.format("PHOTON_CPU_LUA %s window %s %d seconds %.1f gc_kb_per_unit %.2f gc_cycles %d", realm, SERVER and "ticks" or "frames",
		n, SysTime() - t0, gc_kb / n, gc_cycles))
	table.sort(recs, function(a, b) return a.t > b.t end)
	for _, r in ipairs(recs) do
		if r.n > 0 then
			print(string.format("PHOTON_CPU_LUA %s %s %s calls %.2f ms %.4f", realm, r.ev, r.id, r.n / n, 1000 * r.t / n))
		end
	end
end

-- the GC sampler: once per frame (client) or tick (server), outside the wrapped functions
if CLIENT then
	hook.Add("PostRender", "PhotonCpuProbeGC", gc_sample)
	concommand.Add("photon_cpu_lua", function(_, _, a) if a[1] == "start" then start() elseif a[1] == "stop" then stop() end end)
else
	hook.Add("Tick", "PhotonCpuProbeGC", gc_sample)
	concommand.Add("photon_cpu_lua_sv", function(_, _, a) if a[1] == "start" then start() elseif a[1] == "stop" then stop() end end)
end
-- "photon_cpu_flag <name> 0|1": a runtime switch for photon_cpu.dll, which polls data/photon_cpu_<name>.txt (e.g. bones_off: the C3c fix
-- off while the file exists), so one run can show the same frozen frame with a feature on and off
if CLIENT then
	concommand.Add("photon_cpu_flag", function(_, _, a)
		local f = "photon_cpu_" .. (a[1] or "") .. ".txt"
		if a[2] == "1" then file.Write(f, "1") else file.Delete(f) end
		print(string.format("PHOTON_CPU_LUA flag %s %s", a[1] or "?", a[2] == "1" and "on" or "off"))
	end)
end
-- C3c IK gate: "photon_cpu_walk N x1 y1 x2 y2" (server): N citizens walk back and forth between two ground points (z from a trace down),
-- re-ordered every 5 s, so their feet plant on the slope between (foot IK active). Needs ai_disabled 0.
if SERVER then
	local walkers, goal = {}, 1
	local function ground(x, y)
		local tr = util.TraceLine({ start = Vector(x, y, 4000), endpos = Vector(x, y, -4000), mask = MASK_SOLID_BRUSHONLY })
		return tr.HitPos + Vector(0, 0, 8)
	end
	concommand.Add("photon_cpu_walk", function(_, _, a)
		local n = tonumber(a[1]) or 8
		local p1, p2 = ground(tonumber(a[2]) or 600, tonumber(a[3]) or 0), ground(tonumber(a[4]) or 0, tonumber(a[5]) or 0)
		for i = 1, n do
			local e = ents.Create("npc_citizen")
			if IsValid(e) then
				e:SetPos(p1 + Vector(0, (i - n / 2) * 48, 0)); e:Spawn(); e:Activate()
				walkers[#walkers + 1] = e
			end
		end
		local pts = { p1, p2 }
		timer.Create("PhotonCpuWalk", 5, 0, function()
			goal = 3 - goal
			for i, e in ipairs(walkers) do
				if IsValid(e) then
					e:SetLastPosition(pts[goal] + Vector(0, (i - n / 2) * 48, 0))
					e:SetSchedule(SCHED_FORCED_GO)
				end
			end
		end)
		print(string.format("PHOTON_CPU_LUA walk %d citizens between %s and %s", #walkers, tostring(p1), tostring(p2)))
	end)
end
-- C1b / C3c addon gate: "photon_cpu_models N [x y z] [player]" (server): N citizens, every other one wearing a Workshop player model (jiggle bones, eyes)
-- in a row facing +x (the busy-scene camera), and with "player" the local player wears the first model too. The models come from the
-- addon folders the run mounts (modules/cpu/scripts/c1b_bones.sh).
local cpu_models = { "models/player/noire_default.mdl", "models/player/noire_nightwear.mdl", "models/player/noire_swimwear.mdl",
	"models/player/toz_alisha.mdl" }
if SERVER then
	concommand.Add("photon_cpu_models", function(ply, _, a)
		local n, bx, by, bz = tonumber(a[1]) or 12, tonumber(a[2]) or 560, tonumber(a[3]) or -200, tonumber(a[4]) or -140
		local made = 0
		for i = 1, n do
			local e = ents.Create("npc_citizen")
			if IsValid(e) then
				e:SetPos(Vector(bx - math.floor((i - 1) / 12) * 60, by + ((i - 1) % 12) * 45, bz)); e:SetAngles(Angle(0, 0, 0))
				e:Spawn(); e:Activate()
				-- every other one keeps its stock citizen model (no jiggle bones), so the fix is tested next to the Lua bone readers too
				if i % 2 == 0 then e:SetModel(cpu_models[(i / 2 - 1) % #cpu_models + 1]) end
				made = made + 1
			end
		end
		ply = IsValid(ply) and ply or Entity(1)
		if a[5] == "player" and IsValid(ply) then ply:SetModel(cpu_models[1]) end
		print(string.format("PHOTON_CPU_LUA models %d citizens, model %s valid %s", made, cpu_models[1], tostring(util.IsValidModel(cpu_models[1]))))
	end)
end
-- "photon_cpu_luabones 1" (client): Lua that reads and changes bones every frame, as PAC-style addons do. For every NPC: GetAttachment
-- (eyes) and GetBonePosition (head) after the opaque pass; every third NPC gets a BuildBonePositions callback that tilts its spine bone by
-- a fixed angle (time-independent, so a recompute in the same frame gives the same bones); the local player's head is turned by a fixed
-- ManipulateBoneAngles.
if CLIENT then
	local on, reads = false, 0
	concommand.Add("photon_cpu_luabones", function(_, _, a)
		on = a[1] == "1"
		local cb = 0
		for i, e in ipairs(ents.FindByClass("npc_citizen")) do
			if on and i % 3 == 0 and not e.PhotonCpuCB then
				e.PhotonCpuCB = e:AddCallback("BuildBonePositions", function(ent, nb)
					local b = ent:LookupBone("ValveBiped.Bip01_Spine2")
					local m = b and ent:GetBoneMatrix(b)
					if m then m:Rotate(Angle(0, 0, 8)); ent:SetBoneMatrix(b, m) end
				end)
				cb = cb + 1
			end
		end
		local lp = LocalPlayer()
		local hb = IsValid(lp) and lp:LookupBone("ValveBiped.Bip01_Head1")
		if hb then lp:ManipulateBoneAngles(hb, on and Angle(0, 20, 0) or Angle(0, 0, 0)) end
		print(string.format("PHOTON_CPU_LUA luabones %s: %d BuildBonePositions callbacks, player head %s", on and "on" or "off", cb, tostring(hb)))
	end)
	hook.Add("PostDrawOpaqueRenderables", "PhotonCpuLuaBones", function()
		if not on then return end
		for _, e in ipairs(ents.FindByClass("npc_citizen")) do
			local at = e:LookupAttachment("eyes")
			if at and at > 0 then local t = e:GetAttachment(at); if t then reads = reads + 1 end end
			local hb = e:LookupBone("ValveBiped.Bip01_Head1")
			if hb then e:GetBonePosition(hb); reads = reads + 1 end
		end
	end)
end
-- C1b part 2: "photon_cpu_e2 N" (server): N Expression 2 chips in a row in front of the player, each running a 300-step math loop every
-- tick (runOnTick), through Wiremod's own spawn function (MakeWireExpression2, which compiles the code)
if SERVER then
	local e2code = "@name photon bench\nrunOnTick(1)\nN = 0\nfor (I = 1, 300) { N += sin(I * curtime()) }\n"
	concommand.Add("photon_cpu_e2", function(ply, _, a)
		ply = IsValid(ply) and ply or Entity(1)
		if not MakeWireExpression2 then print("PHOTON_CPU_LUA e2: Wiremod not loaded") return end
		local n, made = tonumber(a[1]) or 10, 0
		local fwd = Angle(0, ply:EyeAngles().y, 0):Forward()
		for i = 1, n do
			local pos = ply:GetPos() + fwd * 120 + fwd:Cross(Vector(0, 0, 1)) * ((i - n / 2) * 20) + Vector(0, 0, 4)
			local e = MakeWireExpression2(ply, pos, Angle(0, 0, 0), "models/beer/wiremod/gate_e2.mdl", e2code, "photon bench", { {}, {} }, { {}, {} }, {}, {})
			if IsValid(e) then made = made + 1 end
		end
		print(string.format("PHOTON_CPU_LUA e2 %d of %d chips running", made, n))
	end)
end
-- C7 GC study: "photon_cpu_gclog start|stop" (client) / "photon_cpu_gclog_sv start|stop" (server). Every frame (tick): its time and the
-- Lua heap (collectgarbage("count")). At stop: the collector's pause and step multiplier (read by setting them to the same value), heap
-- min / max, the cycles that finished (the heap drops), and for each the slowest frame among the 5 around the drop's start, next to the
-- window's median frame: whether the 1% low frames coincide with the end of a cycle (the atomic step and the start of the sweep).
do
	local gc_on, ft, heap, t_last
	local function gcparams()
		local pause = collectgarbage("setpause", 200); collectgarbage("setpause", pause)
		local mul = collectgarbage("setstepmul", 200); collectgarbage("setstepmul", mul)
		return pause, mul
	end
	local function gc_sample()
		if not gc_on then return end
		local now = SysTime()
		if t_last then ft[#ft + 1] = (now - t_last) * 1000; heap[#heap + 1] = collectgarbage("count") end
		t_last = now
	end
	local function gc_cmd(_, _, a)
		if a[1] == "start" then gc_on, ft, heap, t_last = true, {}, {}, nil
		elseif a[1] == "stop" and gc_on then
			gc_on = false
			local n = #ft
			if n < 20 then return end
			local sorted = {}
			for i = 1, n do sorted[i] = ft[i] end
			table.sort(sorted)
			local med, p99 = sorted[math.ceil(n / 2)], sorted[math.ceil(n * 0.99)]
			local hmin, hmax = math.huge, 0
			for i = 1, n do hmin = math.min(hmin, heap[i]); hmax = math.max(hmax, heap[i]) end
			local pause, mul = gcparams()
			print(string.format("PHOTON_CPU_GC %s frames %d median %.2f p99 %.2f heap %.0f..%.0f KB pause %d stepmul %d", realm, n, med, p99,
				hmin, hmax, pause, mul))
			local drops, spikes_at_drop, spikes = 0, 0, 0
			for i = 1, n do if ft[i] >= p99 then spikes = spikes + 1 end end
			local i = 2
			while i <= n do
				if heap[i] < heap[i - 1] - 64 then   -- a drop of more than 64 KB: a sweep under way
					drops = drops + 1
					local worst, wi = 0, i
					for j = math.max(1, i - 3), math.min(n, i + 1) do if ft[j] > worst then worst, wi = ft[j], j end end
					if worst >= p99 then spikes_at_drop = spikes_at_drop + 1 end
					if drops <= 12 then
						print(string.format("PHOTON_CPU_GC %s cycle end at frame %d: heap %.0f -> %.0f KB, slowest frame around it %.2f ms",
							realm, i, heap[i - 1], heap[i], worst))
					end
					while i <= n and heap[i] < heap[i - 1] + 1 do i = i + 1 end   -- skip the rest of this sweep
				else
					i = i + 1
				end
			end
			print(string.format("PHOTON_CPU_GC %s sweeps %d, p99 frames %d, of which next to a sweep start %d", realm, drops, spikes,
				spikes_at_drop))
		end
	end
	if CLIENT then
		hook.Add("PostRender", "PhotonCpuGcLog", gc_sample)
		concommand.Add("photon_cpu_gclog", gc_cmd)
	else
		hook.Add("Tick", "PhotonCpuGcLog", gc_sample)
		concommand.Add("photon_cpu_gclog_sv", gc_cmd)
	end
end
-- C6b LuaJIT traces: "photon_cpu_jit start|stop" (client) / "photon_cpu_jit_sv start|stop" (server). jit.attach on the trace events:
-- traces started, completed and aborted; aborts grouped by reason (error number, its message where jit.vmdef has it, the extra value)
-- and by the source line the trace started at. Shows how much hot Lua runs compiled and why the rest is not. jit.status() first.
do
	local on, n_start, n_stop, n_abort, by_reason, by_place
	local vmdef = nil   -- jit.vmdef is not shipped with GMod (requiring it logs an error even inside pcall)
	local function cb(what, tr, func, pc, otr, oex)
		if what == "start" then n_start = n_start + 1
		elseif what == "stop" then n_stop = n_stop + 1
		elseif what == "abort" then
			n_abort = n_abort + 1
			local msg = vmdef and vmdef.traceerr and vmdef.traceerr[otr] or ("err " .. tostring(otr))
			if type(msg) == "string" and oex ~= nil then msg = msg:gsub("%%d", tostring(oex)):gsub("%%s", tostring(oex)) end
			local r = tostring(msg) .. (vmdef and "" or (" / " .. tostring(oex)))
			by_reason[r] = (by_reason[r] or 0) + 1
			local info = func and debug.getinfo(func, "S")
			local place = info and (info.short_src .. ":" .. tostring(info.linedefined)) or "?"
			local key = place .. " | " .. r
			by_place[key] = (by_place[key] or 0) + 1
		end
	end
	local function jit_cmd(_, _, a)
		if not jit or not jit.attach then print("PHOTON_CPU_JIT " .. realm .. ": no jit.attach") return end
		if a[1] == "start" and not on then
			on, n_start, n_stop, n_abort, by_reason, by_place = true, 0, 0, 0, {}, {}
			local st = { jit.status() }
			for i = 1, #st do st[i] = tostring(st[i]) end
			print("PHOTON_CPU_JIT " .. realm .. " status: " .. table.concat(st, " ") .. " (vmdef " .. tostring(vmdef ~= nil) .. ")")
			if not pcall(jit.attach, cb, "trace") then   -- GMod builds LuaJIT with the vmevent API disabled
				print("PHOTON_CPU_JIT " .. realm .. ": jit.attach is disabled in this build (vmevent API); use the native sampler instead")
				on = false
			end
		elseif a[1] == "stop" and on then
			pcall(jit.attach, cb)
			on = false
			print(string.format("PHOTON_CPU_JIT %s traces started %d, completed %d, aborted %d", realm, n_start, n_stop, n_abort))
			local function top(t, n, label)
				local keys = {}
				for k in pairs(t) do keys[#keys + 1] = k end
				table.sort(keys, function(x, y) return t[x] > t[y] end)
				for i = 1, math.min(n, #keys) do print(string.format("PHOTON_CPU_JIT %s %s %6d  %s", realm, label, t[keys[i]], keys[i])) end
			end
			top(by_reason, 12, "reason")
			top(by_place, 20, "place")
		end
	end
	concommand.Add(CLIENT and "photon_cpu_jit" or "photon_cpu_jit_sv", jit_cmd)
end
-- C8 hitches: "photon_cpu_hitch <label>" (client) starts a 4 s window; at its end it prints the window's 5 slowest frames (ms) and the
-- median, so the first appearance of a never-seen model or weapon (spawned in the same step) shows as its worst frame
if CLIENT then
	local hw
	hook.Add("PostRender", "PhotonCpuHitch", function()
		if not hw then return end
		local now = SysTime()
		if hw.last then hw.ft[#hw.ft + 1] = (now - hw.last) * 1000 end
		hw.last = now
		if now - hw.t0 >= 4 then
			local s = {}
			for i = 1, #hw.ft do s[i] = hw.ft[i] end
			table.sort(s, function(a, b) return a > b end)
			local med = #s > 0 and s[math.ceil(#s / 2)] or 0
			print(string.format("PHOTON_CPU_HITCH %s frames %d median %.2f worst %.1f %.1f %.1f %.1f %.1f", hw.label, #s, med,
				s[1] or 0, s[2] or 0, s[3] or 0, s[4] or 0, s[5] or 0))
			hw = nil
		end
	end)
	concommand.Add("photon_cpu_hitch", function(_, _, a) hw = { label = a[1] or "?", t0 = SysTime(), ft = {} } end)
end
-- Phase 2 A1, hook dispatch: "photon_cpu_hooks start|stop" (client; photon_cpu_hooks_sv on the server) swaps hook.Call for a counter
-- for the window and prints, per frame or tick, each event's dispatches and its current number of hooks:
--   PHOTON_CPU_HOOKS <realm> window <frames|ticks> <n> library <GMod|ULib> engine_seen <0|1>
--   PHOTON_CPU_HOOKS <realm> <event> calls <per unit> hooks <n> gm <0|1>
-- engine_seen 1 means the engine's own dispatches (Think, Tick, HUDPaint ...) reached the swapped function, i.e. the engine looks hook.Call
-- up for each event instead of caching it. "photon_cpu_hookbench" (both realms, _sv on the server) times the live hook.Call on private
-- events with 0..20 no-op hooks, plus a plain loop over the same functions (the floor any dispatcher can reach):
--   PHOTON_CPU_HOOKBENCH <realm> <library> hooks <n> call_ns <ns per hook.Call> floor_ns <ns> gm_ns <ns, gamemode function only>
do
	local Hstate
	local function hooks_of(ev)
		local n = 0
		if hook.GetULibTable then
			local t = hook.GetULibTable()[ev]
			if t then for i = -2, 2 do for _ in pairs(t[i] or {}) do n = n + 1 end end end
		else
			for _ in pairs(hook.GetTable()[ev] or {}) do n = n + 1 end
		end
		return n
	end
	local function hstart()
		if Hstate then return end
		local orig = hook.Call
		local cnt = {}
		Hstate = { orig = orig, cnt = cnt, u0 = unit() }
		hook.Call = function(name, ...)
			cnt[name] = (cnt[name] or 0) + 1
			return orig(name, ...)
		end
		Hstate.w = hook.Call
	end
	local function hstop()
		if not Hstate then return end
		if hook.Call == Hstate.w then hook.Call = Hstate.orig end
		local n = math.max(1, unit() - Hstate.u0)
		local gm = GAMEMODE or GM
		local seen = (Hstate.cnt[SERVER and "Tick" or "Think"] or 0) > 0 and 1 or 0
		print(string.format("PHOTON_CPU_HOOKS %s window %s %d library %s engine_seen %d", realm, SERVER and "ticks" or "frames", n,
			hook.GetULibTable and "ULib" or "GMod", seen))
		local evs = {}
		for ev, c in pairs(Hstate.cnt) do evs[#evs + 1] = { ev = ev, c = c } end
		table.sort(evs, function(a, b) return a.c > b.c end)
		for _, e in ipairs(evs) do
			print(string.format("PHOTON_CPU_HOOKS %s %s calls %.2f hooks %d gm %d", realm, tostring(e.ev), e.c / n, hooks_of(e.ev),
				(gm and isfunction(gm[e.ev])) and 1 or 0))
		end
		Hstate = nil
	end
	concommand.Add(CLIENT and "photon_cpu_hooks" or "photon_cpu_hooks_sv", function(_, _, a)
		if a[1] == "start" then hstart() elseif a[1] == "stop" then hstop() end
	end)

	local function noop() end
	local function bench()
		local lib = hook.GetULibTable and "ULib" or "GMod"
		local N = 200000
		local gmt = { PhotonBenchGM = noop }
		for _, h in ipairs({ 0, 1, 2, 5, 10, 20 }) do
			local ev = "PhotonBench" .. h
			local fs = {}
			for i = 1, h do
				local f = function() end
				fs[i] = f
				hook.Add(ev, "photon_bench_" .. i, f)
			end
			local call = hook.Call
			for _ = 1, 1000 do call(ev, nil, 1, 2) end   -- warm up (and let the JIT see it)
			local t = SysTime()
			for _ = 1, N do call(ev, nil, 1, 2) end
			local tc = SysTime() - t
			t = SysTime()
			for _ = 1, N do for i = 1, h do fs[i](1, 2) end end
			local tf = SysTime() - t
			t = SysTime()
			for _ = 1, N do call("PhotonBenchGM", gmt, 1, 2) end
			local tg = SysTime() - t
			print(string.format("PHOTON_CPU_HOOKBENCH %s %s hooks %d call_ns %.1f floor_ns %.1f gm_ns %.1f", realm, lib, h, tc / N * 1e9,
				tf / N * 1e9, tg / N * 1e9))
			for i = 1, h do hook.Remove(ev, "photon_bench_" .. i) end
		end
	end
	concommand.Add(CLIENT and "photon_cpu_hookbench" or "photon_cpu_hookbench_sv", bench)
end
-- A1b: "photon_cpu_hookcount <frames>" (client; _sv on the server, in ticks) counts every dispatch, the engine's included (it calls the
-- hook.Call closure from C, which neither a hook.Call swap nor a debug call hook sees). For the window, the entries of the library's own
-- event table (hook.GetULibTable() for ULib, hook.GetTable() for GMod: the table Call indexes) move to a shadow table, and the emptied
-- table gets a metatable whose __index counts each lookup by event and answers from the shadow (__newindex writes to the shadow). Every
-- dispatch then looks its event up through __index; Add and Remove do too, but they are rare. At the end the entries move back and the
-- metatable goes. During the window, pairs() over that table sees no events: a measurement window only. (GMod has no debug.setupvalue.)
--   PHOTON_CPU_HOOKCOUNT <realm> window <n> dispatches <per unit> empty <per unit, events with no hooks> gm_only <no hooks, a GM function>
--   PHOTON_CPU_HOOKCOUNT <realm> <event> calls <per unit> hooks <n> gm <0|1>
do
	local function hooks_of(ev)
		local n = 0
		if hook.GetULibTable then
			local t = hook.GetULibTable()[ev]
			if t then for i = -2, 2 do for _ in pairs(t[i] or {}) do n = n + 1 end end end
		else
			for _ in pairs(hook.GetTable()[ev] or {}) do n = n + 1 end
		end
		return n
	end
	local function hookcount(_, _, a)
		local want = tonumber(a[1]) or 60
		local real = hook.GetULibTable and hook.GetULibTable() or hook.GetTable()
		if getmetatable(real) ~= nil then print("PHOTON_CPU_HOOKCOUNT " .. realm .. " event table has a metatable, not counting") return end
		local shadow, cnt, u0 = {}, {}, unit()
		for k, v in pairs(real) do shadow[k] = v end
		for k in pairs(shadow) do real[k] = nil end
		setmetatable(real, {
			__index = function(_, k) cnt[k] = (cnt[k] or 0) + 1 return shadow[k] end,
			__newindex = function(_, k, v) shadow[k] = v end,
		})
		local id = "PhotonCpuHookCount" .. realm
		timer.Create(id, 0, 0, function()
			if unit() - u0 < want then return end
			setmetatable(real, nil)
			for k, v in pairs(shadow) do real[k] = v end
			timer.Remove(id)
			local n = unit() - u0
			local gm = GAMEMODE or GM
			local evs, tot, empty, gmonly = {}, 0, 0, 0
			for ev, c in pairs(cnt) do
				local h, g = hooks_of(ev), (gm and isfunction(gm[ev])) and 1 or 0
				evs[#evs + 1] = { ev = ev, c = c, h = h, g = g }
				tot = tot + c
				if h == 0 then if g == 1 then gmonly = gmonly + c else empty = empty + c end end
			end
			table.sort(evs, function(x, y) return x.c > y.c end)
			print(string.format("PHOTON_CPU_HOOKCOUNT %s window %d dispatches %.1f empty %.1f gm_only %.1f", realm, n, tot / n, empty / n,
				gmonly / n))
			for _, e in ipairs(evs) do
				print(string.format("PHOTON_CPU_HOOKCOUNT %s %s calls %.2f hooks %d gm %d", realm, tostring(e.ev), e.c / n, e.h, e.g))
			end
		end)
	end
	concommand.Add(CLIENT and "photon_cpu_hookcount" or "photon_cpu_hookcount_sv", hookcount)
end
-- A1c: "photon_cpu_hooktrace <event>" (client; _sv on the server) adds a one-shot hook that prints the stack it was called from
-- (PHOTON_CPU_HOOKTRACE lines), so the engine's dispatch path is visible: through hook.lua's Call, or straight from C.
concommand.Add(CLIENT and "photon_cpu_hooktrace" or "photon_cpu_hooktrace_sv", function(_, _, a)
	local ev = a[1] or "Think"
	hook.Add(ev, "PhotonCpuHookTrace", function()
		hook.Remove(ev, "PhotonCpuHookTrace")
		for l in debug.traceback("", 1):gmatch("[^\n]+") do print("PHOTON_CPU_HOOKTRACE " .. realm .. " " .. ev .. " " .. l) end
		for i = 1, 6 do
			local info = debug.getinfo(i, "Sf")
			if not info then break end
			print(string.format("PHOTON_CPU_HOOKTRACE %s %s level %d %s %s:%d is_hook_call %s", realm, ev, i, info.what, info.short_src,
				info.linedefined or -1, tostring(info.func == hook.Call)))
		end
	end)
end)
-- GORE bench (shared with Photon Render): "photon_cpu_gore N period [class] [x y]" (server) spawns N NPCs (npc_citizen by default) in a
-- 6-wide grid around x y (default 380 0, gm_construct's field in front of the fixed camera at 704 132 facing 180), and every period
-- seconds kills them all with bullet damage from the player's side (1000 damage, force along the line of fire, hit at chest height),
-- then respawns the wave one period later. "photon_cpu_gore 0" stops and removes them. With ai_serverragdolls 1 the corpses stay as
-- server ragdolls (what active-ragdoll mods act on); each wave's ragdolls are removed when the next wave spawns.
--   PHOTON_CPU_GORE wave <n> killed <k> ragdolls <entities of class prop_ragdoll>
if SERVER then
	local G
	local function gore_clear()
		if not G then return end
		timer.Remove("PhotonCpuGore")
		for _, e in ipairs(G.npcs) do if IsValid(e) then e:Remove() end end
		for _, e in ipairs(ents.FindByClass("prop_ragdoll")) do if IsValid(e) and e.PhotonCpuGore then e:Remove() end end
		G = nil
	end
	hook.Add("CreateEntityRagdoll", "PhotonCpuGore", function(_, rag) if G and IsValid(rag) then rag.PhotonCpuGore = true end end)
	concommand.Add("photon_cpu_gore", function(ply, _, a)
		gore_clear()
		local n = tonumber(a[1]) or 0
		if n <= 0 then print("PHOTON_CPU_GORE stopped") return end
		local period, class = tonumber(a[2]) or 3, a[3] or "npc_citizen"
		local cx, cy = tonumber(a[4]) or 380, tonumber(a[5]) or 0
		G = { npcs = {}, wave = 0, alive = false }
		local function ground(x, y)
			local tr = util.TraceLine({ start = Vector(x, y, 4000), endpos = Vector(x, y, -4000), mask = MASK_SOLID_BRUSHONLY })
			return tr.HitPos + Vector(0, 0, 4)
		end
		local function spawn()
			for _, e in ipairs(ents.FindByClass("prop_ragdoll")) do if IsValid(e) and e.PhotonCpuGore then e:Remove() end end
			G.npcs = {}
			for i = 0, n - 1 do
				local e = ents.Create(class)
				if not IsValid(e) then break end
				e:SetPos(ground(cx - 60 * math.floor(i / 6), cy + 60 * (i % 6 - 2.5)))
				e:SetAngles(Angle(0, 0, 0))
				e:Spawn()
				G.npcs[#G.npcs + 1] = e
			end
			G.alive = true
		end
		local function kill()
			local att = IsValid(ply) and ply or game.GetWorld()
			local from = IsValid(ply) and ply:EyePos() or Vector(704, 132, -80)
			local k = 0
			for _, e in ipairs(G.npcs) do
				if IsValid(e) and e:Health() > 0 then
					local hit = e:GetPos() + Vector(0, 0, 50)
					local dir = (hit - from):GetNormalized()
					local d = DamageInfo()
					d:SetDamage(1000); d:SetDamageType(DMG_BULLET); d:SetAttacker(att); d:SetInflictor(att)
					d:SetDamagePosition(hit); d:SetDamageForce(dir * 30000)
					e:TakeDamageInfo(d)
					k = k + 1
				end
			end
			G.wave = G.wave + 1
			G.alive = false
			timer.Simple(0.5, function()
				if not G then return end
				local r = 0
				for _, e in ipairs(ents.FindByClass("prop_ragdoll")) do if e.PhotonCpuGore then r = r + 1 end end
				print(string.format("PHOTON_CPU_GORE wave %d killed %d ragdolls %d", G.wave, k, r))
			end)
		end
		spawn()
		timer.Create("PhotonCpuGore", period, 0, function()
			if not G then return end
			if G.alive then kill() else spawn() end
		end)
		print(string.format("PHOTON_CPU_GORE start n %d period %.1f class %s", n, period, class))
	end)
end
-- F5 gate: "photon_cpu_fsgate own" (client) checks that the game's own writes are seen at once: a missing folder and file are looked up
-- (so the cache learns they are missing), then created, written, read back and deleted, checking file.Exists / file.IsDir after each
-- step. "photon_cpu_fsgate ext" checks file.Exists("photon_fsgate_ext.txt") once a second and logs each change, for the outside-writer
-- selftest (PHOTON_CPU_FSCACHE_SELFTEST=<s> writes it that many seconds after the cache starts and deletes it 10 s later, without the
-- game's file system).
--   PHOTON_CPU_FSGATE <step> <expected> <got> ok|FAIL
if CLIENT then
	concommand.Add("photon_cpu_fsgate", function(_, _, a)
		local function chk(step, want, got)
			print(string.format("PHOTON_CPU_FSGATE %s %s %s %s", step, tostring(want), tostring(got), want == got and "ok" or "FAIL"))
		end
		if a[1] == "own" then
			local d, f = "photon_fsgate", "photon_fsgate/a.txt"
			file.Delete(f); file.Delete(d)
			chk("dir_missing", false, file.IsDir(d, "DATA"))
			chk("file_missing", false, file.Exists(f, "DATA"))
			chk("file_missing_again", false, file.Exists(f, "DATA"))
			file.CreateDir(d)
			chk("dir_created", true, file.IsDir(d, "DATA"))
			chk("file_still_missing", false, file.Exists(f, "DATA"))
			file.Write(f, "photon")
			chk("file_written", true, file.Exists(f, "DATA"))
			chk("file_read", "photon", file.Read(f, "DATA"))
			chk("file_listed", 1, #(file.Find("photon_fsgate/*.txt", "DATA")))
			file.Delete(f)
			chk("file_deleted", false, file.Exists(f, "DATA"))
			file.Write(f, "again")
			chk("file_rewritten", "again", file.Read(f, "DATA"))
			file.Delete(f); file.Delete(d)
			chk("dir_deleted", false, file.IsDir(d, "DATA"))
		elseif a[1] == "ext" then
			local last, nextt = nil, 0
			hook.Add("PostRender", "PhotonCpuFsGate", function()
				if SysTime() < nextt then return end   -- once a second: frame rates range from 1 (VM) to 200 (laptop)
				nextt = SysTime() + 1
				local e = file.Exists("photon_fsgate_ext.txt", "DATA")
				if e ~= last then print(string.format("PHOTON_CPU_FSGATE ext frame %d exists %s", FrameNumber(), tostring(e))) last = e end
			end)
		end
	end)
end
-- physvalid gate without mods: "photon_cpu_physpoke 1|0" (server) calls IsValid, GetVelocity and AddAngleVelocity(0) on every physics
-- object of every prop_ragdoll each tick (each PhysObj method passes through IsPhysicsObjectValid), and removes a ragdoll every 2 s, so
-- objects come and go while the checks run. PHOTON_CPU_PHYSPOKE ticks <n> calls <n> lines when stopped.
if SERVER then
	local poke
	concommand.Add("photon_cpu_physpoke", function(_, _, a)
		if a[1] == "1" then
			poke = { ticks = 0, calls = 0, t = CurTime() }
			hook.Add("Tick", "PhotonCpuPhysPoke", function()
				poke.ticks = poke.ticks + 1
				local rags = ents.FindByClass("prop_ragdoll")
				for _, r in ipairs(rags) do
					for i = 0, r:GetPhysicsObjectCount() - 1 do
						local ph = r:GetPhysicsObjectNum(i)
						if IsValid(ph) then ph:GetVelocity(); ph:AddAngleVelocity(vector_origin); poke.calls = poke.calls + 3 end
					end
				end
				if CurTime() - poke.t > 2 and rags[1] then rags[1]:Remove(); poke.t = CurTime() end
			end)
		else
			hook.Remove("Tick", "PhotonCpuPhysPoke")
			if poke then print(string.format("PHOTON_CPU_PHYSPOKE ticks %d calls %d", poke.ticks, poke.calls)) end
			poke = nil
		end
	end)
end
-- Phase 2 E1: "photon_cpu_jitwin <seconds>" (client) / "photon_cpu_jitwin_sv <seconds>" (server) profiles that realm's Lua state for
-- the given time with LuaJIT's sampling profiler, when photon_cpu.dll registered photon_cpu_jitprof (PHOTON_CPU_PROBE_JIT=1); the
-- report goes to photon_proxy.log ("jitprof:" lines). One realm at a time.
concommand.Add(CLIENT and "photon_cpu_jitwin" or "photon_cpu_jitwin_sv", function(_, _, a)
	if not photon_cpu_jitprof then print("PHOTON_CPU_JIT " .. realm .. " photon_cpu_jitprof not registered") return end
	local secs = tonumber(a[1]) or 10
	photon_cpu_jitprof("start")
	print("PHOTON_CPU_JIT " .. realm .. " profiling " .. secs .. " s")
	timer.Simple(secs, function() photon_cpu_jitprof("stop") print("PHOTON_CPU_JIT " .. realm .. " stopped") end)
end)
-- C4c gate: "photon_cpu_capture <name>" (client) writes the next rendered frame, lossless, to data/photon_cap_<name>.png
-- (render.Capture in PostRender: the frame as it will be presented, HUD and all if drawn).
if CLIENT then
	local want
	hook.Add("PostRender", "PhotonCpuCapture", function()
		if not want then return end
		local name = want
		want = nil
		local data = render.Capture({ format = "png", x = 0, y = 0, w = ScrW(), h = ScrH(), alpha = false })
		if data then file.Write("photon_cap_" .. name .. ".png", data) end
		print(string.format("PHOTON_CPU_CAPTURE %s %s %d bytes", name, data and "ok" or "failed", data and #data or 0))
		-- the water views' render targets as this frame left them (C4c compares them inside the water rect)
		for _, rt in ipairs({ "_rt_WaterReflection", "_rt_WaterRefraction" }) do
			local tex = GetRenderTarget(rt, ScrW(), ScrH())
			if tex then
				render.PushRenderTarget(tex)
				local d = render.Capture({ format = "png", x = 0, y = 0, w = tex:Width(), h = tex:Height(), alpha = false })
				render.PopRenderTarget()
				if d then file.Write("photon_cap_" .. name .. rt:lower() .. ".png", d) end
				print(string.format("PHOTON_CPU_CAPTURE %s %s %dx%d %s", name, rt, tex:Width(), tex:Height(), d and "ok" or "failed"))
			end
		end
	end)
	concommand.Add("photon_cpu_capture", function(_, _, a) want = (a[1] or "x"):gsub("[^%w_]", "") end)
end
-- C4c gate: "photon_cpu_camera x y z pitch yaw roll fov" (client) fixes the view exactly through CalcView (no player movement involved);
-- "photon_cpu_camera off" releases it. With the game paused, the same command gives the same frame in every run.
if CLIENT then
	local cam
	hook.Add("CalcView", "PhotonCpuCamera", function()
		if not cam then return end
		return { origin = cam.o, angles = cam.a, fov = cam.fov, drawviewer = false }
	end)
	concommand.Add("photon_cpu_camera", function(_, _, a)
		if a[1] == "off" or #a < 6 then cam = nil return end
		cam = { o = Vector(tonumber(a[1]), tonumber(a[2]), tonumber(a[3])), a = Angle(tonumber(a[4]), tonumber(a[5]), tonumber(a[6])),
			fov = tonumber(a[7]) or 75 }
	end)
end
print("PHOTON_CPU_LUA " .. realm .. " probe loaded")
-- AI1: "photon_cpu_battle N [x0 x1 y0]" (server): N Combine soldiers and N armed rebels in two facing lines (x0 and x1, from y0 along
-- +y, 60 units apart), fighting each other (ai_ignoreplayers 1). Every kill is replaced 2 s later with the same class at its own line,
-- so the battle stays the same size for a measurement. "photon_cpu_battle 0" stops the respawning. PHOTON_CPU_BATTLE lines: kills.
if SERVER then
	local battle = { on = false, kills = 0 }
	-- the nearest spot to (x, y, z) where an NPC's hull is free (a spiral of 32-unit steps), so no NPC (or corpse) starts in a wall
	local function free_spot(x, y, z)
		for r = 0, 8 do
			for a = 0, (r == 0 and 0 or 7) do
				local p = Vector(x + math.cos(a * math.pi / 4) * r * 32, y + math.sin(a * math.pi / 4) * r * 32, z + 4)
				local tr = util.TraceHull({ start = p, endpos = p, mins = Vector(-16, -16, 0), maxs = Vector(16, 16, 72), mask = MASK_NPCSOLID })
				if not tr.Hit and not tr.StartSolid then return p end
			end
		end
		return Vector(x, y, z)
	end
	local function spawn(class, x, y, z, wep)
		local e = ents.Create(class)
		if not IsValid(e) then return end
		e:SetPos(free_spot(x, y, z))
		e:SetAngles(Angle(0, class == "npc_combine_s" and 0 or 180, 0))
		if class == "npc_citizen" then e:SetKeyValue("citizentype", "3") end
		e:SetKeyValue("additionalequipment", wep)
		e:Spawn()
		e:Activate()
		e.photon_battle = { class = class, x = x, y = y, z = z, wep = wep }
	end
	hook.Add("OnNPCKilled", "photon_cpu_battle", function(npc)
		local b = npc.photon_battle
		if not b or not battle.on then return end
		battle.kills = battle.kills + 1
		timer.Simple(2, function() if battle.on then spawn(b.class, b.x, b.y, b.z, b.wep) end end)
	end)
	concommand.Add("photon_cpu_battle", function(_, _, a)
		local n = tonumber(a[1]) or 10
		if n <= 0 then battle.on = false; print("PHOTON_CPU_BATTLE stopped, kills " .. battle.kills) return end
		local x0, x1, y0, z = tonumber(a[2]) or 120, tonumber(a[3]) or 560, tonumber(a[4]) or -300, -140
		RunConsoleCommand("ai_ignoreplayers", "1")
		RunConsoleCommand("ai_disabled", "0")
		battle.on, battle.kills = true, 0
		for i = 0, n - 1 do
			spawn("npc_combine_s", x0, y0 + i * 60, z, i % 2 == 0 and "weapon_smg1" or "weapon_ar2")
			spawn("npc_citizen", x1, y0 + i * 60, z, "weapon_ar2")
		end
		print(string.format("PHOTON_CPU_BATTLE %d a side, lines x=%d and x=%d", n, x0, x1))
	end)
	timer.Create("photon_cpu_battle_stats", 10, 0, function()
		if not battle.on then return end
		local n = 0
		for _, e in ipairs(ents.GetAll()) do if e.photon_battle and e:IsNPC() then n = n + 1 end end
		print(string.format("PHOTON_CPU_BATTLE alive %d kills %d", n, battle.kills))
		-- the physics census: entities with physics objects by class, how many awake (the AI1 runs' physics cost grows with them)
		local by = {}
		for _, e in ipairs(ents.GetAll()) do
			local np = e:GetPhysicsObjectCount()
			if np > 0 then
				local c = by[e:GetClass()] or { n = 0, objs = 0, awake = 0 }
				by[e:GetClass()] = c
				c.n = c.n + 1
				for i = 0, np - 1 do
					local p = e:GetPhysicsObjectNum(i)
					if IsValid(p) then c.objs = c.objs + 1; if p:IsAsleep() == false and p:IsMotionEnabled() then c.awake = c.awake + 1 end end
				end
			end
		end
		local rows = {}
		for k, c in pairs(by) do rows[#rows + 1] = { k, c } end
		table.sort(rows, function(a, b) return a[2].objs > b[2].objs end)
		local parts = {}
		for i = 1, math.min(10, #rows) do parts[i] = string.format("%s %d/%d/%d", rows[i][1], rows[i][2].n, rows[i][2].objs, rows[i][2].awake) end
		print("PHOTON_CPU_BATTLE physics (class ents/objects/awake): " .. table.concat(parts, ", "))
	end)
end

-- photon_cpu_waterscan (C4b test scenes): traces straight down every 128 units over the world's bounds and prints where water is (the
-- water's extent per 512-unit cell), so a camera with the water in the PVS but hidden behind buildings can be chosen.
if SERVER then
	concommand.Add("photon_cpu_waterscan", function()
		local w = game.GetWorld()
		local mn, mx = w:GetModelBounds()
		local cells, n = {}, 0
		for x = mn.x, mx.x, 128 do
			for y = mn.y, mx.y, 128 do
				local tr = util.TraceLine({ start = Vector(x, y, mx.z - 8), endpos = Vector(x, y, mn.z + 8), mask = MASK_WATER })
				if tr.Hit and bit.band(util.PointContents(tr.HitPos - Vector(0, 0, 4)), CONTENTS_WATER) ~= 0 then
					local k = math.floor(x / 512) .. "," .. math.floor(y / 512)
					local c = cells[k]
					if not c then c = { x = x, y = y, z = tr.HitPos.z, n = 0 }; cells[k] = c end
					c.n = c.n + 1
					n = n + 1
				end
			end
		end
		print(string.format("PHOTON_CPU_WATERSCAN %d water points over %s .. %s", n, tostring(mn), tostring(mx)))
		for _, c in pairs(cells) do print(string.format("PHOTON_CPU_WATERSCAN cell at %.0f %.0f z %.0f (%d points)", c.x, c.y, c.z, c.n)) end
	end)
end
