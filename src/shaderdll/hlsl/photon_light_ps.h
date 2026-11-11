// photon_light_ps.h: Photon per-pixel light math for the substituted bump and skin ps_3_0 shaders (P2; P5's light loop).
// Included only from inside //<PHOTON> regions, under #if PHOTON_ENABLED; it declares the c32..c55 light block
// (src/proxy_shaderapi/photon_lightblock.h is the C side and the layout reference). The lights, the per-light formula (the stock VS light
// math, common_vs_fxc.h, evaluated per pixel) and the loop over the engine's lights and the light grid are photon_lights_ps.h's, with
// the half-Lambert switch passed in by the caller instead of read from c32.z. The caller may define PHOTON_GRID_SAMPLER_REG (default s9).
#ifndef PHOTON_LIGHT_PS_H
#define PHOTON_LIGHT_PS_H

const float4 g_PhotonFlags						: register( c32 );	// x enable, y light count 0..4
const float3 g_PhotonAmbientCube[6]				: register( c33 );	// through c38
const float4 g_PhotonLights[16]					: register( c39 );	// through c54
const float4 g_PhotonTuning						: register( c55 );	// x light, y ambient scale, z floor, w debug
#include "photon_ambvol_ps.h"

#if !defined( PHOTON_GRID_SAMPLER_REG )
#define PHOTON_GRID_SAMPLER_REG s9
#endif
#include "photon_lights_ps.h"

// Sum of every light (the c39.. engine lights, count c32.y, then the pixel's grid cell: photon_lights_ps.h) at a unit normal, scaled by c55.x
float3 PhotonDirectPS( const float3 n, const float3 worldPos, const float fHalfLambert )
{
	return PhotonDirectSum( n, worldPos, fHalfLambert ) * g_PhotonTuning.x;
}

// The ambient term at a unit normal: the c33..c38 cube, plus the volume when this draw has it.
float3 PhotonAmbientPS( const float3 n, const float3 worldPos )
{
	return PhotonAmbientWithVolume( PixelShaderAmbientLight( n, g_PhotonAmbientCube ), n, worldPos );
}

#endif
