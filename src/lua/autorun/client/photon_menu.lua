-- Photon settings (UI1): a window on the context menu (hold C, the Photon icon). Client only: it writes data/photon/settings.txt, which
-- Photon's DLL reads within a few seconds (the same keys as cfg/photon.cfg; this file wins over it). Nothing here runs on or talks to the
-- server. Each setting is also a client ConVar (photon_ui_*), so it can be changed from the console where client Lua runs.
-- Layout (user, 2026-11-09: "Each shader should have its specific settings ... move into the context menu with our own custom UI"): the
-- modes on the left (Optimized, Better lighting, Best quality, Path traced, Custom), the chosen mode's own settings on the right,
-- then the settings every mode shares.
local FILE = "photon/settings.txt"
-- key, label, kind, choices / range, help. -1 means "follow the preset".
local SETTINGS = {
	{ "photon_preset", "Preset", "choice", { { 0, "Optimized (default look, faster)" }, { 1, "Better lighting (v2, balanced)" }, { 2, "Best quality (v2, sharpest)" }, { 3, "Custom (only the settings below)" } },
	  "Optimized keeps the game's look with Photon's speed-ups. Better lighting adds Photon's sun shadows. Best quality uses bigger, softer shadows.", "mode" },
	{ "photon_target_fps", "Dynamic quality: target FPS", "choice", { { 0, "Off" }, { 30, "30" }, { 60, "60" }, { 75, "75" }, { 120, "120" }, { 144, "144" }, { 165, "165" }, { 240, "240" } },
	  "Holds this frame rate by taking the steps listed below, in their order, before frames get slow (it acts when the slowest 1% of frames near the target), and undoing them one at a time once there is room again. It knows whether the processor or the graphics card limits the frame and takes a step that helps that side. Never above your own settings.", "general" },
	{ "photon_shadows", "Sun shadows", "choice", { { -1, "Auto (preset)" }, { 1, "On" }, { 0, "Off (game's blob shadows)" } },
	  "Sharp shadows cast from the map's sun instead of the game's soft blobs under props.", "lighting" },
	{ "photon_shadow_res", "Shadow resolution", "choice", { { -1, "Auto (preset)" }, { 1024, "1024 (fastest)" }, { 2048, "2048" }, { 4096, "4096 (sharpest)" } },
	  "Detail of the shadow map. Higher is sharper and uses more video memory.", "lighting" },
	{ "photon_shadow_dist", "Shadow distance", "choice", { { -1, "Auto (preset)" }, { 512, "Short (512)" }, { 1024, "Medium (1024)" }, { 2048, "Far (2048)" }, { 4096, "Very far (4096)" } },
	  "How far around you props cast sun shadows. Farther covers more but each shadow gets blurrier.", "lighting" },
	{ "photon_shadow_soft", "Shadow edges", "choice", { { -1, "Auto (preset)" }, { 0, "Hard (faster)" }, { 1, "Soft" } },
	  "Soft edges take more samples per pixel.", "lighting" },
	{ "photon_shadow_strength", "Shadow darkness", "slider", { -1, 1, 2 },
	  "How dark sun shadows are (0 to 1; -1 follows the preset).", "lighting" },
	{ "photon_shadow_props", "Props cast shadows", "choice", { { 1, "On" }, { 0, "Off" } },
	  "Off: physics props cast no shadow at all (neither Photon's nor the game's). Shadows still fall on them.", "lighting" },
	{ "photon_shadow_characters", "Players and NPCs cast shadows", "choice", { { 1, "On" }, { 0, "Off" } },
	  "Off: players, NPCs and ragdolls cast no shadow at all. Shadows still fall on them.", "lighting" },
	{ "photon_shadow_static", "Map props cast sun shadows", "choice", { { 0, "Off (the map's own look)" }, { 1, "On" } },
	  "Trees, rocks and other models built into the map also cast Photon's sun shadows. Off keeps the shadows the map's author baked.", "lighting" },
	{ "photon_light_shadows", "Lamp shadows", "choice", { { -1, "Auto (preset)" }, { 0, "Off" }, { 1, "1 lamp" }, { 2, "2 lamps" }, { 4, "4 lamps" } },
	  "The map's own lamps cast real-time shadows of props and characters near them (the rest of the lighting stays baked). More lamps cost more.", "lighting" },
	{ "photon_light_shadow_res", "Lamp shadow resolution", "choice", { { -1, "Auto (preset)" }, { 512, "512 (fastest)" }, { 1024, "1024 (sharpest)" } },
	  "Detail of each lamp's shadow.", "lighting" },
	{ "photon_rebake", "Sharper map lighting (re-bake)", "choice", { { -1, "Auto (preset)" }, { 1, "On" }, { 0, "Off" } },
	  "Re-bakes the map lamps' shadows sharper than the map's own lighting (a few seconds in the background the first time a map loads, then cached).", "lighting" },
	{ "photon_rebake_res", "Re-bake detail", "choice", { { -1, "Auto (preset)" }, { 8, "8 units (faster bake)" }, { 4, "4 units (sharpest)" } },
	  "Size of the re-bake's cells. 4 units bakes about four times longer and uses about three times the memory.", "lighting" },
	{ "photon_pathtrace", "Path tracer", "choice", { { 2, "Real time (Vulkan; from the next game start)" }, { 1, "Baked (re-lights the map once, cached)" } },
	  "Real time: Photon's live path tracer (bounced light, glossy reflections, PBR packs) on the Vulkan route; it starts with the next game start. Baked: the map's lightmaps path-traced once in the background and cached (the rays and bounces below are for this one).", "pt" },
	{ "photon_pt_rays", "Rays per sample", "choice", { { 2, "2 (fastest bake, noisy)" }, { 4, "4" }, { 8, "8 (default)" }, { 16, "16" }, { 32, "32" }, { 64, "64 (slowest, cleanest)" } },
	  "Light paths traced per lighting sample in each pass. More rays: less noise, longer bake (doubling the rays about doubles the time). The result is cached per setting.", "pt" },
	{ "photon_pt_bounces", "Light bounces", "choice", { { 0, "0 (direct light only)" }, { 1, "1" }, { 2, "2" }, { 3, "3 (default)" }, { 4, "4" }, { 6, "6" }, { 8, "8" } },
	  "How many times light bounces off surfaces. More bounces: brighter, more colour bleeding in rooms, slower.", "pt" },
	{ "photon_nanite", "Map props drawn by Photon (Nanite, experimental)", "choice", { { 0, "Off" }, { 1, "On" } },
	  "Photon draws the map's static props (trees, rocks, furniture) itself on the Vulkan route, in big batches, instead of the engine drawing each one: much faster on prop-heavy maps. Experimental; applies from the next game start.", "general" },
	{ "photon_auto_lod", "Auto LOD for addon models", "choice", { { 0, "Off" }, { 1, "On" } },
	  "Builds simpler versions of addon models that have none, in the background, into garrysmod/addons/!photon_lod (delete the folder to undo). They are used from the next game start, only where the difference is under a pixel.", "general" },
	{ "photon_taa", "Anti-aliasing (TAA)", "choice", { { 0, "Off" }, { 1, "On" } },
	  "Smooths jagged edges by blending each frame with the last ones (with sharpening so it stays crisp). Moving objects are followed by their own motion, so they stay smooth too.", "general" },
	{ "photon_upscale", "Upscaling (when the 3D view renders below full size)", "choice", { { 1, "Photon temporal (sharp)" }, { 3, "AMD FSR 2 (Vulkan)" }, { 0, "Game's stretch (blurry)" } },
	  "When dynamic quality (or mat_viewportscale) renders the 3D view smaller, Photon rebuilds full-resolution detail from several jittered frames instead of stretching one. No effect at full size.", "general" },
	{ "photon_render_scale", "Render scale (3D view)", "choice", { { 1, "100% (native)" }, { 0.85, "85%" }, { 0.75, "75%" }, { 0.67, "67%" }, { 0.5, "50% (fastest)" } },
	  "Renders the 3D view below full size and upscales it (with Photon's temporal upscaling: see Upscaling). Saves GPU time; the HUD stays sharp. Dynamic quality lowers it further only when needed.", "general" },
	{ "photon_taa_sharpen", "TAA sharpening", "choice", { { 0, "Off (softest)" }, { 0.4, "Light" }, { 0.8, "Default" }, { 1, "Strong" } },
	  "How much the anti-aliased frame is sharpened. Higher: crisper textures, a little more shimmer on fine detail.", "general" },
	{ "photon_lights", "Per-pixel dynamic lights", "choice", { { 1, "On" }, { 0, "Off" } },
	  "Lights (lamps, muzzle flashes, flashlights of others) light props per pixel with no 4-light limit. Applies on the next map.", "general" },
	{ "photon_cubemap_blend", "Smooth reflections", "choice", { { 1, "On" }, { 0, "Off" } },
	  "Reflections fade between the map's cubemaps instead of snapping. Applies on the next map.", "general" },
}

