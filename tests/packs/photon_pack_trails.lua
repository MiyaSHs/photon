-- Photon shader pack example (analysis/47): the previous frame as an input: ghost trails (each frame keeps a share of the last one).
local TRAILS = [[
sampler2D frame : register(s0);
sampler2D prev : register(s2);
float4 flags : register(c14);   // x: s2 (last frame's result) valid
float4 p0 : register(c10);      // x persistence
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(frame, uv).rgb;
    if (flags.x > 0.5) c = lerp(c, tex2D(prev, uv).rgb, p0.x);
    return float4(c, 1);
}
]]
local function reg()
	Photon.RegisterPass({ pack = "trails", name = "ghost", order = 95, hlsl = TRAILS, params = { { "Persistence", 0.75, 0, 0.95 } } })
end
if Photon and Photon.RegisterPass then reg() else hook.Add("PhotonPacksReady", "photon_pack_trails", reg) end
