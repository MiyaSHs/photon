-- GMod syntax: the new engine must print exactly what GMod's lua_shared prints.
local out = {}
local function p(...) local t = {} for i = 1, select("#", ...) do t[#t + 1] = tostring((select(i, ...))) end out[#out + 1] = table.concat(t, " ") end
// a C++ line comment
/* a block
   comment over lines */ p("comments", 1 /* inline */ + 2)
p("not", !true, !false, !nil, not 0)
p("ne", 1 != 2, 1 != 1, "a" != "a")
p("and/or", true && false, false || "x", nil && 1, 1 || 2)
local a = 0
for i = 1, 10 do if i % 2 == 0 then continue end a = a + i end
p("fornum continue", a)
local b = {}
for k, v in ipairs({ 1, 2, 3, 4, 5 }) do if v == 3 then continue end b[#b + 1] = v end
p("forin continue", table.concat(b, ","))
local i, c = 0, 0
while i < 10 do i = i + 1 if i > 5 then continue end c = c + 1 end
p("while continue", i, c)
local j, d = 0, 0
repeat j = j + 1 if j % 3 == 0 then continue end d = d + j until j >= 9
p("repeat continue", j, d)
local nested = 0
for x = 1, 3 do for y = 1, 3 do if y == 2 then continue end nested = nested + x * y end end
p("nested continue", nested)
-- closures over loop locals with continue (upvalue closing)
local fs = {}
for k = 1, 4 do local kk = k * 10 if k == 2 then continue end fs[#fs + 1] = function() return kk end end
local r = {} for _, f in ipairs(fs) do r[#r + 1] = f() end
p("closures", table.concat(r, ","))
p("goto", (function() local n = 0 ::top:: n = n + 1 if n < 3 then goto top end return n end)())
p("division", 7 / 2) // a trailing comment after code
-- iteration order of string-keyed tables (hook order depends on it)
local t = {}
for _, k in ipairs({ "Think", "Tick", "HUDPaint", "PlayerSpawn", "zeta", "alpha", "PostDrawOpaqueRenderables", "a1", "b2", "c3", "PhotonCpuProbeGC", "CreateMove", "StartCommand", "SetupMove" }) do t[k] = true end
local order = {} for k in pairs(t) do order[#order + 1] = k end
p("pairs order", table.concat(order, " "))
local t2 = {} for n = 1, 40 do t2["hook_" .. n] = n end
local o2 = {} for k in pairs(t2) do o2[#o2 + 1] = k end
p("pairs order 40", table.concat(o2, " "))
return table.concat(out, "\n")
