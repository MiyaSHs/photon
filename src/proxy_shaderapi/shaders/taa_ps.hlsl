// AA (taa.c): the main view resolved against its history. The pixel's world point (scene depth through this frame's jittered inverse
// rows) back into last frame's unjittered view gives where it was; the history there is clamped to this frame's 3x3 neighbourhood
// (YCoCg min / max: little ghosting where things move or appear) and blended with this frame's pixel (prm.z of it).
// s0 the scene depth (INTZ), s1 this frame (a copy), s2 the history. c0..c3 the inverse rows, c4..c7 last frame's rows (unjittered),
// c8: 1 / width, 1 / height, the blend, history valid; c9 last frame's eye (w: the distance check on); s3 last frame's distances;
// s4 the motion vectors (velocity_ps: b 1 where a moving model is, rg its offset to last frame's uv, a its distance then), c10.x s4 on.
sampler2D sceneDepth : register(s0);
sampler2D cur : register(s1);
sampler2D hist : register(s2);
sampler2D prevDist : register(s3);   // last frame's distances from its eye (taa_dist_ps)
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
float4 prevEye : register(c9);       // last frame's eye; w 1: the distance check on
float4 prm2 : register(c10);   // x s4 on, y the motion debug view, z the rejection debug view (red: this frame's pixel, no history)
// the history read with a Catmull-Rom filter from 5 bilinear taps (the corners dropped): bilinear reads blur a little more every frame the
// camera moves, which is most of TAA's softness. prm.xy is one texel.
float3 histCR(float2 uv) {
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
float3 ycocg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 rgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
float4 main(float2 uv0 : TEXCOORD0, float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * prm.xy;   // exact pixel centres: D3D9's half-texel offset would make the history drift and blur
    float3 c = tex2Dlod(cur, float4(uv, 0, 0)).rgb;
    if (prm.w < 0.5) return float4(c, 1);
    float d = tex2Dlod(sceneDepth, float4(uv, 0, 0)).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, min(d, 0.999999), 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    float4 p = float4(w.xyz / w.w, 1);
    float4 pc = float4(dot(p, prevRow0), dot(p, prevRow1), dot(p, prevRow2), dot(p, prevRow3));
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    float want = length(p.xyz - prevEye.xyz);
    float4 mv = prm2.x > 0.5 ? tex2Dlod(motion, float4(uv, 0, 0)) : float4(0, 0, 0, 0);
    if (prm2.y > 0.5) return float4(saturate(abs(mv.rg) * 50), mv.b * 0.5 + 0.25 * c.g, 1);   // PHOTON_TAA_MV_DEBUG: the motion (|uv offset| x 50)
    if (mv.b > 0.5) { puv = uv + mv.rg; want = mv.a; pc.w = 1; }   // a moving model: its own motion, not the camera's alone
    float slope = max(abs(ddx(want)), abs(ddy(want)));   // (used by the distance check below; taken before any branch)
    if (pc.w <= 0 || any(puv < 0) || any(puv > 1)) return prm2.z > 0.5 ? float4(1, 0, 0, 1) : float4(c, 1);
    float3 mn = ycocg(c), mx = mn;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            float3 s = ycocg(tex2Dlod(cur, float4(uv + float2(x, y) * prm.xy, 0, 0)).rgb);
            mn = min(mn, s); mx = max(mx, s);
        }
    // the history belongs to this point only if last frame saw it there: its distance from last frame's eye against what was stored
    // (a disocclusion, or a moving object without motion vectors: no ghost trail, this frame's pixel instead)
    // tolerance: 2% + 4 units, plus how much the distance changes from one pixel to the next here (the sub-pixel jitter moves the sample
    // that far: grazing floors and depth edges would otherwise fail every frame); the 3D sky's depth (at the far plane) is not checked
    if (prevEye.w > 0.5 && d < 0.99999) {
        float had = tex2Dlod(prevDist, float4(puv, 0, 0)).r;
        if (abs(had - want) > 0.02 * want + 4 + 1.5 * slope) return prm2.z > 0.5 ? float4(1, 0, 0, 1) : float4(c, 1);
    }
    float3 h = rgb(clamp(ycocg(prm2.w > 0.5 ? tex2Dlod(hist, float4(puv, 0, 0)).rgb : histCR(puv)), mn, mx));   // c10.w 1: bilinear (A/B)
    if (prm2.z > 0.5) return float4(c * 0.5, 1);
    return float4(lerp(h, c, prm.z), 1);
}
