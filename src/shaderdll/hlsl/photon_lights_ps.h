// photon_lights_ps.h: the P5 light loop (analysis/18_p5_lights.md). One loop replaces the unrolled 4-light code of the three pairs: its first
// c32.y iterations take the engine's lights from the c39..c54 block (a select, as ps_3_0 cannot index c# in a loop), the rest take the lights
// of this pixel's light-grid cell from the grid texture. Included only from inside //<PHOTON> regions under PHOTON_ENABLED, after the block
// (g_PhotonFlags, g_PhotonLights, g_PhotonTuning) and photon_ambvol_ps.h (g_PhotonDraw) are declared. The caller defines
// PHOTON_GRID_SAMPLER_REG (s9 in VLG and skin, s10 in bump: samplers stock's cache never had enabled in our scenes, 18 Design).
//
// The grid (C side src/ambient/lg_grid.h, whose LG_* constants are these): world cells of 1 / c63.w units, a window of GX x GY x GZ cells
// from cell c63.xyz (world cell coordinates), rebuilt on a worker every frame. One RGBA32F texture of W x H texels, read with point fetches at
// texel centres, by tex2Dlod only (rule C3):
//   rows [0, GY GZ)  cell (x, y, z), taken mod (GX, GY, GZ) (toroidal: a camera move rewrites only the new cells), owns the 4 texels from
//                    column 4 (x mod GX) of row (y mod GY) + GY (z mod GZ): (n, i0, i1, i2) then 4 record indices per texel, so n <= 15;
//   rows [REC, H)    light records, 4 texels each, exactly the c39.. layout (photon_lightblock.h); record p at row REC + p / 32, column 4 (p mod 32).
// c64.z is this draw's grid weight (0: the grid is off for this draw and nothing below fetches; the loop is then exactly the engine path).
#ifndef PHOTON_LIGHTS_PS_H
#define PHOTON_LIGHTS_PS_H

#define PHOTON_LG_W			128.0f		// = LG_TEX_W
#define PHOTON_LG_H			528.0f		// = LG_TEX_H
#define PHOTON_LG_GX		32.0f		// = LG_GX (= W / 4)
#define PHOTON_LG_GY		32.0f
#define PHOTON_LG_GZ		16.0f
#define PHOTON_LG_REC		512.0f		// = LG_REC_ROW (= GY GZ)
#if !defined( PHOTON_LOOP_MAX )
#define PHOTON_LOOP_MAX		19			// 4 engine lights + up to 15 grid lights (LG_KMAX)
#endif

const float4 g_PhotonGrid				: register( c63 );	// xyz: the window's first cell (world cell coordinates); w: 1 / cell size
sampler PhotonGridSampler				: register( PHOTON_GRID_SAMPLER_REG );

// component c (0..3, integer valued) of v
float PhotonPick4( const float4 v, const float c )
{
	float4 m = saturate( 1.0f - abs( c - float4( 0.0f, 1.0f, 2.0f, 3.0f ) ) );
	return dot( v, m );
}

// The loop's extent at worldPos: nTot = engine lights + this cell's grid lights; list = (u, v) of the cell's first texel
void PhotonLightRange( const float3 worldPos, out float nTot, out float2 list )
{
	nTot = g_PhotonFlags.y;
	list = float2( 0.0f, 0.0f );
	[branch] if ( g_PhotonDraw.z > 0.0f )
	{
		float3 wc = floor( worldPos * g_PhotonGrid.w );
		float3 rel = wc - g_PhotonGrid.xyz;
		float3 t = frac( wc * float3( 1.0f / PHOTON_LG_GX, 1.0f / PHOTON_LG_GY, 1.0f / PHOTON_LG_GZ ) );	// (cell mod G) / G, exact
		list = float2( t.x + 0.5f / PHOTON_LG_W, dot( t.yz, float2( PHOTON_LG_GY, PHOTON_LG_GY * PHOTON_LG_GZ ) ) * ( 1.0f / PHOTON_LG_H ) + 0.5f / PHOTON_LG_H );
		float n = tex2Dlod( PhotonGridSampler, float4( list, 0.0f, 0.0f ) ).x;
		bool inside = all( rel >= 0.0f ) && all( rel < float3( PHOTON_LG_GX, PHOTON_LG_GY, PHOTON_LG_GZ ) );
		nTot += inside ? n : 0.0f;
	}
}

