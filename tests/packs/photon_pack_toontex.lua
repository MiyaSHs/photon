-- Photon shader pack example (analysis/47, SP layer 1b: the texture stage): cartoony textures. Each base texture of the map and models
-- is smoothed (a 3x3 mean kills fine noise), its brightness posterized into a few bands (the hue kept), its colour pushed a little more
-- saturated, and strong edges in its own detail inked dark.
local TOONTEX = [[
sampler2D src : register(s0);
float4 info : register(c0);   // xy: 1 / the copy's size
float4 p0 : register(c10);    // x bands, y saturation boost, z ink strength, w ink threshold
float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float2 d = info.xy;
    float4 c0 = tex2D(src, uv);
    float3 m = 0; float lx = 0, ly = 0;
    [unroll] for (int j = -1; j <= 1; j++) [unroll] for (int i = -1; i <= 1; i++) {
        float3 s = tex2D(src, uv + float2(i, j) * d).rgb;
        m += s;
        float l = lum(s);
        lx += l * i * (j == 0 ? 2 : 1); ly += l * j * (i == 0 ? 2 : 1);   // Sobel
    }
    m /= 9;
    float l = max(lum(m), 1e-3);
    float bands = max(p0.x, 2);
    float lq = (floor(l * bands) + 0.5) / bands;
    float3 c = m / l * lq;                                  // the hue kept, the brightness in bands
    c = lerp(lum(c).xxx, c, 1 + p0.y);                      // more saturated
    float edge = sqrt(lx * lx + ly * ly);
    c *= 1 - p0.z * smoothstep(p0.w, p0.w * 2, edge);       // ink
    return float4(saturate(c), c0.a);
}
]]
local function reg()
	Photon.RegisterTextureFilter({ pack = "toontex", name = "cartoon", hlsl = TOONTEX,
		params = { { "Bands", 4, 2, 8 }, { "Saturation boost", 0.35, 0, 1 }, { "Ink", 0.7, 0, 1 }, { "Ink threshold", 0.25, 0.05, 1 } } })
end
if Photon and Photon.RegisterTextureFilter then reg() else hook.Add("PhotonPacksReady", "photon_pack_toontex", reg) end
