// Added in OPM: SMAA 1x (tr_smaa.c, r_smaa), the vertex side of its three
// passes. SMAA_PASS 0 finds the edges, 1 weighs how each edge pixel blends,
// 2 blends. The SMAA library (smaa_lib.glsl) is put in at the marker.

uniform vec4 u_BloomTexel; // SMAA_RT_METRICS: 1/width, 1/height, width, height

#define SMAA_RT_METRICS u_BloomTexel
#define SMAA_GLSL_3
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0

//SMAA_LIB_HERE

attribute vec4 attr_Position;
attribute vec4 attr_TexCoord0;

varying vec2 var_Tex;
#if SMAA_PASS == 1
varying vec2 var_Pix;
#endif
#if SMAA_PASS == 2
varying vec4 var_Offset0;
#else
varying vec4 var_Offset[3];
#endif

void main()
{
	gl_Position = attr_Position;
	var_Tex = attr_TexCoord0.xy;

#if SMAA_PASS == 0
	SMAAEdgeDetectionVS(var_Tex, var_Offset);
#elif SMAA_PASS == 1
	SMAABlendingWeightCalculationVS(var_Tex, var_Pix, var_Offset);
#else
	SMAANeighborhoodBlendingVS(var_Tex, var_Offset0);
#endif
}
