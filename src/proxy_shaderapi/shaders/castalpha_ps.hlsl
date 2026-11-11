// PT-RT (analysis/43): an alpha-tested caster's pixel (s0 its base texture): cut where alpha < 0.5, as the material draws.
sampler2D base : register(s0);
float4 main(float2 uv : TEXCOORD0) : COLOR { clip(tex2D(base, uv).a - 0.5); return 0; }
