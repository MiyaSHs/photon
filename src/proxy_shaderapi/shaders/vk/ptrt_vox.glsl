// PT-RT (analysis/43): the voxel cascades on the GPU (the twin of src/ambient/vox_cascade.c), shared by ptrt_trace.comp and
// ptrt_cache.comp: the pack, a ray through the cascades, a lamp's light, and the radiance cache.
// the pack (ptrt.c): words 0..19 level origins (ivec4 x 5, in bricks), 20..23 sun direction (toward the sun), 24..27 sun colour,
// 28..31 sky colour, 32 brick count; pages from word 64 (5 x 4096: brick index + 1, 0 empty); bricks after: 512 voxels x 2 words
// (x: albedo rgb8 + coverage8, y: normal snorm8 x 3)
layout(std430, binding = 0) readonly buffer Pack { uint w[]; } pk;
// the radiance cache (ptrt.c): per voxel and side (a voxel can hold both faces of a thin wall: side 0 along its stored normal, 1 against),
// brick-major ((brick index - 1) x 512 + voxel) x 2 + side, 2 words: half rgb of the light leaving it (its
// albedo x the light arriving: sun, lamps, and one gather ray that reads the cache where it lands, so bounces add up over frames) and a
// half sample count; cleared when a new pack arrives (the bricks are renumbered)
layout(std430, binding = 7) RAD_ACCESS buffer Rad { uint r[]; } rc;

const int LEVELS = 5, WIN = 16, BRICK = 8, PAGES = 64, BRICKS = 64 + LEVELS * 4096;

uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
float rnd(inout uint s) { s = hash(s); return float(s >> 8) * (1.0 / 16777216.0); }

ivec3 level_org(int l) { return ivec3(int(pk.w[l * 4]), int(pk.w[l * 4 + 1]), int(pk.w[l * 4 + 2])); }
vec3 fvec(int at) { return vec3(uintBitsToFloat(pk.w[at]), uintBitsToFloat(pk.w[at + 1]), uintBitsToFloat(pk.w[at + 2])); }

struct Hit { float t; vec3 n; vec3 albedo; uint vid; };   // vid: the voxel's index ((brick - 1) x 512 + voxel)

// one level's window, t in [t0, t1]: bricks, then voxels inside an occupied brick (as trace_level in vox_cascade.c)
bool trace_level(int l, vec3 o, vec3 d, float t0, float t1, out Hit h) {
    float vs = float(8 << l), bu = vs * BRICK;
    vec3 base = vec3(level_org(l)) * bu;
    vec3 p = o + d * (t0 + 1e-3) - base;
    ivec3 k = clamp(ivec3(floor(p / bu)), ivec3(0), ivec3(WIN - 1));
    ivec3 st = ivec3(sign(d));
    vec3 inv = 1.0 / max(abs(d), vec3(1e-9));
    vec3 tm = vec3(t0) + (mix(vec3(k) * bu, vec3(k + 1) * bu, greaterThan(d, vec3(0))) - p) * sign(d) * inv;
    vec3 td = bu * inv;
    tm = mix(vec3(1e30), tm, notEqual(st, ivec3(0)));
    float t = t0;
    for (int it = 0; it < 3 * WIN + 3; it++) {
        int ax = tm.x < tm.y ? (tm.x < tm.z ? 0 : 2) : (tm.y < tm.z ? 1 : 2);
        float te = min(tm[ax], t1);
        uint bi = pk.w[PAGES + l * 4096 + (k.z * WIN + k.y) * WIN + k.x];
        if (bi != 0u) {
            uint b0 = uint(BRICKS) + (bi - 1u) * 1024u;
            vec3 bb = base + vec3(k) * bu;
            vec3 q = o + d * (t + 1e-3) - bb;
            ivec3 v = clamp(ivec3(floor(q / vs)), ivec3(0), ivec3(BRICK - 1));
            vec3 vm = vec3(t) + (mix(vec3(v) * vs, vec3(v + 1) * vs, greaterThan(d, vec3(0))) - q) * sign(d) * inv;
            vm = mix(vec3(1e30), vm, notEqual(st, ivec3(0)));
            vec3 vd = vs * inv;
            float tv = t;
            for (int j = 0; j < 3 * BRICK + 3; j++) {
                uint i = uint((v.z * BRICK + v.y) * BRICK + v.x);
                uint a = pk.w[b0 + i * 2u];
                if ((a >> 24) != 0u) {
                    uint nn = pk.w[b0 + i * 2u + 1u];
                    h.t = tv;
                    h.vid = (bi - 1u) * 512u + i;
                    h.albedo = vec3(float(a & 255u), float((a >> 8) & 255u), float((a >> 16) & 255u)) / 255.0;
                    h.n = normalize(vec3(float(int(nn << 24) >> 24), float(int(nn << 16) >> 24), float(int(nn << 8) >> 24)) + 1e-4);
                    return true;
                }
                int a2 = vm.x < vm.y ? (vm.x < vm.z ? 0 : 2) : (vm.y < vm.z ? 1 : 2);
                tv = vm[a2];
                if (tv > te) break;
                v[a2] += st[a2];
                if (v[a2] < 0 || v[a2] >= BRICK) break;
                vm[a2] += vd[a2];
            }
        }
        t = tm[ax];
        if (t > t1) return false;
        k[ax] += st[ax];
        if (k[ax] < 0 || k[ax] >= WIN) return false;
        tm[ax] += td[ax];
    }
    return false;
}

