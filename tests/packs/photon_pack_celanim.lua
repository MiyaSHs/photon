-- Photon shader pack example: cel-style choppy animation (Photon CPU's animation stepping, 12 poses a second) through Photon.SetFeature.
local function reg() Photon.SetFeature("celanim", "photon_cpu_anim_step", 12) end
if Photon and Photon.SetFeature then reg() else hook.Add("PhotonPacksReady", "photon_pack_celanim", reg) end
