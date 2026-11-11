#version 450
// NANITE: fade dither, then the material's base texture (set 1; alpha-tested materials cut out where alpha < 0.5, sun.w = 1) times the prop's baked vertex
// light from the vertex shader, and the D3D depth for the D3D9 merge.
layout(push_constant) uniform P { vec4 vp[4]; vec4 sun; vec4 eye; vec4 fog; vec4 fogc; } pc;
layout(set = 1, binding = 0) uniform sampler2D base;
layout(location = 0) in vec3 i_nrm;
layout(location = 1) in vec3 i_col;
layout(location = 2) in float i_z;
layout(location = 3) in vec2 i_uv;
layout(location = 4) flat in float i_fade;
layout(location = 5) in float i_pz;
layout(location = 0) out vec4 o_col;
layout(location = 1) out vec4 o_depth;
void main() {
    if (i_fade < 1.0) {   // fading out: a 4x4 ordered dither instead of the engine's alpha blend (the merge keeps depth, so no blending)
        const float bayer[16] = float[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
        ivec2 q = ivec2(gl_FragCoord.xy) & 3;
        if (i_fade <= (bayer[q.y * 4 + q.x] + 0.5) / 16.0) discard;
    }
    vec4 t = texture(base, i_uv);
    if (pc.sun.w > 0.5 && t.a < 0.5) discard;
    // the texture (sampled as sRGB, so linear) x the prop's baked light (linear), encoded to sRGB as the stock shader's sRGB write does
    // then the HDR light scale (fogc.w) and the stock range fog (fog.x start / range, fog.y 1 / range, fog.z max density; fogc.rgb linear)
    // fog.w 1 (PT-RT): the surface colour alone, as the world's flat lightmaps leave it, for PT-RT's composite to light
    vec3 l = pc.fog.w > 0.5 ? t.rgb : t.rgb * i_col * pc.fogc.w;
    float f = clamp(min(pc.fog.z, i_pz * pc.fog.y - pc.fog.x), 0.0, 1.0);
    l = mix(l, pc.fogc.rgb, f * f);
    o_col = vec4(mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l)), 1.0);
    o_depth = vec4(i_z, 0, 0, 1);
}
