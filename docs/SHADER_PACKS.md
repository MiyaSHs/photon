# Writing a Photon shader pack

This guide is for someone who has never seen Photon's code: it is everything a pack author needs. A **shader pack** is an ordinary
Garry's Mod addon (Workshop or `garrysmod/addons/<name>/`) that requires Photon. Photon is the engine: it owns every rendering feature
(lighting, shadows, path tracing, PBR, anti-aliasing, fog, ...). A pack **configures** those features, **adds** its own pieces
through fixed hook points (screen passes, lights), and later will **replace** single stages of Photon's pipelines with its own shaders.
This file is the contract: what it lists stays compatible across Photon updates.

Without Photon installed, a pack does nothing (its files are only data).

## 1. Files

```
myaddon/
  addon.json                         { "title": "My Photon pack", "type": "effects", "tags": ["fun"] }
  lua/autorun/client/mypack.lua      the pack (client side only)
```

Workshop addons may not contain DLLs or compiled shaders. Shaders therefore travel as **source text inside the Lua**; Photon compiles them.

## 2. Registering

Photon's API lives in the global `Photon` table, loaded by Photon's own `lua/autorun/client/photon_packs.lua`. Autorun files load in
alphabetical order, so a pack must cope with loading first:

```lua
local function reg()
    -- everything the pack does goes here (sections 3-5)
end
if Photon and Photon.RegisterPass then reg() else hook.Add("PhotonPacksReady", "mypack", reg) end
```

Registering is idempotent: calling it again (Lua refresh) updates the pack. Use one lowercase pack name (letters, digits, `_ - .`).

## 3. Configure Photon's features: `Photon.SetFeature(pack, key, value)`

While the pack is enabled, its values override the user's own settings; when the user turns the pack off, their settings return. Values
outside a key's range are clamped. Keys marked *(start)* apply from the next game start, *(map)* from the next map; the rest within seconds.

| key | values | what it does |
|---|---|---|
| `photon_preset` | 0 Optimized, 1 Better lighting, 2 Best quality, 3 Custom | the lighting preset |
| `photon_pathtrace` | 0 off, 1 baked *(map)*, 2 real time *(start)* | path-traced lighting |
| `photon_pt_rays`, `photon_pt_bounces` | 1-64, 0-8 | baked path tracer's rays per sample and bounces |
| `photon_shadows` | -1 preset, 0 off, 1 on | Photon's sun shadows |
| `photon_shadow_res` / `_dist` / `_soft` / `_strength` | 1024-4096 / units / 0-1 / 0-1 (-1 = preset) | sun shadow map size, reach, softness, darkness |
| `photon_shadow_props`, `photon_shadow_characters`, `photon_shadow_static` | 0 / 1 | who casts shadows |
| `photon_light_shadows`, `photon_light_shadow_res` | 0-4 lamps, tile size | map lamps with real-time shadows |
| `photon_rebake`, `photon_rebake_res` | 0 / 1, 8 or 4 units *(map)* | sharper re-baked lamp shadows |
| `photon_light_scale`, `photon_ambient_scale` | 0-16 | direct / ambient light multipliers |
| `photon_halflambert` | -1 auto, 0, 1 | soft wrap lighting on models |
| `photon_taa`, `photon_taa_sharpen` | 0 / 1, 0-1 | temporal anti-aliasing and its sharpening |
| `photon_upscale`, `photon_render_scale` | 0-3, 0.5-1 | upscaler and 3D render scale |
| `photon_nanite` | 0 / 1 *(start)* | Photon draws the map's static props |
| `photon_fog` | 0 / 1 | Photon's volumetric fog (sun shafts where Photon's sun shadows are on: preset 1+) |
| `photon_fog_density`, `photon_fog_height`, `photon_fog_falloff` | per 1000 units (0.3-2 typical), world z, units | how thick, where it pools, how fast it thins above |
| `photon_fog_dist`, `photon_fog_steps` | units, 4-64 | how far the fog is computed; quality vs cost |
| `photon_fog_r` / `_g` / `_b`, `photon_fog_sun`, `photon_fog_aniso` | 0-4, 0-50, -0.95-0.95 | ambient colour, sun-shaft strength, forward scattering |
| `photon_cpu_anim_step` | 0 off, 1-8 every Nth frame, 12-30 poses per second | choppy, cel-style animation of NPCs and other players (first version: the local player, viewmodel and ragdolls are never stepped) |

