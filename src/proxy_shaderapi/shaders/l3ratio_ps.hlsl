// L3c (analysis/37): the re-baked lamp shadows applied to the lightmapped frame as sharp / blurry. s0: the main view's depth (INTZ);
// s1 l1 (L16 3D, point), s2 l2 (L16 3D, point), s3 the brick atlas (A8R8G8B8 3D, linear), s4 the bricks' lamps (A32B32G32R32F, point),
// s5 the lamp table (A32B32G32R32F, 4 texels per worldlight: origin+type, lum x style + stopdot + stopdot2, axis + exponent, c l q).
// c0..c3 inverse main view rows (as shadow_ps); c4: 1/voxel, signed-distance scale (world units at byte 255), sharp half width, blurry
// half width; c5: super-brick origin (sb_min) x y z, lamp table rows; c6: l1 dims; c7: l2 dims; c8: atlas dims; c9: bounce fraction,
// floor, strength, debug. Output: ratio / 2 for the blend dst x 2 src (so 0..2); pixels with ratio ~ 1 are clipped.
sampler2D sceneDepth : register(s0);
sampler3D l1Tex : register(s1);
sampler3D l2Tex : register(s2);
sampler3D atlasTex : register(s3);
sampler2D brickLamps : register(s4);
sampler2D lampTab : register(s5);
sampler2D worldDepth : register(s6);   // the world-only depth (depthcopy_ps), when cv2.y = 1
float4 invRow[4] : register(c0);
float4 cv : register(c4);
float4 sbMin : register(c5);
float4 l1Dim : register(c6);
float4 l2Dim : register(c7);
float4 atDim : register(c8);
float4 prm : register(c9);
float4 cv2 : register(c10);   // x: 1 = the correction may also brighten (PHOTON_L3_BRIGHTEN); y: 1 = s6 holds the world-only depth; z: 1 = skip thin geometry (PHOTON_L3_THIN, default on)

float4 lampTexel(float id, float t) { return tex2Dlod(lampTab, float4((t + 0.5) / 4, (id + 0.5) / sbMin.w, 0, 0)); }

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float d = tex2D(sceneDepth, uv).r;
    clip(0.99999 - d);
    if (cv2.y > 0.5 && d < tex2D(worldDepth, uv).r - 1e-6) { clip(-1); return 0; }   // a model in front of the world: not ours to correct
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, invRow[0]), dot(ndc, invRow[1]), dot(ndc, invRow[2]), dot(ndc, invRow[3]));
    float3 p = w.xyz / w.w;
    float3 n = normalize(cross(ddx(p), ddy(p)));
    float4 e = float4(invRow[0].w, invRow[1].w, invRow[2].w, invRow[3].w);   // ndc (0,0,0,1): a point on the near plane, to face n
    if (dot(n, e.xyz / e.w - p) < 0) n = -n;
    // thin geometry (a rail, a fixture strip, a ledge's edge): the 8-unit voxels cannot resolve it, so a neighbour about one voxel away on
    // screen that leaves this pixel's plane by over half a voxel leaves the pixel to the map's own lighting (analysis/37, mobenix slivers)
    if (cv2.z > 0.5) {
        float upx = max(length(ddx(p)), 1e-4);           // world units per pixel here
        float k = clamp(1.0 / (cv.x * upx), 1, 12);       // one voxel, in pixels
        float2 tx = abs(ddx(uv)) + abs(ddy(uv));          // one pixel in uv (x and y)
        float2 o[4] = { float2(k * tx.x, 0), float2(-k * tx.x, 0), float2(0, k * tx.y), float2(0, -k * tx.y) };
        [unroll] for (int j = 0; j < 4; j++) {
            float dq = tex2Dlod(sceneDepth, float4(uv + o[j], 0, 0)).r;
            float4 nq = float4((uv.x + o[j].x) * 2 - 1, 1 - (uv.y + o[j].y) * 2, dq, 1);
            float4 wq = float4(dot(nq, invRow[0]), dot(nq, invRow[1]), dot(nq, invRow[2]), dot(nq, invRow[3]));
            if (abs(dot(wq.xyz / wq.w - p, n)) * cv.x > 0.5) { clip(-1); return 0; }
        }
    }
    float3 pv = p * cv.x;                                // voxel units
    float3 b = floor(pv / 8);                            // brick
    float3 sb = floor(b / 8);                            // super-brick
    float3 c = sb - sbMin.xyz;
    if (any(c < 0) || any(c >= l1Dim.xyz)) { clip(-1); return 0; }
    float g = floor(tex3Dlod(l1Tex, float4((c + 0.5) / l1Dim.xyz, 0)).r * 65535 + 0.5);
    if (g < 0.5) { clip(-1); return 0; }
    g -= 1;
    float3 pg = float3(g - 8 * floor(g / 8), floor(g / 8) - 8 * floor(g / 64), floor(g / 64));
    float bi = floor(tex3Dlod(l2Tex, float4((pg * 8 + (b - sb * 8) + 0.5) / l2Dim.xyz, 0)).r * 65535 + 0.5);
    if (bi < 0.5) { clip(-1); return 0; }
    bi -= 1;
    float3 ab = float3(bi - 16 * floor(bi / 16), floor(bi / 16) - 16 * floor(bi / 256), floor(bi / 256));
    float3 lv = clamp(pv - b * 8 - 0.5, 0, 7);           // voxel centres at +0.5; kept inside the brick (no apron)
    float4 sd = (tex3Dlod(atlasTex, float4((ab * 8 + lv + 0.5) / atDim.xyz, 0)) * 255 - 128) / 127 * cv.y;
    float4 ids = tex2Dlod(brickLamps, float4((bi - 256 * floor(bi / 256) + 0.5) / 256, (floor(bi / 256) + 0.5) / atDim.w, 0, 0));
    float ss = 0, sbl = 0, su = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        float id = ids[k];
        if (id < -0.5) continue;
        float4 t0 = lampTexel(id, 0), t1 = lampTexel(id, 1), t2 = lampTexel(id, 2), t3 = lampTexel(id, 3);
        float3 v = t0.xyz - p;
        float dist = length(v);
        float3 l = v / max(dist, 1e-3);
        float ndl = saturate(dot(n, l));
        float spot = 1;
        if (t0.w > 1.5) {                                // spot: stopdot t1.y, stopdot2 t1.z, axis t2.xyz, exponent t2.w
            float cs = dot(-l, t2.xyz);
            spot = cs <= t1.z ? 0 : cs >= t1.y ? 1 : pow(saturate((cs - t1.z) / max(t1.y - t1.z, 1e-4)), t2.w);
        }
        float D = t1.x * ndl * spot / max(t3.x + t3.y * dist + t3.z * dist * dist, 1e-3);
        float s = sd[k];
        ss += D * saturate(0.5 + s / (2 * cv.z));
        sbl += D * saturate(0.5 + s / (2 * cv.w));
        su += D;
    }
    float B = prm.x * su + prm.y;
    float ratio = lerp(1, (B + ss) / (B + sbl), prm.z);
    if (cv2.x < 0.5) ratio = min(ratio, 1);   // darken only (default): where the model's edge sits off the lightmap's it must not add light the map lacks
    if (prm.w > 0.5) return float4(saturate(ratio * 0.5).xxx + float3(0, 0.12, 0), 1);   // PHOTON_L3_DEBUG: the correction plus a green mark on bricks
    clip(abs(ratio - 1) - 0.004);
    return float4(saturate(ratio * 0.5).xxx, 1);
}
