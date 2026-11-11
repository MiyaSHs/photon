// T4.2 (analysis/34): the full-screen pass's vertex shader. Positions arrive in clip space with the half-pixel offset applied on the CPU.
struct VSIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOut main(VSIn i) { VSOut o; o.pos = i.pos; o.uv = i.uv; return o; }
