// NANITE N2b (analysis/44): Photon's static props merged into the frame: their colour where they cover a pixel, with their depth (oDepth),
// depth-tested against the scene depth (LESSEQUAL), so the world and models in front hide them and later draws sort against them.
// s0: the props' colour (RGBA8, alpha 0 where none); s1: their D3D depth (R32F).
sampler2D col : register(s0);
sampler2D dep : register(s1);
struct O { float4 c : COLOR0; float d : DEPTH; };
O main(float2 uv : TEXCOORD0) {
    float4 c = tex2D(col, uv);
    clip(c.a - 0.5);
    O o;
    o.c = float4(c.rgb, 1);
    o.d = tex2D(dep, uv).r;
    return o;
}
