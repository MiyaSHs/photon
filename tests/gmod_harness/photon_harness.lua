-- photon_harness.lua: Photon headless test driver (T2.1). Client autorun.
-- Active only when data/photon_harness.txt exists (scripts/run_gmod.sh creates and removes it),
-- so normal play is unaffected. Once in game it prints "PHOTON_HARNESS map loaded <map>",
-- takes one screenshot ("jpeg" -> garrysmod/screenshots/), waits, then quits the game.
-- Timing uses RealTime() from a DrawOverlay hook, not timer.Simple: single-player pauses CurTime
-- while the game UI (console, loading screen) is up, and timers would never fire. Do not launch
-- with -console for the same reason.
if not file.Exists("photon_harness.txt", "DATA") then return end

local cfg = file.Read("photon_harness.txt", "DATA") or ""
local settle = tonumber(cfg:match("settle=(%d+)")) or 10   -- seconds after InitPostEntity before the shot
local linger = tonumber(cfg:match("linger=(%d+)")) or 8    -- seconds after the last shot before quitting
-- Optional steps (T1.8+): run_gmod.sh STEPS="cmd1;;cmd2" defines aliases photon_step1..N in a cfg (Lua may not
-- run most engine commands directly). Step i runs stepgap seconds after the previous shot; a jpeg follows each step.
local steps = tonumber(cfg:match("steps=(%d+)")) or 0
local stepgap = tonumber(cfg:match("stepgap=(%d+)")) or 6
local shotgap = tonumber(cfg:match("shotgap=(%d+)")) or stepgap   -- seconds from a shot to the next step
-- "noshot=1" (Photon CPU, VProf windows): the shots keep their timing and lines but take no jpeg, so no ~350 ms frame lands in a measured window
local noshot = cfg:match("noshot=1") ~= nil
local function shot() if not noshot then RunConsoleCommand("jpeg") end end

print("PHOTON_HARNESS autorun active (settle=" .. settle .. " linger=" .. linger .. ")")

