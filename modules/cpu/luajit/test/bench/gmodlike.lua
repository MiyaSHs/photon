-- GMod-style Lua workloads, timed with os.clock (both engines have it). Returns one line per workload: name and milliseconds.
local out = {}
local function bench(name, f, n)
	f(math.floor(n / 10))   -- warm up (JIT)
	local t = os.clock()
	f(n)
	out[#out + 1] = string.format("%-22s %8.1f ms", name, (os.clock() - t) * 1000)
end
-- hook.Call-like: pairs over a string-keyed table of functions, calling each
local hooks = {} for i = 1, 12 do hooks["hook_" .. i] = function(a, b) return nil end end
bench("hook dispatch (pairs)", function(n) for _ = 1, n do for k, v in pairs(hooks) do local r = v(1, 2) if r ~= nil then break end end end end, 400000)
-- entity-style __index metamethod with a per-object table (GMod's Entity __index shape)
local meta = { GetPos = function(self) return 1 end }
local objtab = setmetatable({}, { __mode = "k" })
meta.__index = function(self, key) local v = meta[key] if v ~= nil then return v end local t = objtab[self] if t then return t[key] end end
local ents = {} for i = 1, 200 do local e = setmetatable({}, meta) objtab[e] = { Health = i, Speed = i * 2 } ents[i] = e end
bench("entity field access", function(n) local s = 0 for _ = 1, n do for i = 1, #ents do local e = ents[i] s = s + e.Health + e.Speed + e:GetPos() end end return s end, 2000)
-- vector math with tables (like Vector allocations)
bench("vector alloc math", function(n) local acc = { x = 0, y = 0, z = 0 } for i = 1, n do local v = { x = i, y = i * 2, z = i * 3 } acc = { x = acc.x + v.x, y = acc.y + v.y, z = acc.z + v.z } end return acc end, 2000000)
-- string building and formatting
bench("string format/concat", function(n) local t = {} for i = 1, n do t[#t + 1] = string.format("%s_%d:%.2f", "ent", i, i / 3) end return table.concat(t, ",") end, 300000)
-- closures and varargs
bench("closures + varargs", function(n) local s = 0 for i = 1, n do local f = function(...) return select("#", ...) end s = s + f(i, i, i) end return s end, 2000000)
-- table.insert/remove queue and sort
bench("tables sort/insert", function(n) for _ = 1, n do local t = {} for i = 1, 200 do table.insert(t, (i * 7919) % 1000) end table.sort(t) while #t > 0 do table.remove(t) end end end, 2000)
return table.concat(out, "\n")