Example, a pack that is only a look: `Photon.SetFeature("noir", "photon_preset", 2); Photon.SetFeature("noir", "photon_shadow_strength", 1)`.
Several packs setting the same key: the pack whose name sorts last wins. Change settings only to serve your pack's look: the user chose
the rest.

## 4. Add a screen pass: `Photon.RegisterPass{ ... }`

```lua
Photon.RegisterPass({
    pack = "mypack", name = "grade", order = 50,            -- order: lower runs first (Photon's own passes run before all packs)
    hlsl = [[ ... ]],                                        -- the pixel shader source (below)
    params = { { "Strength", 0.5, 0, 1 }, { "Warmth", 0.2, -1, 1 } },   -- up to 16: label, default, min, max (sliders in the menu)
})
```

The pass runs once per frame on the main 3D view, after Photon's own work (lighting, path tracing, TAA) and before the weapon viewmodel
and the HUD. It reads the frame so far and writes the new one. It works on every route (native D3D9 and Vulkan).

**The shader**: HLSL Shader Model 3 (`ps_3_0`), entry point `main`:

```hlsl
sampler2D frame : register(s0);   // the frame so far (sRGB colour as displayed), bilinear
sampler2D depth : register(s1);   // scene depth, 0 near .. 1 far (1 = sky), point
float4 info  : register(c0);      // x 1/width, y 1/height, z seconds since load, w frame number
float4 inv0 : register(c1); float4 inv1 : register(c2); float4 inv2 : register(c3); float4 inv3 : register(c4);   // inverse view-projection rows
float4 eye  : register(c5);       // camera position (world units)
float4 vp0  : register(c6); float4 vp1 : register(c7); float4 vp2 : register(c8); float4 vp3 : register(c9);       // view-projection rows
float4 p0 : register(c10); float4 p1 : register(c11); float4 p2 : register(c12); float4 p3 : register(c13);       // your 16 params, in order
float4 flags : register(c14);     // x: s2 valid, y: s3 valid, z: s4 valid (this frame)
float4 pv0 : register(c15); float4 pv1 : register(c16); float4 pv2 : register(c17); float4 pv3 : register(c18);   // LAST frame's view-projection
sampler2D prev   : register(s2);  // the previous frame's final result (after all packs): trails, feedback, temporal effects
sampler2D motion : register(s3);  // moving models only: rg = offset to their uv last frame, b = 1 where a moving model is (else 0)
sampler2D normal : register(s4);  // world normals (rgb * 2 - 1), a = 1 where drawn; Path traced mode only
// camera motion: project world_at(uv) with pv0..pv3 (as uv_of does with vp0..vp3) to find where a static point was last frame

float3 world_at(float2 uv) {      // the world position seen at uv
    float4 ndc = float4(uv.x * 2 - 1, 1 - uv.y * 2, tex2D(depth, uv).r, 1);
    float4 w = float4(dot(ndc, inv0), dot(ndc, inv1), dot(ndc, inv2), dot(ndc, inv3));
    return w.xyz / w.w;
}
float2 uv_of(float3 p) {          // the screen position of a world point (w > 0: in front of the camera)
    float4 q = float4(p, 1);
    float4 c = float4(dot(q, vp0), dot(q, vp1), dot(q, vp2), dot(q, vp3));
    return float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
}
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(frame, uv).rgb;
    // ... your effect; p0.x is your first parameter
    return float4(c, 1);
}
```

World units: Source units (1 unit ~ 1.9 cm); +Z is up. Pixels: one texel is `info.xy` in uv. The frame is in display (sRGB) space:
convert with `pow(c, 2.2)` before light-like maths and back with `pow(c, 1/2.2)`.

**Errors and testing**: a compile error keeps the last working version running; the message is shown in the Photon settings window (hold
C > Photon) under the pack, and is written to `garrysmod/data/photon/packs/<pack>/<pass>.err.txt`. Console commands: `photon_packs`
(list, state, parameters, errors), `photon_pack_toggle <pack>/<pass>`, `photon_pack_param <pack>/<pass> <n> <value>`. Edit your Lua and
re-run it (`lua_openscript_cl` or a map reload): Photon recompiles only passes whose source changed.

**Cost**: a pass costs what its shader costs, at full resolution, every frame. Keep loops short; prefer few texture reads. With no pack
installed Photon does no pack work at all.

Other calls: `Photon.SetPassEnabled(key, on)`, `Photon.SetPassParam(key, n, v)`, `Photon.SetPackEnabled(pack, on)`, `Photon.Passes()`,
`Photon.PackFeatures()`, `Photon.PassError(key)` (`key` = `"<pack>/<pass>"`, lowercase).

