// L1c (analysis/35): the screen-space shadow of up to 4 map lamps, each from its own perspective tile of one INTZ atlas (s1). s0: the main
// view's depth. c0..c3: rows of the inverse of the main view's transpose(V*P), as shadow_ps. c4: x darkening (0..1), y soft edges (1: 4x4 taps), z lamps
// in use, w debug (1: red inside a lamp's cone, blue where its test shadows). Per lamp i, from c8 + 10 i: four clip rows (clip = dot(world, row), w the
// view distance), origin + reach, colour luminance (.x), K (.y: the rest of the light at
// its casters, in the lamps' linear units) + near (.w), spot axis + far, attenuation c/l/q + is-spot,
// cone stopdot / stopdot2 / exponent + cube (.w), tile offset xy (cube: .y the faces' half-angle tangent) + scale z + texel w.
// A cube lamp keeps its six faces' tile offsets in the clip rows' place.
// A pixel is darkened by darkening x shadowed fraction x Lc / (Lc + K), Lc = the lamp's own light here (lum / attenuation x cone x N.L),
// so a lamp that dominates a spot shadows it strongly and a dim one in a bright room weakly; several lamps combine as 1 - prod(1 - d).
sampler2D sceneDepth : register(s0);
sampler2D atlas : register(s1);
float4 invRow[4] : register(c0);
float4 params : register(c4);
float4 lamp[40] : register(c8);

float lampDark(int b, float3 p, float3 n, inout float inside, inout float shad) {
    float4 o = lamp[b + 4];
    float3 v = o.xyz - p;
    float d = length(v);
    if (d > o.w) return 0;
    float3 l = v / d;
    float ndl = dot(n, l);
    if (ndl <= 0) return 0;
    float4 at = lamp[b + 7];
    float spot = 1;
    if (at.w > 0.5) {
        float4 cone = lamp[b + 8];
        float c = dot(-l, lamp[b + 6].xyz);
        if (c <= cone.y) return 0;
        spot = c >= cone.x ? 1 : pow(saturate((c - cone.y) / max(cone.x - cone.y, 1e-4)), cone.z);
    }
    float lc = lamp[b + 5].x * spot * ndl / max(at.x + at.y * d + at.z * d * d, 1e-3);
    float4 t = lamp[b + 9];
    float2 ndc, tile;
    float4 clipc;
    if (lamp[b + 8].w > 0.5) {
        // cube: the face of the major axis of p - lamp, a 90-degree view (half-angle tangent t.y), its tile from rows 0..2 (xy, zw per
        // face; x < 0: the face has no casters). Bases as the C side's: +-X (y, z), +-Y (x, z), +-Z (x, y).
        float3 a = abs(-v);
        float3 zx, xx, yx;
        float2 off;
        if (a.x >= a.y && a.x >= a.z) { zx = float3(-v.x > 0 ? 1 : -1, 0, 0); xx = float3(0, 1, 0); yx = float3(0, 0, 1); off = -v.x > 0 ? lamp[b].xy : lamp[b].zw; }
        else if (a.y >= a.z) { zx = float3(0, -v.y > 0 ? 1 : -1, 0); xx = float3(1, 0, 0); yx = float3(0, 0, 1); off = -v.y > 0 ? lamp[b + 1].xy : lamp[b + 1].zw; }
        else { zx = float3(0, 0, -v.z > 0 ? 1 : -1); xx = float3(1, 0, 0); yx = float3(0, 1, 0); off = -v.z > 0 ? lamp[b + 2].xy : lamp[b + 2].zw; }
        if (off.x < 0) return 0;
        float3 w3 = -v;
        clipc = float4(dot(w3, xx) / t.y, dot(w3, yx) / t.y, 0, dot(w3, zx));
        tile = off;
    } else {
        float4 pw = float4(p, 1);
        clipc = float4(dot(pw, lamp[b]), dot(pw, lamp[b + 1]), dot(pw, lamp[b + 2]), dot(pw, lamp[b + 3]));
        tile = t.xy;
    }
    if (clipc.w <= 1) return 0;
    ndc = clipc.xy / clipc.w;
    if (abs(ndc.x) >= 1 || abs(ndc.y) >= 1) return 0;   // outside the views aimed at the casters: nothing of theirs can shade it
    inside = 1;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * t.z + tile;
    float nr = lamp[b + 5].w, fr = lamp[b + 6].w;
    float bias = 2 + 0.01 * clipc.w;                     // world units, growing with distance from the lamp
    float s = 0;
    if (params.y > 0.5) {                                // soft (photon_shadow_soft): 4x4 taps, 1.5 texels around
        [unroll] for (int ky = 0; ky < 4; ky++)
            [unroll] for (int kx = 0; kx < 4; kx++) {
                float z = tex2Dlod(atlas, float4(uv + (float2(kx, ky) - 1.5) * t.w, 0, 0)).r;
                float zl = nr * fr / (fr - z * (fr - nr));
                s += (z < 0.99999 && clipc.w > zl + bias) ? 0.0625 : 0;
            }
    } else {
        const float2 tap[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
        [unroll] for (int k = 0; k < 4; k++) {           // 2x2 point taps, compared as linear distance from the lamp
            float z = tex2Dlod(atlas, float4(uv + tap[k] * t.w, 0, 0)).r;   // lod: inside dynamic branches
            float zl = nr * fr / (fr - z * (fr - nr));
            s += (z < 0.99999 && clipc.w > zl + bias) ? 0.25 : 0;
        }
    }
    shad = max(shad, s);
    return s * lc / (lc + lamp[b + 5].y);   // K: the other lamps at this lamp's casters + a floor (C side)
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float d = tex2D(sceneDepth, uv).r;
    clip(0.99999 - d);
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, invRow[0]), dot(ndc, invRow[1]), dot(ndc, invRow[2]), dot(ndc, invRow[3]));
    float3 p = w.xyz / w.w;
    float3 n = normalize(cross(ddx(p), ddy(p)));
    float4 e = float4(dot(float4(0, 0, 0, 1), invRow[0]), dot(float4(0, 0, 0, 1), invRow[1]), dot(float4(0, 0, 0, 1), invRow[2]), dot(float4(0, 0, 0, 1), invRow[3]));
    float3 eye = e.xyz / e.w;                            // the camera (ndc 0,0,0 is on the near plane: close enough to face the normal)
    if (dot(n, eye - p) < 0) n = -n;
    float keep = 1, inside = 0, shad = 0;
    [unroll] for (int i = 0; i < 4; i++)
        if (i < params.z) keep *= 1 - lampDark(i * 10, p, n, inside, shad);
    if (params.w > 0.5) return shad > 0 ? float4(0, 0, 1, 0.8) : inside > 0 ? float4(1, 0, 0, 0.3) : float4(0, 0, 0, 0);   // PHOTON_L1_DEBUG
    float a = params.x * (1 - keep);
    clip(a - 0.004);
    return float4(0, 0, 0, a);
}
