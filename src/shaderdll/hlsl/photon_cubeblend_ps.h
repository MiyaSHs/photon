// photon_cubeblend_ps.h: the two-cubemap blend (lighting plan v2 P4b, analysis/15a section 3b, analysis/17b). Included only from inside
// //<PHOTON> regions, under #if PHOTON_CUBEBLEND, before photon_envocc_ps.h.
//
// The proxy binds the engine's cubemap A (or, with photon_cubemap_pick 1, the visible one from the pick table) on the stock envmap sampler
// and the second sample B on a sampler of ours (VLG and bump: s13; skin: s5, whose normalisation cube only the stock lighting path reads,
// which is dead whenever the Photon path runs, see analysis/17b). Per pixel, B's weight is the distance past the plane halfway between the two
// samples, over a blend width W, as one plane equation the proxy folds:
//   c57 = ( (B - A) / (|B - A| W), 0.5 - dot((A + B) / 2, (B - A) / |B - A|) / W )      wB = saturate( dot(worldPos, c57.xyz) + c57.w )
//   c58.x = 1 when B is bound for this draw, else 0
// so wB = 0.5 on the bisector (where the engine switches from A to B), and it is symmetric in A and B: when the pick swaps them at the
// boundary, the image does not change.
// PHOTON_CUBEBLEND_BRANCH 1 (VLG, bump): B's fetches sit behind a uniform [branch] on c58.x, as photon_ambvol_ps.h guards the volume with
// c64.y: with the blend off (the default) no B fetch runs and every function here returns its stock input itself, not a + 0 * (b - a). Inside
// the branch B is a texldd with the gradients of A's direction, taken before the branch (C3: no implicit-gradient fetch inside a dynamic
// branch), so B gets A's mip level.
// PHOTON_CUBEBLEND_BRANCH 0 (skin): the branch costs about 20 slots and skin's heaviest combos would reach 529 of 512 (509 without), so
// skin keeps the unbranched form: B is a plain texld every pixel, and with c58.x = 0 the weight is 0 and lerp(a, b, 0) = a + 0 * (b - a) = a
// for any finite b (the golden comparison checks it bit for bit).
#if !defined( PHOTON_CUBEBLEND_BRANCH )
#define PHOTON_CUBEBLEND_BRANCH 1
#endif
#ifndef PHOTON_CUBEBLEND_PS_H
#define PHOTON_CUBEBLEND_PS_H

const float4 g_PhotonCubePlane					: register( c57 );
const float4 g_PhotonCube						: register( c58 );

float PhotonCubeWeight( const float3 worldPos )
{
	return saturate( dot( worldPos, g_PhotonCubePlane.xyz ) + g_PhotonCubePlane.w ) * g_PhotonCube.x;
}

// the reflection: A's fetch (the stock texCUBE, unchanged) blended with B's at the same direction
float3 PhotonCubeBlend( const float3 a, sampler envmapB, const float3 reflectVect, const float3 worldPos )
{
#if !PHOTON_CUBEBLEND_BRANCH
	return lerp( a, texCUBE( envmapB, reflectVect ).rgb, PhotonCubeWeight( worldPos ) );
#else
	float3 dx = ddx( reflectVect ), dy = ddy( reflectVect );
	float3 r = a;
	[branch] if ( g_PhotonCube.x > 0.0f )
	{
		r = lerp( a, texCUBEgrad( envmapB, reflectVect, dx, dy ).rgb, PhotonCubeWeight( worldPos ) );
	}
	return r;
#endif
}

#endif