bool trace(vec3 o, vec3 d, float tmin, float tmax, out Hit h) {
    float t = tmin;
    for (int l = 0; l < LEVELS && t < tmax; l++) {
        float bu = float(8 << l) * BRICK;
        vec3 lo = vec3(level_org(l)) * bu, hi = lo + WIN * bu;
        vec3 inv = 1.0 / d;
        vec3 u = (lo - o) * inv, v = (hi - o) * inv;
        vec3 mn = min(u, v), mx = max(u, v);
        float t0 = max(t, max(mn.x, max(mn.y, mn.z))), t1 = min(tmax, min(mx.x, min(mx.y, mx.z)));
        if (t0 > t1) continue;
        if (trace_level(l, o, d, t0, t1, h)) return true;
        t = max(t, t1);
    }
    return false;
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
// a lamp's light at p (normal n) without its shadow, as vrad's direct term (tools/pt_ref.c direct()); l: toward the lamp, ld: its distance
vec3 lamp_light(uint at, vec3 p, vec3 n, out vec3 l, out float ld) {
    vec3 o = fvec(int(at));
    vec3 v = o - p;
    ld = length(v);
    l = v / max(ld, 1e-3);
    float radius = uintBitsToFloat(pk.w[at + 7u]), ndl = dot(n, l);
    if (ndl <= 0.0 || (radius > 0.0 && ld > radius)) return vec3(0);
    float a = ndl / (uintBitsToFloat(pk.w[at + 8u]) + uintBitsToFloat(pk.w[at + 9u]) * ld + uintBitsToFloat(pk.w[at + 10u]) * ld * ld);
    if (uintBitsToFloat(pk.w[at + 3u]) > 1.5) {   // spot
        float d2 = -dot(l, fvec(int(at) + 12)), stop = uintBitsToFloat(pk.w[at + 15u]), stop2 = uintBitsToFloat(pk.w[at + 16u]);
        if (d2 <= stop2) return vec3(0);
        if (d2 <= stop) a *= pow((d2 - stop2) / max(stop - stop2, 1e-4), uintBitsToFloat(pk.w[at + 11u]));
    }
    return a * fvec(int(at) + 4);
}


#ifdef DYN_LAMPS
// SP layer 2 (analysis/47): this frame's dynamic lamps (the engine's dlights / elights), the same 20-float records as the pack's
layout(std430, binding = 20) readonly buffer Dyn { float d[]; } dy;
vec3 dvec(uint a) { return vec3(dy.d[a], dy.d[a + 1u], dy.d[a + 2u]); }
vec3 lamp_light_dyn(uint at, vec3 p, vec3 n, out vec3 l, out float ld) {
    vec3 v = dvec(at) - p;
    ld = length(v);
    l = v / max(ld, 1e-3);
    float radius = dy.d[at + 7u], ndl = dot(n, l);
    if (ndl <= 0.0 || (radius > 0.0 && ld > radius)) return vec3(0);
    float a = ndl / (dy.d[at + 8u] + dy.d[at + 9u] * ld + dy.d[at + 10u] * ld * ld);
    if (dy.d[at + 3u] > 1.5) {
        float d2 = -dot(l, dvec(at + 12u)), stop = dy.d[at + 15u], stop2 = dy.d[at + 16u];
        if (d2 <= stop2) return vec3(0);
        if (d2 <= stop) a *= pow((d2 - stop2) / max(stop - stop2, 1e-4), dy.d[at + 11u]);
    }
    return a * dvec(at + 4u);
}
#endif

// the cached light leaving side s of voxel v (rgb) and its sample count (w); the side a ray of direction d sees: against the normal it meets
vec4 rad_get(uint v, uint s) { uint c = v * 2u + s; vec2 a = unpackHalf2x16(rc.r[c * 2u]), b = unpackHalf2x16(rc.r[c * 2u + 1u]); return vec4(a, b); }
uint side_seen(vec3 n, vec3 d) { return dot(n, d) > 0.0 ? 1u : 0u; }
