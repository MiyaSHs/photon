// T4.2 stage 1c: the screen-space sun shadow. s0: the main view's depth (INTZ); s1: the sun shadow map (INTZ). c0..c3: rows of the
// inverse of the main view's transpose(V*P) (world = (dot(ndc,c0..c3)) / w); c5..c7: rows of the sun's orthographic view-projection
// (clip = dot(world, c5..c7), xy in -1..1, z in 0..1 along the light); c4: x depth bias (light clip units), y darkening (0..1),
// z 1/shadow-map size. A pixel whose depth along the light is behind the map's nearest caster by more than the bias is darkened.
sampler2D sceneDepth : register(s0);
sampler2D shadowMap : register(s1);
float4 invRow0 : register(c0);
float4 invRow1 : register(c1);
float4 invRow2 : register(c2);
float4 invRow3 : register(c3);
float4 params : register(c4);
float4 lightRow0 : register(c5);
float4 lightRow1 : register(c6);
float4 lightRow2 : register(c7);
// F1 sun cascade: s2 the near map (INTZ), c8..c10 its rows (a quarter of the box, sharper), c11: x bias, z 1/size, w uv scale, y 1 = on
sampler2D nearMap : register(s2);
float4 nearRow0 : register(c8);
float4 nearRow1 : register(c9);
float4 nearRow2 : register(c10);
float4 nearPrm : register(c11);
float shadowAt(sampler2D m, float2 uv, float z, float bias, float h) {
    float s = 0;
    s += (z > tex2Dlod(m, float4(uv, 0, 0)).r + bias) ? 1 : 0;
    s += (z > tex2Dlod(m, float4(uv + float2(h, 0), 0, 0)).r + bias) ? 1 : 0;
    s += (z > tex2Dlod(m, float4(uv + float2(0, h), 0, 0)).r + bias) ? 1 : 0;
    s += (z > tex2Dlod(m, float4(uv + float2(h, h), 0, 0)).r + bias) ? 1 : 0;
    return s * 0.25;
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float d = tex2D(sceneDepth, uv).r;
    clip(0.99999 - d);
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    float4 p = float4(w.xyz / w.w, 1);
    float3 lc = float3(dot(p, lightRow0), dot(p, lightRow1), dot(p, lightRow2));
    clip(1 - abs(lc.xy));                           // outside the map: no shadow
    float edge = saturate((1 - max(abs(lc.x), abs(lc.y))) * 5);   // fade over the box's outer 20%, so shadows do not end in a line
    float2 suv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5) * params.w;   // the map may use only a corner (params.w = used / full)
    float s = shadowAt(shadowMap, suv, lc.z, params.x, params.z);
    if (nearPrm.y > 0.5) {                          // the near cascade where it covers the pixel, blended into the far one over its outer 20%
        float3 ln = float3(dot(p, nearRow0), dot(p, nearRow1), dot(p, nearRow2));
        float m = max(abs(ln.x), abs(ln.y));
        if (m < 1) {
            float2 nuv = float2(ln.x * 0.5 + 0.5, 0.5 - ln.y * 0.5) * nearPrm.w;
            s = lerp(shadowAt(nearMap, nuv, ln.z, nearPrm.x, nearPrm.z), s, saturate((m - 0.8) * 5));
        }
    }
    clip(s - 0.01);
    return float4(0, 0, 0, params.y * s * edge);
}
