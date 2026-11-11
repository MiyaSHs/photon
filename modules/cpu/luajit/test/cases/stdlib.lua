-- The standard libraries as luaL_openlibs leaves them: names, and behaviour with deterministic results.
local out = {}
local function p(...) local t = {} for i = 1, select("#", ...) do t[#t + 1] = tostring((select(i, ...))) end out[#out + 1] = table.concat(t, " ") end
local function keys(t) local k = {} for n, v in pairs(t) do k[#k + 1] = n .. ":" .. type(v) end table.sort(k) return table.concat(k, ",") end
p("_G", keys(_G))
for _, lib in ipairs({ "string", "table", "math", "os", "io", "debug", "coroutine", "package", "jit", "bit" }) do
	p(lib, type(_G[lib]) == "table" and keys(_G[lib]) or tostring(_G[lib]))
end
p("version", _VERSION, jit and jit.version or "-")
-- numbers to strings
for _, v in ipairs({ 1, -0.0, 0.1, 1/3, 1e15, 1e16, 2^53, 2^63, 1e100, 123456789012, -1.5e-7, 0/0 ~= 0/0, math.huge, -math.huge }) do p("num", v) end
p("fmt", string.format("%d %5.2f %g %e %x %s %q", 42, 3.14159, 0.0001, 12345.678, 255, nil, "a\nb\0c"))
p("fmt2", string.format("%.14g %.17g %5s|%-5s|", 0.1, 0.1, "ab", "cd"))
p("tonumber", tonumber("0x10"), tonumber("  12  "), tonumber("1e2"), tonumber("z", 36), tonumber(""), tonumber("0x"), tonumber("1e"))
-- tables
local h = { 1, 2, nil, 4 } p("len holes", #h)
local h2 = { 1, nil, 3 } p("len holes 2", #h2)
local h3 = {} h3[1] = 1 h3[2] = 2 h3[4] = 4 p("len holes 3", #h3)
local s = { 5, 3, 8, 1, 9, 2, 7 } table.sort(s) p("sort", table.concat(s, ","))
local s2 = { 5, 3, 8, 1 } table.sort(s2, function(a, b) return a > b end) p("sort desc", table.concat(s2, ","))
local ti = { 1, 2, 3 } table.insert(ti, 2, 9) table.insert(ti, 7) p("insert", table.concat(ti, ","), table.remove(ti), table.remove(ti, 1), table.concat(ti, ","))
p("unpack", unpack({ 1, 2, 3 })) p("select", select("#", 1, nil, 3), select(2, "a", "b", "c"))
p("maxn", table.maxn({ [1] = 1, [5] = 2, [3.5] = 3 }))
local nk = {} for n = 1, 20 do nk[n * 7 % 23] = n end nk[100] = 1 nk[-5] = 2 nk[2.5] = 3 nk[true] = 4
local no = {} for k in pairs(nk) do no[#no + 1] = tostring(k) end p("pairs numkeys", table.concat(no, " "))
local mix = { "a", "b", x = 1, y = 2, [10] = "c", "d" } local mo = {} for k in pairs(mix) do mo[#mo + 1] = tostring(k) end p("pairs mixed", table.concat(mo, " "))
-- strings
p("gsub", string.gsub("hello world", "o", "0"), string.gsub("abc", "%w", "%0%0"), string.gsub("x = 1, y = 2", "(%w+) = (%w+)", "%2=%1"))
p("find", string.find("hello", "l"), string.find("a.b", ".", 1, true), string.find("abc", "(b)(c)"))
p("match", string.match("key=val", "(%w+)=(%w+)"), string.match("  trim  ", "^%s*(.-)%s*$"))
local g = {} for w in string.gmatch("one two  three", "%a+") do g[#g + 1] = w end p("gmatch", table.concat(g, "|"))
p("rep/sub/byte", string.rep("ab", 3, ","), ("hello"):sub(-3), ("hello"):sub(2, 100), string.byte("ABC", 1, -1), string.char(72, 105))
p("upper/lower/len/reverse", ("MiXeD"):upper(), ("MiXeD"):lower(), #"\0\0", ("abc"):reverse())
-- errors
local ok, e = pcall(function() local x = nil; return x.y end) p("err index", e)
ok, e = pcall(function() return 1 + {} end) p("err arith", e)
ok, e = pcall(function() return #nil end) p("err len", e)
ok, e = pcall(function() error("msg", 0) end) p("err level0", e)
ok, e = pcall(function() error("msg") end) p("err level1", e)
ok, e = pcall(function() error({ code = 1 }) end) p("err table", type(e), e and e.code)
ok, e = pcall(function() local t = setmetatable({}, { __index = function(t, k) return k .. "!" end }) return t.x end) p("index mm", ok, e)
ok, e = pcall(string.rep) p("err arg", e)
ok, e = pcall(function() return ("x"):bad() end) p("err method", e)
p("xpcall", xpcall(function() error("boom", 0) end, function(m) return "handled:" .. m end))
-- coroutines
local co = coroutine.create(function(a, b) local c = coroutine.yield(a + b) local d = coroutine.yield(c * 2) return d, "end" end)
p("co", coroutine.resume(co, 1, 2)) p("co", coroutine.resume(co, 10)) p("co", coroutine.resume(co, "x")) p("co", coroutine.resume(co), coroutine.status(co))
p("wrap", coroutine.wrap(function() coroutine.yield(1) end)())
-- environments and loading
local f = function() return gv end if setfenv then setfenv(f, { gv = "fenv" }) p("setfenv", f(), getfenv(f).gv) else p("setfenv absent") end
p("loadstring", loadstring and loadstring("return 1 + 1")() or "absent", loadstring and select(2, loadstring("return +")) or "absent")
p("load mode", load and pcall(load, string.dump and string.dump(function() end) or "", "x", "t") or "absent")
p("math", math.floor(-1.5), math.ceil(-1.5), math.fmod(7, 3), math.fmod(-7, 3), 7 % -3, -7 % 3, math.max(1, 5, 3), math.abs(-0.0))
p("bit", bit and bit.band(0xff, 0x0f) or "-", bit and bit.tohex(255) or "-", bit and bit.lshift(1, 31) or "-")
p("tostring types", type(print), type(nil), type(2), tostring(nil), tostring(true))
p("string meta", ("a"):rep(2), getmetatable("").__index == string)
return table.concat(out, "\n")