## 4b. Change the textures: `Photon.RegisterTextureFilter{ ... }`

Changes how the map's and models' textures look, before any lighting: posterize, flatten, ink lines, palette swaps, pixel art.

```lua
Photon.RegisterTextureFilter({ pack = "mypack", name = "cartoon", order = 50, hlsl = [[ ... ]],
                               params = { { "Bands", 4, 2, 8 } } })
```

Each base texture (sampler 0 of the world and model shaders: loaded, mipmapped textures of at least 64 px; also sprites and particles,
never fonts, UI, render targets or cubemaps) is run **once** through your shader into Photon's own copy at half size, with mips; Photon
binds the copy from then on. So the filter costs nothing per frame; textures convert as they first appear (a few per frame). One filter
is active at a time (the enabled one with the lowest `order`). Changing its parameters converts everything again.

```hlsl
sampler2D src : register(s0);     // the original texture (bilinear)
float4 info : register(c0);       // xy: 1 / the copy's size (one texel)
float4 p0 : register(c10); float4 p1 : register(c11); float4 p2 : register(c12); float4 p3 : register(c13);   // your params
float4 main(float2 uv : TEXCOORD0) : COLOR {
    float4 c = tex2D(src, uv);
    // ... keep c.a: it is the texture's transparency or its reflection mask
    return c;
}
```

Example: `tests/packs/photon_pack_toontex.lua` (smoothing, brightness bands with the hue kept, saturation, inked edges). `Photon.SetTextureFilterParam(key, n, v)`, `Photon.TextureFilters()`.

## 5. Lights

No Photon API is needed: GMod's own `DynamicLight(index)` (client) lights the map and models in every Photon mode, including the real-time
path tracer (which traces a shadow for one such light per pixel). Keep `dietime` short and refresh it every frame for attached lights.
Example: `tests/packs/photon_pack_eyeglow.lua` (a light at each Combine soldier's eyes).

## 5b. Per-frame data: `photon_cpu.Push(channel, floats)`

For things that move every frame (smoke along a bullet's path, a storm cell), push numbers to Photon each frame from client Lua. Guard
it: `if photon_cpu and photon_cpu.Push then ... end` (it returns false on an older Photon). Channels:

- `"volumes"`: fog volumes, up to 8, 8 numbers each: `x, y, z, radius, density, type (0 sphere, 1 box), half-height (boxes), 0`. They add
  to Photon's fog (`photon_fog` must be on). Latest push wins; a channel not pushed for 10 frames falls back to
  `data/photon/packs/volumes.txt` (the same numbers per line: for volumes that never move, write it once).

Example: `tests/packs/photon_pack_weather.lua` (fog with sun shafts plus a moving smoke sphere).

## 6. Examples in Photon's repository

- `tests/packs/photon_pack_example.lua`: a height fog from depth and world positions, and a vignette with animated grain (two passes).
- `tests/packs/photon_pack_eyeglow.lua`: lights attached to NPCs.
- `tests/packs/photon_pack_toontex.lua`: cartoony textures (the texture stage).
- `tests/packs/photon_pack_trails.lua`: the previous frame as an input (ghost trails).
- `tests/packs/photon_pack_celanim.lua`: cel-style choppy animation through a feature.
- `tests/packs/photon_pack_weather.lua`: Photon's fog configured by a pack, plus a pushed moving smoke volume.

## 6b. Publishing on the Workshop

Upload the addon folder as usual (`gmad` / `gmpublish`, or the Workshop tools). In the title or description, say that the pack requires
Photon and link Photon's GitHub page: without Photon installed it does nothing. Workshop rules forbid DLLs and compiled shaders; a pack
needs neither, since its shaders are source text in its Lua.

## 7. Not available yet (planned; ask before relying on them)

- **Per-material texture filters**: today one filter applies to every base texture; choosing by material name (skin vs metal) is next.
- **Path tracer stages**: replacing the path tracer's shading rule, bounce rule or composite with your own GLSL compute shader, with
  Photon's world voxels, light cache, normals, PBR, motion and history as inputs (path-traced stylised looks at the built-in cost).
- **Volumes, next**: lamp-lit fog, more and softer volume shapes (tubes for bullet smoke), rain streaks, half-resolution marching.
- More pass inputs: Photon's light buffer, normals outside Path traced, render targets of your own and multi-pass chains.
