// UPSCALE U4 (analysis/45): motion vectors for every pixel of the scaled view (FSR 2 takes them for the whole frame): the moving models'
// own (velocity_ps: b 1) where they are, else the camera's from the depth: rg = where the point was last frame - where it is (the
// target's uv). s0 the scene depth (INTZ), s1 the models' motion vectors; c0..c3 the inverse rows (jittered), c4..c7 last frame's rows
// (unjittered), c8: 1 / width, 1 / height, x the models' vectors on.
sampler2D sceneDepth : register(s0);
sampler2D motion : register(s1);
float4 invRow0 : register(c0);
float4 invRow1 : register(c1);
float4 invRow2 : register(c2);
float4 invRow3 : register(c3);
float4 prevRow0 : register(c4);
float4 prevRow1 : register(c5);
float4 prevRow2 : register(c6);
float4 prevRow3 : register(c7);
float4 prm : register(c8);
float4 main(float2 uv0 : TEXCOORD0, float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * prm.xy;
    if (prm.z > 0.5) {
        float4 mv = tex2Dlod(motion, float4(uv, 0, 0));
        if (mv.b > 0.5) return float4(mv.rg, 0, 1);
    }
    float d = tex2Dlod(sceneDepth, float4(uv, 0, 0)).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, min(d, 0.999999), 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    float4 p = float4(w.xyz / w.w, 1);
    float4 pc = float4(dot(p, prevRow0), dot(p, prevRow1), dot(p, prevRow2), dot(p, prevRow3));
    if (pc.w <= 0) return float4(0, 0, 0, 1);
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    return float4(puv - uv, 0, 1);
}
