# Photon

Photon makes Garry's Mod look better and run faster, without changing how it plays. Your maps, models, weapons, vehicles and
addons keep working. You get:
- real-time sun and lamp shadows;
- better model lighting;
- an optional real-time path tracer;
- anti-aliasing and upscaling;
- a long list of CPU speed-ups.

Photon is free and open source (MIT-0, see [LICENSE](LICENSE)). It sends nothing anywhere: no telemetry, no accounts.

> **Multiplayer:** Photon replaces one of the game's engine files (`shaderapidx9.dll`). Play singleplayer, or host and join friends'
> servers started with `-insecure`. **Never join public VAC-secured servers with Photon installed.**

## Install

1. In Steam, switch Garry's Mod to the 64-bit branch: Garry's Mod > Properties > Betas > `x86-64 - Chromium + 64-bit binaries`.
   Let it update.
2. Download the newest `photon-*.zip` from this repository's Releases page and unzip it anywhere.
3. Close Garry's Mod and run `install.bat`. It finds the game through Steam. If it can't, run `install.bat "D:\path\to\GarrysMod"`.
   It asks two optional questions; both default to no:
   - **VPhysics-Jolt** (community physics): much better with big prop piles, slower in large battles.
   - **GModPatchTool** (video codecs and fixes for the in-game browser).
4. Add `-insecure` to Garry's Mod's launch options in Steam.
5. In game, open the context menu (hold C) and pick **Photon**.

**Update:** run `update.bat`. It downloads the newest release from GitHub and applies it; your settings stay.

**Uninstall:** run `uninstall.bat`. It restores every original file. The installer backs up each game file it replaces once, in
`GarrysMod\photon_backup`. Steam > Properties > Installed Files > Verify integrity also restores the game.

## What it changes, and the settings

All settings live on the Photon page of the context menu and apply live, unless the help text says "next map" or "next game
start".

### Lighting modes

| Mode | What you get | Cost |
|---|---|---|
| **Optimized** (default) | The game's own look, Photon's speed-ups only | Faster than stock |
| **Better lighting** | Real-time sun shadows from props, players and NPCs (soft, two cascades). One nearby map lamp casts real-time shadows. Per-pixel model lighting, smooth reflections | Moderate |
| **Best quality** | Sharper, farther sun shadows (4096 px, ±2048 units). Four lamps cast real-time shadows. The map lamps' baked shadows are re-baked sharper in the background (cached per map) | Highest raster cost |
| **Path traced, real time** | A live path tracer (details below) | Heavy; use render scale and upscaling |
| **Path traced, baked** | The map's lightmaps are path-traced once in the background, then cached. Moving objects use the raster lighting | Free once baked |

The map's own baked lighting stays the base in the raster modes, so maps keep the look their authors gave them. Photon adds what
Source can't do: moving things cast real shadows from the sun and from lamps.

### The real-time path tracer

- **Bounce light and sky light:** traced through a voxel copy of the map (8-unit bricks, built when the map loads) with a
  light cache that builds up while you play. Light bounces into rooms and bleeds colour.
- **Lamps and dynamic lights:** the map's lamps and the engine's dynamic lights (muzzle flashes, glowing effects) are traced,
  with shadow rays.
- **Reflections:** glossy reflections are traced, with screen-space reflections for detail.
- **Contact shadows:** small shadows where objects meet.
- **The sun:** comes from the raster shadow map, like the other modes.
- **PBR packs** (below) give surfaces real roughness and bumps.
- **Current limit:** moving objects (props, NPCs) receive light and cast sun shadows, but they don't yet block or bounce the
  traced light.

### Image quality

- **Anti-aliasing (TAA):** a temporal anti-aliasing that follows moving objects, with adjustable sharpening.
- **Render scale and upscaling:** renders the 3D view smaller (100% down to 50%) and rebuilds full detail with Photon's
  temporal upscaler or AMD FSR 2. The HUD stays sharp.
- **Dynamic quality:** set a target frame rate. Photon steps quality down before frames get slow, and back up once there is
  room.

### PBR texture packs

