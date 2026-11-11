-- Photon shader pack example (analysis/47, SP layer 2): Combine soldiers' eyes cast light. Plain GMod Lua: a small engine DynamicLight at
-- each soldier's "eyes" attachment every frame; Photon's presets light with it (per-pixel dynamic lights) and so does the path tracer
-- (dynamic lamps, layer 2). Elite soldiers red, the rest cyan.
local function eyes(e)
	local id = e:LookupAttachment("eyes")
	local a = id and id > 0 and e:GetAttachment(id)
	if a then return a.Pos + a.Ang:Forward() * 6 end
	return e:EyePos() + e:GetForward() * 6
end
hook.Add("Think", "photon_pack_eyeglow", function()
	for _, e in ipairs(ents.FindByClass("npc_combine_s")) do
		if IsValid(e) and e:Health() > 0 then
			local elite = e:GetModel() and string.find(e:GetModel(), "super_soldier", 1, true)
			local d = DynamicLight(e:EntIndex() + 4096)
			if d then
				d.pos = eyes(e)
				d.r, d.g, d.b = elite and 255 or 60, elite and 40 or 200, elite and 30 or 255
				d.brightness = 2; d.decay = 1000; d.size = 160; d.dietime = CurTime() + 0.1
			end
		end
	end
end)
