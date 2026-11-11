// TOON (TASKS): outlines on silhouettes. s0: the main view's depth (INTZ); c0..c3 the inverse view-projection rows (as l3ratio_ps);
// c4: x 1/width, y 1/height, z the relative depth jump that counts as an edge, w the line's darkness. A pixel whose neighbours one pixel
// away are much farther (relative to its own view distance) is on a silhouette: it gets a dark line (alpha blend, src alpha).
sampler2D sceneDepth : register(s0);
float4 invRow[4] : register(c0);
float4 cv : register(c4);

float3 pos(float2 uv, float d) {
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, invRow[0]), dot(ndc, invRow[1]), dot(ndc, invRow[2]), dot(ndc, invRow[3]));
    return w.xyz / w.w;
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float d = tex2D(sceneDepth, uv).r;
    clip(0.99999 - d);
    float3 e = float3(invRow[0].w, invRow[1].w, invRow[2].w) / invRow[3].w;   // a point on the near plane: the eye's side
    float3 p = pos(uv, d);
    float dist = length(p - e);
    float edge = 0, crease = 0;
    [unroll] for (int k = 1; k <= 2; k++) {   // 1 and 2 pixels out: 2-pixel lines
        float2 ox = float2(cv.x * k, 0), oy = float2(0, cv.y * k);
        float2 o[4] = { ox, -ox, oy, -oy };
        float dd[4];
        [unroll] for (int j = 0; j < 4; j++) {
            float dn = tex2Dlod(sceneDepth, float4(uv + o[j], 0, 0)).r;
            dd[j] = dn >= 0.99999 ? dist * 4 : length(pos(uv + o[j], dn) - e);
            edge = max(edge, (dd[j] - dist) / dist);   // only the near side of a jump draws the line
        }
        if (k == 1) crease = max(abs(dd[0] + dd[1] - 2 * dist), abs(dd[2] + dd[3] - 2 * dist)) / dist;   // a kink: second difference
    }
    edge = max(edge, crease * 4);
    float a = saturate((edge - cv.z) / cv.z) * cv.w;
    clip(a - 0.01);
    return float4(0, 0, 0, a);
}
