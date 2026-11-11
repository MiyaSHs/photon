// AA (motion vectors, dmirror.c photon_dmirror_velocity): a moving model's draw again at the main view's end, from its recorded
// buffers, to the velocity target. Its position now with the draw's own (jittered) view-projection, so it covers the pixels the engine
// drew it on; its position last frame (last frame's bones or model matrix, last frame's unjittered view-projection) to the pixel
// shader. Skinning as the stock vertex-lit shaders: three bones from BLENDINDICES (D3DCOLOR) and two weights (the third 1 - both),
// SHORT2 weights when the vertices are compressed. The flex deltas are left out (as in the sun map: their stream is rewritten).
// Also the world normal (PT-RT models: the composite shades them with it): NORMAL float3, or UBYTE4 when compressed (the stock
// _DecompressUByte4Normal), c0.w 0 / 1; -1: none (the output's alpha 0).
// c0: x skinned, y compressed weights, z the bone row's v in s0, w the normal's encoding; c4..c7 last frame's rows (unjittered, the full target's
// uv space); c8..c11 the draw's own rows; c12 last frame's eye; c58.. this frame's bones (rigid: c58..c60), c61..c63 last frame's
// model matrix (rigid). s0 (vertex texture): last frame's bone palettes, one row of 159 registers per draw.
float4 prm : register(c0);
float4 prevRow0 : register(c4);
float4 prevRow1 : register(c5);
float4 prevRow2 : register(c6);
float4 prevRow3 : register(c7);
float4 curRow0 : register(c8);
float4 curRow1 : register(c9);
float4 curRow2 : register(c10);
float4 curRow3 : register(c11);
float4 prevEye : register(c12);
float4 bones[159] : register(c58);
sampler2D prevBones : register(s0);
struct VS_INPUT {
    float4 pos : POSITION;
    float4 weights : BLENDWEIGHT;
    float4 indices : BLENDINDICES;
    float4 normal : NORMAL;
};
struct VS_OUTPUT {
    float4 pos : POSITION;
    float4 prevClip : TEXCOORD0;
    float3 prevWorld : TEXCOORD1;
    float4 normal : TEXCOORD2;   // world normal, w 1 when the vertex has one
};
float3 ubyte4Normal(float4 v) {   // the stock shaders' _DecompressUByte4Normal
    float2 ztSigns = (v.xy - 128.0) < 0;
    float2 xyAbs = abs(v.xy - 128.0) - ztSigns;
    float2 xySigns = (xyAbs - 64.0) < 0;
    float3 n;
    n.xy = (abs(xyAbs - 64.0) - xySigns) / 63.0;
    n.z = 1.0 - n.x - n.y;
    n = normalize(n);
    n.xy *= lerp(float2(1, 1), float2(-1, -1), xySigns);
    n.z *= lerp(1.0, -1.0, ztSigns.x);
    return n;
}
float4 prevReg(int r) { return tex2Dlod(prevBones, float4((r + 0.5) / 256.0, prm.z, 0, 0)); }
VS_OUTPUT main(VS_INPUT v) {
    VS_OUTPUT o;
    float4 p = float4(v.pos.xyz, 1);
    float3 cur, prev, nm = prm.w > 0.5 ? ubyte4Normal(v.normal) : v.normal.xyz, wn;
    if (prm.x > 0.5) {
        int3 bi = D3DCOLORtoUBYTE4(v.indices).xyz * 3;
        float3 w = prm.y > 0.5 ? (v.weights.xyz + 1) / 32768 : v.weights.xyz;
        w.z = 1 - (w.x + w.y);
        float4 r0 = bones[bi.x] * w.x + bones[bi.y] * w.y + bones[bi.z] * w.z;
        float4 r1 = bones[bi.x + 1] * w.x + bones[bi.y + 1] * w.y + bones[bi.z + 1] * w.z;
        float4 r2 = bones[bi.x + 2] * w.x + bones[bi.y + 2] * w.y + bones[bi.z + 2] * w.z;
        cur = float3(dot(p, r0), dot(p, r1), dot(p, r2));
        wn = float3(dot(nm, r0.xyz), dot(nm, r1.xyz), dot(nm, r2.xyz));
        float4 q0 = prevReg(bi.x) * w.x + prevReg(bi.y) * w.y + prevReg(bi.z) * w.z;
        float4 q1 = prevReg(bi.x + 1) * w.x + prevReg(bi.y + 1) * w.y + prevReg(bi.z + 1) * w.z;
        float4 q2 = prevReg(bi.x + 2) * w.x + prevReg(bi.y + 2) * w.y + prevReg(bi.z + 2) * w.z;
        prev = float3(dot(p, q0), dot(p, q1), dot(p, q2));
    } else {
        cur = float3(dot(p, bones[0]), dot(p, bones[1]), dot(p, bones[2]));
        wn = float3(dot(nm, bones[0].xyz), dot(nm, bones[1].xyz), dot(nm, bones[2].xyz));
        prev = float3(dot(p, bones[3]), dot(p, bones[4]), dot(p, bones[5]));
    }
    float4 c = float4(cur, 1), q = float4(prev, 1);
    o.pos = float4(dot(c, curRow0), dot(c, curRow1), dot(c, curRow2), dot(c, curRow3));
    o.prevClip = float4(dot(q, prevRow0), dot(q, prevRow1), dot(q, prevRow2), dot(q, prevRow3));
    o.prevWorld = prev - prevEye.xyz;
    o.normal = float4(wn, prm.w > -0.5 && dot(wn, wn) > 1e-6 ? 1 : 0);
    return o;
}