local t_ipe, t_shot, quitting
-- a mode that is still working (suntrace=1) puts a key here, and the harness waits for it to go before it quits
local harness_hold = {}
local step, t_step = 0, nil
-- "steps_persist=1" (P4a): the step count survives map changes (data/photon_harness_step.txt, which the runner deletes before and after), so a step may
-- change the map and the next map goes on with the next step. "stepgaps=g1,g2,...": seconds after step i before its shot (default stepgap).
local stepgaps = {}
for g in (cfg:match("stepgaps=(%S+)") or ""):gmatch("[^,]+") do stepgaps[#stepgaps + 1] = tonumber(g) end
local persist = cfg:match("steps_persist=1") ~= nil
hook.Add("InitPostEntity", "PhotonHarness", function()
	t_ipe = RealTime()
	if persist then
		step = tonumber(file.Read("photon_harness_step.txt", "DATA") or "") or 0
		print(string.format("PHOTON_HARNESS steps continue after step %d on %s", step, game.GetMap()))
	end
	-- the sandbox hint convar only exists once the gamemode is loaded, so the startup cfg cannot set it
	if ConVarExists("cl_showhints") then RunConsoleCommand("cl_showhints", "0") end
	print(string.format("PHOTON_HARNESS InitPostEntity map=%s systime=%.1f", game.GetMap(), SysTime()))
end)

-- Optional debug-quad test (T3.2, analysis/11 §9): data-file line "debugquad=1" (run_gmod.sh HARNESS_OPTS).
-- Creates a Photon_Debug material (override the shader with debugquad_shader=<name>), draws it as a 256x256 screen
-- quad at (64,64) every HUDPaint, and twice (2 s before the shot, debugquad_late s after it) reads the quad centre
-- (192,192) losslessly with render.CapturePixels + render.ReadPixel and prints "PHOTON_DEBUGQUAD r g b"
-- (scripts/run_photon_dbg.sh checks it). The harness LINGER must exceed debugquad_late.
if cfg:match("debugquad=1") then
	local dq_shader = cfg:match("debugquad_shader=(%S+)") or "Photon_Debug"
	local dq_mat, dq_done
	local dq_late = tonumber(cfg:match("debugquad_late=(%d+)")) or 3
	hook.Add("InitPostEntity", "PhotonHarnessDQ", function()
		dq_mat = CreateMaterial("photon_dbg_mat", dq_shader, { ["$photoncolor"] = "[0.2 0.6 1.0]" })
		print(string.format("PHOTON_DEBUGQUAD material requested=%s shader=%s error=%s", dq_shader,
			tostring(dq_mat:GetShader()), tostring(dq_mat:IsError())))
	end)
	hook.Add("HUDPaint", "PhotonHarnessDQ", function()
		if not dq_mat then return end
		render.SetMaterial(dq_mat)
		render.DrawScreenQuadEx(64, 64, 256, 256)
		-- capture 1: 2 s before the shot; capture 2: dq_late s after the shot (queued mode is only active by then:
		-- PROGRESS T1.7, the render thread takes over around the first shot)
		local cap = (not dq_done and t_ipe and RealTime() - t_ipe >= math.max(settle - 2, 1))
			or (dq_done == 1 and t_shot and RealTime() - t_shot >= dq_late)
		if cap then
			dq_done = (dq_done or 0) + 1
			render.CapturePixels()
			local r, g, b = render.ReadPixel(192, 192)
			local r2, g2, b2 = render.ReadPixel(70, 70)
			local r3, g3, b3 = render.ReadPixel(400, 192)
			print(string.format("PHOTON_DEBUGQUAD %d %d %d", r, g, b))
			print(string.format("PHOTON_DEBUGQUAD_EXTRA capture=%d corner(70,70) %d %d %d outside(400,192) %d %d %d frame=%d",
				dq_done, r2, g2, b2, r3, g3, b3, FrameNumber()))
		end
	end)
end

-- Optional order test (T3.6, analysis/13_v1_prereqs.md): data-file line "order=1". From 1 s after the first shot
-- (queued mode is active by then), for order_frames frames, a PostDrawOpaqueRenderables hook draws 4 NoDraw
-- clientside watermelons placed in front of the player, each bracketed by a queued int render parm token:
--   SetLightingMode(2) draw 1, SetLightingMode(1) draw 2, SetLightingMode(2) draw 3, SetLightingMode(1) draw 4,
--   SetLightingMode(0)
-- (render.SetLightingMode = IMatRenderContext::SetIntRenderingParameter(0, m), analysis/05 §4.2). The proxy, run
-- with PHOTON_DIAG=1, logs the tokens and the SetLightingOrigin of each draw as it reaches shaderapi
-- ("ORDER ..." lines); scripts/run_photon_diag.sh matches the origins to the positions printed here.
if cfg:match("order=1") then
	local order_n = tonumber(cfg:match("order_frames=(%d+)")) or 20
	local ents, done_frames, last_frame = nil, 0, -1
	hook.Add("PostDrawOpaqueRenderables", "PhotonHarnessOrder", function(depth, sky)
		if depth or sky or not t_shot or done_frames >= order_n or RealTime() - t_shot < 1 then return end
		local fn = FrameNumber()
		if fn == last_frame then return end
		last_frame = fn
		if not ents then
			ents = {}
			local ply = LocalPlayer()
			local fwd = ply:GetAimVector(); fwd.z = 0; fwd:Normalize()
			local right = fwd:Cross(Vector(0, 0, 1))
			local base = ply:EyePos() + fwd * 150 - Vector(0, 0, 16)
			for i = 1, 4 do
				local e = ClientsideModel("models/props_junk/watermelon01.mdl", RENDERGROUP_OPAQUE)
				e:SetNoDraw(true)
				e:SetPos(base + right * ((i - 2.5) * 48))
				ents[i] = e
				local p = e:GetPos()
				print(string.format("PHOTON_ORDER ent %d pos %.3f %.3f %.3f model=%s", i, p.x, p.y, p.z, tostring(e:GetModel())))
			end
			print(string.format("PHOTON_ORDER cvars mat_queue_mode=%s studio_queue_mode=%s gmod_mcore_test=%s",
				tostring(GetConVarNumber("mat_queue_mode")), tostring(GetConVarNumber("studio_queue_mode")),
				tostring(GetConVarNumber("gmod_mcore_test"))))
		end
		for i = 1, 4 do
			render.SetLightingMode(i % 2 == 1 and 2 or 1)
			ents[i]:DrawModel()
		end
		render.SetLightingMode(0)
		done_frames = done_frames + 1
		print(string.format("PHOTON_ORDER issued %d frame=%d", done_frames, fn))
	end)
end

-- P3h: "amb_mark=1": a read also draws one model at a far position that encodes a number: the proxy's PHOTON_TEST_AMBTRACE rows then show which frame that read was, and the
-- weight at that moment (tools/p3h/trace.py). "PHOTON_P3H mark <n> <tag>" names the number. Position x = -15000, y = -15000 + 32 n, z = 15000.
local amb_mark_q, amb_mark_e, amb_mark_n = {}, nil, 0
local function amb_mark(tag)
	if not cfg:match("amb_mark=1") then return end
	amb_mark_n = amb_mark_n + 1
	amb_mark_q[#amb_mark_q + 1] = amb_mark_n
	print(string.format("PHOTON_P3H mark %d %s frame=%d systime=%.2f", amb_mark_n, tostring(tag), FrameNumber(), SysTime()))
end
if cfg:match("amb_mark=1") then
	hook.Add("PostDrawOpaqueRenderables", "PhotonAmbMark", function(depth, sky)
		if depth or sky or #amb_mark_q == 0 then return end
		if not IsValid(amb_mark_e) then
			amb_mark_e = ClientsideModel("models/props_junk/watermelon01.mdl", RENDERGROUP_OPAQUE)
			amb_mark_e:SetNoDraw(true)
		end
		for _, n in ipairs(amb_mark_q) do
			amb_mark_e:SetPos(Vector(-15000, -15000 + 32 * n, 15000)); amb_mark_e:SetupBones(); amb_mark_e:DrawModel()
		end
		amb_mark_q = {}
	end)
end

-- Optional T3.4f props scene (analysis/14c, A5 step 3/6): data-file line "scene=t34f" (HARNESS_OPTS), optional
-- "scene_pm=<mdl>,<mdl>" (extra models, e.g. workshop player models extracted to garrysmod/addons/ for the run).
-- P1 glow scene: "scene_models=<mdl>,..." replaces the default row, "scene_noovr=1" drops the override model,
-- "scene_dist=" / "scene_gap=" set the row distance (190) and spacing (44), "scene_mask=1" draws the row as flat magenta
-- silhouettes (pixel mask); tools/t34f/glow.sh.
-- The console command "photon_t34f_place <light 0|1>" (use it in a run_gmod.sh step after setpos/setang) places a row
-- of ClientsideModels 1.5 s later in front of the camera, grounded by a trace (so the placement is a pure function of
-- the pose), plus an override model (render.SuppressEngineLighting + a red render.SetModelLighting cube, drawn in
-- PostDrawOpaqueRenderables) in front of the row. With light 1 a DynamicLight (orange, brightness 4, size 350) sits
-- left of and above the row. P2 sets: "photon_t34f_place <light> <set> <ovr>" places row <set> (1 = scene_models +
-- scene_pm, 2 = scene_models2, 3 = scene_models3; distances scene_dist/scene_dist2/scene_dist3) and removes the other
-- sets' models; <ovr> 1 places the override model (scene_ovr=<mdl>, default the wood crate) even with scene_noovr=1,
-- 0 hides it (without the argument the scene_noovr rule applies). 2 s after placing, the mean RGB of a 5x5 block at each model's projected OBB centre
-- and its screen bbox are printed ("PHOTON_T34F model ..."), read losslessly with render.CapturePixels.
if cfg:match("scene=t34f") then
	local list = {
		"models/props_c17/oildrum001.mdl", "models/props_junk/watermelon01.mdl",
		"models/props_borealis/bluebarrel001.mdl", "models/props_junk/trafficcone001a.mdl",
		"models/props_junk/wood_crate001a.mdl", "models/props_c17/canister01a.mdl",
		"models/player/kleiner.mdl",
	}
	-- "scene_models=<mdl>,<mdl>" replaces the default row (P1 glow scene); "scene_noovr=1" drops the override model
	if cfg:match("scene_models=%S+") then list = {} end
	for m in (cfg:match("scene_models=(%S+)") or ""):gmatch("[^,]+") do list[#list + 1] = m end
	for m in (cfg:match("scene_pm=(%S+)") or ""):gmatch("[^,]+") do list[#list + 1] = m end
	local sets, set_dist = { list }, { tonumber(cfg:match("scene_dist=(%d+)")) or 190 }
	for k = 2, 3 do
		sets[k] = {}
		for m in (cfg:match("scene_models" .. k .. "=(%S+)") or ""):gmatch("[^,]+") do sets[k][#sets[k] + 1] = m end
		set_dist[k] = tonumber(cfg:match("scene_dist" .. k .. "=(%d+)")) or set_dist[1]
	end
	local cur_set, ovr_arg = 1, nil
	local noovr = cfg:match("scene_noovr=1") ~= nil
	local mask_mat
	local ovr_model = cfg:match("scene_ovr=(%S+)") or "models/props_junk/wood_crate001a.mdl"
	local ents, ovr, place_at, read_at, light_on, light_pos = {}, nil, nil, nil, false, nil
	local function bbox(e)
		local mn, mx = e:OBBMins(), e:OBBMaxs()
		local x0, y0, x1, y1 = 1e9, 1e9, -1e9, -1e9
		for i = 0, 7 do
			local c = Vector(bit.band(i, 1) > 0 and mx.x or mn.x, bit.band(i, 2) > 0 and mx.y or mn.y, bit.band(i, 4) > 0 and mx.z or mn.z)
			local s = e:LocalToWorld(c):ToScreen()
			x0, y0, x1, y1 = math.min(x0, s.x), math.min(y0, s.y), math.max(x1, s.x), math.max(y1, s.y)
		end
		return math.floor(x0), math.floor(y0), math.ceil(x1), math.ceil(y1)
	end
	local function place()
		local ply = LocalPlayer()
		local eye, ang = ply:EyePos(), ply:EyeAngles()
		local fwd = ang:Forward(); fwd.z = 0; fwd:Normalize()
		local right = fwd:Cross(Vector(0, 0, 1))
		if list ~= sets[cur_set] then
			for _, e in pairs(ents) do if IsValid(e) then e:Remove() end end
			ents, list = {}, sets[cur_set]
		end
		local n, dist, gap = #list, set_dist[cur_set], tonumber(cfg:match("scene_gap=(%d+)")) or 44
		local function ground(e, p)
			local tr = util.TraceLine({ start = p + Vector(0, 0, 40), endpos = p - Vector(0, 0, 600), mask = MASK_SOLID_BRUSHONLY })
			e:SetPos(tr.HitPos - Vector(0, 0, e:OBBMins().z) + Vector(0, 0, 1))
			return tr.HitPos
		end
		for i, m in ipairs(list) do
			local e = ents[i]
			if not IsValid(e) then
				e = ClientsideModel(m, RENDERGROUP_OPAQUE); ents[i] = e
				-- "scene_mask=1": flat magenta UnlitGeneric silhouettes (a pixel mask of the row for tools/t34f/glowstat.py)
				if cfg:match("scene_mask=1") then
					mask_mat = mask_mat or CreateMaterial("photon_mask", "UnlitGeneric", { ["$basetexture"] = "color/white", ["$color2"] = "[1 0 1]" })
					e:SetMaterial("!photon_mask")
				end
				-- characters: player models idle_all_01; HL2 NPC models (P2 set) idle_subtle / lineidle01 / idle01
				for _, q in ipairs({ "idle_all_01", "idle_subtle", "lineidle01", "idle01" }) do
					local s = e:LookupSequence(q); if s and s >= 0 then e:ResetSequence(s); e:SetCycle(0); e.photon_char = true; break end
				end
			end
			e:SetAngles(Angle(0, ang.y + 180 + ((m:find("^models/player/") or e.photon_char) and 0 or 25), 0))
			local g = ground(e, eye + fwd * dist + right * ((i - (n + 1) / 2) * gap))
			local mats = {}
			for _, mt in ipairs(e:GetMaterials()) do
				local M = Material(mt)
				mats[#mats + 1] = string.format("%s[%s bump=%s phong=%s envmap=%s]", mt, M:GetShader(), tostring(M:GetString("$bumpmap") ~= nil),
					tostring(M:GetInt("$phong") or 0), tostring((M:GetString("$envmap"))))
			end
			print(string.format("PHOTON_T34F place %d %s pos %.1f %.1f %.1f ground %.1f err=%s mats %s", i, m, e:GetPos().x, e:GetPos().y,
				e:GetPos().z, g.z, tostring(not util.IsValidModel(m)), table.concat(mats, " ")))
		end
		if ovr_arg == false and IsValid(ovr) then ovr:Remove(); ovr = nil end
		if ovr_arg or (ovr_arg == nil and not noovr) then
			if not IsValid(ovr) then ovr = ClientsideModel(ovr_model, RENDERGROUP_OPAQUE); ovr:SetNoDraw(true) end
			ovr:SetAngles(Angle(0, ang.y + 180 + 25, 0))
			ground(ovr, eye + fwd * (dist * 0.62) + right * 70)
		end
		light_pos = eye + fwd * (dist - 40) - right * 80 + Vector(0, 0, 0)
		light_pos.z = ents[1]:GetPos().z + 60
		local op = IsValid(ovr) and ovr:GetPos() or Vector(0, 0, 0)
		print(string.format("PHOTON_T34F light %s pos %.1f %.1f %.1f; override pos %.1f %.1f %.1f", tostring(light_on), light_pos.x,
			light_pos.y, light_pos.z, op.x, op.y, op.z))
	end
	concommand.Add("photon_t34f_place", function(_, _, args)
		light_on = args[1] == "1"; place_at = RealTime() + 1.5; read_at = nil
		local k = tonumber(args[2] or "1") or 1; if sets[k] and #sets[k] > 0 then cur_set = k end
		if args[3] then ovr_arg = args[3] == "1" end
	end)
	hook.Add("Think", "PhotonT34f", function()
		if place_at and RealTime() >= place_at then place_at = nil; place(); read_at = RealTime() + 2 end
		if light_on and light_pos then
			local d = DynamicLight(4242)
			if d then
				d.pos = light_pos; d.r, d.g, d.b = 255, 170, 90; d.brightness = 4; d.size = 350; d.decay = 0
				d.dietime = CurTime() + 1
			end
		end
	end)
	hook.Add("PostDrawOpaqueRenderables", "PhotonT34f", function(depth, sky)
		if depth or sky or not IsValid(ovr) then return end
		render.SuppressEngineLighting(true)
		for side = 0, 5 do render.SetModelLighting(side, 1, 0, 0) end
		ovr:DrawModel()
		render.SuppressEngineLighting(false)
	end)
	hook.Add("DrawOverlay", "PhotonT34fRead", function()
		if not read_at or RealTime() < read_at then return end
		read_at = nil
		amb_mark("t34f")
		render.CapturePixels()
		local function probe(tag, i, e)
			local c = e:LocalToWorld(e:OBBCenter()):ToScreen()
			local cx, cy, r, g, b, k = math.floor(c.x), math.floor(c.y), 0, 0, 0, 0
			for dy = -2, 2 do for dx = -2, 2 do
				local x, y = cx + dx, cy + dy
				if x >= 0 and y >= 0 and x < ScrW() and y < ScrH() then
					local pr, pg, pb = render.ReadPixel(x, y); r, g, b, k = r + pr, g + pg, b + pb, k + 1
				end
			end end
			k = math.max(k, 1)
			local x0, y0, x1, y1 = bbox(e)
			print(string.format("PHOTON_T34F %s %d %s centre %d %d rgb %.1f %.1f %.1f bbox %d %d %d %d", tag, i, e:GetModel(), cx, cy,
				r / k, g / k, b / k, x0, y0, x1, y1))
		end
		for i, e in ipairs(ents) do if IsValid(e) then probe("model", i, e) end end
		if IsValid(ovr) then probe("override", 0, ovr) end
	end)
end

-- P3h acceptance (analysis/15d section 8): data-file line "scene=p3h". Props at absolute positions, reads at chosen points on them, a settled camera, walks. tools/p3h/ reads the output.
-- data/photon_p3h_plan.txt (scripts/run_p3h_accept.sh writes it): lines "<section> <name> <model> x y z pitch yaw roll <mat: white|- > <probe: line|centre>".
--   photon_p3h_load <section>                  spawns that section's props (the previous ones go unless a second argument "keep")
--   photon_p3h_clear                           removes every prop
--   photon_p3h_read <tag> [frames]             after that many frames: PHOTON_P3H read <tag> <name> <t> wx wy wz sx sy r g b vis frame
--   photon_p3h_cam x y z pitch yaw [fov]       locks the view there through CalcView (no argument: unlocks)
--   photon_p3h_walk <name> <tag> <step> <preroll frames> x y z [x y z...]: the prop moves <step> units a frame along the points, the camera locked 60 above it looking down;
--                                              PHOTON_P3H walk <tag> <i> px py pz r g b (9 x 9 mean at the screen centre, the top of the prop) on every frame, from the preroll's end
--   photon_p3h_rt x y z pitch yaw              draws that view into a target and on a 256 px tile at (8, 8); a read also gives PHOTON_P3H rt <tag> r g b at its centre
--   photon_p3h_every <tag> <n>                 reads every frame for n frames
--   photon_p3h_origin <name> <point name>      SetLightingOriginEntity to a prop at that point
--   photon_p3h_walkcam dx dy dz pitch yaw      P5b: a walk's camera sits at the prop + (dx, dy, dz) with these angles instead of 60 above it looking down,
--                                              so the read at the screen centre is one oblique point of the prop all walk long (no argument: back)
if cfg:match("scene=p3h") then
	local props, reads, cam, walk, rt, every, walkcam = {}, {}, nil, nil, nil, nil, nil
	local rt_tex, rt_mat
	local function white()
		return CreateMaterial("photon_p3h_white", "VertexLitGeneric", { ["$basetexture"] = "color/white", ["$model"] = "1" })
	end
	-- "tree": white, and $treesway, a VS combo no pair of ours builds (TREESWAY=0 only): the draw stays on the stock shader. No wind: strength and speed 0.
	local function tree()
		return CreateMaterial("photon_p3h_tree", "VertexLitGeneric", { ["$basetexture"] = "color/white", ["$model"] = "1", ["$treesway"] = "1",
			["$treeswaystrength"] = "0", ["$treeswayspeed"] = "0", ["$treeswayheight"] = "1000", ["$treeswaystartheight"] = "1" })
	end
	-- P4a: envmapped whites for the cubemap probe, one per pair of ours: "chrome" (vlg), "chromebump" (bump), "chromephong" (skin)
	local function chrome(kind)
		local t = { ["$basetexture"] = "color/white", ["$model"] = "1", ["$envmap"] = "env_cubemap" }
		if kind ~= "chrome" then t["$bumpmap"] = "models/alyx/alyx_sheet_normal" end
		if kind == "chromephong" then t["$phong"] = "1"; t["$phongexponent"] = "20"; t["$phongboost"] = "1" end
		-- P4b cubewalk: "chromeblack" (vlg) and "chromeblackphong" (skin): no diffuse colour, so a read is the reflection alone
		if kind == "chromeblack" or kind == "chromeblackphong" then t["$color"] = "[0 0 0]"; t["$color2"] = "[0 0 0]" end
		if kind == "chromeblack" then t["$bumpmap"] = nil end
		if kind == "chromeblackphong" then t["$phong"] = "1"; t["$phongexponent"] = "20"; t["$phongboost"] = "0" end
		return CreateMaterial("photon_p4a_" .. kind, "VertexLitGeneric", t)
	end
	local function clear()
		for _, p in ipairs(props) do if IsValid(p.e) then p.e:Remove() end end
		props = {}
	end
	concommand.Add("photon_p3h_clear", clear)
	concommand.Add("photon_p3h_load", function(_, _, a)
		if a[2] ~= "keep" then clear() end
		local txt = file.Read("photon_p3h_plan.txt", "DATA") or ""
		for line in txt:gmatch("[^\n]+") do
			local f = string.Explode(" ", line)
			if f[1] == a[1] then
				local e = ClientsideModel(f[3], RENDERGROUP_OPAQUE)
				if IsValid(e) then
					e:SetPos(Vector(tonumber(f[4]), tonumber(f[5]), tonumber(f[6])))
					local yaw = tonumber(f[8])
					if not yaw then   -- "alongx" / "alongy": the model's long axis on that world axis
						local d = e:OBBMaxs() - e:OBBMins()
						local longx = d.x >= d.y
						yaw = ((f[8] == "alongx") == longx) and 0 or 90
					end
					e:SetAngles(Angle(tonumber(f[7]), yaw, tonumber(f[9])))
					if f[10] == "white" then white(); e:SetMaterial("!photon_p3h_white") end
					if f[10] == "tree" then tree(); e:SetMaterial("!photon_p3h_tree") end
					if f[10] and f[10]:match("^chrome") then chrome(f[10]); e:SetMaterial("!photon_p4a_" .. f[10]) end
					for _, q in ipairs({ "idle_all_01", "idle_subtle", "lineidle01", "idle01" }) do
						local sq = e:LookupSequence(q); if sq and sq >= 0 then e:ResetSequence(sq); e:SetCycle(0); break end
					end
					props[#props + 1] = { name = f[2], e = e, probe = f[11] or "centre" }
					print(string.format("PHOTON_P3H prop %s %s pos %.1f %.1f %.1f mins %s maxs %s", f[2], f[3], e:GetPos().x, e:GetPos().y, e:GetPos().z, tostring(e:OBBMins()), tostring(e:OBBMaxs())))
				end
			end
		end
	end)
	-- "steps_map=<map>": the STEPS run only on that map, so a step may change the map (a map mounted from a GMA: photon_p3h_mount <path under garrysmod/>, then "map <name>")
	-- "steps_away=<k>" with it: steps 1 .. k-1 run on that map, and k .. the last on the map it changes to
	hook.Add("InitPostEntity", "PhotonP3hMap", function()
		local home = cfg:match("steps_map=(%S+)")
		local away = tonumber(cfg:match("steps_away=(%d+)"))
		-- "steps_end=<map>": no steps on that map, so the last step may change to it and the run ends there
		if cfg:match("steps_end=(%S+)") == game.GetMap() then steps = 0; print("PHOTON_P3H on map " .. game.GetMap() .. ": the last map, no steps")
		elseif home and game.GetMap() ~= home then
			if away then step = away - 1; print("PHOTON_P3H on map " .. game.GetMap() .. ": steps from " .. away)
			else steps = 0; print("PHOTON_P3H on map " .. game.GetMap() .. ": no steps") end
		elseif home and away then steps = away - 1 end
	end)
	concommand.Add("photon_p3h_mount", function(_, _, a)
		local ok, files = game.MountGMA(a[1])
		print(string.format("PHOTON_P3H mount %s %s, %d files; maps/gm_fork.bsp visible %s", a[1], tostring(ok), files and #files or -1, tostring(file.Exists("maps/gm_fork.bsp", "GAME"))))
	end)
	-- photon_p3h_point <name> x y z: also read this world point on every read (a static prop has no entity here)
	local points = {}
	concommand.Add("photon_p3h_point", function(_, _, a)
		points[#points + 1] = { name = a[1], w = Vector(tonumber(a[2]), tonumber(a[3]), tonumber(a[4])) }
	end)
	concommand.Add("photon_p3h_read", function(_, _, a)
		reads[#reads + 1] = { tag = a[1] or "read", at = FrameNumber() + (tonumber(a[2]) or 0) }
	end)
	concommand.Add("photon_p3h_every", function(_, _, a)
		every = { tag = a[1] or "every", n = tonumber(a[2]) or 10, i = 0 }
	end)
	concommand.Add("photon_p3h_cam", function(_, _, a)
		if #a >= 5 then cam = { pos = Vector(tonumber(a[1]), tonumber(a[2]), tonumber(a[3])), ang = Angle(tonumber(a[4]), tonumber(a[5]), 0), fov = tonumber(a[6]) }
		else cam = nil end
	end)
	concommand.Add("photon_p3h_rt", function(_, _, a)
		if #a >= 5 then rt = { pos = Vector(tonumber(a[1]), tonumber(a[2]), tonumber(a[3])), ang = Angle(tonumber(a[4]), tonumber(a[5]), 0) }
		else rt = nil end
	end)
	concommand.Add("photon_p3h_origin", function(_, _, a)
		for _, p in ipairs(props) do
			if p.name == a[1] then
				for _, q in ipairs(props) do
					if q.name == a[2] then
						p.e:SetLightingOriginEntity(q.e)
						print("PHOTON_P3H lighting origin " .. a[1] .. " -> " .. a[2])
					end
				end
			end
		end
	end)
	-- photon_p3h_walkfix: as photon_p3h_walk, but the view stays where photon_p3h_cam put it, and the read is at the prop's projected centre
	local function walk_cmd(a, fixed)
		local w = { name = a[1], tag = a[2], step = tonumber(a[3]), pre = tonumber(a[4]), pts = {}, i = 0, seg = 1, dist = 0, fixed = fixed }
		for k = 5, #a - 2, 3 do w.pts[#w.pts + 1] = Vector(tonumber(a[k]), tonumber(a[k + 1]), tonumber(a[k + 2])) end
		-- name "all" (P4a): every loaded prop moves together, on the same point
		for _, p in ipairs(props) do if p.name == w.name or (w.name == "all" and not w.e) then w.e = p.e end end
		if w.name == "all" then w.all = {}; for _, p in ipairs(props) do w.all[#w.all + 1] = p.e end end
		if not w.e or #w.pts < 2 then print("PHOTON_P3H walk: bad arguments"); return end
		w.e:SetPos(w.pts[1])
		for _, e in ipairs(w.all or {}) do e:SetPos(w.pts[1]) end
		walk = w
	end
	concommand.Add("photon_p3h_walkcam", function(_, _, a)
		if #a >= 5 then walkcam = { off = Vector(tonumber(a[1]), tonumber(a[2]), tonumber(a[3])), ang = Angle(tonumber(a[4]), tonumber(a[5]), 0) }
		else walkcam = nil end
	end)
	concommand.Add("photon_p3h_walk", function(_, _, a) walk_cmd(a, false) end)
	concommand.Add("photon_p3h_walkfix", function(_, _, a) walk_cmd(a, true) end)
	hook.Add("Think", "PhotonP3hWalk", function()
		local w = walk
		if not w or w.done then return end
		if w.pre > 0 then w.pre = w.pre - 1; return end
		local a, b = w.pts[w.seg], w.pts[w.seg + 1]
		local seg = b - a
		w.dist = w.dist + w.step
		while w.dist > seg:Length() and w.seg < #w.pts - 1 do
			w.dist = w.dist - seg:Length(); w.seg = w.seg + 1
			a, b = w.pts[w.seg], w.pts[w.seg + 1]; seg = b - a
		end
		if w.dist > seg:Length() then w.done = true; print("PHOTON_P3H walk done " .. w.tag); return end
		w.e:SetPos(a + seg:GetNormalized() * w.dist)
		for _, e in ipairs(w.all or {}) do e:SetPos(w.e:GetPos()) end
		w.moved = true
	end)
	hook.Add("CalcView", "PhotonP3hCam", function()
		local w = walk
		if w and w.e and not w.done and not w.fixed then
			if walkcam then return { origin = w.e:GetPos() + walkcam.off, angles = walkcam.ang, fov = 75, drawviewer = false } end
			return { origin = w.e:GetPos() + Vector(0, 0, 60), angles = Angle(89.9, 0, 0), fov = 75, drawviewer = false }
		end
		if cam then return { origin = cam.pos, angles = cam.ang, fov = cam.fov or 75, drawviewer = false } end
	end)
	hook.Add("HUDPaint", "PhotonP3hRT", function()
		if not rt then return end
		rt_tex = rt_tex or GetRenderTarget("photon_p3h_rt", 256, 256)
		rt_mat = rt_mat or CreateMaterial("photon_p3h_rtm", "UnlitGeneric", { ["$basetexture"] = rt_tex:GetName(), ["$ignorez"] = 1 })
		render.PushRenderTarget(rt_tex)
		render.RenderView({ origin = rt.pos, angles = rt.ang, x = 0, y = 0, w = 256, h = 256, aspect = 1, fov = 60, drawviewmodel = false, drawhud = false, dopostprocess = false, bloomtone = false })
		render.PopRenderTarget()
		surface.SetDrawColor(255, 255, 255, 255); surface.SetMaterial(rt_mat); surface.DrawTexturedRect(8, 8, 256, 256)
	end)
	local function block(cx, cy, r)
		local tr, tg, tb, k = 0, 0, 0, 0
		for dy = -r, r do for dx = -r, r do
			local x, y = cx + dx, cy + dy
			if x >= 0 and y >= 0 and x < ScrW() and y < ScrH() then
				local pr, pg, pb = render.ReadPixel(x, y); tr, tg, tb, k = tr + pr, tg + pg, tb + pb, k + 1
			end
		end end
		k = math.max(k, 1)
		return tr / k, tg / k, tb / k
	end
	local function read_props(tag)
		local eye, fnum = EyePos(), FrameNumber()
		print(string.format("PHOTON_P3H readstart %s frame=%d systime=%.2f eye %.1f %.1f %.1f", tag, fnum, SysTime(), eye.x, eye.y, eye.z))
		for _, p in ipairs(props) do
			local e = p.e
			if IsValid(e) then
				local mn, mx = e:OBBMins(), e:OBBMaxs()
				local pts = {}
				if p.probe == "line" then
					local d = mx - mn
					local ax = (d.x >= d.y) and "x" or "y"
					for _, t in ipairs({ 0.06, 0.2, 0.35, 0.5, 0.65, 0.8, 0.94 }) do
						local c = (mn + mx) * 0.5
						c.z = mx.z
						if ax == "x" then c.x = mn.x + d.x * t else c.y = mn.y + d.y * t end
						pts[#pts + 1] = { t = t, w = e:LocalToWorld(c) }
					end
				else
					pts[1] = { t = 0.5, w = e:LocalToWorld(e:OBBCenter()) }
				end
				for _, q in ipairs(pts) do
					local s = q.w:ToScreen()
					local tr = util.TraceLine({ start = eye, endpos = q.w, mask = MASK_SOLID_BRUSHONLY })
					local cx, cy = math.floor(s.x), math.floor(s.y)
					local r, g, b = block(cx, cy, 2)
					print(string.format("PHOTON_P3H read %s %s %.2f %.1f %.1f %.1f %d %d %.1f %.1f %.1f %d %d", tag, p.name, q.t, q.w.x, q.w.y, q.w.z, cx, cy, r, g, b, (tr.Fraction > 0.98 and s.visible) and 1 or 0, fnum))
				end
			end
		end
		for _, q in ipairs(points) do
			local sc = q.w:ToScreen()
			local cx, cy = math.floor(sc.x), math.floor(sc.y)
			local r, g, b = block(cx, cy, 2)
			print(string.format("PHOTON_P3H read %s %s 0.50 %.1f %.1f %.1f %d %d %.1f %.1f %.1f %d %d", tag, q.name, q.w.x, q.w.y, q.w.z, cx, cy, r, g, b, sc.visible and 1 or 0, fnum))
		end
		if rt then print(string.format("PHOTON_P3H rt %s %.1f %.1f %.1f", tag, block(8 + 128, 8 + 128, 3))) end
	end
	hook.Add("DrawOverlay", "PhotonP3hRead", function()
		local w = walk
		if w and w.moved and cfg:match("walk_noread=1") then   -- P4a: positions only, no pixel reads (the proxy logs what it needs)
			w.moved = false
			w.i = w.i + 1
			if w.i % 50 == 1 then local p = w.e:GetPos(); print(string.format("PHOTON_P3H walkpos %s %d %.1f %.1f %.1f %d", w.tag, w.i, p.x, p.y, p.z, FrameNumber())) end
		elseif w and w.moved then
			w.moved = false
			w.i = w.i + 1
			render.CapturePixels()
			local p = w.e:GetPos()
			local cx, cy = math.floor(ScrW() / 2), math.floor(ScrH() / 2)
			if w.fixed then local sc = w.e:LocalToWorld(w.e:OBBCenter()):ToScreen(); cx, cy = math.floor(sc.x), math.floor(sc.y) end
			local r, g, b = block(cx, cy, w.fixed and 2 or 4)
			print(string.format("PHOTON_P3H walk %s %d %.1f %.1f %.1f %.1f %.1f %.1f %d", w.tag, w.i, p.x, p.y, p.z, r, g, b, FrameNumber()))
			if w.i == 1 then amb_mark("walk_" .. w.tag) end
		end
		local due = false
		for _, q in ipairs(reads) do if FrameNumber() >= q.at then due = true end end
		if not due and not every then return end
		render.CapturePixels()
		for i = #reads, 1, -1 do
			if FrameNumber() >= reads[i].at then
				amb_mark(reads[i].tag)
				read_props(reads[i].tag)
				table.remove(reads, i)
			end
		end
		if every then
			every.i = every.i + 1
			read_props(every.tag .. "_" .. every.i)
			if every.i == 1 then amb_mark(every.tag) end
			if every.i >= every.n then every = nil end
		end
	end)
	-- P5 (analysis/18_p5_lights.md): Lua DynamicLights, renewed every frame.
	--   photon_p5_ring <n> x y z R h radius r g b bright   n lights on a ring of radius R, h above (x, y, z), none on yet
	--   photon_p5_on <k>                                   the first k ring lights on (0: none)
	--   photon_p5_orbit x y z R h radius r g b bright deg  one more light orbiting (x, y, z) at R, h above, deg degrees a frame (no arguments: off)
	--   photon_p5_elight 0|1                               the ring as elights (DynamicLight(i, true)) instead of dlights
	local ring, ring_on, orbit, as_elight = nil, 0, nil, false
	concommand.Add("photon_p5_ring", function(_, _, a)
		local n = tonumber(a[1])
		ring = { c = Vector(tonumber(a[2]), tonumber(a[3]), tonumber(a[4])), R = tonumber(a[5]), h = tonumber(a[6]), radius = tonumber(a[7]),
			r = tonumber(a[8]), g = tonumber(a[9]), b = tonumber(a[10]), bright = tonumber(a[11]), n = n }
		ring_on = 0
		print(string.format("PHOTON_P5 ring %d at %.1f %.1f %.1f R %.1f h %.1f radius %.1f", n, ring.c.x, ring.c.y, ring.c.z, ring.R, ring.h, ring.radius))
	end)
	concommand.Add("photon_p5_on", function(_, _, a) ring_on = tonumber(a[1]) or 0; print("PHOTON_P5 on " .. ring_on .. " frame " .. FrameNumber()) end)
	concommand.Add("photon_p5_elight", function(_, _, a) as_elight = a[1] == "1" end)
	concommand.Add("photon_p5_orbit", function(_, _, a)
		if #a < 11 then orbit = nil; return end
		orbit = { c = Vector(tonumber(a[1]), tonumber(a[2]), tonumber(a[3])), R = tonumber(a[4]), h = tonumber(a[5]), radius = tonumber(a[6]),
			r = tonumber(a[7]), g = tonumber(a[8]), b = tonumber(a[9]), bright = tonumber(a[10]), deg = tonumber(a[11]), t = 0 }
		print("PHOTON_P5 orbit on, frame " .. FrameNumber())
	end)
	local function light(i, pos, s, elight)
		local d = DynamicLight(4000 + i, elight)
		if not d then return end
		d.pos = pos; d.r = s.r; d.g = s.g; d.b = s.b; d.brightness = s.bright; d.size = s.radius; d.decay = 0; d.dietime = CurTime() + 5
		d.minlight = 0; d.style = 0
	end
	hook.Add("Think", "PhotonP5Lights", function()
		if ring then
			for i = 1, ring.n do
				if i <= ring_on then
					local t = (i - 1) * 2 * math.pi / ring.n
					light(i, ring.c + Vector(ring.R * math.cos(t), ring.R * math.sin(t), ring.h), ring, as_elight)
				else
					local d = DynamicLight(4000 + i, as_elight)
					if d then d.dietime = CurTime() - 1; d.size = 0 end
				end
			end
		end
		if orbit then
			orbit.t = orbit.t + orbit.deg * math.pi / 180
			light(99, orbit.c + Vector(orbit.R * math.cos(orbit.t), orbit.R * math.sin(orbit.t), orbit.h), orbit, false)
		end
	end)
end

-- P3c route probe (analysis/15d sections 3 and 5): data-file line "scene=route". "photon_route_place" (a step) puts one
-- model of each material kind in front of the camera and starts drawing a grid of screen tiles every frame: one row per
-- model, one column per way of lighting it (real Lua API calls, the same paths addons use). "photon_route_read <tag>"
-- reads the centre of every tile losslessly and prints "PHOTON_ROUTE <tag> <case> <model> r g b". With PHOTON_TEST_ROUTE=1
-- the proxy blacks out draws it routes ENGINE, so tools/p3_routecheck.py can tell the route of each case from the pixels.
-- "photon_route_icon" rebuilds a real spawn icon. Also read: the same models drawn normally in the world, and the viewmodel.
if cfg:match("scene=route") then
	-- vlg with a detail texture, vlg, bump, skin; then the control: an UnlitGeneric material no pair of ours covers, which blackout
	-- must leave alone in every case
	local models = { "models/props_c17/oildrum001.mdl", "models/props_c17/canister01a.mdl", "models/items/boxmrounds.mdl", "models/alyx.mdl",
		"models/props_junk/wood_crate001a.mdl" }
	local CONTROL = 5
	local T = 66   -- tile size
	local placed, read_tag, icon = false, nil, nil
	local draw_e, world_e, world_scr, panels, rts = {}, {}, {}, {}, {}
	local eye, ang
	local function lit_red() for side = 0, 5 do render.SetModelLighting(side, 1, 0, 0) end end
	-- each case draws entity e; every one runs on the real render.* path
	local cases = {
		{ "engine", function(e) e:DrawModel() end },
		{ "suppress", function(e)
			render.SuppressEngineLighting(true); e:DrawModel(); render.SuppressEngineLighting(false) end },
		{ "cube", function(e)
			render.SuppressEngineLighting(true); lit_red(); e:DrawModel(); render.SuppressEngineLighting(false) end },
		{ "reset_origin", function(e)
			render.SuppressEngineLighting(true); render.ResetModelLighting(0, 1, 0); render.SetLightingOrigin(e:GetPos())
			e:DrawModel(); render.SuppressEngineLighting(false) end },
		{ "local_lights", function(e)
			render.SuppressEngineLighting(true); render.ResetModelLighting(0, 0, 0)
			render.SetLocalModelLights({ { type = MATERIAL_LIGHT_POINT, color = Vector(1, 1, 0), pos = e:GetPos() + (eye - e:GetPos()):GetNormalized() * 60,
				fiftyPercentDistance = 100, zeroPercentDistance = 300 } })
			e:DrawModel(); render.SetLocalModelLights(); render.SuppressEngineLighting(false) end },
		{ "unsuppressed_reset", function(e)   -- no suppress: stock's engine lighting replaces this cube at the draw
			render.ResetModelLighting(0, 0, 1); render.SetLightingOrigin(e:GetPos()); e:DrawModel() end },
	}
	local function tile(i, j) return (i - 1) * (T + 4) + 8, (j - 1) * (T + 4) + 8 end
	local n_cases = #cases + 2   -- + dmodelpanel, rt
	local function centre(e) return e:LocalToWorld(e:OBBCenter()) end
	local function view_of(e)
		local c = centre(e)
		local r = (e:OBBMaxs() - e:OBBMins()):Length() * 0.5
		local d = (c - eye); d.z = 0; d:Normalize()
		return c - d * r * 1.9, d:Angle(), c
	end
	local function place()
		local ply = LocalPlayer()
		eye, ang = ply:EyePos(), ply:EyeAngles()
		local fwd = Angle(0, ang.y, 0):Forward()
		local right = fwd:Cross(Vector(0, 0, 1))
		for j, m in ipairs(models) do
			local e = ClientsideModel(m, RENDERGROUP_OPAQUE); e:SetNoDraw(true)
			if j == CONTROL then e:SetMaterial("lights/white") end
			e:SetPos(eye + fwd * 90 - Vector(0, 0, 30)); e:SetAngles(Angle(0, ang.y + 180 + 25, 0)); e:SetupBones()
			draw_e[j] = e
			local w = ClientsideModel(m, RENDERGROUP_OPAQUE)
			if j == CONTROL then w:SetMaterial("lights/white") end
			w:SetPos(eye + fwd * 190 + right * ((j - 3) * 60) - Vector(0, 0, 60)); w:SetAngles(Angle(0, ang.y + 180 + 25, 0))
			world_e[j] = w
			local x, y = tile(#cases + 1, j)
			local p = vgui.Create("DModelPanel")
			p:SetPos(x, y); p:SetSize(T, T); p:SetModel(m); p:SetFOV(60)
			p:SetAmbientLight(Color(255, 0, 0))
			for side = 0, 5 do p:SetDirectionalLight(side, Color(255, 0, 0)) end
			local pe = p:GetEntity()
			if j == CONTROL then pe:SetMaterial("lights/white") end
			local c = pe:OBBCenter()
			p:SetLookAt(c); p:SetCamPos(c + Vector(1, 0, 0) * (pe:OBBMaxs() - pe:OBBMins()):Length() * 0.95)
			function p:LayoutEntity() end
			panels[j] = p
			local rt = GetRenderTarget("photon_route_rt" .. j, 256, 256)
			rts[j] = { rt = rt, mat = CreateMaterial("photon_route_rtm" .. j, "UnlitGeneric", { ["$basetexture"] = rt:GetName(), ["$ignorez"] = 1 }) }
		end
		placed = true
		print("PHOTON_ROUTE placed eye " .. tostring(eye) .. " ang " .. tostring(ang))
	end
	concommand.Add("photon_route_place", place)
	concommand.Add("photon_route_read", function(_, _, a) read_tag = a[1] or "read" end)
	concommand.Add("photon_route_icon", function()
		icon = vgui.Create("SpawnIcon"); icon:SetPos(-200, -200); icon:SetSize(64, 64); icon:SetModel(models[4]); icon:RebuildSpawnIcon()
		print("PHOTON_ROUTE icon rebuild requested " .. models[4])
	end)
	hook.Add("HUDPaint", "PhotonRoute", function()
		if not placed then return end
		-- screen positions now, in the main view: cam.Start3D below would change what ToScreen means
		for j, w in ipairs(world_e) do world_scr[j] = centre(w):ToScreen() end
		for j, e in ipairs(draw_e) do
			if IsValid(e) then
				local campos, camang = view_of(e)
				for i, c in ipairs(cases) do
					local x, y = tile(i, j)
					cam.Start3D(campos, camang, 60, x, y, T, T, 4, 4000)
					render.Clear(40, 40, 40, 255, true, true)
					c[2](e)
					cam.End3D()
				end
				-- render target: a world model seen through render.RenderView, as weapon scopes do
				local wp, wa = view_of(world_e[j])
				local x, y = tile(#cases + 2, j)
				render.PushRenderTarget(rts[j].rt)
				render.RenderView({ origin = wp, angles = wa, x = 0, y = 0, w = 256, h = 256, aspect = 1, fov = 60,
					drawviewmodel = false, drawhud = false, dopostprocess = false, bloomtone = false })
				render.PopRenderTarget()
				surface.SetDrawColor(255, 255, 255, 255); surface.SetMaterial(rts[j].mat); surface.DrawTexturedRect(x, y, T, T)
			end
		end
	end)
	local function block(x, y)
		local r, g, b, k = 0, 0, 0, 0
		for dy = -2, 2 do for dx = -2, 2 do
			local pr, pg, pb = render.ReadPixel(x + dx, y + dy); r, g, b, k = r + pr, g + pg, b + pb, k + 1
		end end
		return r / k, g / k, b / k
	end
	hook.Add("DrawOverlay", "PhotonRouteRead", function()
		if not read_tag or not placed then return end
		local tag = read_tag; read_tag = nil
		render.CapturePixels()
		local names = {}
		for i, c in ipairs(cases) do names[i] = c[1] end
		names[#cases + 1] = "dmodelpanel"; names[#cases + 2] = "rt"
		for j, m in ipairs(models) do
			for i = 1, n_cases do
				local x, y = tile(i, j)
				print(string.format("PHOTON_ROUTE %s %s %s %.1f %.1f %.1f", tag, names[i], m, block(x + T / 2, y + T / 2)))
			end
			local s = world_scr[j]
			print(string.format("PHOTON_ROUTE %s world %s %.1f %.1f %.1f", tag, m, block(math.floor(s.x), math.floor(s.y))))
		end
		print(string.format("PHOTON_ROUTE %s viewmodel - %.1f %.1f %.1f", tag, block(1040, 640)))
	end)
end

-- P3d volume probe (analysis/15d section 5): data-file line "scene=vol". Draws a Photon_DebugVol quad at (64, 64), 256 px,
-- which samples the volume the proxy binds on s15 (PHOTON_TEST_VOL=1). "photon_vol_read <tag>" prints a grid of lossless
-- pixels, "PHOTON_VOL <tag> x y r g b", that tools/p3_volcheck.py compares with the pattern. "photon_vol_spawn" loads a model
-- not seen yet, so texture downloads happen while the proxy uploads. "steps_map=<map>": the STEPS run only on that map, so a
-- step may change the map; on any other map one read happens by itself ("map2").
if cfg:match("scene=vol") then
	local mat, read_tag, spawned
	local xs = { 72, 120, 168, 190, 198, 202, 206, 214, 250, 300 }
	local ys = { 72, 120, 160, 184, 190, 194, 198, 204, 250, 300 }
	hook.Add("InitPostEntity", "PhotonVol", function()
		mat = CreateMaterial("photon_vol_mat", "Photon_DebugVol", { ["$photoncolor"] = "[1 0 1]" })
		print("PHOTON_VOL material shader=" .. tostring(mat:GetShader()) .. " map=" .. game.GetMap())
		local home = cfg:match("steps_map=(%S+)")
		if home and game.GetMap() ~= home then
			steps = 0
			hook.Add("Think", "PhotonVolAuto", function()
				if t_ipe and RealTime() - t_ipe >= math.max(settle - 2, 1) then read_tag = "map2"; hook.Remove("Think", "PhotonVolAuto") end
			end)
		end
	end)
	concommand.Add("photon_vol_read", function(_, _, a) read_tag = a[1] or "read" end)
	concommand.Add("photon_vol_spawn", function()
		local ply = LocalPlayer()
		spawned = ClientsideModel("models/props_wasteland/laundry_cart001.mdl", RENDERGROUP_OPAQUE)
		spawned:SetPos(ply:EyePos() + ply:GetAimVector() * 200)
		print("PHOTON_VOL spawned " .. tostring(spawned:GetModel()))
	end)
	hook.Add("HUDPaint", "PhotonVol", function()
		if not mat then return end
		render.SetMaterial(mat)
		render.DrawScreenQuadEx(64, 64, 256, 256)
	end)
	hook.Add("DrawOverlay", "PhotonVolRead", function()
		if not read_tag or not mat then return end
		local tag = read_tag; read_tag = nil
		render.CapturePixels()
		print(string.format("PHOTON_VOL read %s %dx%d", tag, ScrW(), ScrH()))
		for _, y in ipairs(ys) do for _, x in ipairs(xs) do
			print(string.format("PHOTON_VOL %s %d %d %d %d %d", tag, x, y, render.ReadPixel(x, y)))
		end end
	end)
end

-- P3c2 coverage sweep: data-file line "scene=coverage". Walks models/ on every mounted path, priority folders first (weapons, hands,
-- player models, vehicles), and draws each model once in a small tile, so its materials load and take their shader snapshots; the
-- proxy logs which statics were asked for (PHOTON_TEST_COVERAGE=1). "cov_max=<n>" bounds the model count. Prints "PHOTON_COV" progress.
if cfg:match("scene=coverage") then
	local cov_max = tonumber(cfg:match("cov_max=(%d+)")) or 3000
	local queue = { "models/weapons", "models/player", "models/humans", "models/vehicles", "models/gta5", "models/blu", "models/items", "models/props_c17",
		"models/props_junk", "models/props_interiors", "models/props_lab", "models/props_wasteland", "models" }
	local seen, list, nvisit, head, i, frames = {}, {}, 0, 1, 1, 0
	local function walk(budget)
		while head <= #queue and budget > 0 and #list < cov_max do
			local d = queue[head]; head = head + 1; budget = budget - 1
			if not seen[d] then
				seen[d] = true
				local fs, ds = file.Find(d .. "/*", "GAME")
				for _, f in ipairs(fs or {}) do if f:sub(-4) == ".mdl" then list[#list + 1] = d .. "/" .. f end end
				for _, s in ipairs(ds or {}) do queue[#queue + 1] = d .. "/" .. s end
			end
		end
	end
	hook.Add("HUDPaint", "PhotonCoverage", function()
		walk(20)
		frames = frames + 1
		if frames % 60 == 0 then print(string.format("PHOTON_COV models %d of %d listed, folders left %d", i - 1, #list, #queue - head + 1)) end
		local eye, ang = EyePos(), Angle(0, 0, 0)
		local fwd = ang:Forward()
		cam.Start3D(eye, ang, 60, 0, 0, 96, 96, 4, 8000)
		for _ = 1, 6 do
			local m = list[i]
			if not m then break end
			i = i + 1
			-- no util.IsValidModel here: it is false for anything not loaded yet, which is nearly everything
			local e = ClientsideModel(m, RENDERGROUP_OPAQUE)
			if IsValid(e) then
				e:SetNoDraw(true); e:SetPos(eye + fwd * 200); e:SetupBones(); e:DrawModel(); e:Remove()
				nvisit = nvisit + 1
			end
		end
		cam.End3D()
		if i > #list and (head > #queue or #list >= cov_max) and not seen.done then
			seen.done = true
			steps = 0   -- no more steps: the harness lingers and quits, which writes the proxy's final coverage dump
			print(string.format("PHOTON_COV done: %d models drawn", nvisit))
		end
	end)
end

hook.Add("DrawOverlay", "PhotonHarness", function()
	if not t_ipe or quitting then return end
	local now = RealTime()
	if not t_shot and now - t_ipe >= settle then
		t_shot = now
		print(string.format("PHOTON_HARNESS map loaded %s systime=%.1f frames=%d curtime=%.1f gameui=%s",
			game.GetMap(), SysTime(), FrameNumber(), CurTime(), tostring(gui.IsGameUIVisible())))
		shot()
	elseif t_shot and step < steps and not t_step and now - t_shot >= shotgap then
		step = step + 1
		t_step = now
		if persist then file.Write("photon_harness_step.txt", tostring(step)) end
		print(string.format("PHOTON_HARNESS step %d systime=%.1f frames=%d", step, SysTime(), FrameNumber()))
		RunConsoleCommand("photon_step" .. step)
	elseif t_step and now - t_step >= (stepgaps[step] or stepgap) then
		t_step = nil
		t_shot = now
		print(string.format("PHOTON_HARNESS step %d shot systime=%.1f frames=%d", step, SysTime(), FrameNumber()))
		shot()
	elseif t_shot and not t_step and step >= steps and not quitting and now - t_shot >= linger and next(harness_hold) == nil then
		quitting = true
		print("PHOTON_HARNESS quitting")
		-- "quit" is blocked for Lua (client and server); run_gmod.sh defines this alias in a cfg
		RunConsoleCommand("photon_harness_exit")
	end
end)

-- Frame times per pose (data-file line "frametimes=1"): every frame's SysTime delta after the first shot, keyed by the current step
-- (0 = the settle pose), skipping the first second after each step change (teleport, streaming). Printed at shutdown as
-- "PHOTON_FT pose <step> n <frames> median <ms> p99 <ms> low1 <ms>" (low1 = mean of the slowest 1%). Same for stock and Photon.
if cfg:match("frametimes=1") then
	-- "spikes=1" too: per pose, the frames over twice the pose median: "PHOTON_SPK pose <step> n <count> gaps_ms <ms between spikes ...>"
	-- (the first 60), to see whether stalls come in a rhythm (stock and Photon alike)
	local ft, last, cur, t_cur = {}, nil, -1, 0
	local spikes, spk_t = cfg:match("spikes=1") ~= nil, {}
	-- the harness's own jpeg (one per step, at the end of the pose) costs a ~350 ms frame and a few seconds of slow ones (writing ~2.7 MB):
	-- the 3 s after each shot are left out, so p99 and the 1% lows measure the game, not the harness
	local shot_seen, t_shotsys = nil, -100
	hook.Add("DrawOverlay", "PhotonFrameTimes", function()
		local now = SysTime()
		if t_shot ~= shot_seen then shot_seen, t_shotsys = t_shot, now end
		if t_shot and step ~= cur then cur, t_cur = step, now end
		if last and t_shot and now - t_cur >= 1 and now - t_shotsys >= 3 then
			ft[cur] = ft[cur] or {}
			table.insert(ft[cur], (now - last) * 1000)
			if spikes then
				spk_t[cur] = spk_t[cur] or {}
				table.insert(spk_t[cur], now)
			end
		end
		last = now
	end)
	hook.Add("ShutDown", "PhotonFrameTimes", function()
		for p = 0, steps do
			local v = ft[p]
			if v and #v >= 20 then
				local raw = {}
				for i = 1, #v do raw[i] = v[i] end   -- frame order, for the spike times
				table.sort(v)
				local n, k = #v, math.max(1, math.floor(#v / 100))
				local low = 0
				for i = n - k + 1, n do low = low + v[i] end
				if spikes and spk_t[p] then
					local ts = spk_t[p]
					local med, prev, gaps, cnt = v[math.ceil(n / 2)], nil, {}, 0
					for i = 1, n do
						if raw[i] > 2 * med then
							cnt = cnt + 1
							if prev and #gaps < 60 then gaps[#gaps + 1] = string.format("%d", (ts[i] - prev) * 1000) end
							prev = ts[i]
						end
					end
					print(string.format("PHOTON_SPK pose %d n %d of %d gaps_ms %s", p, cnt, n, table.concat(gaps, " ")))
				end
				print(string.format("PHOTON_FT pose %d n %d median %.2f p99 %.2f low1 %.2f", p, n, v[math.ceil(n / 2)], v[math.ceil(n * 0.99)], low / k))
			end
		end
	end)
end

-- SHC study (data-file line "movecount=1", analysis/29): per pose, how many prop_physics change origin or angles from one frame to the
-- next on the client (each change re-projects that prop's shadow), split into tiny moves (all of |dpos| < 0.01 u and |dang| < 0.01 deg:
-- physics jitter at rest) and real ones. Printed at shutdown as "PHOTON_MOVE pose <step> frames <n> moved <per frame> tiny <per frame>
-- maxpos <u> maxang <deg>" (max over the tiny ones). Costs a pass over the props every frame, so not for frame-time runs.
if cfg:match("movecount=1") then
	local prev, st, cur = {}, {}, -1
	hook.Add("DrawOverlay", "PhotonMoveCount", function()
		if not t_shot then return end
		if step ~= cur then cur = step; prev = {} end
		local r = st[cur] or { f = 0, moved = 0, tiny = 0, mp = 0, ma = 0 }
		st[cur] = r
		r.f = r.f + 1
		for _, e in ipairs(ents.FindByClass("prop_physics")) do
			local p, a, q = e:GetPos(), e:GetAngles(), prev[e]
			if q then
				local dp = math.max(math.abs(p.x - q[1]), math.abs(p.y - q[2]), math.abs(p.z - q[3]))
				local da = math.max(math.abs(math.AngleDifference(a.p, q[4])), math.abs(math.AngleDifference(a.y, q[5])),
					math.abs(math.AngleDifference(a.r, q[6])))
				if dp > 0 or da > 0 then
					r.moved = r.moved + 1
					if dp < 0.01 and da < 0.01 then
						r.tiny = r.tiny + 1
						r.mp, r.ma = math.max(r.mp, dp), math.max(r.ma, da)
					end
				end
			end
			prev[e] = { p.x, p.y, p.z, a.p, a.y, a.r }
		end
	end)
	hook.Add("ShutDown", "PhotonMoveCount", function()
		for p = 0, steps do
			local r = st[p]
			if r and r.f > 0 then
				print(string.format("PHOTON_MOVE pose %d frames %d moved %.1f tiny %.1f maxpos %.5f maxang %.5f", p, r.f, r.moved / r.f,
					r.tiny / r.f, r.mp, r.ma))
			end
		end
	end)
end

-- T4.5 study (data-file line "study=1", analysis/21): a frame marker every second ("PHOTON_FM os <unix s> sys <SysTime> frames <n>", to
-- turn an outside sampling window into a frame count), and "photon_hookcount start|stop": Lua hook calls per frame by name while it
-- runs, then a census of the physics props (count, not dormant, per model with its materials and bones). Only the window it runs in pays
-- for the hook.Call wrapper; stop puts the original back.
if cfg:match("study=1") then
	local fm_last = 0
	hook.Add("DrawOverlay", "PhotonStudyFM", function()
		local now = SysTime()
		if now - fm_last >= 1 then
			fm_last = now
			print(string.format("PHOTON_FM os %d sys %.3f frames %d", os.time(), now, FrameNumber()))
		end
	end)
	local orig, counts, f0
	concommand.Add("photon_hookcount", function(_, _, a)
		if a[1] == "start" and not orig then
			orig, counts, f0 = hook.Call, {}, FrameNumber()
			hook.Call = function(name, gm, ...)
				counts[name] = (counts[name] or 0) + 1
				return orig(name, gm, ...)
			end
		elseif a[1] == "stop" and orig then
			hook.Call = orig; orig = nil
			local frames = math.max(1, FrameNumber() - f0)
			local names = {}
			for k in pairs(counts) do names[#names + 1] = k end
			table.sort(names, function(x, y) return counts[x] > counts[y] end)
			print(string.format("PHOTON_HOOKS client frames %d", frames))
			for _, k in ipairs(names) do print(string.format("PHOTON_HOOKS client %s %.2f per frame", k, counts[k] / frames)) end
			local n, awake, models = 0, 0, {}
			for _, e in ipairs(ents.FindByClass("prop_physics")) do
				n = n + 1
				if not e:IsDormant() then awake = awake + 1 end
				local m = e:GetModel() or "?"
				local r = models[m] or { n = 0, skin = {}, mats = #(e:GetMaterials() or {}), bones = e:GetBoneCount(), ro = 0 }
				r.n = r.n + 1; r.skin[e:GetSkin()] = true
				if e.RenderOverride then r.ro = r.ro + 1 end
				models[m] = r
			end
			print(string.format("PHOTON_CENSUS prop_physics %d not_dormant %d", n, awake))
			for m, r in pairs(models) do
				local s = 0
				for _ in pairs(r.skin) do s = s + 1 end
				print(string.format("PHOTON_CENSUS model %s n %d skins %d materials %d bones %d renderoverride %d", m, r.n, s, r.mats, r.bones, r.ro))
			end
		end
	end)
end

-- P6 ceiling (data-file line "suppress=1", analysis/21 §3.1): render.SuppressEngineLighting(true) for the opaque model pass sets the same
-- [CModelRender+0x48] byte P6 would set per draw, so the engine skips its per-model light setup for every model. The image is wrong; it only
-- measures how much frame time that setup costs on stock.
if cfg:match("suppress=1") then
	hook.Add("PreDrawOpaqueRenderables", "PhotonP6Ceiling", function() render.SuppressEngineLighting(true) end)
	hook.Add("PostDrawOpaqueRenderables", "PhotonP6Ceiling", function() render.SuppressEngineLighting(false) end)
	hook.Add("PreDrawTranslucentRenderables", "PhotonP6Ceiling", function() render.SuppressEngineLighting(true) end)
	hook.Add("PostDrawTranslucentRenderables", "PhotonP6Ceiling", function() render.SuppressEngineLighting(false) end)
	print("PHOTON_HARNESS suppress=1: engine model lighting suppressed in the model passes (P6 ceiling test)")
end

-- P6 sun visibility against the engine (data-file line "suntrace=1", analysis/22_p6_sun.md). The engine's own sun test for a model's light
-- setup is a ray from the lighting point along -normal of the emit_skylight worldlight, 57016.32 units, MASK_OPAQUE, world only, lit iff the
-- hit surface is SURF_SKY (engine.dll FUN_1801da890). Lua cannot read the worldlight, so the direction comes from the map's light_environment
-- (vrad's SetupLightNormalFromProps, public SDK map_utils.cpp), read from the entity lump of the BSP itself; util.GetSunInfo() (the env_sun
-- sprite, which may point elsewhere) is printed beside it. Points: kind f, a grid of fixed heights over sunbox; kind g, every floor of a
-- column (traced down, MASK_SOLID_BRUSHONLY) + 8 and + 48 units. Per point: "PHOTON_SUN pt <i> <kind> x y z" then the raw TraceLine
-- (HitSky StartSolid Fraction HitTexture) and the world-only answer (the same fields after stepping past static props and entities, which the
-- engine's world-only filter does not see). sunbox=x0,y0,z0,x1,y1,z1 (default gm_construct's playable box), sunstep (column step, 128),
-- sunheights=z1,z2,.. (the fixed heights), sunrate (points a frame, 200).
if cfg:match("suntrace=1") then
	local function num_list(s) local t = {} for v in (s or ""):gmatch("[^,]+") do t[#t + 1] = tonumber(v) end return t end
	local box = num_list(cfg:match("sunbox=(%S+)"))
	if #box ~= 6 then box = { -6144, -5120, -1024, 3072, 7168, 3072 } end
	local step = tonumber(cfg:match("sunstep=(%d+)")) or 128
	local heights = num_list(cfg:match("sunheights=(%S+)"))
	if #heights == 0 then heights = { -128, 64, 256, 512, 1024 } end
	local rate = tonumber(cfg:match("sunrate=(%d+)")) or 200
	local L = 57016.3203125
	harness_hold.suntrace = true

	-- light_environment from the entity lump (lump 0: offset and length at bytes 8 and 12 of the header)
	local function env_normal()
		local f = file.Open("maps/" .. game.GetMap() .. ".bsp", "rb", "GAME")
		if not f then return nil, "no bsp" end
		f:Seek(8)
		local ofs, len = f:ReadLong(), f:ReadLong()
		f:Seek(ofs)
		local ents = f:Read(len) or ""
		f:Close()
		for block in ents:gmatch("{(.-)}") do
			if block:find('"classname"%s*"light_environment"') then
				local kv = {}
				for k, v in block:gmatch('"([^"]*)"%s*"([^"]*)"') do kv[k] = v end
				local a = {}
				for v in (kv.angles or "0 0 0"):gmatch("%S+") do a[#a + 1] = tonumber(v) end
				local angle, pitch = tonumber(kv.angle or "0") or 0, tonumber(kv.pitch or "0") or 0
				local n
				if angle == -1 then n = Vector(0, 0, 1)
				elseif angle == -2 then n = Vector(0, 0, -1)
				else
					if angle == 0 then angle = a[2] or 0 end
					n = Vector(math.cos(math.rad(angle)), math.sin(math.rad(angle)), 0)
				end
				if pitch == 0 then pitch = a[1] or 0 end
				n.z = math.sin(math.rad(pitch))
				n.x = n.x * math.cos(math.rad(pitch))
				n.y = n.y * math.cos(math.rad(pitch))
				return n, string.format("angles_%s_pitch_%s_angle_%s", tostring(kv.angles):gsub("%s", ":"), tostring(kv.pitch), tostring(kv.angle))
			end
		end
		return nil, "no_light_environment"
	end

	local function tex(t) local s = (t or ""):gsub("%s", "_") if s == "" then s = "-" end return s end
	local function trace_sun(p, dir)
		local raw = util.TraceLine({ start = p, endpos = p + dir * L, mask = MASK_OPAQUE, filter = LocalPlayer() })
		local tr, start, n, filter = raw, p, 0, { LocalPlayer() }
		-- the engine's world-only filter sees no entity: an entity hit goes into the filter and the same ray is traced again from the
		-- same start (stepping past an entity instead would also step past world brushes it overlaps, such as gm_construct's colour room)
		while tr.Hit and not tr.HitSky and IsValid(tr.Entity) and not tr.Entity:IsWorld() and n < 16 do
			n = n + 1
			filter[#filter + 1] = tr.Entity
			tr = util.TraceLine({ start = p, endpos = p + dir * L, mask = MASK_OPAQUE, filter = filter })
		end
		-- a static prop ("**studio**", none on gm_construct or gm_flatgrass's sun rays) cannot be filtered: its texture says so in the
		-- world-only field, and suntest --compare counts those points apart
		return raw, tr
	end

	local pts, built, idx, t_start = nil, false, 0, nil
	local dir
	local function build()
		pts = {}
		for _, z in ipairs(heights) do
			for x = box[1], box[4], step * 2 do
				for y = box[2], box[5], step * 2 do pts[#pts + 1] = { "f", Vector(x + 0.37, y + 0.21, z + 0.13) } end
			end
		end
		for x = box[1], box[4], step do
			for y = box[2], box[5], step do
				local top = Vector(x + 0.29, y + 0.43, box[6])
				for _ = 1, 4 do
					local tr = util.TraceLine({ start = top, endpos = Vector(top.x, top.y, box[3] - 64), mask = MASK_SOLID_BRUSHONLY, filter = LocalPlayer() })
					if not tr.Hit or tr.AllSolid then break end
					if tr.HitNormal.z > 0.5 and tr.HitPos.z >= box[3] - 64 then
						pts[#pts + 1] = { "g", tr.HitPos + Vector(0, 0, 8) }
						pts[#pts + 1] = { "g", tr.HitPos + Vector(0, 0, 48) }
					end
					top = tr.HitPos - Vector(0, 0, 1)
					if top.z <= box[3] - 64 then break end
				end
			end
		end
	end
	hook.Add("DrawOverlay", "PhotonSunTrace", function()
		if not t_ipe or RealTime() - t_ipe < 3 then return end
		if not built then
			built = true
			local n, how = env_normal()
			local si = util.GetSunInfo and util.GetSunInfo()
			print(string.format("PHOTON_SUN dir %s env %s %s getsuninfo %s len %.4f", game.GetMap(), n and string.format("%.9f %.9f %.9f", n.x, n.y, n.z) or "none", how,
				si and si.direction and string.format("%.6f %.6f %.6f obstruction %.3f", si.direction.x, si.direction.y, si.direction.z, si.obstruction or -1) or "none", L))
			if not n then harness_hold.suntrace = nil; hook.Remove("DrawOverlay", "PhotonSunTrace"); return end
			dir = -n
			-- "sunprobe=x:y:z;x:y:z..." (for a difference): each point traced up, down and toward the sun with three masks, every field printed
			for spec in (cfg:match("sunprobe=(%S+)") or ""):gmatch("[^;]+") do
				local c = num_list(spec:gsub(":", ","))
				if #c == 3 then
					local p = Vector(c[1], c[2], c[3])
					for _, d in ipairs({ { "up", Vector(0, 0, 1) }, { "down", Vector(0, 0, -1) }, { "sun", dir } }) do
						for _, mk in ipairs({ { "opaque", MASK_OPAQUE }, { "solid", MASK_SOLID }, { "all", MASK_ALL } }) do
							local tr = util.TraceLine({ start = p, endpos = p + d[2] * L, mask = mk[2], filter = LocalPlayer() })
							print(string.format("PHOTON_SUN probe %.3f %.3f %.3f %s %s hit %d sky %d world %d startsolid %d allsolid %d frac %.7f left %.7f pos %.3f %.3f %.3f normal %.3f %.3f %.3f tex %s contents %s ent %s",
								p.x, p.y, p.z, d[1], mk[1], tr.Hit and 1 or 0, tr.HitSky and 1 or 0, tr.HitWorld and 1 or 0, tr.StartSolid and 1 or 0, tr.AllSolid and 1 or 0,
								tr.Fraction, tr.FractionLeftSolid or -1, tr.HitPos.x, tr.HitPos.y, tr.HitPos.z, tr.HitNormal.x, tr.HitNormal.y, tr.HitNormal.z, tex(tr.HitTexture),
								tostring(tr.Contents), tostring(tr.Entity)))
						end
					end
					print(string.format("PHOTON_SUN probe %.3f %.3f %.3f pointcontents %d", p.x, p.y, p.z, util.PointContents(p)))
				end
			end
			if cfg:match("sunprobeonly=1") then pts = {} else build() end
			t_start = SysTime()
			print(string.format("PHOTON_SUN start %s points %d box %s step %d", game.GetMap(), #pts, table.concat(box, ","), step))
		end
		for _ = 1, rate do
			idx = idx + 1
			local q = pts[idx]
			if not q then
				print(string.format("PHOTON_SUN done %s points %d seconds %.1f", game.GetMap(), #pts, SysTime() - t_start))
				harness_hold.suntrace = nil
				hook.Remove("DrawOverlay", "PhotonSunTrace")
				return
			end
			local p = q[2]
			local raw, w = trace_sun(p, dir)
			print(string.format("PHOTON_SUN pt %d %s %.3f %.3f %.3f %d %d %.6f %s %d %d %.6f %s", idx, q[1], p.x, p.y, p.z, raw.HitSky and 1 or 0, raw.StartSolid and 1 or 0,
				raw.Fraction, tex(raw.HitTexture), w.HitSky and 1 or 0, w.StartSolid and 1 or 0, w.Fraction, tex(w.HitTexture)))
		end
	end)
	print("PHOTON_HARNESS suntrace=1: sun visibility samples after the map loads")
end
