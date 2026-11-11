// PT-RT (analysis/43): the world's smooth normal, encoded (alpha 1 = written), only where the scene shows this triangle: the scene's
// point at the pixel (its depth, s0 INTZ, through the inverse view rows, as PT-RT's trace rebuilds it) within 1.5% + 4 units of the
// triangle's own point, both as distances from the eye. c0: 1 / width, 1 / height; c1..c4 the inverse rows; c5 the eye.
sampler2D sceneDepth : register(s0);
float4 prm : register(c0);
float4 invRow0 : register(c1);
float4 invRow1 : register(c2);
float4 invRow2 : register(c3);
float4 invRow3 : register(c4);
float4 eye : register(c5);
float4 atl : register(c6);   // PBR packs: x 1 = the atlases are bound (s1 roughness / metalness / height, s2 normals; 32 x 32 tiles)
sampler2D rmhAtlas : register(s1);
sampler2D nrmAtlas : register(s2);
float2 atlas_uv(float tile, float2 uv, float side) {   // one repetition of the texture inside its tile, half a texel in from the edges
    float ty = floor(tile / 32.0), tx = tile - ty * 32.0;
    return (float2(tx, ty) + frac(uv) * ((side - 1.0) / side) + 0.5 / side) / 32.0;
}
// COLOR1 (PBR step 2, when a second target is bound): the material's roughness (r), metalness (g), a 1 where written.
struct PsOut { float4 n : COLOR0; float4 pbr : COLOR1; };
PsOut main(float3 n : TEXCOORD0, float3 wp : TEXCOORD1, float4 pbr : TEXCOORD2, float4 tc : TEXCOORD3, float3 su : TEXCOORD4, float3 sv : TEXCOORD5,
           float2 vpos : VPOS) {
    PsOut o;
    o.pbr = float4(pbr.r, pbr.g, 0, 1);
    float3 nn = normalize(n);
    if (atl.x > 0.5 && tc.z > -0.5) {   // the pack's per-texel maps (the material's constants without one)
        float4 rmh = tex2Dlod(rmhAtlas, float4(atlas_uv(tc.z, tc.xy, 64.0), 0, 0));
        o.pbr = float4(rmh.r, rmh.g, 0, 1);
        if (tc.w > 0.5) {   // a generated normal map (the normal first turned to face the eye, as the composite turns it: displacements'
                            // smooth normals can point down, and perturbing those bent the grass sideways, unlit; laptop pk5)
            // its x along +u, y along -v (tools/pbr_derive.py), in the texture's frame on this surface
            if (dot(nn, eye.xyz - wp) < 0) nn = -nn;
            float3 t = tex2Dlod(nrmAtlas, float4(atlas_uv(tc.z, tc.xy, 128.0), 0, 0)).xyz * 2 - 1;
            float3 tu = su - nn * dot(su, nn), tv = sv - nn * dot(sv, nn);   // a texture axis along the normal (steep displacement): no frame
            if (dot(tu, tu) > 1e-3 && dot(tv, tv) > 1e-3) {
                float3 m = normalize(tu) * t.x - normalize(tv) * t.y + nn * max(t.z, 0.05);
                if (dot(m, m) > 1e-6) nn = normalize(m);
            }
        }
    }
    float2 uv = (vpos + 0.5) * prm.xy;
    float d = tex2D(sceneDepth, uv).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    float ds = length(w.xyz / w.w - eye.xyz), dt = length(wp - eye.xyz);
    if (prm.z > 1.5) { o.n = float4(0, 1, 1, 1); return o; }   // debug: the known test triangle (cyan)
    if (prm.z > 0.5) { o.n = float4(1, d > 0.5 ? 1 : 0, 0, 1); return o; }   // debug: raster coverage (red) and whether the depth sample is non-zero (yellow)
    clip(0.015 * dt + 4 - abs(ds - dt));
    o.n = float4(nn * 0.5 + 0.5, 1);
    return o;
}
