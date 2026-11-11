-- photon_cpu_findclass.lua (Photon CPU, phase 2 CF1): ents.FindByClass answered from a cache while no entity has been created or
-- removed. CF0 (analysis/15): in the play scene the client called it ~375 times a second (LVS skidmarks, every translucent pass, for
-- "lvs_wheeldrive_wheel"), 23 us each, every call a walk of the whole entity list.
-- Same answers as GMod's:
--   - one cached list per argument string (wildcards included), dropped on every OnEntityCreated and EntityRemoved, and at the start of
--     every frame (so a handler of another addon that runs before ours inside the same hook can only see a list from this frame);
--   - each call returns a new table (callers may change the one they get), with the entities in the engine's order;
--   - installed only while ents.FindByClass is still the engine's C function.
-- Shared (both realms). Inert unless data/photon_harness.txt holds "cpufind=1" (serve) or "cpufind=2" (check: every call also asks the
-- engine, returns the engine's answer, and counts any difference; PHOTON_CPU_FIND lines).
if not file.Exists("photon_harness.txt", "DATA") then return end
local opt = (file.Read("photon_harness.txt", "DATA") or ""):match("cpufind=(%d)")
if opt ~= "1" and opt ~= "2" then return end
local check = opt == "2"
local realm = SERVER and "server" or "client"

local FindByClass = ents.FindByClass
if not isfunction(FindByClass) or debug.getinfo(FindByClass, "S").what ~= "C" then
	print("PHOTON_CPU_FIND " .. realm .. " not installed: ents.FindByClass is not the engine's")
	return
end

local cache = {}
local stats = { hits = 0, misses = 0, mismatches = 0, drops = 0 }
local function drop() if next(cache) then cache = {}; stats.drops = stats.drops + 1 end end
hook.Add("OnEntityCreated", "photon_cpu_findclass", drop)
hook.Add("EntityRemoved", "photon_cpu_findclass", drop)
local frame = -1
local frame_of = CLIENT and FrameNumber or engine.TickCount

local function copy(t)
	local r = {}
	for i = 1, #t do r[i] = t[i] end
	return r
end

ents.FindByClass = function(class)
	local f = frame_of()
	if f ~= frame then frame = f; cache = {} end
	local key = class
	local c = isstring(key) and cache[key]
	if not c then
		local r = FindByClass(class)
		if isstring(key) then cache[key] = copy(r) end
		stats.misses = stats.misses + 1
		return r
	end
	if check then
		local real = FindByClass(class)
		local same = #real == #c
		if same then for i = 1, #c do if real[i] ~= c[i] then same = false; break end end end
		if not same then
			stats.mismatches = stats.mismatches + 1
			if stats.mismatches <= 10 then
				print(string.format("PHOTON_CPU_FIND %s MISMATCH %s: cached %d entities, engine %d", realm, tostring(class), #c, #real))
			end
			cache[key] = copy(real)
			return real
		end
	end
	stats.hits = stats.hits + 1
	return copy(c)
end

local function report()
	print(string.format("PHOTON_CPU_FIND %s %s hits %d misses %d drops %d mismatches %d", realm, check and "check" or "serve",
		stats.hits, stats.misses, stats.drops, stats.mismatches))
end
concommand.Add(SERVER and "photon_cpu_find_stats" or "photon_cpu_find_stats_cl", report)
timer.Create("photon_cpu_find_stats", 15, 0, report)
print("PHOTON_CPU_FIND " .. realm .. " installed (" .. (check and "check" or "serve") .. ")")
