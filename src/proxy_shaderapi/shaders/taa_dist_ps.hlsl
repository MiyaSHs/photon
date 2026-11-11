// AA (taa.c): this frame's distance from the eye per pixel (R32F), for the next frame's history check. s0 the scene depth (INTZ);
// c0..c3 the inverse rows, c4 the eye, c5: 1 / width, 1 / height, zw the view's scale (0: none).
sampler2D sceneDepth : register(s0);
float4 invRow0 : register(c0);
float4 invRow1 : register(c1);
float4 invRow2 : register(c2);
float4 invRow3 : register(c3);
float4 eye : register(c4);
float4 prm : register(c5);
float4 main(float2 uv0 : TEXCOORD0, float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * prm.xy;
    if (prm.z > 0) uv *= prm.zw;   // UPSCALE: an output pixel's point in the scaled view's corner
    float d = tex2Dlod(sceneDepth, float4(uv, 0, 0)).r;
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, min(d, 0.999999), 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    return float4(length(w.xyz / w.w - eye.xyz), 0, 0, 1);
}
