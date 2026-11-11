-- Busy scene: 200 physics props dropped from staggered heights in front of the camera, so they keep arriving and colliding.
local models = { "models/props_c17/oildrum001.mdl", "models/props_junk/wood_crate001a.mdl", "models/props_borealis/bluebarrel001.mdl",
	"models/props_c17/canister01a.mdl" }
local n = 0
for k = 0, 199 do
	local e = ents.Create("prop_physics")
	e:SetModel(models[k % #models + 1])
	e:SetPos(Vector(0 - (k % 10) * 45, -200 + math.floor(k / 10) % 10 * 45, 0 + math.floor(k / 100) * 900 + (k % 7) * 120))
	e:SetAngles(Angle(k * 37 % 360, k * 53 % 360, 0))
	e:Spawn()
	n = n + 1
end
print("PHOTON_PERF spawned " .. n .. " physics props")
