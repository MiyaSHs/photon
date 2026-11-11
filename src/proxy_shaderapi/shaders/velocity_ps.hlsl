// AA (motion vectors): where this pixel's surface point was last frame, as an offset in the full target's uv (r, g), b 1 (a model's
// motion here: the TAA resolve takes it instead of the camera-only reprojection), a its distance from last frame's eye (the resolve's
// history check). c0: 1 / width, 1 / height of the target.
float4 prm : register(c0);
// COLOR1 (PT-RT models, a second target bound): the world normal (xyz * 0.5 + 0.5), a 1 where the vertices had one.
struct PS_OUT { float4 motion : COLOR0; float4 normal : COLOR1; };
PS_OUT main(float4 prevClip : TEXCOORD0, float3 prevWorld : TEXCOORD1, float4 nrm : TEXCOORD2, float2 vpos : VPOS) {
    PS_OUT o;
    float2 uv = (vpos + 0.5) * prm.xy;
    float2 puv = float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5);
    if (prevClip.w <= 0) puv = float2(-10, -10);   // behind last frame's eye: off screen, the history is not used
    o.motion = float4(puv - uv, 1, length(prevWorld));
    float3 n = nrm.xyz * rsqrt(max(dot(nrm.xyz, nrm.xyz), 1e-12));
    o.normal = float4(n * 0.5 + 0.5, nrm.w > 0.5 ? 1 : 0);
    return o;
}
