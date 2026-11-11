-- Photon shader pack example (analysis/47, SP layer 3): volumetric fog with sun shafts (Photon's fog, configured by the pack) and a
-- moving smoke volume pushed every frame (photon_cpu.Push, the Lua -> Photon channel).
local function reg()
	Photon.SetFeature("weather", "photon_fog", 1)
	Photon.SetFeature("weather", "photon_fog_density", 0.6)
	Photon.SetFeature("weather", "photon_fog_height", -120)
	Photon.SetFeature("weather", "photon_fog_falloff", 350)
	Photon.SetFeature("weather", "photon_fog_sun", 8)
end
if Photon and Photon.SetFeature then reg() else hook.Add("PhotonPacksReady", "photon_pack_weather", reg) end
hook.Add("Think", "photon_pack_weather_smoke", function()
	local p = LocalPlayer()
	if not (IsValid(p) and photon_cpu and photon_cpu.Push) then return end
	local c = p:EyePos() + p:GetAimVector() * 300 + Vector(0, 0, math.sin(CurTime()) * 40)
	photon_cpu.Push("volumes", { c.x, c.y, c.z, 90, 25, 0, 0, 0 })   -- x y z radius density type(sphere) halfh 0
end)
