-- Busy scene: 600 frozen props in a 30 x 20 grid on gm_construct's spawn field, in view of setpos 704 132 -143, setang 8 180 0.
local models = { "models/props_c17/oildrum001.mdl", "models/props_junk/wood_crate001a.mdl", "models/props_c17/canister01a.mdl",
	"models/items/boxmrounds.mdl", "models/props_lab/reciever01a.mdl", "models/props_c17/cashregister01a.mdl",
	"models/props_c17/furnituredrawer002a.mdl", "models/items/ammocrate_smg1.mdl", "models/roller.mdl", "models/props_borealis/bluebarrel001.mdl" }
local n = 0
for i = 0, 29 do
	for j = 0, 19 do
		local e = ents.Create("prop_physics")
		e:SetModel(models[(i * 20 + j) % #models + 1])
		e:SetPos(Vector(450 - i * 60, -560 + j * 60, -120))
		e:Spawn()
		local p = e:GetPhysicsObject()
		if IsValid(p) then p:EnableMotion(false) end
		n = n + 1
	end
end
print("PHOTON_PERF spawned " .. n .. " frozen props")