local values = {}
local function load()
	values = {}
	local s = file.Read(FILE, "DATA")
	if not s then return end
	for line in string.gmatch(s, "[^\r\n]+") do
		local k, v = string.match(line, "^%s*([%w_]+)%s+\"?([%-%d%.]+)\"?")
		if k then values[k] = tonumber(v) end
		local o = string.match(line, "^%s*photon_dyn_order%s+\"?([^\"\r\n]*)\"?")
		if o then values.photon_dyn_order = o end
	end
end
local function save()
	if not file.IsDir("photon", "DATA") then file.CreateDir("photon") end
	local out = { "// Photon settings, written by the Spawn menu page (lua/autorun/client/photon_menu.lua)" }
	for _, s in ipairs(SETTINGS) do
		local v = values[s[1]]
		if v ~= nil then out[#out + 1] = string.format("%s %s", s[1], tostring(v)) end
	end
	if values.photon_dyn_order and values.photon_dyn_order ~= "" then out[#out + 1] = string.format("photon_dyn_order \"%s\"", values.photon_dyn_order) end
	file.Write(FILE, table.concat(out, "\n") .. "\n")
end
local function set(key, v)
	values[key] = v
	save()
end

load()

-- Other Photon modules add their own sections (the CPU module, modules/cpu/): PhotonUI.AddSection(id, title, settings), the settings in
-- the SETTINGS format above (key, label, "choice" / "slider", options, help). Their values go to the same data/photon/settings.txt and
-- photon_ui_* ConVars; the module reads its keys from that file. Sections show under every mode, after the shared settings. A file that
-- loads before this one (autorun runs alphabetically: photon_cpu_* first) queues instead:
--   PhotonUI = PhotonUI or {}; PhotonUI.Pending = PhotonUI.Pending or {}; table.insert(PhotonUI.Pending, { id, title, settings })
-- or waits for hook "PhotonUIReady".
PhotonUI = PhotonUI or {}
local SECTIONS = {}
local function register(s)
	local key, default = s[1], (s[3] == "slider") and -1 or s[4][1][1]
	if ConVarExists("photon_ui_" .. string.sub(key, 8)) then return end
	local cvar = CreateClientConVar("photon_ui_" .. string.sub(key, 8), tostring(values[key] or default), true, false, s[5])
	cvars.AddChangeCallback(cvar:GetName(), function(_, _, new) set(key, tonumber(new)) end, "photon_ui")
end
function PhotonUI.AddSection(id, title, settings)
	for _, s in ipairs(settings) do
		s[6] = "section:" .. id
		SETTINGS[#SETTINGS + 1] = s
		register(s)
	end
	SECTIONS[#SECTIONS + 1] = { id, title }
end

-- DYN: the dynamic-quality steps the order editor lists: { name, label, side }, Photon CPU's first (its default order), then the GPU ones.
-- A module can add more: PhotonUI.AddDynSteps({ { name, label, "cpu" / "gpu" }, ... }), or queue them in PhotonUI.PendingDyn first.
local DYN_STEPS = {
	-- Photon CPU's (photon-64, modules/cpu/src/dyn.c), its default order; only gameplay-spawned things, never map or player-built props
	{ "loot", "Remove dropped loot", "cpu" },
	{ "debris", "Remove gibs and debris", "cpu" },
	{ "corpses64", "At most 64 corpses", "cpu" },
	{ "corpses32", "At most 32 corpses", "cpu" },
	{ "corpses16", "At most 16 corpses", "cpu" },
	{ "corpsedraw", "Draw corpses only nearby", "cpu" },
	{ "tickcap4", "At most 4 game ticks per frame", "cpu" },
	{ "tickcap2", "At most 2 game ticks per frame", "cpu" },
}
function PhotonUI.AddDynSteps(steps)
	for _, st in ipairs(steps) do DYN_STEPS[#DYN_STEPS + 1] = st end
end
for _, p in ipairs(PhotonUI.PendingDyn or {}) do PhotonUI.AddDynSteps(p) end
PhotonUI.PendingDyn = nil
local GPU_STEPS = {
	{ "shadows1", "Hard shadow edges, one lamp shadow", "gpu" },
	{ "shadows2", "Half shadow distance", "gpu" },
	{ "shadows3", "Half shadow resolution", "gpu" },
	{ "shadows4", "Quarter shadow distance", "gpu" },
	{ "shadows5", "The game's own shadows", "gpu" },
	{ "nanitelod", "Simpler map props at half the distance (Nanite)", "gpu" },
	{ "ptcheck", "Path tracer: half the pixels per frame", "gpu" },
	{ "renderscale1", "3D view at 80% resolution", "gpu" },   -- carried out by Photon CPU's client Lua (mat_viewportscale reacts only to the console)
	{ "renderscale2", "3D view at 65% resolution", "gpu" },
}
-- the order in effect: { name, on } rows; photon_dyn_order empty = every step, Photon CPU's first
local function dyn_rows()
	local all, by = {}, {}
	for _, st in ipairs(DYN_STEPS) do all[#all + 1] = st end
	for _, st in ipairs(GPU_STEPS) do all[#all + 1] = st end
	for _, st in ipairs(all) do by[st[1]] = st end
	local rows, seen = {}, {}
	local o = values.photon_dyn_order or ""
	if o == "" then for _, st in ipairs(all) do rows[#rows + 1] = { st, true } end return rows end
	for name in string.gmatch(o, "[^,%s]+") do if by[name] and not seen[name] then rows[#rows + 1] = { by[name], true } seen[name] = true end end
	for _, st in ipairs(all) do if not seen[st[1]] then rows[#rows + 1] = { st, false } end end
	return rows
end
local function dyn_save(rows)
	local on = {}
	for _, r in ipairs(rows) do if r[2] then on[#on + 1] = r[1][1] end end
	values.photon_dyn_order = #on > 0 and table.concat(on, ",") or "none"
	save()
end
local function add_dyn_order(parent)
	local h = parent:Add("DLabel")
	h:SetText("Dynamic quality: steps, first tried first") h:SetDark(true) h:Dock(TOP) h:DockMargin(0, 2, 0, 2)
	local lv = parent:Add("DListView")
	lv:Dock(TOP) lv:SetTall(150) lv:SetMultiSelect(false)
	lv:AddColumn("On"):SetFixedWidth(32) lv:AddColumn("Step") lv:AddColumn("Saves"):SetFixedWidth(46)
	lv:SetTooltip("Dynamic quality takes these in order (only the ones that are on), on the side that limits the frame: CPU (processor) or GPU (graphics card). Select a row to move it or turn it off.")
	local rows = dyn_rows()
	local function fill(sel)
		lv:Clear()
		for i, r in ipairs(rows) do
			local line = lv:AddLine(r[2] and "x" or "", r[1][2], string.upper(r[1][3]))
			line.idx = i
			if i == sel then lv:SelectItem(line) end
		end
	end
	fill()
	local bar = parent:Add("DPanel")
	bar:Dock(TOP) bar:SetTall(24) bar:DockMargin(0, 2, 0, 6) bar:SetPaintBackground(false)
	local function selected() local _, line = lv:GetSelectedLine() return line and line.idx end
	local function btn(text, fn)
		local b = bar:Add("DButton") b:SetText(text) b:Dock(LEFT) b:SetWide(70) b:DockMargin(0, 0, 4, 0) b.DoClick = fn
	end
	btn("Up", function() local i = selected() if i and i > 1 then rows[i], rows[i - 1] = rows[i - 1], rows[i] dyn_save(rows) fill(i - 1) end end)
	btn("Down", function() local i = selected() if i and i < #rows then rows[i], rows[i + 1] = rows[i + 1], rows[i] dyn_save(rows) fill(i + 1) end end)
	btn("On / off", function() local i = selected() if i then rows[i][2] = not rows[i][2] dyn_save(rows) fill(i) end end)
	btn("Reset", function() values.photon_dyn_order = nil save() rows = dyn_rows() fill() end)
end

-- PBR packs (TASKS PBR-PACK; user, 2026-11-10: "Enable PBR for: HL2, EP1, etc ... and warns you of the size"): per game, roughness,
-- metalness, height and normal maps for the Path traced mode, made without AI from the game's own textures (tools/make_pbr_pack.py).
-- Photon reads them as GMA files from garrysmod/data/photon/pbr/ or from the paths in data/photon/pbr_packs.txt (src/proxy_shaderapi/
-- pbrpack.c), at the next map load. A download comes from the Steam Workshop once a pack is published there (wsid below).
local PBR_PACKS = {
	{ "hl2", "Half-Life 2", 183, nil, nil },
	{ "ep1", "Half-Life 2: Episode One", 8, "episodic", nil },
	{ "ep2", "Half-Life 2: Episode Two", 48, "ep2", nil },
	{ "css", "Counter-Strike: Source", 180, "cstrike", nil },
	{ "gmod", "Garry's Mod (gm_construct, gm_flatgrass ...)", 7, nil, nil },
}   -- id, title, MiB, the game to mount (IsMounted) or nil, Workshop id (nil: not published yet)
local function pbr_listed()
	local t = {}
	for line in string.gmatch(file.Read("photon/pbr_packs.txt", "DATA") or "", "[^\r\n]+") do t[line] = true end
	return t
end
local function pbr_installed(id)
	if file.Exists("photon/pbr/photon_pbr_" .. id .. ".gma", "DATA") then return true end
	for line in pairs(pbr_listed()) do if string.find(line, "photon_pbr_" .. id, 1, true) or string.find(line, "#" .. id, 1, true) then return true end end
	return false
end
local function add_pbr_packs(parent)
	local h = parent:Add("DLabel")
	h:SetText("PBR packs (per-texture roughness, metal and surface detail for Path traced)") h:SetFont("DermaDefaultBold") h:SetDark(true)
	h:Dock(TOP) h:DockMargin(0, 8, 0, 4)
	for _, pk in ipairs(PBR_PACKS) do
		local id, title, mib, game_id, wsid = pk[1], pk[2], pk[3], pk[4], pk[5]
		local row = parent:Add("DPanel")
		row:Dock(TOP) row:SetTall(24) row:DockMargin(0, 0, 0, 3) row:SetPaintBackground(false)
		local lab = row:Add("DLabel")
		local mounted = not game_id or IsMounted(game_id)
		local inst = pbr_installed(id)
		lab:SetText(string.format("%s, %d MB%s", title, mib, inst and ": installed" or (mounted and "" or " (game not mounted)")))
		lab:SetDark(true) lab:Dock(FILL)
		local b = row:Add("DButton") b:Dock(RIGHT) b:SetWide(110)
		if inst then
			b:SetText("Remove") b.DoClick = function()
				file.Delete("photon/pbr/photon_pbr_" .. id .. ".gma")
				local keep = {}
				for line in pairs(pbr_listed()) do if not (string.find(line, "photon_pbr_" .. id, 1, true) or string.find(line, "#" .. id, 1, true)) then keep[#keep + 1] = line end end
				file.Write("photon/pbr_packs.txt", table.concat(keep, "\n"))
				lab:SetText(title .. ": removed (the next map load)") b:SetEnabled(false)
			end
		elseif wsid then
			b:SetText("Download") b.DoClick = function()
				Derma_Query(string.format("Download the %s PBR pack (%d MB) from the Steam Workshop? It applies from the next map load.", title, mib), "Photon",
					"Download", function()
						lab:SetText(title .. ": downloading ...") b:SetEnabled(false)
						steamworks.DownloadUGC(wsid, function(path)
							if not path then lab:SetText(title .. ": download failed") b:SetEnabled(true) return end
							local cur = file.Read("photon/pbr_packs.txt", "DATA") or ""
							file.CreateDir("photon")
							file.Write("photon/pbr_packs.txt", cur .. (cur ~= "" and "\n" or "") .. path .. " #" .. id)
							lab:SetText(title .. ": installed (the next map load)")
						end)
					end, "Cancel")
			end
		else
			b:SetText("How to install") b.DoClick = function()
				Derma_Message("This pack is not on the Workshop yet. To install it by hand, put photon_pbr_" .. id .. ".gma into garrysmod/data/photon/pbr/ and load a map.", "Photon", "OK")
			end
		end
		b:SetEnabled(b:IsEnabled() and (mounted or inst))
	end
end

-- SP layer 1 (analysis/47): the shader packs' passes (photon_packs.lua): on / off, their parameters, compile errors; only when a pack exists
local function add_shader_packs(parent)
	if not (Photon and Photon.Passes) then return end
	local passes, feats = Photon.Passes(), Photon.PackFeatures and Photon.PackFeatures() or {}
	local packs = {}
	for k, p in pairs(passes) do packs[string.lower(p.pack)] = true end
	for pk in pairs(feats) do packs[pk] = true end
	if not next(packs) then return end
	local h = parent:Add("DLabel")
	h:SetText("Shader packs (installed addons that require Photon)") h:SetFont("DermaDefaultBold") h:SetDark(true) h:Dock(TOP) h:DockMargin(0, 8, 0, 4)
	for pk in SortedPairs(packs) do
		local pcb = parent:Add("DCheckBoxLabel")
		pcb:SetText(pk) pcb:SetFont("DermaDefaultBold") pcb:SetDark(true) pcb:SetValue(Photon.PackEnabled(pk)) pcb:Dock(TOP) pcb:DockMargin(0, 4, 0, 2)
		pcb.OnChange = function(_, v) Photon.SetPackEnabled(pk, v) end
		if feats[pk] and next(feats[pk]) then
			local list = {}
			for key, v in SortedPairs(feats[pk]) do list[#list + 1] = string.sub(key, 8) .. " = " .. tostring(v) end
			local f = parent:Add("DLabel")
			f:SetText("Sets Photon's features: " .. table.concat(list, ", ")) f:SetWrap(true) f:SetAutoStretchVertical(true) f:SetDark(true) f:Dock(TOP) f:DockMargin(16, 0, 0, 2)
		end
		for k, p in SortedPairs(passes) do
			if string.lower(p.pack) == pk then
				local cb = parent:Add("DCheckBoxLabel")
				cb:SetText(p.name) cb:SetDark(true) cb:SetValue(p.on) cb:Dock(TOP) cb:DockMargin(16, 2, 0, 2)
				cb.OnChange = function(_, v) Photon.SetPassEnabled(k, v) end
				local err = Photon.PassError(k)
				if err ~= "" then
					local e = parent:Add("DLabel")
					e:SetText("Compile error: " .. string.sub(err, 1, 300)) e:SetWrap(true) e:SetAutoStretchVertical(true) e:SetTextColor(Color(180, 30, 30)) e:Dock(TOP) e:DockMargin(16, 0, 0, 0)
				end
				for i, prm in ipairs(p.params) do
					local sl = parent:Add("DNumSlider")
					sl:Dock(TOP) sl:SetTall(22) sl:DockMargin(32, 0, 0, 0) sl:SetText(prm[1]) sl:SetDark(true)
					sl:SetMinMax(prm[3] or 0, prm[4] or 1) sl:SetDecimals(2) sl:SetValue(p.values[i] or prm[2] or 0)
					sl.OnValueChanged = function(_, v) Photon.SetPassParam(k, i, v) end
				end
			end
		end
	end
end

for _, s in ipairs(SETTINGS) do register(s) end
for _, p in ipairs(PhotonUI.Pending or {}) do PhotonUI.AddSection(p[1], p[2], p[3]) end
PhotonUI.Pending = nil
hook.Run("PhotonUIReady")

local MODES = {
	{ "Optimized", "The game's own look with Photon's speed-ups.", { photon_preset = 0, photon_pathtrace = 0 }, nil },
	{ "Better lighting", "Photon's real-time sun shadows and one lamp's shadows, close to the game's look.", { photon_preset = 1, photon_pathtrace = 0 }, "lighting" },
	{ "Best quality", "Sharper and softer shadows, four lamps' shadows, the map's lighting re-baked sharper.", { photon_preset = 2, photon_pathtrace = 0 }, "lighting" },
	{ "Path traced", "Light traced by Photon from the map's own sun and lamps: bounced light, colour bleeding and glossy reflections, in real time on the Vulkan route (from the next game start), or baked into the map once.", { photon_preset = 0, photon_pathtrace = 2 }, "pt" },
	{ "Custom", "Only the settings below, nothing from a preset.", { photon_preset = 3, photon_pathtrace = 0 }, "lighting" },
}
local function current_mode()
	if (values.photon_pathtrace or 0) ~= 0 then return 4 end
	local p = values.photon_preset or 0
	return p == 3 and 5 or p + 1
end
local function cv(key, v) RunConsoleCommand("photon_ui_" .. string.sub(key, 8), tostring(v)) end

local function add_setting(parent, s)
	local key, label, kind, opts, help = s[1], s[2], s[3], s[4], s[5]
	local row = parent:Add("DPanel")
	row:Dock(TOP) row:DockMargin(0, 0, 0, 6) row:SetTall(44) row:SetPaintBackground(false)
	local lab = row:Add("DLabel")
	lab:SetText(label) lab:SetDark(true) lab:Dock(TOP) lab:SetTall(16)
	if kind == "choice" then
		local box = row:Add("DComboBox")
		box:Dock(TOP) box:SetTall(22)
		local cur = values[key]
		for _, o in ipairs(opts) do box:AddChoice(o[2], o[1], cur == o[1] or (cur == nil and o == opts[1])) end
		box.OnSelect = function(_, _, _, data) cv(key, data) end
		box:SetTooltip(help)
	else
		local sl = row:Add("DNumSlider")
		sl:Dock(TOP) sl:SetTall(22) sl:SetText("") sl:SetMinMax(opts[1], opts[2]) sl:SetDecimals(opts[3])
		sl:SetConVar("photon_ui_" .. string.sub(key, 8)) sl:SetTooltip(help)
	end
	lab:SetTooltip(help)
end
local function add_group(parent, title, group)
	local h = parent:Add("DLabel")
	h:SetText(title) h:SetFont("DermaDefaultBold") h:SetDark(true) h:Dock(TOP) h:DockMargin(0, 8, 0, 4)
	for _, s in ipairs(SETTINGS) do if s[6] == group then add_setting(parent, s) end end
end

local function build(window)
	window:SetTitle("Photon")
	window:SetSize(math.min(ScrW() - 40, 760), math.min(ScrH() - 40, 560))
	window:SetSizable(true) window:SetMinWidth(560) window:SetMinHeight(360)
	local left = window:Add("DPanel")
	left:Dock(LEFT) left:SetWide(170) left:DockPadding(6, 6, 6, 6)
	local right = window:Add("DScrollPanel")
	right:Dock(FILL) right:DockMargin(8, 0, 0, 0)
	local buttons = {}
	local function show(m)
		right:Clear()
		local md = MODES[m]
		local d = right:Add("DLabel")
		d:SetText(md[1] .. ": " .. md[2]) d:SetWrap(true) d:SetAutoStretchVertical(true) d:SetDark(true) d:Dock(TOP) d:DockMargin(0, 4, 0, 4)
		if md[4] == "lighting" then add_group(right, "Lighting and shadows", "lighting")
		elseif md[4] == "pt" then add_group(right, "Path tracer", "pt") add_pbr_packs(right)
		end
		add_group(right, "All modes", "general")
		add_shader_packs(right)
		add_dyn_order(right)
		for _, sec in ipairs(SECTIONS) do add_group(right, sec[2], "section:" .. sec[1]) end
		for i, b in ipairs(buttons) do b:SetToggle(i == m) end
	end
	for i, md in ipairs(MODES) do
		local b = left:Add("DButton")
		b:SetText(md[1]) b:Dock(TOP) b:DockMargin(0, 0, 0, 4) b:SetTall(30) b:SetIsToggle(true) b:SetTooltip(md[2])
		b.DoClick = function()
			for key, v in pairs(md[3]) do cv(key, v) end
			show(i)
		end
		buttons[i] = b
	end
	local note = left:Add("DLabel")
	note:SetText("Client side only. Changes apply within a few seconds; lights and reflections on the next map; CPU settings at the next game start.")
	note:SetWrap(true) note:SetAutoStretchVertical(true) note:SetDark(true) note:Dock(BOTTOM)
	show(current_mode())
end

list.Set("DesktopWindows", "PhotonSettings", {
	title = "Photon",
	icon = "icon64/tool.png",
	width = 760, height = 560,
	onewindow = true,
	init = function(icon, window) load() build(window) end,
})
concommand.Add("photon_settings", function()   -- the same window from the console
	local f = vgui.Create("DFrame")
	load() build(f) f:Center() f:MakePopup()
end)
