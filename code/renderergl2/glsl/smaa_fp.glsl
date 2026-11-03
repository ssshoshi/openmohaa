// Added in OPM: SMAA 1x (tr_smaa.c, r_smaa), the pixel side of its three
// passes. The SMAA library (smaa_lib.glsl) is put in at the marker.
//
// The edges are found on the tone mapped frame's luma (pass 0), each edge
// pixel is weighed against the area texture by the shape of the edge it is on
// (pass 1), and every pixel is blended with the neighbour its weights name
// (pass 2). OpenGL's frame and textures are the other way up from Direct3D's,
// but all three passes and the two lookup textures are, together, so the
// algorithm runs on the image mirrored and gives the mirrored result.

uniform vec4 u_BloomTexel; // SMAA_RT_METRICS

#define SMAA_RT_METRICS u_BloomTexel
#define SMAA_GLSL_3
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1

//SMAA_LIB_HERE

uniform sampler2D u_ScreenImageMap; // pass 0 and 2: the frame; pass 1: the edges
uniform sampler2D u_LightMap;       // pass 1: the area texture; pass 2: the blend weights
uniform sampler2D u_NormalMap;      // pass 1: the search texture

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
#if SMAA_PASS == 0
	gl_FragColor = vec4(SMAALumaEdgeDetectionPS(var_Tex, var_Offset, u_ScreenImageMap), 0.0, 0.0);
#elif SMAA_PASS == 1
	gl_FragColor = SMAABlendingWeightCalculationPS(var_Tex, var_Pix, var_Offset, u_ScreenImageMap, u_LightMap, u_NormalMap, vec4(0.0));
#else
	gl_FragColor = SMAANeighborhoodBlendingPS(var_Tex, var_Offset0, u_ScreenImageMap, u_LightMap);
#endif
}
