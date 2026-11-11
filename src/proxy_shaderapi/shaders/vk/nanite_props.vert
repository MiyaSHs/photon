#version 450
// NANITE (analysis/44): static props drawn by Photon. Vertices pulled from a storage buffer (position, normal, colour, uv, 0 per vertex,
// 12 floats), the placement's model-to-world rows from another (indexed by the instance: firstInstance = the model's first placement).
layout(std430, binding = 0) readonly buffer Verts { float v[]; } vb;
layout(std430, binding = 1) readonly buffer Insts { vec4 rows[]; } ib;   // 4 per placement: 3 matrix rows, then x = the baked colours' base (int bits),
                                                                         // yz the fade range, w = 1 + its cube and sun in cols (int bits; 0: none)
layout(std430, binding = 2) readonly buffer Cols { uint c[]; } cb;      // the placements' baked vertex light (VRAD .vhv: B G R A bytes)
layout(std430, binding = 3) readonly buffer Vis { uint v[]; } vis;      // this frame's visible placements (culled on the CPU), by instance
layout(push_constant) uniform P { vec4 vp[4]; vec4 sun; vec4 eye; vec4 fog; vec4 fogc; } pc;    // vp: view-projection rows (clip_j = rows_j . (p, 1)); sun.xyz toward the sun;
                                                                          // eye.xyz the camera, eye.w 1 = fades on; fog, fogc: see the .frag
layout(location = 0) out vec3 o_nrm;
layout(location = 1) out vec3 o_col;
layout(location = 2) out float o_z;
layout(location = 3) out vec2 o_uv;
layout(location = 4) flat out float o_fade;
layout(location = 5) out float o_pz;   // clip z before the divide (the stock range fog's projPos.z)
void main() {
    uint b = uint(gl_VertexIndex) * 12u;
    vec4 lp = vec4(vb.v[b], vb.v[b + 1u], vb.v[b + 2u], 1.0);
    vec3 ln = vec3(vb.v[b + 3u], vb.v[b + 4u], vb.v[b + 5u]);
    uint r = vis.v[gl_InstanceIndex] * 4u;
    vec3 wp = vec3(dot(ib.rows[r], lp), dot(ib.rows[r + 1u], lp), dot(ib.rows[r + 2u], lp));
    o_nrm = normalize(vec3(dot(ib.rows[r].xyz, ln), dot(ib.rows[r + 1u].xyz, ln), dot(ib.rows[r + 2u].xyz, ln)));
    // the stock vertex shader's static light: GammaToLinear(colour x overbright 2) (common_vs_fxc.h DoLighting)
    int base = floatBitsToInt(ib.rows[r + 3u].x), dyn = floatBitsToInt(ib.rows[r + 3u].w);
    if (dyn > 0) {   // no baked light: as the engine lights a model (common_vs_fxc.h AmbientLight: n^2-weighted cube faces), plus the sun's lambert
        uint a = uint(dyn - 1);
        vec3 n = o_nrm, n2 = n * n;
        uint fx = a + (n.x < 0.0 ? 3u : 0u), fy = a + (n.y < 0.0 ? 9u : 6u), fz = a + (n.z < 0.0 ? 15u : 12u);
        vec3 cx = vec3(uintBitsToFloat(cb.c[fx]), uintBitsToFloat(cb.c[fx + 1u]), uintBitsToFloat(cb.c[fx + 2u]));
        vec3 cy = vec3(uintBitsToFloat(cb.c[fy]), uintBitsToFloat(cb.c[fy + 1u]), uintBitsToFloat(cb.c[fy + 2u]));
        vec3 cz = vec3(uintBitsToFloat(cb.c[fz]), uintBitsToFloat(cb.c[fz + 1u]), uintBitsToFloat(cb.c[fz + 2u]));
        vec3 sun = vec3(uintBitsToFloat(cb.c[a + 18u]), uintBitsToFloat(cb.c[a + 19u]), uintBitsToFloat(cb.c[a + 20u]));
        o_col = n2.x * cx + n2.y * cy + n2.z * cz + sun * max(dot(n, pc.sun.xyz), 0.0);
    } else {
        uint packed = base == -2147483648 ? cb.c[0] : cb.c[uint(base + gl_VertexIndex)];
        vec3 vhv = vec3(float((packed >> 16) & 255u), float((packed >> 8) & 255u), float(packed & 255u)) / 255.0;
        o_col = pow(vhv * 2.0, vec3(2.2));
    }
    o_uv = vec2(vb.v[b + 9u], vb.v[b + 10u]);
    // the engine's fade: 1 before the fade min distance from the placement's origin, 0 at the fade max (rows[r + 3].yz; max 0 = none)
    vec2 fr = ib.rows[r + 3u].yz;
    float dist = length(vec3(ib.rows[r].w, ib.rows[r + 1u].w, ib.rows[r + 2u].w) - pc.eye.xyz);
    o_fade = pc.eye.w < 0.5 || fr.y <= 0.0 ? 1.0 : fr.x < fr.y ? clamp((fr.y - dist) / (fr.y - fr.x), 0.0, 1.0) : float(dist <= fr.y);
    vec4 c = vec4(dot(pc.vp[0], vec4(wp, 1)), dot(pc.vp[1], vec4(wp, 1)), dot(pc.vp[2], vec4(wp, 1)), dot(pc.vp[3], vec4(wp, 1)));
    o_z = c.z / c.w;
    o_pz = c.z;
    gl_Position = vec4(c.x, -c.y, c.z, c.w);   // D3D's clip y is up, Vulkan's down
}
