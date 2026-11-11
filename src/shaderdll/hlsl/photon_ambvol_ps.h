// photon_ambvol_ps.h: the per-pixel ambient volume (P3f; analysis/15d sections 4 and 8; bake in src/ambient/). Included only from inside
// //<PHOTON> regions, after g_PhotonTuning is declared. The proxy binds s15 and writes these registers (photon_lightblock.h); until it
// does, c64.y is 0 and every path is exactly the engine cube.
// The fetch is the one src/ambient/amb_bake.h documents: U stays inside a face band with its gutter, V and W wrap in the sampler.
// Clamping to the valid box first means nothing can wrap around to another place's voxels (15d section 8.1).
#ifndef PHOTON_AMBVOL_PS_H
#define PHOTON_AMBVOL_PS_H

const float4 g_PhotonVolA		: register( c59 );	// xyz = 1 / (voxels * voxel size), w = offset along the normal (world units)
const float4 g_PhotonVolB		: register( c60 );	// x = 0.5 / X, y = X / texels, z = 0.5 / texels, w = 1 / 6; texels = 6 (X + 1)
const float4 g_PhotonVolMin		: register( c61 );	// xyz: the valid box, world units
const float4 g_PhotonVolMax		: register( c62 );
const float4 g_PhotonDraw		: register( c64 );	// y = this draw's volume weight f (0 = off)
sampler3D g_PhotonVolumeSampler	: register( s15 );

float3 PhotonVolumeFetch( const float3 n, const float3 worldPos )
{
	float3 p = min( max( worldPos + n * g_PhotonVolA.w, g_PhotonVolMin.xyz ), g_PhotonVolMax.xyz );
	float3 t = p * g_PhotonVolA.xyz;
	float3 u = ( ( n >= 0.0f ) ? float3( 0.0f, 2.0f, 4.0f ) : float3( 1.0f, 3.0f, 5.0f ) ) * g_PhotonVolB.w
			 + frac( t.x - g_PhotonVolB.x ) * g_PhotonVolB.y + g_PhotonVolB.z;
	float3 n2 = n * n;
	return n2.x * tex3Dlod( g_PhotonVolumeSampler, float4( u.x, t.yz, 0.0f ) ).rgb
		 + n2.y * tex3Dlod( g_PhotonVolumeSampler, float4( u.y, t.yz, 0.0f ) ).rgb
		 + n2.z * tex3Dlod( g_PhotonVolumeSampler, float4( u.z, t.yz, 0.0f ) ).rgb;
}

// The ambient term: the volume at weight f plus the c33..c38 cube (which holds the folded lights, 15d section 8.2), scaled by c55.y and
// floored at c55.z. With c64.y = 0 this is the plain cube.
float3 PhotonAmbientWithVolume( const float3 cube, const float3 n, const float3 worldPos )
{
	float3 a = cube;
	[branch] if ( g_PhotonDraw.y > 0.0f )
	{
		a += g_PhotonDraw.y * PhotonVolumeFetch( n, worldPos );
	}
	return max( a * g_PhotonTuning.y, g_PhotonTuning.zzz );
}

#endif
