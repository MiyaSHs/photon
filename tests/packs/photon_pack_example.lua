-- Photon shader pack example (analysis/47, SP layer 1): a height fog from the scene depth (world positions rebuilt through the inverse
-- rows), then a vignette with animated film grain. Copy to garrysmod/lua/autorun/client/ (or ship it as a Workshop addon that requires
-- Photon). Toggle: photon_pack_toggle example/heightfog; tune: photon_packs, photon_pack_param example/heightfog 1 0.8
local FOG = [[
sampler2D frame : register(s0);
sampler2D depth : register(s1);
float4 info : register(c0);
float4 inv0 : register(c1); float4 inv1 : register(c2); float4 inv2 : register(c3); float4 inv3 : register(c4);
float4 eye : register(c5);
float4 p0 : register(c10);   // x density, y height (world z) where the fog thins out, z falloff (units), w max
float4 p1 : register(c11);   // xyz colour (sRGB 0..1)
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(frame, uv).rgb;
    float d = tex2D(depth, uv).r;
    if (d >= 0.99999) return float4(c, 1);          // the sky keeps its own look
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, inv0), dot(ndc, inv1), dot(ndc, inv2), dot(ndc, inv3));
    float3 p = w.xyz / w.w;
    float dist = length(p - eye.xyz);
    float h = saturate((p0.y - p.z) / p0.z + 0.5);  // thicker low down
    float f = min(1 - exp(-dist * p0.x * 0.001 * h), p0.w);
    return float4(lerp(c, p1.rgb, f), 1);
}
]]
local VIGNETTE = [[
sampler2D frame : register(s0);
float4 info : register(c0);
float4 p0 : register(c10);   // x vignette strength, y radius, z grain amount
float hash(float2 q) { return frac(sin(dot(q, float2(12.9898, 78.233))) * 43758.5453); }
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(frame, uv).rgb;
    float2 q = uv - 0.5;
    float v = 1 - p0.x * smoothstep(p0.y * 0.5, p0.y * 0.5 + 0.45, length(q));
    float g = (hash(uv / info.xy * 0.37 + frac(info.z * 7.13)) - 0.5) * p0.z;
    return float4(saturate(c * v + g), 1);
}
]]
local function reg()
	Photon.RegisterPass({ pack = "example", name = "heightfog", order = 40, hlsl = FOG,
		params = { { "Density", 0.6, 0, 4 }, { "Fog height (world z)", -64, -2000, 2000 }, { "Falloff (units)", 400, 10, 4000 }, { "Max", 0.85, 0, 1 },
		           { "Red", 0.62, 0, 1 }, { "Green", 0.68, 0, 1 }, { "Blue", 0.74, 0, 1 } } })
	Photon.RegisterPass({ pack = "example", name = "vignette", order = 90, hlsl = VIGNETTE,
		params = { { "Vignette", 0.45, 0, 1 }, { "Radius", 0.8, 0.2, 1.5 }, { "Grain", 0.04, 0, 0.2 } } })
end
if Photon and Photon.RegisterPass then reg() else hook.Add("PhotonPacksReady", "photon_pack_example", reg) end
