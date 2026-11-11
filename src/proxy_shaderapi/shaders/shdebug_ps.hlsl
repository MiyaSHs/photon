// T4.2 stage 1b debug (PHOTON_SHADOWS_DEBUG=1): reads the main view's INTZ depth, rebuilds the world position with the inverse of the
// view-projection (c0..c3 hold its rows: world = (dot(ndc, c0), dot(ndc, c1), dot(ndc, c2), dot(ndc, c3)) / w), and draws a world-space
// grid (lines every c4.x units, red) over a tint of the world position, at alpha c4.y. A correct reconstruction keeps the grid fixed to
// the ground while the camera moves. Sky (depth >= 0.99999) is left alone.
sampler2D depthTex : register(s0);
float4 invRow0 : register(c0);
float4 invRow1 : register(c1);
float4 invRow2 : register(c2);
float4 invRow3 : register(c3);
float4 params : register(c4);
float4 main(float2 uv : TEXCOORD0) : COLOR {
    if (params.z > 0.5) return float4(1, 0, 1, 0.5);   // PHOTON_SHADOWS_DEBUG=2: a flat tint, to see where the pass lands
    float d = tex2D(depthTex, uv).r;
    clip(0.99999 - d);
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    float3 p = w.xyz / w.w;
    float2 g = abs(frac(p.xy / params.x) - 0.5);
    float lines = step(0.46, max(g.x, g.y));
    float3 tint = frac(p / (params.x * 8));
    return float4(lerp(tint * 0.5, float3(1, 0, 0), lines), params.y);
}
