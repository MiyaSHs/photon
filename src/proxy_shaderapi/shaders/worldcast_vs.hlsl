// PT-RT (analysis/43): the world's own triangles (model 0, from the PT-RT worker) into Photon's sun maps, so the composite's sun has the
// world's shadows as sharp as the props'. Input: world positions; c8..c11: the light's rows (as the caster pass sets them).
float4 lightRow0 : register(c8);
float4 lightRow1 : register(c9);
float4 lightRow2 : register(c10);
float4 lightRow3 : register(c11);
float4 main(float3 p : POSITION) : POSITION {
    float4 w = float4(p, 1);
    return float4(dot(w, lightRow0), dot(w, lightRow1), dot(w, lightRow2), dot(w, lightRow3));
}
