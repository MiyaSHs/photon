// UPSCALE U1 (analysis/45): the scaled 3D view (the back buffer's corner, copied to s1) stretched to the full target, bilinear: Photon's
// stand-in for the engine's dev/upscale draw, which proves the hook (U2 replaces it with the temporal upscale).
// c0: xy the corner's size / the target's (uv scale), zw 1 / target size.
sampler2D unused : register(s0);
sampler2D src : register(s1);
float4 prm : register(c0);
float4 main(float2 uv0 : TEXCOORD0, float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * prm.zw;
    return float4(tex2Dlod(src, float4(uv * prm.xy, 0, 0)).rgb, 1);
}
