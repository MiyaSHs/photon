// SP layer 3, first version (analysis/47; fog.c): volumetric fog over the main view. Each pixel marches from the eye to the scene point
// (or MAXDIST for the sky) in N jittered steps; at each step the medium's density (a height fog plus up to 8 pack volumes) scatters the
// sun, tested against Photon's sun shadow map (shafts), and an ambient term; the frame is attenuated by the transmittance and the
// in-scattered light added (in linear light). s0 the frame (sRGB), s1 the scene depth (INTZ), s4 the sun shadow map (INTZ).
// c0..c3 inverse view-projection rows; c4 eye (w: steps); c5 (density, fog height z, falloff, max distance); c6 fog colour (w sun shaft
// strength); c7 sun direction toward the sun (xyz, w: shadow map valid); c8 sun colour (w: phase g); c9..c11 the sun map's light rows;
// c12 (bias, -, 1/map size, used / map size); c13 (frame, -, -, volume count); c14..c21 volumes (xyz centre, w radius) and c22..c29 their
// (density, type 0 sphere 1 box, half height for boxes, -).
sampler2D frame : register(s0);
sampler2D sceneDepth : register(s1);
sampler2D shadowMap : register(s4);
float4 inv0 : register(c0); float4 inv1 : register(c1); float4 inv2 : register(c2); float4 inv3 : register(c3);
float4 eye : register(c4);
float4 fog : register(c5);
float4 fogCol : register(c6);
float4 sunDir : register(c7);
float4 sunCol : register(c8);
float4 lr0 : register(c9); float4 lr1 : register(c10); float4 lr2 : register(c11);
float4 sprm : register(c12);
float4 misc : register(c13);
float4 vpos_r[8] : register(c14);
float4 vprm[8] : register(c22);

float3 to_lin(float3 c) { return pow(max(c, 0), 2.2); }
float3 to_srgb(float3 c) { return pow(max(c, 0), 1.0 / 2.2); }
float sun_vis(float3 p) {
    if (sunDir.w < 0.5) return 1;
    float4 p4 = float4(p, 1);
    float3 lc = float3(dot(p4, lr0), dot(p4, lr1), dot(p4, lr2));
    if (any(abs(lc.xy) > 1)) return 1;
    float2 suv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5) * sprm.w;
    float d = tex2Dlod(shadowMap, float4(suv, 0, 0)).r;
    return lc.z - sprm.x > d ? 0 : 1;
}
float density_at(float3 p) {
    float h = fog.x * exp(-max(p.z - fog.y, 0) / max(fog.z, 1));   // thick below the fog height, thinning above it
    for (int i = 0; i < 8; i++) {
        if (i >= (int)misc.w) break;
        float3 d = p - vpos_r[i].xyz;
        float inside = vprm[i].y > 0.5 ? (all(abs(d.xy) < vpos_r[i].w) && abs(d.z) < vprm[i].z ? 1 : 0)
                                        : saturate(1 - length(d) / vpos_r[i].w) * 2;   // spheres soft at the edge
        h += vprm[i].x * saturate(inside);
    }
    return h;
}
float4 main(float2 uv : TEXCOORD0, float2 vp : VPOS) : COLOR {
    float3 c = to_lin(tex2D(frame, uv).rgb);
    float d = tex2D(sceneDepth, uv).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d >= 0.99999 ? 0.9999 : d, 1);
    float4 w = float4(dot(ndc, inv0), dot(ndc, inv1), dot(ndc, inv2), dot(ndc, inv3));
    float3 p = w.xyz / w.w, ro = eye.xyz, rd = normalize(p - ro);
    float len = min(d >= 0.99999 ? fog.w : length(p - ro), fog.w);
    int steps = (int)eye.w;
    float ds = len / steps;
    float jit = frac(52.9829189 * frac(dot(vp + misc.x * float2(5.588, 2.837), float2(0.06711056, 0.00583715))));   // interleaved gradient noise
    float cosT = dot(rd, sunDir.xyz), g = sunCol.w;
    float phase = (1 - g * g) / (12.566 * pow(max(1 + g * g - 2 * g * cosT, 1e-3), 1.5));   // Henyey-Greenstein
    float T = 1; float3 L = 0;
    for (int i = 0; i < 64; i++) {
        if (i >= steps) break;
        float3 q = ro + rd * ((i + jit) * ds);
        float s = density_at(q) * 0.001;   // densities per 1000 units
        if (s <= 0) continue;
        float3 scat = fogCol.rgb * 0.15 + sunCol.rgb * fogCol.w * phase * sun_vis(q);   // a faint ambient (it is not occluded indoors) and the sun
        float a = exp(-s * ds);
        L += T * (1 - a) * scat;
        T *= a;
        if (T < 0.01) break;
    }
    return float4(to_srgb(c * T + L), 1);
}
