// photon_envocc_ps.h: Photon envmap specular occlusion (lighting plan v2 P1, the "glow" fix; analysis/15 §1.5 with
// critique revisions C1 and C3, analysis/15a §3a). Included only from inside //<PHOTON> regions of the substituted
// ps_3_0 shaders (photon_vlg_ps2x.fxc, photon_bump_ps2x.fxc), under #if PHOTON_ENVOCC.
//
// Stock adds the cubemap reflection unlit (vertexlit_and_unlit_generic_ps2x.fxc:456-467, bump_ps2x.fxc:280-290), so a
// prop in shadow reflects the cubemap at the brightness of the place it was captured. This scales the finished stock
// specularLighting (after mask, tint, contrast and saturation) by the ratio of the local lighting to the cubemap's own
// irradiance in the normal direction:
//   x = Y(diffuseLighting) / (Y(ENV_MAP_SCALE * texCUBElod(envmap, N, lowest mip)) + 1e-4)
//   t = saturate(log2(x) * c56.x + c56.y);   spec *= smoothstep01(t) * c56.z + c56.w        (c56 below)
// [C1] diffuseLighting is the shader's FINAL diffuse lighting, whatever produced it (Photon per-pixel, stock vertex
// lighting incl. VHV static props, the SLL lightmap, stock bump PixelShaderDoLighting), so the term is valid on baked
// static props. [C2, P4b] with two cubemaps (photon_cubeblend_ps.h) the overload below blends the two lowest mips; with one it is the
// texture's lowest mip (texCUBElod clamps the LOD to the last level; a NOMIP cubemap gives its level-0 texel, noisy,
// hence the clamp). [C3] the only fetch inside the branch is an explicit-LOD texldl, so FXC can keep it a real
// dynamic branch: with c56.x <= 0 the branch is skipped and the output is the stock result.
//
// The curve is a smoothstep on log2(x) between x = k / 4 (reflection gone) and x = k (reflection kept), k = 0.5 by
// default (P1 re-test, PROGRESS 2026-11-03): in game x is log-distributed and bimodal. In a truly dark room with a
// bright cubemap (gm_construct ColourRoom, lights off) the envmap-weighted median x is 0.03-0.12; on the shadowed
// side of a prop in a bright place (the roller's lower half in the spawn field, the static sink) it is 0.36-1.0,
// because the leaf ambient cube's dark faces sit below the cubemap mip's matching faces (a calibration gap that
// [C2, P4] removes). A linear clamp(x / k) cannot keep both apart: k = 1 left the roller at 0.74 and the sink at
// 0.76 x stock; k = 0.5 reaches the targets with no margin (0.29 dark, 0.92 bright, measured x replayed offline).
//
// c56 is written by photon_shaderapi.dll (src/proxy_shaderapi/photon_lightblock.h PHOTON_LB_ENVOCC,
// photon_lb_envocc()) folded from the config: strength s (photon_envmap_occlusion, 0..1), the factor range
// [kMin, kMax] (photon_envmap_occlusion_min/_max: the factor at x <= k/4 and at x >= k) and the knee k
// (photon_envmap_calibration). With W = PHOTON_LB_ENVOCC_OCTAVES (2), lo = 1 + s (kMin - 1), hi = 1 + s (kMax - 1):
//   c56 = ( 1 / W, (W - log2 k) / W, hi - lo, lo )
//   =>   factor = lerp( 1, kMin + (kMax - kMin) * smoothstep( log2(k / 2^W), log2(k), log2(x) ), s )
// log2(0) = -inf saturates to t = 0 (x is >= 0: a luma over a positive denominator). s = 0 writes c56 = (0, 0, 0, 1):
// the branch is off (and the factor would be 1 exactly even without it).
#ifndef PHOTON_ENVOCC_PS_H
#define PHOTON_ENVOCC_PS_H

const float4 g_PhotonEnvOcc						: register( c56 );

#define PHOTON_LUMA float3( 0.2126f, 0.7152f, 0.0722f )

float3 PhotonEnvmapOcclusion( float3 spec, const float3 diffuseLighting, const float3 n, sampler envmap )
{
	[branch] if ( g_PhotonEnvOcc.x > 0.0f )
	{
		// a cube lookup needs no normalised direction; LOD 16 clamps to the last mip
		// + 1e-4 instead of max(, 1e-4): FXC expands rcp(max()) into 5 instructions (mad, mul, rcp, cmp + a mov)
		float flCube = dot( texCUBElod( envmap, float4( n, 16.0f ) ).rgb, PHOTON_LUMA ) * ENV_MAP_SCALE + 1.0e-4f;
		float flRatio = dot( diffuseLighting, PHOTON_LUMA ) * rcp( flCube );
		float t = saturate( log2( flRatio ) * g_PhotonEnvOcc.x + g_PhotonEnvOcc.y );
		spec *= ( t * t * ( 3.0f - 2.0f * t ) ) * g_PhotonEnvOcc.z + g_PhotonEnvOcc.w;
	}
	return spec;
}

#if PHOTON_CUBEBLEND
// P4b [C2]: with two cubemaps the denominator is the irradiance of what is shown, the blend of A's and B's lowest mips at the same weight
// (photon_cubeblend_ps.h), B's fetch behind the same c58.x branch (unbranched in skin, PHOTON_CUBEBLEND_BRANCH 0). With c58.x = 0 this is the
// function above.
float3 PhotonEnvmapOcclusion( float3 spec, const float3 diffuseLighting, const float3 n, sampler envmap, sampler envmapB, const float3 worldPos )
{
	[branch] if ( g_PhotonEnvOcc.x > 0.0f )
	{
#if PHOTON_CUBEBLEND_BRANCH
		float3 cube = texCUBElod( envmap, float4( n, 16.0f ) ).rgb;
		[branch] if ( g_PhotonCube.x > 0.0f )
		{
			cube = lerp( cube, texCUBElod( envmapB, float4( n, 16.0f ) ).rgb, PhotonCubeWeight( worldPos ) );
		}
#else
		float3 cube = lerp( texCUBElod( envmap, float4( n, 16.0f ) ).rgb, texCUBElod( envmapB, float4( n, 16.0f ) ).rgb, PhotonCubeWeight( worldPos ) );
#endif
		float flCube = dot( cube, PHOTON_LUMA ) * ENV_MAP_SCALE + 1.0e-4f;
		float flRatio = dot( diffuseLighting, PHOTON_LUMA ) * rcp( flCube );
		float t = saturate( log2( flRatio ) * g_PhotonEnvOcc.x + g_PhotonEnvOcc.y );
		spec *= ( t * t * ( 3.0f - 2.0f * t ) ) * g_PhotonEnvOcc.z + g_PhotonEnvOcc.w;
	}
	return spec;
}
#endif

#endif