// Light k of the loop: an engine light from c39.. (k < c32.y, weight 1) or grid light k - c32.y of the cell (weight c64.z)
void PhotonEngineRecord( const float k, out float4 lPos, out float4 lCol, out float4 lDir, out float4 lAtt )
{
	lPos = g_PhotonLights[0]; lCol = g_PhotonLights[1]; lDir = g_PhotonLights[2]; lAtt = g_PhotonLights[3];
	[flatten] if ( k > 0.5f ) { lPos = g_PhotonLights[4]; lCol = g_PhotonLights[5]; lDir = g_PhotonLights[6]; lAtt = g_PhotonLights[7]; }
	[flatten] if ( k > 1.5f ) { lPos = g_PhotonLights[8]; lCol = g_PhotonLights[9]; lDir = g_PhotonLights[10]; lAtt = g_PhotonLights[11]; }
	[flatten] if ( k > 2.5f ) { lPos = g_PhotonLights[12]; lCol = g_PhotonLights[13]; lDir = g_PhotonLights[14]; lAtt = g_PhotonLights[15]; }
}
// grid light j of the cell whose first texel is at list: index j sits in component (j + 1) mod 4 of texel (j + 1) / 4
void PhotonGridRecord( const float j, const float2 list, out float4 lPos, out float4 lCol, out float4 lDir, out float4 lAtt )
{
	float q = j + 1.0f;
	float qt = floor( q * 0.25f );
	float p = PhotonPick4( tex2Dlod( PhotonGridSampler, float4( list.x + qt * ( 1.0f / PHOTON_LG_W ), list.y, 0.0f, 0.0f ) ), q - 4.0f * qt );
	float a = p * ( 1.0f / 32.0f );
	float fa = frac( a );
	float u = fa + 0.5f / PHOTON_LG_W;
	float v = ( PHOTON_LG_REC + 0.5f + ( a - fa ) ) * ( 1.0f / PHOTON_LG_H );
	lPos = tex2Dlod( PhotonGridSampler, float4( u, v, 0.0f, 0.0f ) );
	lCol = tex2Dlod( PhotonGridSampler, float4( u + 1.0f / PHOTON_LG_W, v, 0.0f, 0.0f ) );
	lDir = tex2Dlod( PhotonGridSampler, float4( u + 2.0f / PHOTON_LG_W, v, 0.0f, 0.0f ) );
	lAtt = tex2Dlod( PhotonGridSampler, float4( u + 3.0f / PHOTON_LG_W, v, 0.0f, 0.0f ) );
}
void PhotonLightRecord( const float k, const float2 list, out float4 lPos, out float4 lCol, out float4 lDir, out float4 lAtt, out float w )
{
	[branch] if ( k < g_PhotonFlags.y )
	{
		PhotonEngineRecord( k, lPos, lCol, lDir, lAtt );
		w = 1.0f;
	}
	else
	{
		PhotonGridRecord( k - g_PhotonFlags.y, list, lPos, lCol, lDir, lAtt );
		w = g_PhotonDraw.z;
	}
}

// Attenuation (distance, spot cone, range window) and the unit vector to the light of one record: the stock VS formula
// (common_vs_fxc.h VertexAttenInternal / CosineTermInternal) per pixel, photon_light_ps.h's math before P5
float PhotonLightAtten( const float4 lPos, const float4 lCol, const float4 lDir, const float4 lAtt, const float3 worldPos, out float3 lightDir )
{
	float3 toLight = lPos.xyz - worldPos;
	float flDist2 = max( dot( toLight, toLight ), 1.0e-6f );
	float ooDist = rsqrt( flDist2 );
	lightDir = toLight * ooDist;

	float flDistAtten = 1.0f / max( dot( lAtt.xyz, float3( 1.0f, flDist2 * ooDist, flDist2 ) ), 1.0e-6f );
	float flSpotAtten = saturate( pow( max( 0.0001f, dot( lDir.xyz, -lightDir ) - lAtt.w ), lDir.w ) );
	float flRange = flDist2 * lPos.w;
	float flWindow = saturate( 1.0f - flRange * flRange );
	float flAtten = ( ( lCol.w > 0.5f ) ? flDistAtten * flSpotAtten : flDistAtten ) * flWindow * flWindow;

	bool bDirectional = lCol.w > 1.5f;
	lightDir = bDirectional ? -lDir.xyz : lightDir;
	return bDirectional ? 1.0f : flAtten;
}

// photon_debug 3 (the proxy sets c64.x = 1 and writes the ambient off): the pixel's grid light count n instead of the lighting: green for
// n >= 1, red for n > 4, blue n / 32
float3 PhotonLightHeat( const float n )
{
	return float3( n > 4.5f ? 1.0f : 0.0f, n > 0.5f ? 1.0f : 0.0f, n * ( 1.0f / 32.0f ) );
}

// The diffuse sum of every light at a unit normal (VLG, bump), before the c55.x scale: N.L or half-Lambert squared (fHalfLambert 0/1)
float3 PhotonDirectSum( const float3 n, const float3 worldPos, const float fHalfLambert )
{
	float nTot;
	float2 list;
	PhotonLightRange( worldPos, nTot, list );
	float3 lit = float3( 0.0f, 0.0f, 0.0f );
	float k = 0.0f;
	// The exit is the loop condition, not an if + break: FXC lowers [branch] if ( k >= nTot ) break; to if_ge + break_ne c, c with both operands the
	// same negated constant in some combos, a break that never fires (found in the emulator, P5)
	[loop] for ( int i = 0; i < PHOTON_LOOP_MAX && k < nTot; i++ )
	{
		float4 lPos, lCol, lDir, lAtt;
		float w;
		PhotonLightRecord( k, list, lPos, lCol, lDir, lAtt, w );
		float3 lightDir;
		float flAtten = PhotonLightAtten( lPos, lCol, lDir, lAtt, worldPos, lightDir );
		float NDotL = dot( n, lightDir );
		float flHalfLambert = NDotL * 0.5f + 0.5f;
		NDotL = lerp( max( 0.0f, NDotL ), flHalfLambert * flHalfLambert, fHalfLambert );
		lit += ( lCol.rgb * w ) * ( NDotL * flAtten );
		k += 1.0f;
	}
	return ( g_PhotonDraw.x > 0.5f ) ? PhotonLightHeat( nTot - g_PhotonFlags.y ) : lit;
}

#endif
