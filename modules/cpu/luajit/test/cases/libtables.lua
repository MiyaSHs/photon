-- library tables in pairs() order (GMod's sandbox set, its table layout) plus package.loaded / preload keys
local out = {}
local function keys(name, t)
  local ks = {}
  for k in pairs(t) do ks[#ks + 1] = tostring(k) end
  out[#out + 1] = name .. " " .. table.concat(ks, ",")
end
keys("os", os) keys("package", package) keys("jit", jit) keys("jit.util", jit.util) keys("string", string) keys("math", math)
keys("table", table) keys("debug", debug) keys("bit", bit) keys("loaded", package.loaded) keys("preload", package.preload)
local g = {}
for k in pairs(_G) do g[#g + 1] = tostring(k) end
out[#out + 1] = "_G " .. table.concat(g, ",")
return table.concat(out, "\n")
