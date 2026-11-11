// PT-RT (analysis/43): the world's smooth normals from the main view (the composite's sun shades with them instead of the depth's
// per-triangle normal, which facets displacement terrain). c8..c11: the main view's rows (clip = rows . p). The pixel shader keeps only
// the pixels where this triangle is what the scene shows (its view depth against the scene depth's).
float4 vpRow0 : register(c8);
float4 vpRow1 : register(c9);
float4 vpRow2 : register(c10);
float4 vpRow3 : register(c11);
// PBR packs: TEXCOORD3 (u, v in texture repetitions, atlas tile or -1, has a pack normal), TEXCOORD4 / 5 the texture's u / v axes
struct VsOut { float4 pos : POSITION; float3 n : TEXCOORD0; float3 wp : TEXCOORD1; float4 pbr : TEXCOORD2; float4 tc : TEXCOORD3; float3 su : TEXCOORD4; float3 sv : TEXCOORD5; };
VsOut main(float3 p : POSITION, float3 n : NORMAL, float4 pbr : COLOR0, float4 tc : TEXCOORD3, float3 su : TEXCOORD4, float3 sv : TEXCOORD5) {   // COLOR0: PBR step 2's roughness, metalness (stream 1, when bound)
    VsOut o;
    float4 w = float4(p, 1);
    o.pos = float4(dot(w, vpRow0), dot(w, vpRow1), dot(w, vpRow2), dot(w, vpRow3));
    o.n = n;
    o.wp = p;
    o.pbr = pbr;
    o.tc = tc; o.su = su; o.sv = sv;
    return o;
}
