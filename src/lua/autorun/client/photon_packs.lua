-- Photon shader packs, layer 1 (analysis/47; user, 2026-11-11: "workshop addons that require photon"): the API a pack's Lua calls.
-- A pack is a normal addon (lua/autorun/client/<pack>.lua). It registers screen passes written in HLSL (ps_3_0, entry "main"):
--
--   local function reg()
--     Photon.RegisterPass({ pack = "mypack", name = "vignette", order = 50, hlsl = [[ ... ]],
--                           params = { { "Strength", 0.6, 0, 1 }, { "Radius", 0.75, 0.2, 1.5 } } })
--   end
--   if Photon and Photon.RegisterPass then reg() else hook.Add("PhotonPacksReady", "mypack", reg) end
--
-- Inputs of a pass (Photon's proxy, src/proxy_shaderapi/packs.c): s0 the frame so far, s1 the scene depth (0 near .. 1 far); c0 (1/w, 1/h,
-- seconds, frame), c1..c4 inverse view-projection rows, c5 eye, c6..c9 view-projection rows, c10..c13 the 16 parameters in order.
-- Photon writes the source to data/photon/packs/<pack>/<name>.txt and the active list to data/photon/packs/active.txt; the proxy
-- compiles and runs them after its own passes; compile errors land in <name>.err.txt (Photon.PassError). Without Photon's proxy
-- nothing happens (the files are only data).
Photon = Photon or {}
local passes = {}      -- "pack/name" -> { pack, name, order, hlsl, params = { {label, default, min, max} }, values = {}, on }
local DIR = "photon/packs"
local STATE = DIR .. "/enabled.txt"   -- "pack/name 0|1" per line: the user's choices

local function clean(s) return string.lower(string.gsub(tostring(s or ""), "[^%w_%-%.]", "_")) end
local function key(pack, name) return clean(pack) .. "/" .. clean(name) end

local function load_state()
	local t = {}
	for line in string.gmatch(file.Read(STATE, "DATA") or "", "[^\r\n]+") do
		local k, v = string.match(line, "^(%S+)%s+(%d)")
		if k then t[k] = v == "1" end
	end
	return t
