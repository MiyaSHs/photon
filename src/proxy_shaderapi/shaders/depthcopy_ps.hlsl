// L3 model mask (analysis/37): the scene depth (INTZ, s0) copied as it is to an R32F target, taken just before the first model draw of
// the main view, so it holds the world alone. The L3 pass leaves pixels nearer than it (models, static props) to Photon's model lighting.
sampler2D sceneDepth : register(s0);
float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(sceneDepth, uv).rrrr; }
