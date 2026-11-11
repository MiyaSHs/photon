-- photon_cpu_index.lua (Photon CPU, phase 2 E2): GMod's Entity, Weapon and Player __index without a C call per lookup.
-- GMod's own metamethods (lua/includes/extensions/entity.lua, weapon.lua, player.lua) look a key up in the metatable, then in the
-- entity's table through the C function Entity.GetTable. That C call ends LuaJIT's trace, so the metamethod and the addon code around it
-- run interpreted: ~32% of the executed client Lua with the play set firing (analysis/13). Here the entity's table is cached in a
-- weak-keyed table, so the metamethods are pure Lua and compile.
-- Same answers as GMod's:
--   - the same lookup order (metatable(s), entity table, then the legacy "Owner" key where GMod has it);
--   - the cached table is the one GetTable returned for that entity; from EntityRemoved on, the entity is never served from the cache
--     again (its OnRemove can still read its table, through GetTable), and SetTable drops the entry; if anything replaces Entity.GetTable, every lookup calls the current one again, as GMod's code does;
--   - installed only while the three metamethods are GMod's own.
-- Shared (both realms). Inert unless data/photon_harness.txt holds "cpuindex=1" (serve) or "cpuindex=2" (check: every lookup also
-- calls GetTable, uses its answer, and counts any difference; PHOTON_CPU_INDEX lines).
if not file.Exists("photon_harness.txt", "DATA") then return end
local opt = (file.Read("photon_harness.txt", "DATA") or ""):match("cpuindex=(%d)")
if opt ~= "1" and opt ~= "2" then return end
local check = opt == "2"
local realm = SERVER and "server" or "client"

local entity, weapon, player = FindMetaTable("Entity"), FindMetaTable("Weapon"), FindMetaTable("Player")
local function ours(f, src)
	local i = isfunction(f) and debug.getinfo(f, "S")
	return i and i.short_src == src
end
if not (entity and weapon and player and ours(entity.__index, "lua/includes/extensions/entity.lua")
	and ours(weapon.__index, "lua/includes/extensions/weapon.lua") and ours(player.__index, "lua/includes/extensions/player.lua")) then
	print("PHOTON_CPU_INDEX " .. realm .. " not installed: the metamethods are not GMod's own")
	return
end

local GetTable, SetTable = entity.GetTable, entity.SetTable
local cache = setmetatable({}, { __mode = "k" })
local stats = { hits = 0, misses = 0, mismatches = 0 }

local classes = check and setmetatable({}, { __mode = "k" }) or nil   -- check mode: the class when cached, for the mismatch lines

local function tab_of(self)
	if entity.GetTable ~= GetTable then return entity.GetTable(self) end   -- replaced by someone: call it, as GMod's code would
	local t = cache[self]
	if t == false then return GetTable(self) end   -- being removed (EntityRemoved seen): never cached again
	if t == nil then
		t = GetTable(self)
		if t ~= nil then cache[self] = t; if check then classes[self] = self:GetClass() end end
		if check then stats.misses = stats.misses + 1 end
		return t
	end
	if check then
		local real = GetTable(self)
		if not rawequal(real, t) then
			stats.mismatches = stats.mismatches + 1
			if stats.mismatches <= 10 then
				print(string.format("PHOTON_CPU_INDEX %s MISMATCH %s (class when cached: %s): cached %s, GetTable %s", realm, tostring(self),
					tostring(classes[self]), tostring(t), tostring(real)))
			end
			cache[self] = real ~= nil and real or false
			return real
		end
		stats.hits = stats.hits + 1
	end
	return t
end

function entity:__index(key)
	local val = entity[key]
	if val ~= nil then return val end
	local tab = tab_of(self)
	if tab then
		local tabval = tab[key]
		if tabval ~= nil then return tabval end
	end
	if key == "Owner" then return entity.GetOwner(self) end
	return nil
end

function weapon:__index(key)
	local val = weapon[key]
	if val ~= nil then return val end
	val = entity[key]
	if val ~= nil then return val end
	local tab = tab_of(self)
	if tab ~= nil then
		val = tab[key]
		if val ~= nil then return val end
	end
	if key == "Owner" then return entity.GetOwner(self) end
	return nil
end

function player:__index(key)
	local val = player[key]
	if val ~= nil then return val end
	local entval = entity[key]
	if entval ~= nil then return entval end
	local tab = tab_of(self)
	if tab then return tab[key] end
	return nil
end

if SetTable then
	function entity:SetTable(t)
		local dying = cache[self] == false
		SetTable(self, t)
		cache[self] = dying and false or nil
	end
end

-- removal: the entity stays valid through EntityRemoved and its OnRemove, so its table may still be read; mark it instead of dropping it,
-- so those reads call GetTable and nothing caches it again before it turns NULL
hook.Add("EntityRemoved", "PhotonCpuIndex", function(ent) cache[ent] = false end)

if check then
	timer.Create("PhotonCpuIndexStats", 15, 0, function()
		print(string.format("PHOTON_CPU_INDEX %s check: %d hits, %d misses, %d mismatches", realm, stats.hits, stats.misses, stats.mismatches))
	end)
end
print("PHOTON_CPU_INDEX " .. realm .. " installed (" .. (check and "check" or "serve") .. ")")
