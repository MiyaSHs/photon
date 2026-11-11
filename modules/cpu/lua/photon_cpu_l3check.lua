-- photon_cpu_l3check.lua (client and server autorun): checks the replacement lua_shared in game. Prints "PHOTON_L3 <realm> <check> ok|FAIL ..."
-- lines a script greps. Checks the CLuaInterface paths GMod itself drives: include, require of a Lua module, pcall errors, metatables
-- (MetaName/MetaID), the hook table, Color, and the version string.
local realm = SERVER and "sv" or "cl"
local function check(name, ok, info)
  local line = ("PHOTON_L3 %s %s %s %s"):format(realm, name, ok and "ok" or "FAIL", tostring(info or ""))
  Msg(line .. "\n")   -- the engine console directly
  print("print: " .. line)   -- and through print, to check print itself
end
local pi = jit.util.funcinfo(print)
check("print_origin", true, "ffid=" .. tostring(pi.ffid) .. " addr=" .. tostring(pi.addr) .. " " .. tostring(print))
check("version", jit.version == "LuaJIT 2.1.0-beta3", jit.version)
check("sandbox", load == nil and dofile == nil and loadstring == nil and isfunction(require) and isfunction(include), "")
local okp, err = pcall(error, "boom")
check("pcall", not okp and tostring(err):find("boom") ~= nil, err)
local mt = FindMetaTable("Entity")
check("metatable", istable(mt) and mt.MetaName == "Entity" and mt.MetaID == TYPE_ENTITY, mt and (tostring(mt.MetaName) .. "/" .. tostring(mt.MetaID)))
check("vector", Vector(1, 2, 3):Length() > 3.7 and isvector(Vector()) and isangle(Angle()), Vector(1, 2, 3))
local c = Color(1, 2, 3, 4)
check("color", IsColor(c) and c.r == 1 and c.a == 4, c)
check("hook", istable(hook) and isfunction(hook.Add) and istable(hook.GetTable()), "")
-- error paths (a missing require, script errors) are checked separately: ERRORS=1 in l3_vm.sh
if PHOTON_L3_ERRORS then
  local okr = pcall(require, "photon_l3_nonexistent")
  check("require_missing_errors", not okr, "")
end
check("debug_trimmed", debug.setlocal == nil and debug.setupvalue == nil and isfunction(debug.getinfo), "")
check("compile", isfunction(CompileString("return 1 + 1", "l3test")) and CompileString("return 1 + 1", "l3test")() == 2, "")
local bad = CompileString("return +", "l3bad", false)
check("compile_error", isstring(bad), bad)
hook.Add("Think", "photon_l3", function()
  hook.Remove("Think", "photon_l3")
  check("think", true, CurTime())
end)
-- the connection, as the server sees it (a client that never joins shows up here with the reason)
if SERVER then
  for _, ev in ipairs({ "player_connect", "player_disconnect", "player_activate" }) do
    gameevent.Listen(ev)
    hook.Add(ev, "photon_l3", function(data)
      local parts = {}
      for k, v in pairs(data) do parts[#parts + 1] = tostring(k) .. "=" .. tostring(v) end
      table.sort(parts)
      Msg("PHOTON_L3 sv event " .. ev .. " " .. table.concat(parts, " ") .. "\n")
    end)
  end
end
-- the scene in use (l3_use.sh): what actually happened, every 5 s, so ours and stock can be compared
if SERVER then
  local kills, rags, dmg = 0, 0, 0
  hook.Add("OnNPCKilled", "photon_l3", function() kills = kills + 1 end)
  hook.Add("CreateEntityRagdoll", "photon_l3", function() rags = rags + 1 end)
  hook.Add("EntityTakeDamage", "photon_l3", function() dmg = dmg + 1 end)
  timer.Create("photon_l3_use", 5, 0, function()
    local npcs, ragents = 0, 0
    for _, e in ipairs(ents.GetAll()) do
      if e:IsNPC() then npcs = npcs + 1 elseif e:GetClass() == "prop_ragdoll" then ragents = ragents + 1 end
    end
    local p = Entity(1)
    local w = IsValid(p) and p:GetActiveWeapon()
    Msg(("PHOTON_L3 sv use t=%d kills=%d ragdolls_made=%d damage_events=%d npcs=%d prop_ragdolls=%d weapon=%s\n"):format(
      math.floor(CurTime()), kills, rags, dmg, npcs, ragents, IsValid(w) and w:GetClass() or "-"))
  end)
end
