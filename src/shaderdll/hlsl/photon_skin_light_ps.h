// photon_skin_light_ps.h: Photon per-pixel lights for the substituted skin ps_3_0 (lighting plan v2 P2, the
// phong path: VertexLitGeneric $phong -> DrawSkin_DX9; P5's light grid). Included only from inside //<PHOTON> regions of
// photon_skin_ps2x.fxc, under #if PHOTON_ENABLED, after photon_light_ps.h (the c32..c55 block, photon_lights_ps.h).
//
// Stock skin already lights per pixel from <= 4 lights (PixelShaderDoLighting + PixelShaderDoSpecularLighting,
// common_vertexlitgeneric_dx9.h), but each light's attenuation (distance, spot cone) is the VS value interpolated
// across the triangle (TEXCOORD1.xyz + TEXCOORD6.w) and its position comes from the PS cLightInfo c20..c25. With
// c32.x > 0.5 the Photon path takes the lights from the c39.. block (and, P5, the pixel's light-grid cell) instead and
// evaluates their attenuation per pixel (photon_lights_ps.h math, the stock VS formula plus the range window), and runs
// skin's OWN per-light terms on them (plan v2 rev C4: a skin light loop that dropped per-light specular would be a
// visible look change):
//   diffuse  DiffuseTerm (GMod's switch: half-Lambert squared only with PHONG_HALFLAMBERT=1, else saturate(N.L);
//            $lightwarptexture 2 * warp(f), half-Lambert not squared, as the SDK), + the c33..c38 ambient cube;
//   specular SpecularAndRimTerms: pow(saturate(R.L), exponent), optionally * $phongwarptexture(spec, fresnel),
//            * saturate(N.L) * light; rim pow(saturate(R.L), $rimlightexponent) * saturate(N.L) * light.
// Everything after the light loop (spec mask, boost, tint, Fresnel, rim fold-in, envmap, selfillum) is stock code.
// Differences from stock with the flag on, by design: per-pixel attenuation and spot cone (the point of Photon); the
// range window; the warp textures are read with an explicit LOD 0 (tex2Dlod: plan v2 rev C3 forbids implicit-gradient
// fetches inside the dynamic branch; stock lets the hardware pick the mip from the warp coordinate's gradient); the
// N.L masks are always saturated (GMod's old fxc dropped that saturate in some ps_2_b combos, GMOD_FXC_UNSAT_NDOTL,
// which the flag-off path reproduces); c55.x scales the light colours (diffuse, specular and rim alike).
#ifndef PHOTON_SKIN_LIGHT_PS_H
#define PHOTON_SKIN_LIGHT_PS_H

#if !defined( PHOTON_SKIN_NEG_NOSPEC )
#define PHOTON_SKIN_NEG_NOSPEC 0	// negative-control build only (drops the per-light specular); the shipped value is 0
#endif

// One light's diffuse, specular and rim terms (skin_ps20b.fxc PixelShaderDoLighting / PixelShaderDoSpecularLighting per light): L the unit
// vector to the light, color its colour * attenuation * c55.x
void PhotonSkinLightTerms( const float3 n, const float3 vReflect, const float3 L, const float3 color, const float fSpecularExponent,
						   const bool bHalfLambert, const bool bDoLightingWarp, in sampler lightWarpSampler,
						   const bool bDoSpecularWarp, in sampler specularWarpSampler, const float fFresnel,
						   const bool bDoRimLighting, const float fRimExponent,
						   inout float3 diffuseLighting, inout float3 specularLighting, inout float3 rimLighting )
{
	float NDotL = dot( n, L );

	// DiffuseTerm (common_vertexlitgeneric_dx9.h) with GMod's PHONG_HALFLAMBERT switch
	float fResult = bHalfLambert ? saturate( NDotL * 0.5f + 0.5f ) : saturate( NDotL );
	if ( bHalfLambert && !bDoLightingWarp )
		fResult *= fResult;
	float3 fDiffuse = float3( fResult, fResult, fResult );
	if ( bDoLightingWarp )
		fDiffuse = 2.0f * tex2Dlod( lightWarpSampler, float4( fResult, fResult, 0.0f, 0.0f ) ).rgb;
	diffuseLighting += color * fDiffuse;

	// SpecularAndRimTerms
	float LdotR = saturate( dot( vReflect, L ) );
	float3 spec = pow( LdotR, fSpecularExponent );
	if ( bDoSpecularWarp )
		spec *= tex2Dlod( specularWarpSampler, float4( spec.x, fFresnel, 0.0f, 0.0f ) ).rgb;
	float fMask = saturate( NDotL );
#if !PHOTON_SKIN_NEG_NOSPEC
	specularLighting += spec * fMask * color;
#endif
	if ( bDoRimLighting )
		rimLighting += pow( LdotR, fRimExponent ) * fMask * color;
}