end
local function save_state()
	local st = load_state()
	for k, p in pairs(passes) do st[k] = p.on end
	local out = {}
	for k, v in SortedPairs(st) do out[#out + 1] = k .. " " .. (v and "1" or "0") end
	file.Write(STATE, table.concat(out, "\n"))
end
local function write_active()
	local list = {}
	for k, p in pairs(passes) do if p.on then list[#list + 1] = p end end
	table.sort(list, function(a, b) if a.order ~= b.order then return a.order < b.order end return a.key < b.key end)
	local out = { "# written by photon_packs.lua: <pack>/<pass> and its parameters, in run order" }
	for _, p in ipairs(list) do
		local v = {}
		for i = 1, 16 do v[i] = string.format("%g", p.values[i] or 0) end
		out[#out + 1] = p.key .. " " .. table.concat(v, " ")
	end
	file.CreateDir(DIR)
	file.Write(DIR .. "/active.txt", table.concat(out, "\n"))
end

function Photon.RegisterPass(t)
	assert(type(t) == "table" and t.pack and t.name and type(t.hlsl) == "string", "Photon.RegisterPass{ pack, name, hlsl, [order], [params] }")
	local k = key(t.pack, t.name)
	local state = load_state()
	local p = passes[k] or {}
	p.key, p.pack, p.name, p.order, p.hlsl, p.params = k, t.pack, t.name, tonumber(t.order) or 50, t.hlsl, t.params or {}
	p.values = p.values or {}
	for i, prm in ipairs(p.params) do if p.values[i] == nil then p.values[i] = tonumber(prm[2]) or 0 end end
	if p.on == nil then p.on = state[k] ~= false and state["pack:" .. clean(t.pack)] ~= false end   -- on unless the user turned it (or its pack) off
	passes[k] = p
	file.CreateDir(DIR); file.CreateDir(DIR .. "/" .. clean(t.pack))
	local path = DIR .. "/" .. k .. ".txt"
	if file.Read(path, "DATA") ~= t.hlsl then file.Write(path, t.hlsl) end   -- unchanged source: no recompile
	write_active()
	return k
end
-- Photon's own features from a pack (user, 2026-11-11: "photon is built with tons of possibilities and features, shader packs simply modify
-- and disable/enable some"): Photon.SetFeature(pack, "photon_toon", 1) etc. (any key of Photon's settings: photon_menu.lua, vlg.c). While
-- the pack is enabled its choices override the user's own settings (data/photon/packs/features.txt, read by the proxy like settings.txt);
-- turning the pack off returns the user's settings. Some features apply from the next map or game start, as in the menu.
local features = {}    -- pack -> { key -> value }
local function pack_on(pack)
	local st = load_state()
	return st["pack:" .. clean(pack)] ~= false
end
local function write_features()
	local out, seen = {}, {}
	for pack, t in SortedPairs(features) do
		if pack_on(pack) then for k, v in SortedPairs(t) do seen[k] = v end end   -- a later pack wins a key
	end
	for k, v in SortedPairs(seen) do out[#out + 1] = k .. " " .. tostring(v) end
	file.CreateDir(DIR)
	file.Write(DIR .. "/features.txt", table.concat(out, "\n"))
end
function Photon.SetFeature(pack, key, value)
	assert(pack and type(key) == "string" and string.sub(key, 1, 7) == "photon_", "Photon.SetFeature(pack, \"photon_<setting>\", value)")
	local p = clean(pack)
	features[p] = features[p] or {}
	features[p][key] = value
	write_features()
end
function Photon.PackFeatures() return features end
function Photon.PackEnabled(pack) return pack_on(pack) end
function Photon.SetPackEnabled(pack, on)   -- the pack's features and all its passes
	local st = load_state()
	st["pack:" .. clean(pack)] = on and true or false
	local out = {}
	for k, v in SortedPairs(st) do out[#out + 1] = k .. " " .. (v and "1" or "0") end
	file.Write(STATE, table.concat(out, "\n"))
	for k, p in pairs(passes) do if clean(p.pack) == clean(pack) then p.on = on and true or false end end
	save_state(); write_active(); write_features(); write_texfilter()
end
-- The texture stage (layer 1b): Photon.RegisterTextureFilter{ pack, name, hlsl, order, params } changes how the map's and models' base
-- textures look. Each texture is run once through the shader (HLSL ps_3_0 "main": s0 the original texture, c0 xy 1/size, c10..c13 the
-- params) into Photon's own copy at half size; one filter is active at a time (the enabled one with the lowest order).
local tfilters = {}
local function write_texfilter()
	local best
	for k, f in pairs(tfilters) do
		if pack_on(f.pack) and (not best or f.order < best.order or (f.order == best.order and k < best.key)) then best = f end
	end
	local line = ""
	if best then
		local v = {}
		for i = 1, 16 do v[i] = string.format("%g", best.values[i] or 0) end
		line = best.key .. " " .. table.concat(v, " ")
	end
	file.CreateDir(DIR)
	file.Write(DIR .. "/texfilter.txt", line)
end
function Photon.RegisterTextureFilter(t)
	assert(type(t) == "table" and t.pack and t.name and type(t.hlsl) == "string", "Photon.RegisterTextureFilter{ pack, name, hlsl, [order], [params] }")
	local k = key(t.pack, t.name)
	local f = tfilters[k] or {}
	f.key, f.pack, f.name, f.order, f.params = k, t.pack, t.name, tonumber(t.order) or 50, t.params or {}
	f.values = f.values or {}
	for i, prm in ipairs(f.params) do if f.values[i] == nil then f.values[i] = tonumber(prm[2]) or 0 end end
	tfilters[k] = f
	file.CreateDir(DIR); file.CreateDir(DIR .. "/" .. clean(t.pack))
	local path = DIR .. "/" .. k .. ".txt"
	if file.Read(path, "DATA") ~= t.hlsl then file.Write(path, t.hlsl) end
	write_texfilter()
	return k
end
function Photon.SetTextureFilterParam(k, i, v) local f = tfilters[k]; if not f then return end; f.values[i] = tonumber(v) or 0; write_texfilter() end
function Photon.TextureFilters() return tfilters end
function Photon.SetPassEnabled(k, on) local p = passes[k]; if not p then return end; p.on = on and true or false; save_state(); write_active() end
function Photon.SetPassParam(k, i, v) local p = passes[k]; if not p then return end; p.values[i] = tonumber(v) or 0; write_active() end
function Photon.Passes() return passes end
function Photon.PassError(k) return file.Read(DIR .. "/" .. k .. ".err.txt", "DATA") or "" end

-- console: list, toggle, set a parameter
concommand.Add("photon_packs", function()
	for k, p in SortedPairs(passes) do
		local e = Photon.PassError(k)
		print(string.format("%s  %s  order %d%s", k, p.on and "on" or "off", p.order, e ~= "" and ("  ERROR: " .. string.sub(e, 1, 200)) or ""))
		for i, prm in ipairs(p.params) do print(string.format("    %d %s = %g (%g .. %g)", i, prm[1], p.values[i], prm[3] or 0, prm[4] or 1)) end
	end
end)
concommand.Add("photon_pack_toggle", function(_, _, a) local p = passes[a[1] or ""]; if p then Photon.SetPassEnabled(a[1], not p.on) end end)
concommand.Add("photon_pack_param", function(_, _, a) Photon.SetPassParam(a[1] or "", tonumber(a[2]) or 1, a[3]) end)

-- passes whose pack is gone (unsubscribed) leave active.txt at the next start: it is rewritten from what registers this session
file.CreateDir(DIR)
file.Write(DIR .. "/active.txt", "")
file.Write(DIR .. "/features.txt", "")   -- (rewritten from what registers this session)
file.Write(DIR .. "/texfilter.txt", "")
hook.Run("PhotonPacksReady")
hook.Add("InitPostEntity", "PhotonPacksReady", function() hook.Run("PhotonPacksReady") end)
