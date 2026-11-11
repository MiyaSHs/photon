// PT-RT (analysis/43): Photon's light composited into the frame. The world is drawn with flat lightmaps (l4work.c mode 3: about its plain
// surface colour), so a world pixel's final colour is its surface colour x its light x exposure, in linear light as the stock shaders
// light (the frame copy decoded from sRGB, the result encoded back), with a soft shoulder on the light (identity up to 1, rolling off
// toward 3) so sunlit patches keep their gradation under the auto exposure. The light: the trace pass's (lamps and bounce, filtered, half
// size) plus the sun's direct light here at full size, sharp: Lambert on the depth's normal x the visibility in Photon's sun shadow map
// (shadow_ps.hlsl's sampling: the far map and the near cascade), the map's props and the world's own triangles included; beyond the map's
// box the trace pass's voxel visibility (s6). Models keep the engine's (Photon's model
// shaders') lighting: where the world-only depth (s2, when c0.y = 1) lies behind the scene depth, the pixel is a model's and is left alone.
// s0: the main view's depth (INTZ); s1: Photon's light (A16B16G16R16F, half size, linear); s2: the world-only depth (R32F); s3: the frame
// copy; s4 the sun map, s5 its near cascade (INTZ), s6 the world's sun visibility. c0: x exposure, y masked, z the sun on, w s6 on; c1..c12: shadow_ps.hlsl's c0..c11; c13 the
// sun's direction (toward it), c14 its colour.
sampler2D sceneDepth : register(s0);
sampler2D light : register(s1);
sampler2D worldDepth : register(s2);
sampler2D frame : register(s3);
sampler2D shadowMap : register(s4);
sampler2D nearMap : register(s5);
sampler2D sunVis : register(s6);
sampler2D worldNrm : register(s7);
sampler2D naniteDepth : register(s8);   // Nanite's props (D3D depth), merged before this pass in Path traced: lit as world   // the world's smooth normals (encoded; alpha 1 where the world was drawn)   // the world's sun visibility (the trace pass's voxel ray, half size): world brushes are not in the sun map
float4 prm : register(c0);
float4 invRow0 : register(c1);
float4 invRow1 : register(c2);
float4 invRow2 : register(c3);
float4 invRow3 : register(c4);
float4 sprm : register(c5);
float4 lightRow0 : register(c6);
float4 lightRow1 : register(c7);
float4 lightRow2 : register(c8);
float4 nearRow0 : register(c9);
float4 nearRow1 : register(c10);
float4 nearRow2 : register(c11);
float4 nearPrm : register(c12);
float4 sunDir : register(c13);
float4 sunCol : register(c14);
float4 prm2 : register(c15);   // x: s7 on, y: the debug view
sampler2D prevFrame : register(s12);  // PBR step 3: last frame's composited frame (screen-space reflections read their hits from it)
float4 vpRow0 : register(c18);        // this frame's view rows (clip = rows . p), the target's uv space
float4 vpRow1 : register(c19);
float4 vpRow2 : register(c20);
float4 vpRow3 : register(c21);
float4 pvRow0 : register(c22);        // last frame's rows (into prevFrame); c26.x 1: prevFrame valid
float4 pvRow1 : register(c23);
float4 pvRow2 : register(c24);
float4 pvRow3 : register(c25);
float4 prm4 : register(c26);
sampler2D specTex : register(s11);    // PBR step 3: the trace's glossy reflections (half size, linear), when c17.z is 1
sampler2D worldPbr : register(s10);   // PBR step 2: the world's roughness (r), metalness (g), a 1 where written (the debug view, c17.y)
sampler2D modelNrm : register(s9);   // the models' world normals (velocity_pass; a 1 where a model's surface is), when c17.x is 1
float4 prm3 : register(c17);   // x the models' normals (s9), y the PBR debug view, z glossy reflections (s10, s11)
float4 camPos : register(c16);   // the eye (world); w 1: models drew their surface colour (photon_ptrt_models): lit here too
float3 shoulder(float3 x) {
    const float k = 1.0, m = 3.0;
    return x <= k ? x : k + (m - k) * (1 - exp(-(x - k) / (m - k)));
}
float3 to_linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 to_srgb(float3 l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055; }
float hash2(float2 c) { return frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453); }
float vnoise(float2 x) {   // value noise in 0..1, smooth between integer lattice points
    float2 i = floor(x), f = frac(x); f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash2(i), hash2(i + float2(1, 0)), f.x), lerp(hash2(i + float2(0, 1)), hash2(i + float2(1, 1)), f.x), f.y);
}
float shadowAt(sampler2D m, float2 uv, float z, float bias, float h) {
    float s = 0;
    s += (z > tex2Dlod(m, float4(uv, 0, 0)).r + bias) ? 1 : 0;
    s += (z > tex2Dlod(m, float4(uv + float2(h, 0), 0, 0)).r + bias) ? 1 : 0;
    s += (z > tex2Dlod(m, float4(uv + float2(0, h), 0, 0)).r + bias) ? 1 : 0;
    s += (z > tex2Dlod(m, float4(uv + float2(h, h), 0, 0)).r + bias) ? 1 : 0;
    return s * 0.25;
}
float3 worldAt(float2 uv, float d) {
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 w = float4(dot(ndc, invRow0), dot(ndc, invRow1), dot(ndc, invRow2), dot(ndc, invRow3));
    return w.xyz / w.w;
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float d = tex2D(sceneDepth, uv).r;
    clip(0.99999 - d);
    float4 wn = prm2.x > 0.5 ? tex2D(worldNrm, uv) : float4(0, 0, 0, 0);
    if (prm2.z > 0.5) return float4(wn.rgb, 1);   // PHOTON_PTRT_WN_DEBUG: the normal pass's raw output
    if (prm3.y > 0.5) { float4 pb = tex2D(worldPbr, uv); return pb.a > 0.5 ? float4(pb.r, pb.g, 0.15, 1) : float4(0, 0, 0.4, 1); }   // PHOTON_PBR_DEBUG
    // a model's pixel (the world-only depth copy behind the scene depth) keeps its own light, unless the world's normal pass covered it
    // (the copy misses world drawn after the first model, such as part of gm_fork's displacement terrain)
    bool nanite = prm2.w > 0.5 && abs(tex2D(naniteDepth, uv).r - d) < 1e-5;   // a Nanite prop here (merged with its surface colour)
    if (camPos.w < 0.5 && prm.y > 0.5 && wn.a < 0.5 && !nanite && d < tex2D(worldDepth, uv).r - 1e-6) { clip(-1); return 0; }
    float3 L = tex2D(light, uv).rgb;
    float3 p = worldAt(uv, d);
    float3 n = normalize(cross(ddx(p), ddy(p)));
    if (wn.a > 0.5) n = normalize(wn.rgb * 2 - 1);   // smooth (displacements shade as the engine shades them)
    else if (prm3.x > 0.5) { float4 mn = tex2D(modelNrm, uv); if (mn.a > 0.5) n = normalize(mn.rgb * 2 - 1); }   // a model's own normal
    // the normal on the eye's side of the surface: judged against the depth's own (geometric) normal, not the view direction, so a
    // bumped normal (PBR packs) that leans away from the eye at a grazing angle is kept, not turned upside down (laptop pk7: half the grass
    // unlit)
    float3 ng = normalize(cross(ddx(p), ddy(p)));
    if (dot(ng, camPos.xyz - p) < 0) ng = -ng;
    // flipped only when both say it faces away: the view alone flipped bumped normals at grazing angles, the depth's normal alone
    // (spanning two surfaces at a crease) lit the wall-floor seams of the PBR test map as thin bright lines
    if (dot(n, ng) < 0 && dot(n, camPos.xyz - p) < 0) n = -n;
    if (prm.z > 0.5) {   // the sun, sharp
        float ndl = dot(n, sunDir.xyz);
        if (prm2.y > 0.5 && ndl <= 0) return float4(0, 0, wn.a, 1);   // the debug view: facing away (b: smooth normal present)
        if (ndl > 0) {
            float4 p4 = float4(p, 1);
            float3 lc = float3(dot(p4, lightRow0), dot(p4, lightRow1), dot(p4, lightRow2));
            float s = 0;
            float vw = prm.w > 0.5 ? tex2D(sunVis, uv).r : 1;   // the voxel visibility: only outside the map's box (the map holds the world now)
            if (all(abs(lc.xy) < 1)) {
                vw = 1;
                float2 suv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5) * sprm.w;
                s = shadowAt(shadowMap, suv, lc.z, sprm.x, sprm.z);
                if (nearPrm.y > 0.5) {
                    float3 nc = float3(dot(p4, nearRow0), dot(p4, nearRow1), dot(p4, nearRow2));
                    float m = max(abs(nc.x), abs(nc.y));
                    if (m < 1) {   // the near cascade where it covers the pixel, blended into the far one over its outer 20% (as shadow_ps)
                        float2 nuv = float2(nc.x * 0.5 + 0.5, 0.5 - nc.y * 0.5) * nearPrm.w;
                        s = lerp(shadowAt(nearMap, nuv, nc.z, nearPrm.x, nearPrm.z), s, saturate((m - 0.8) * 5));
                    }
                }
            }
            L += sunCol.rgb * ndl * (1 - s) * vw;
            if (prm2.y > 0.5) return float4(s, ndl, wn.a, 1);   // PHOTON_PTRT_SUNDEBUG: r map shadow, g lambert, b smooth normal present
        }
    }
    L = shoulder(L * prm.x);
    float3 albedo = to_linear(tex2D(frame, uv).rgb);   // the flat-lightmapped world: its surface colour
    // macro variation (user, 2026-11-10: gm_construct's concrete "so repetitive, no variety"): world pixels' colour varied by +-c26.y over
    // a large-scale value noise (c26.z units across; two octaves) on the surface's main plane, so a tiled texture's grid reads less
    if (prm4.y > 0 && wn.a > 0.5) {
        float3 an = abs(n);
        float2 q = (an.z >= an.x && an.z >= an.y ? p.xy : an.x >= an.y ? p.yz : p.xz) / prm4.z;
        float m = vnoise(q) * 0.65 + vnoise(q * 2.7 + 17.0) * 0.35;
        albedo *= 1.0 + prm4.y * (m * 2.0 - 1.0);
    }
    float3 col = albedo * L;
    if (prm3.z > 0.5) {   // PBR step 3: glossy reflections, Fresnel-weighted (Schlick with roughness), metals tinted by their albedo
        float4 pb = tex2D(worldPbr, uv);
        if (pb.a > 0.5 && (pb.r < 0.45 || pb.g > 0.5)) {
            float rough = pb.r, metal = pb.g;
            float ndv = saturate(dot(n, normalize(camPos.xyz - p)));
            float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
            float3 F = f0 + (max(float3(1 - rough, 1 - rough, 1 - rough), f0) - f0) * pow(1 - ndv, 5);
            float3 S = shoulder(tex2D(specTex, uv).rgb * prm.x);
            // screen-space reflection first (sharp, and every light of the frame): the mirror ray marched against the depth; a hit takes
            // last frame's finished colour there (reprojected). Smooth materials only; the voxel reflection above where nothing is hit
            if (prm4.x > 0.5 && rough < 0.4) {
                float3 r = reflect(-normalize(camPos.xyz - p), n);
                float t = 6, hit = 0; float2 huv = 0;
                for (int i = 0; i < 24; i++) {
                    float4 q = float4(p + r * t, 1);
                    float4 c = float4(dot(q, vpRow0), dot(q, vpRow1), dot(q, vpRow2), dot(q, vpRow3));
                    if (c.w <= 0) break;
                    float2 quv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
                    if (any(quv < 0) || any(quv > 1)) break;
                    float sd = tex2Dlod(sceneDepth, float4(quv, 0, 0)).r;
                    if (sd < c.z / c.w) {   // the ray went behind the surface: a hit if it is close (not passing behind a thin object)
                        float3 sp = worldAt(quv, sd);
                        if (length(sp - q.xyz) < t * 0.2 + 6) {
                            float4 pc = float4(dot(q, pvRow0), dot(q, pvRow1), dot(q, pvRow2), dot(q, pvRow3));
                            if (pc.w > 0) { huv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5); hit = all(huv > 0) && all(huv < 1) ? 1 : 0; }
                        }
                        break;
                    }
                    t *= 1.3;
                }
                if (hit > 0.5) {
                    float2 e = min(huv, 1 - huv);
                    float fade = saturate(min(e.x, e.y) * 10) * saturate(1 - rough * 2.5);   // the screen's edge, and rougher toward the voxels
                    S = lerp(S, to_linear(tex2Dlod(prevFrame, float4(huv, 0, 0)).rgb), fade);
                }
            }
            col = col * (1 - metal) * (1 - F) + F * S;
        }
    }
    return float4(to_srgb(saturate(col)), 1);
}