Packs of roughness, metalness and normal maps for HL2, Episode One, Episode Two and Garry's Mod content. They're used by the
path tracer and by Photon's world lighting. Turn a pack on in the menu and it downloads; it's derived from the games' own
textures, so it only applies where you own the game.

### Shader packs

Shader packs are Lua addons, usually from the Workshop, that require Photon. They add screen effects, texture filters,
volumetric fog and lights, and switch Photon's features. The authors' guide is [docs/SHADER_PACKS.md](docs/SHADER_PACKS.md).
Packs show up on the Photon page, where each can be turned off.

## Performance (CPU)

Big battles in GMod usually slow down because of the CPU, not the graphics card: corpses piling up, physics catching up, AI and
Lua. Photon's CPU module (`photon_cpu.dll`) targets those. Everything is in the Photon menu under **Performance (CPU)**, and every
option can be turned off.

**Corpses and dropped weapons** (on by default)
- Corpses and the weapons they drop **don't collide with each other** (they still collide with the world, you and NPCs). Piles of
  bodies bumping into each other are one of the biggest physics costs in GMod.
- At most **64 corpses** are kept (and as many dropped weapons); past that the oldest is removed, one at a time. Anything you hold
  with the physgun or have roped or welded is kept. Choose 8 to "No limit", or a time limit, in the menu.
- **Ragdoll mods settle properly.** Fedhoria leaves its stumble controllers behind, which keeps every corpse awake forever; Photon
  ends them when the mod's own stumble time is over (61 -> 73 fps in a test battle).
- Optional: lighter corpses (you push them aside instead of getting stuck), and drawing corpses only nearby or not at all.

**Slow physics under heavy load** (on by default)
When physics can't keep up (piles of debris, explosions), Photon slows down physics objects for a moment instead of dropping the
whole game's frame rate. You, NPCs and weapons keep full speed. In multiplayer this keeps players from freezing while the server
catches up (an overloaded test server went from 5.9 to 15.7 fps).

**Dynamic quality** (target frame rate)
Set a target (for example 60 fps) and Photon lowers or raises settings to hold it, acting before the frame rate drops, not after.
It knows whether the CPU or the graphics card is the bottleneck, and starts with whatever measures as the most expensive: cleaning up
loot and corpses for the CPU, render scale or shadows for the GPU. You can reorder or disable each step. In a heavy NPC battle:
44 -> 75 fps.

**Other speed-ups** (on by default, same result as without them)
- Character bones computed once per frame instead of several times (about 20% faster with many NPCs).
- Faster map loading with many addons (remembers which files don't exist).
- Faster physics checks for Lua (large gains with active ragdolls and gore mods).

**Optional, for testing**
- *Slow down instead of freezing* (single player): when the game can't keep up it runs fewer catch-up updates per frame.
- *Multiplayer log*: records what makes a multiplayer session feel laggy into `garrysmod/data/photon/mplog` (no names), for
  troubleshooting. Off by default.

**Physics engine.** Photon works with GMod's own physics and with VPhysics-Jolt (the installer can download it). Measured: Jolt is
much better with big prop piles and colliding debris (GMod's physics can freeze for seconds at 800 props), and slower in battles with
Photon's corpse settings. Pick Jolt if you build or spawn a lot of props.

## Requirements

- **Windows,** with the 64-bit Garry's Mod branch.
- **Graphics card:**
  - The raster modes run on anything Garry's Mod runs on (tested on a GTX 1050 and an RX 580).
  - Real-time path tracing and FSR 2 need a Vulkan 1.1 GPU. Photon runs the game through DXVK, which it ships.

## For developers

- **Building:** a MinGW cross build from Linux or WSL; see `scripts/` and `CMakeLists.txt`.
- **Shader packs:** see [docs/SHADER_PACKS.md](docs/SHADER_PACKS.md).

## Licences

Photon is MIT-0. It ships or downloads these third-party components; their licences are in `licenses/`:
- **DXVK:** zlib.
- **AMD FidelityFX FSR 2:** MIT.
- **LuaJIT:** MIT (only in the optional replacement Lua engine, a test build not in the release package yet).
- **VPhysics-Jolt:** MIT, optional download.
- **GModPatchTool:** GPL-3, optional download, never bundled.
