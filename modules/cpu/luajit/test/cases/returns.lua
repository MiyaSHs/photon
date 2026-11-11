-- what stdlib functions return (GMod changed some: table.insert returns the index), interpreted and JIT-compiled
local out = {}
local function show(name, ...) local t = { n = select("#", ...), ... } local s = {} for i = 1, t.n do s[i] = tostring(t[i]) end out[#out + 1] = name .. ": " .. t.n .. " [" .. table.concat(s, ",") .. "]" end
local t = {}
show("insert push", table.insert(t, "a"))
show("insert pos", table.insert(t, 1, "b"))
show("remove", table.remove(t))
show("sort", table.sort(t))
show("concat", table.concat({ 1, 2 }, ","))
show("unpack", unpack({ 1, 2, 3 }))
show("select", select(2, "a", "b", "c"))
show("tostring", tostring(nil))
show("next empty", next({}))
show("rawset", rawset({}, 1, 2) ~= nil)
show("setmetatable", type(setmetatable({}, {})))
show("string.gsub", string.gsub("aaa", "a", "b"))
show("string.find", string.find("hello", "l"))
-- the same insert in a hot loop, so the JIT records it
local u, last = {}, nil
for i = 1, 200 do last = table.insert(u, i) end
show("insert jit", last, #u)
return table.concat(out, "\n")