// skin's diffuse, specular and rim sums over every light, plus the ambient cube in the diffuse; the arguments are the stock call sites'
// (skin_ps20b.fxc PixelShaderDoLighting / PixelShaderDoSpecularLighting), the stock results as they come in. The loop is photon_lights_ps.h's:
// the c32.y engine lights of the block, then the pixel's grid cell.
void PhotonSkinLighting( const float3 worldPos, const float3 n, const float3 vEyeDir, const float fSpecularExponent,
						 const bool bHalfLambert, const bool bDoLightingWarp, in sampler lightWarpSampler,
						 const bool bDoSpecularWarp, in sampler specularWarpSampler, const float fFresnel,
						 const bool bDoRimLighting, const float fRimExponent,
						 inout float3 diffuseLighting, inout float3 specularLighting, inout float3 rimLighting )
{
	// The stock results come in and are dropped by a select on c64.w, which the proxy always writes 0 (P5): otherwise FXC sinks the stock
	// lighting into the else of the Photon branch, which keeps its inputs (15 registers in the heaviest combos) live through the loop and
	// runs out of ps_3_0's 32 temps there (X4505).
	bool bKeep = g_PhotonDraw.w > 0.0f;
	diffuseLighting = bKeep ? diffuseLighting : PhotonAmbientPS( n, worldPos );
	specularLighting = bKeep ? specularLighting : float3( 0.0f, 0.0f, 0.0f );
	rimLighting = bKeep ? rimLighting : float3( 0.0f, 0.0f, 0.0f );
	float3 vReflect = 2 * n * dot( n, vEyeDir ) - vEyeDir;			// as SpecularAndRimTerms
	float nTot;
	float2 list;
	PhotonLightRange( worldPos, nTot, list );
	float kf = 0.0f;
	[loop] for ( int k = 0; k < PHOTON_LOOP_MAX && kf < nTot; k++ )	// the exit in the condition, as photon_lights_ps.h explains
	{
		float4 lPos, lCol, lDir, lAtt;
		float w;
		PhotonLightRecord( kf, list, lPos, lCol, lDir, lAtt, w );
		float3 L;
		float flAtten = PhotonLightAtten( lPos, lCol, lDir, lAtt, worldPos, L );
		PhotonSkinLightTerms( n, vReflect, L, ( lCol.rgb * w ) * ( flAtten * g_PhotonTuning.x ), fSpecularExponent, bHalfLambert, bDoLightingWarp,
							  lightWarpSampler, bDoSpecularWarp, specularWarpSampler, fFresnel, bDoRimLighting, fRimExponent,
							  diffuseLighting, specularLighting, rimLighting );
		kf += 1.0f;
	}
	[flatten] if ( g_PhotonDraw.x > 0.5f )	// photon_debug 3: the light count (photon_lights_ps.h)
	{
		diffuseLighting = PhotonLightHeat( nTot - g_PhotonFlags.y );
		specularLighting = rimLighting = float3( 0.0f, 0.0f, 0.0f );
	}
}

#endif
