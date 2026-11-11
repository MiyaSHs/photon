// PT-RT (analysis/43): alpha-tested casters (Nanite's leaf cards) into Photon's sun maps: world positions and the texture's uv;
// c8..c11: the light's rows (as the caster pass sets them).
float4 lightRow0 : register(c8);
float4 lightRow1 : register(c9);
float4 lightRow2 : register(c10);
float4 lightRow3 : register(c11);
struct VsOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VsOut main(float3 p : POSITION, float2 uv : TEXCOORD0) {
    VsOut o;
    float4 w = float4(p, 1);
    o.pos = float4(dot(w, lightRow0), dot(w, lightRow1), dot(w, lightRow2), dot(w, lightRow3));
    o.uv = uv;
    return o;
}
