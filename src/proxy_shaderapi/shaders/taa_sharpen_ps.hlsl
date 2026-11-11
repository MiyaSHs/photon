// AA (taa.c): contrast-adaptive sharpening of the resolved frame (after AMD's CAS): the history (s0, unsharpened, what the next frame
// blends with) sharpened for display by a cross of its neighbours, weighted down where the local contrast is already high, so TAA's
// softness goes without halos. c0: 1 / width, 1 / height, the sharpness (0..1).
sampler2D resolved : register(s0);
float4 prm : register(c0);
float4 main(float2 uv0 : TEXCOORD0, float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * prm.xy;
    float3 c = tex2Dlod(resolved, float4(uv, 0, 0)).rgb;
    float3 n = tex2Dlod(resolved, float4(uv + float2(0, -prm.y), 0, 0)).rgb;
    float3 s = tex2Dlod(resolved, float4(uv + float2(0, prm.y), 0, 0)).rgb;
    float3 e = tex2Dlod(resolved, float4(uv + float2(prm.x, 0), 0, 0)).rgb;
    float3 w = tex2Dlod(resolved, float4(uv + float2(-prm.x, 0), 0, 0)).rgb;
    float3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
    float3 amp = sqrt(saturate(min(mn, 2 - mx) / max(mx, 1e-4)));
    float3 wt = -amp * lerp(0.125, 0.2, prm.z);
    return float4(saturate((c + wt * (n + s + e + w)) / (1 + 4 * wt)), 1);
}
