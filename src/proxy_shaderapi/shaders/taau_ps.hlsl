// UPSCALE U2 (analysis/45): Photon's temporal upscale, in place of the engine's dev/upscale draw. Each output pixel (the full target) finds
// its point in the scaled 3D view (the back buffer's corner, uv * scale), reprojects it into last frame's output (its own motion vector
// where a moving model is, else the camera's from the depth), clamps that history to the render-resolution neighbourhood and blends the
// current sample in by how close the jittered sample lies to the output pixel's centre (a Gaussian in output pixels), so the jittered
// frames build up full-resolution detail.
// s0 the scene depth (INTZ), s1 this frame's view (the corner, copied; point), s2 the history (output size, Catmull-Rom from bilinear
// taps), s3 last frame's distances (output size), s4 the motion vectors. c0..c3 the inverse rows (jittered), c4..c7 last frame's rows
// (unjittered), both in the target's uv space; c8: 1 / width, 1 / height (output), scale x, y; c9 last frame's eye (w: history valid);
// c10: x motion vectors on, y unused, zw this frame's jitter in NDC.
sampler2D sceneDepth : register(s0);
sampler2D cur : register(s1);
sampler2D hist : register(s2);
sampler2D prevDist : register(s3);
sampler2D motion : register(s4);
float4 invRow0 : register(c0);
float4 invRow1 : register(c1);
float4 invRow2 : register(c2);
float4 invRow3 : register(c3);
float4 prevRow0 : register(c4);
float4 prevRow1 : register(c5);
float4 prevRow2 : register(c6);
float4 prevRow3 : register(c7);
float4 prm : register(c8);
float4 prevEye : register(c9);
float4 prm2 : register(c10);
float3 ycocg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 rgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float3 histCR(float2 uv) {   // as taa_ps: Catmull-Rom from 5 bilinear taps, at the output's texel size
    float2 sz = 1.0 / prm.xy;
    float2 sp = uv * sz, tp = floor(sp - 0.5) + 0.5, f = sp - tp;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, t0 = (tp - 1) * prm.xy, t3 = (tp + 2) * prm.xy, t12 = (tp + w2 / w12) * prm.xy;
    float3 c = tex2Dlod(hist, float4(t12.x, t0.y, 0, 0)).rgb * (w12.x * w0.y)
             + tex2Dlod(hist, float4(t0.x, t12.y, 0, 0)).rgb * (w0.x * w12.y)
             + tex2Dlod(hist, float4(t12.x, t12.y, 0, 0)).rgb * (w12.x * w12.y)
             + tex2Dlod(hist, float4(t3.x, t12.y, 0, 0)).rgb * (w3.x * w12.y)
             + tex2Dlod(hist, float4(t12.x, t3.y, 0, 0)).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / wsum, 0);
}
float4 main(float2 uv0 : TEXCOORD0, float2 vpos : VPOS) : COLOR {
    float2 uvo = (vpos + 0.5) * prm.xy;   // the output pixel (the full target)
    float2 uvc = uvo * prm.zw;            // its point in the corner (the target's uv space)
    // the render texel holding it, and where that texel's jittered sample lies (in the corner's uv): the jitter moved the geometry by
    // +j in NDC, so the texel's centre sampled the still scene at centre - j
    float2 texel = prm.xy;                // the render texels have the target's size (the corner is a part of it)
    float2 tc = (floor(uvc / texel) + 0.5) * texel;
    float2 spos = tc - float2(prm2.z, -prm2.w) * 0.5 * prm.zw;   // the jitter is in the corner viewport's NDC
    float2 dpx = (spos - uvc) / (texel * prm.zw);   // the sample's offset from the output pixel's centre, in output pixels
    float3 c = tex2Dlod(cur, float4(tc, 0, 0)).rgb;
    float d = tex2Dlod(sceneDepth, float4(uvc, 0, 0)).r;
    float4 ndc = float4(uvc.x * 2 - 1, 1 - uvc.y * 2, min(d, 0.999999), 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    float4 p = float4(w.xyz / w.w, 1);
    float4 pc = float4(dot(p, prevRow0), dot(p, prevRow1), dot(p, prevRow2), dot(p, prevRow3));
    float2 puvc = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    float want = length(p.xyz - prevEye.xyz);
    float4 mv = prm2.x > 0.5 ? tex2Dlod(motion, float4(uvc, 0, 0)) : float4(0, 0, 0, 0);
    if (mv.b > 0.5) { puvc = uvc + mv.rg; want = mv.a; pc.w = 1; }
    float slope = max(abs(ddx(want)), abs(ddy(want)));
    float2 puv = puvc / prm.zw;   // into last frame's output
    // the neighbourhood: this render texel and its 8 neighbours
    float3 mn = ycocg(c), mx = mn, avg = 0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            float3 s = ycocg(tex2Dlod(cur, float4(tc + float2(x, y) * texel, 0, 0)).rgb);
            mn = min(mn, s); mx = max(mx, s);
        }
    if (prevEye.w < 0.5 || pc.w <= 0 || any(puv < 0) || any(puv > 1)) return float4(tex2Dlod(cur, float4(uvc, 0, 0)).rgb, 1);   // no history: bilinear
    if (d < 0.99999) {
        float had = tex2Dlod(prevDist, float4(puv, 0, 0)).r;
        if (abs(had - want) > 0.02 * want + 4 + 1.5 * slope) return float4(tex2Dlod(cur, float4(uvc, 0, 0)).rgb, 1);
    }
    float3 h = rgb(clamp(ycocg(histCR(puv)), mn, mx));
    float wgt = 0.25 * exp(-2.29 * dot(dpx, dpx));   // the sample's weight: full near the output pixel's centre, little far from it
    return float4(lerp(h, c, max(wgt, 0.02)), 1);
}
