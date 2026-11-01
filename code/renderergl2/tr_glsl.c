/*
===========================================================================
Copyright (C) 2006-2009 Robert Beckebans <trebor_7@users.sourceforge.net>

This file is part of XreaL source code.

XreaL source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

XreaL source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XreaL source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_glsl.c
#include "tr_local.h"

#include "tr_dsa.h"

extern const char *fallbackShader_bokeh_vp;
extern const char *fallbackShader_bokeh_fp;
extern const char *fallbackShader_calclevels4x_vp;
extern const char *fallbackShader_calclevels4x_fp;
extern const char *fallbackShader_depthblur_vp;
extern const char *fallbackShader_depthblur_fp;
extern const char *fallbackShader_dlight_vp;
extern const char *fallbackShader_dlight_fp;
extern const char *fallbackShader_down4x_vp;
extern const char *fallbackShader_down4x_fp;
extern const char *fallbackShader_fogpass_vp;
extern const char *fallbackShader_fogpass_fp;
extern const char *fallbackShader_generic_vp;
extern const char *fallbackShader_generic_fp;
extern const char *fallbackShader_lightall_vp;
extern const char *fallbackShader_lightall_fp;
extern const char *fallbackShader_pshadow_vp;
extern const char *fallbackShader_pshadow_fp;
extern const char *fallbackShader_shadowfill_vp;
extern const char *fallbackShader_shadowfill_fp;
extern const char *fallbackShader_shadowmask_vp;
extern const char *fallbackShader_shadowmask_fp;
extern const char *fallbackShader_gtao_vp;
extern const char *fallbackShader_gtao_fp;
extern const char *fallbackShader_volumetric_vp;
extern const char *fallbackShader_volumetric_fp;
extern const char *fallbackShader_froxel_vp;
extern const char *fallbackShader_froxel_fp;
extern const char *fallbackShader_ssao_vp;
extern const char *fallbackShader_ssao_fp;
extern const char *fallbackShader_texturecolor_vp;
extern const char *fallbackShader_texturecolor_fp;
extern const char *fallbackShader_tonemap_vp;
extern const char *fallbackShader_tonemap_fp;
extern const char *fallbackShader_water_vp;
extern const char *fallbackShader_beach_vp;
extern const char *fallbackShader_ripple_vp;
extern const char *fallbackShader_lens_vp;
extern const char *fallbackShader_lens_fp;
extern const char *fallbackShader_bloom_vp;
extern const char *fallbackShader_bloom_fp;
extern const char *fallbackShader_ripple_fp;
extern const char *fallbackShader_beach_fp;
extern const char *fallbackShader_water_fp;
extern const char* fallbackShader_greyscale_vp;
extern const char* fallbackShader_greyscale_fp;

typedef struct uniformInfo_s
{
	char *name;
	int type;
}
uniformInfo_t;

// These must be in the same order as in uniform_t in tr_local.h.
static uniformInfo_t uniformsInfo[] =
{
	{ "u_DiffuseMap",  GLSL_INT },
	{ "u_LightMap",    GLSL_INT },
	{ "u_NormalMap",   GLSL_INT },
	{ "u_DeluxeMap",   GLSL_INT },
	{ "u_SpecularMap", GLSL_INT },

	{ "u_TextureMap", GLSL_INT },
	{ "u_LevelsMap",  GLSL_INT },
	{ "u_CubeMap",    GLSL_INT },

	{ "u_ScreenImageMap", GLSL_INT },
	{ "u_ScreenDepthMap", GLSL_INT },

	{ "u_ShadowMap",  GLSL_INT },
	{ "u_ShadowMap2", GLSL_INT },
	{ "u_ShadowMap3", GLSL_INT },
	{ "u_ShadowMap4", GLSL_INT },

	{ "u_ShadowMvp",  GLSL_MAT16 },
	{ "u_ShadowMvp2", GLSL_MAT16 },
	{ "u_ShadowMvp3", GLSL_MAT16 },
	{ "u_ShadowMvp4", GLSL_MAT16 },

	{ "u_EnableTextures", GLSL_VEC4 },

	{ "u_DiffuseTexMatrix0",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix1",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix2",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix3",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix4",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix5",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix6",  GLSL_VEC4 },
	{ "u_DiffuseTexMatrix7",  GLSL_VEC4 },

	{ "u_TCGen0",        GLSL_INT },
	{ "u_TCGen0Vector0", GLSL_VEC3 },
	{ "u_TCGen0Vector1", GLSL_VEC3 },

	{ "u_DeformGen",    GLSL_INT },
	{ "u_DeformParams", GLSL_FLOAT5 },

	{ "u_ColorGen",  GLSL_INT },
	{ "u_AlphaGen",  GLSL_INT },
	{ "u_Color",     GLSL_VEC4 },
	{ "u_BaseColor", GLSL_VEC4 },
	{ "u_VertColor", GLSL_VEC4 },

	{ "u_DlightInfo",    GLSL_VEC4 },
	{ "u_LightForward",  GLSL_VEC3 },
	{ "u_LightUp",       GLSL_VEC3 },
	{ "u_LightRight",    GLSL_VEC3 },
	{ "u_LightOrigin",   GLSL_VEC4 },
	{ "u_ModelLightDir", GLSL_VEC3 },
	{ "u_LightRadius",   GLSL_FLOAT },
	{ "u_AmbientLight",  GLSL_VEC3 },
	{ "u_DirectedLight", GLSL_VEC3 },

	{ "u_PortalRange", GLSL_FLOAT },

	{ "u_FogDistance",  GLSL_VEC4 },
	{ "u_FogDepth",     GLSL_VEC4 },
	{ "u_FogEyeT",      GLSL_FLOAT },
	{ "u_FogColorMask", GLSL_VEC4 },

	{ "u_GlobalFogColor",  GLSL_VEC4 },
	{ "u_GlobalFogParams", GLSL_VEC2 },

	{ "u_AlphaGenParams",  GLSL_VEC4 },

	{ "u_Texture1Env",     GLSL_INT },
	{ "u_TCGen1",          GLSL_INT },
	{ "u_Texture1Matrix0", GLSL_VEC4 },
	{ "u_Texture1Matrix1", GLSL_VEC4 },
	{ "u_Texture1Matrix2", GLSL_VEC4 },
	{ "u_Texture1Matrix3", GLSL_VEC4 },
	{ "u_Texture1Matrix4", GLSL_VEC4 },
	{ "u_Texture1Matrix5", GLSL_VEC4 },
	{ "u_Texture1Matrix6", GLSL_VEC4 },
	{ "u_Texture1Matrix7", GLSL_VEC4 },

	{ "u_ModelMatrix",               GLSL_MAT16 },
	{ "u_ModelViewProjectionMatrix", GLSL_MAT16 },

	{ "u_Time",          GLSL_FLOAT },
	{ "u_VertexLerp" ,   GLSL_FLOAT },
	{ "u_NormalScale",   GLSL_VEC4 },
	{ "u_SpecularScale", GLSL_VEC4 },

	{ "u_ViewInfo",        GLSL_VEC4 },
	{ "u_ViewOrigin",      GLSL_VEC3 },
	{ "u_LocalViewOrigin", GLSL_VEC3 },
	{ "u_ViewForward",     GLSL_VEC3 },
	{ "u_ViewLeft",        GLSL_VEC3 },
	{ "u_ViewUp",          GLSL_VEC3 },

	{ "u_InvTexRes",           GLSL_VEC2 },
	{ "u_AutoExposureMinMax",  GLSL_VEC2 },
	{ "u_ToneMinAvgMaxLinear", GLSL_VEC3 },

	{ "u_PrimaryLightOrigin",  GLSL_VEC4  },
	{ "u_PrimaryLightColor",   GLSL_VEC3  },
	{ "u_PrimaryLightAmbient", GLSL_VEC3  },
	{ "u_PrimaryLightRadius",  GLSL_FLOAT },

	{ "u_CubeMapInfo", GLSL_VEC4 },

	{ "u_AlphaTest", GLSL_INT },

	{ "u_BoneMatrix", GLSL_MAT16_BONEMATRIX },
	{ "u_Greyscale", GLSL_FLOAT },

	// Added in OPM: the realtime lights (tr_rtlight.c)
	{ "u_RtLights",        GLSL_VEC4_RTLIGHTS },
	{ "u_RtParams",        GLSL_VEC4 },
	{ "u_Linear",          GLSL_FLOAT },
	{ "u_RtSunDir",        GLSL_VEC4 },
	{ "u_RtSunColor",      GLSL_VEC4 },
	{ "u_RtAmbient",       GLSL_VEC4 },
	{ "u_RtStage",         GLSL_INT },
	{ "u_RtShadowStatic",  GLSL_INT },
	{ "u_RtShadowDynamic", GLSL_INT },
	{ "u_RtSunShadow",     GLSL_INT },
	{ "u_RtSunCascade",    GLSL_INT },
	{ "u_RtSunCascadeMvp", GLSL_MAT16 },
	{ "u_RtShadowBaked",   GLSL_INT },
	{ "u_RtCapsules",      GLSL_INT },
	{ "u_RtList",          GLSL_VEC4_RTLIST },
	{ "u_RtSky",           GLSL_VEC4_RTSKY },
	{ "u_RtSkyColor",      GLSL_VEC4 },
	{ "u_RtSkyMap",        GLSL_INT },
	{ "u_RtSurface",       GLSL_VEC4 },
	{ "u_RtSkyLightmap",   GLSL_INT },
	{ "u_RtBounce",        GLSL_VEC4 },
	{ "u_RtBounceMap",     GLSL_INT },
	{ "u_RtAO",            GLSL_VEC4 },
	{ "u_RtAOMap",         GLSL_INT },
	{ "u_AOParams",        GLSL_VEC4 },
	{ "u_VolParams",       GLSL_VEC4 },
	{ "u_VolRange",        GLSL_VEC4 },
	{ "u_Froxel",          GLSL_VEC4 },
	{ "u_FogVolumes",      GLSL_VEC4_WATERSPOTS },
	{ "u_FogParams",       GLSL_VEC4 },
	{ "u_FogAmbient",      GLSL_VEC4 },
	{ "u_FroxelPrevOrigin",  GLSL_VEC4 },
	{ "u_FroxelPrevForward", GLSL_VEC4 },
	{ "u_FroxelPrevLeft",    GLSL_VEC4 },
	{ "u_FroxelPrevUp",      GLSL_VEC4 },
	{ "u_FroxelJitter",    GLSL_VEC4 },
	{ "u_SmokeBlobs",      GLSL_VEC4_SMOKE },
	{ "u_SmokeParams",     GLSL_VEC4 },
	{ "u_SmokeNoise",      GLSL_INT },
	{ "u_FroxelFog",       GLSL_VEC4 },
	{ "u_FroxelFogView",   GLSL_VEC4 },
	{ "u_FroxelFogGrid",   GLSL_VEC4 },
	{ "u_FroxelFogMap",    GLSL_INT },
	{ "u_RtBounceLampMap", GLSL_INT },
	{ "u_RtDirect",        GLSL_VEC4 },
	{ "u_RtDirectMap",     GLSL_INT },
	{ "u_RtDirectDir",     GLSL_INT },
	{ "u_RtLiveMap",       GLSL_INT },
	{ "u_RtLiveDir",       GLSL_INT },
	{ "u_RtProbeR",        GLSL_VEC4 },
	{ "u_RtProbeG",        GLSL_VEC4 },
	{ "u_RtProbeB",        GLSL_VEC4 },
	{ "u_SoftParticle",    GLSL_VEC4 },
	{ "u_GroundCover",     GLSL_VEC4 },
	{ "u_SkelBones",       GLSL_INT },
	{ "u_SkelParams",      GLSL_VEC4 },
	{ "u_SkelAmbient",     GLSL_VEC4 },
	{ "u_SkelLights",      GLSL_VEC4_SKELLIGHTS },
	{ "u_SkelNumDents",    GLSL_INT },
	{ "u_SkelDents",       GLSL_VEC4_SKELDENTS },
	{ "u_SkelMorphMap",    GLSL_INT },
	{ "u_SkelNumMorphs",   GLSL_INT },
	{ "u_SkelMorphs",      GLSL_VEC4_SKELMORPHS },
	// Added in OPM: the water program (tr_water.c)
	{ "u_WaterColor",      GLSL_VEC4 },
	{ "u_WaterBlood",      GLSL_VEC4 },
	{ "u_WaterWaves",      GLSL_VEC4 },
	{ "u_WaterShore",      GLSL_VEC4 },
	{ "u_WaterSky",        GLSL_VEC4 },
	{ "u_WaterLook",       GLSL_VEC4 },
	{ "u_WaterOcean",      GLSL_VEC4 },
	{ "u_WaterBloodZone",  GLSL_VEC4 },
	{ "u_WaterSpots",      GLSL_VEC4_WATERSPOTS },
	{ "u_BeachWet",        GLSL_VEC4 },
	{ "u_SandStains",      GLSL_VEC4_WATERSPOTS },
	{ "u_WorldViewProjection", GLSL_MAT16 },
	{ "u_WaterView",       GLSL_VEC4 },
	{ "u_BeachSwash",      GLSL_VEC4 },
	{ "u_BeachGround",     GLSL_VEC4 },
	{ "u_RippleSim",       GLSL_VEC4 },
	{ "u_RippleDecay",     GLSL_VEC4 },
	{ "u_RippleSources",   GLSL_VEC4_WATERSPOTS },
	{ "u_RippleFoam",      GLSL_VEC4_WATERSPOTS },
	{ "u_WaterRipples",    GLSL_VEC4 },
	{ "u_WaterReflect",    GLSL_VEC4 },
	{ "u_WaterReflectPlane", GLSL_VEC4 },
	{ "u_WaterReflectMvp", GLSL_MAT16 },
	{ "u_WaterReflectMap", GLSL_INT },
	{ "u_LensInfo",        GLSL_VEC4 },
	{ "u_LensSpots",       GLSL_VEC4_WATERSPOTS },
	{ "u_LensLook",        GLSL_VEC4_WATERSPOTS },
	{ "u_BloomParams",     GLSL_VEC4 },
	{ "u_BloomTexel",      GLSL_VEC4 },
	{ "u_Wet",             GLSL_VEC4 },
	{ "u_WetSky",          GLSL_VEC4 },
	{ "u_SsrColorMap",     GLSL_INT },
	{ "u_SsrDepthMap",     GLSL_INT },
	{ "u_SsrViewProj",     GLSL_MAT16 },
	{ "u_SsrPrevViewProj", GLSL_MAT16 },
	{ "u_SsrParams",       GLSL_VEC4 },
	{ "u_WetSun",          GLSL_VEC4 },
	{ "u_RainCover",       GLSL_VEC4 },
	{ "u_RainParams",      GLSL_VEC4 },
	{ "u_RainCoverMap",    GLSL_INT },
	{ "u_RainReflect",     GLSL_VEC4 },
	{ "u_RainReflectMvp",  GLSL_MAT16 },
	{ "u_RainReflectMap",  GLSL_INT },
	{ "u_RainFlash",       GLSL_VEC4 }
};

typedef enum
{
	GLSL_PRINTLOG_PROGRAM_INFO,
	GLSL_PRINTLOG_SHADER_INFO,
	GLSL_PRINTLOG_SHADER_SOURCE
}
glslPrintLog_t;

static void GLSL_PrintLog(GLuint programOrShader, glslPrintLog_t type, qboolean developerOnly)
{
	char           *msg;
	static char     msgPart[1024];
	int             maxLength = 0;
	int             i;
	int             printLevel = developerOnly ? PRINT_DEVELOPER : PRINT_ALL;

	switch (type)
	{
		case GLSL_PRINTLOG_PROGRAM_INFO:
			ri.Printf(printLevel, "Program info log:\n");
			qglGetProgramiv(programOrShader, GL_INFO_LOG_LENGTH, &maxLength);
			break;

		case GLSL_PRINTLOG_SHADER_INFO:
			ri.Printf(printLevel, "Shader info log:\n");
			qglGetShaderiv(programOrShader, GL_INFO_LOG_LENGTH, &maxLength);
			break;

		case GLSL_PRINTLOG_SHADER_SOURCE:
			ri.Printf(printLevel, "Shader source:\n");
			qglGetShaderiv(programOrShader, GL_SHADER_SOURCE_LENGTH, &maxLength);
			break;
	}

	if (maxLength <= 0)
	{
		ri.Printf(printLevel, "None.\n");
		return;
	}

	if (maxLength < 1023)
		msg = msgPart;
	else
		msg = ri.Malloc(maxLength);

	switch (type)
	{
		case GLSL_PRINTLOG_PROGRAM_INFO:
			qglGetProgramInfoLog(programOrShader, maxLength, &maxLength, msg);
			break;

		case GLSL_PRINTLOG_SHADER_INFO:
			qglGetShaderInfoLog(programOrShader, maxLength, &maxLength, msg);
			break;

		case GLSL_PRINTLOG_SHADER_SOURCE:
			qglGetShaderSource(programOrShader, maxLength, &maxLength, msg);
			break;
	}

	if (maxLength < 1023)
	{
		msgPart[maxLength + 1] = '\0';

		ri.Printf(printLevel, "%s\n", msgPart);
	}
	else
	{
		for(i = 0; i < maxLength; i += 1023)
		{
			Q_strncpyz(msgPart, msg + i, sizeof(msgPart));

			ri.Printf(printLevel, "%s", msgPart);
		}

		ri.Printf(printLevel, "\n");

		ri.Free(msg);
	}

}

/*
Added in OPM
The realtime lights (tr_rtlight.c), for the fragment shaders of the programs
built with USE_RTLIGHT. RtLight(P, N) is the light a surface at P facing N
gets from them, in lightmap units: the map's point and spot lights with their
shadows from the atlases, and the sun with its screen shadow.

The generic program's vertex shader has them too (USE_RTLIGHT_PARTICLES,
RT_VERTEX), for RtLightParticle: dust and smoke are big translucent quads piled
deep, which the whole light loop for every pixel of every layer brought to a
crawl, so they are lit at their corners instead.

A point light's shadow is six faces, tiles in a row in an atlas: forward, left
and up as R_RtFaceAxes sets them, a 90 degree view each.
*/
static const char *rt_glsl =
	"#define RT_STAGE_NONE      0\n"
	"#define RT_STAGE_LIGHTING  1\n"
	"#define RT_STAGE_LIGHTING2 2\n"
	"#define RT_STAGE_LIT       3\n"
	"#define RT_STAGE_LIT_BAKED 4\n"
	"#define RT_STAGE_PARTICLE  5\n"
	"#define RT_STAGE_PARTICLE_LIT 6\n"
	"uniform vec4 u_RtLights[RT_MAX_LIGHTS * 4];\n"
	// the lights this draw may take, four indexes a vec4; u_RtParams.x of them
	"uniform vec4 u_RtList[RT_MAX_LIGHTS / 4];\n"
	"uniform vec4 u_RtParams;\n"        // x lights, y mode, z shadows, w darkening floor
	"uniform vec4 u_RtSunDir;\n"        // xyz towards the sun, w 1 with a sun
	"uniform vec4 u_RtSunColor;\n"      // w 1 with its screen shadow
	"uniform vec4 u_RtAmbient;\n"
	"uniform int  u_RtStage;\n"
	"uniform sampler2DShadow u_RtShadowStatic;\n"
	"uniform sampler2DShadow u_RtShadowDynamic;\n"
	"uniform sampler2D u_RtSunShadow;\n"
	// the sun's widest cascade, for particles: where they are, not behind
	// them on the screen (a w of 0: none)
	"uniform sampler2DShadow u_RtSunCascade;\n"
	"uniform mat4 u_RtSunCascadeMvp;\n"
	"uniform sampler2DShadow u_RtShadowBaked;\n"
	// the sky (R_RtUpdateSky): each direction's projection, three rows, then
	// the direction; its light, w the most world a texel covers (0: none)
	"uniform vec4 u_RtSky[RT_SKY_DIRS * 4];\n"
	"uniform vec4 u_RtSkyColor;\n"
	"uniform sampler2DShadow u_RtSkyMap;\n"
	// the draw: x for leaves, two sided, how much of the light on one side
	// comes through the other; y 1 when its vertexes worked out the sky's
	// light (var_RtSky)
	"uniform vec4 u_RtSurface;\n"
	// u_RtSurface.y 2: the sky each texel of the draw's lightmap sees, at rt_skyLM
	"uniform sampler2D u_RtSkyLightmap;\n"
	// the light bounced onto each texel of the lightmap (R_RtBounce), at rt_skyLM; its scale, w 1 on
	"uniform vec4 u_RtBounce;\n"
	"uniform sampler2D u_RtBounceMap;\n"
	// r_gtao: x on, the view's ambient occlusion in u_RtAOMap (RB_RenderDrawSurfList)
	"uniform sampler2D u_RtAOMap;\n"
	"uniform vec4      u_RtAO;\n"
	// the lamps' light bounced, apart: as it would be, under a roof or not (z its scale)
	"uniform sampler2D u_RtBounceLampMap;\n"
	// on what has no lightmap, from the probes round it: for each colour m + v . N (xyz v, w m)
	// the baked lamps' light on the draw's lightmap (R_RtBakeLamps): rgb its scale,
	// w how far it is eased in (0: none, the lamps all live)
	"uniform vec4 u_RtDirect;\n"
	"uniform sampler2D u_RtDirectMap;\n"
	// and the way it comes from, weighted by how bright (R_RtBakeLamps)
	"uniform sampler2D u_RtDirectDir;\n"
	// the baked light of the lamps lit live, which they give all of (R_RtLiveIn)
	"uniform sampler2D u_RtLiveMap;\n"
	"uniform sampler2D u_RtLiveDir;\n"
	"uniform vec4 u_RtProbeR;\n"
	"uniform vec4 u_RtProbeG;\n"
	"uniform vec4 u_RtProbeB;\n"
	"vec2 rt_skyLM = vec2(0.0);\n"
	// the surface's own normal, where N is its normal map's (lightall): the
	// baked lamps' light was worked out for it (RtLight)
	"vec3 rt_geomN = vec3(0.0);\n"
	"#if defined(USE_RTSKY_VERTEX)\n"
	"varying vec3 var_RtSky;\n"
	"#endif\n"
	"#if defined(RT_CAPSULES)\n"
	"uniform sampler2D u_RtCapsules;\n"
	"#endif\n"
	"\n"
	"int RtListed(int k)\n"
	"{\n"
	"	vec4 q = u_RtList[k / 4];\n"
	"	int  c = k - (k / 4) * 4;\n"
	"	return int(c == 0 ? q.x : c == 1 ? q.y : c == 2 ? q.z : q.w);\n"
	"}\n"
	"\n"
	"float RtShadowAtlas(sampler2DShadow atlas, float tile, vec2 faceUV, float depth, float tileTexels)\n"
	"{\n"
	// a light further off: quarter tiles, two a side of a tile, in the
	// rows from RT_SMALL_ROW (R_RtFaceRect)
	"	float tiles  = RT_TILES;\n"
	"	float texels = tileTexels;\n"
	"	if (tile >= RT_SMALL_CODE)\n"
	"	{\n"
	"		tiles  = 2.0 * RT_TILES;\n"
	"		texels = 0.5 * tileTexels;\n"
	"		tile   = tile - RT_SMALL_CODE + 2.0 * RT_SMALL_ROW * tiles;\n"
	"	}\n"
	"	vec2  origin = vec2(mod(tile, tiles), floor(tile / tiles));\n"
	// the four taps (each four texels, filtered) half a texel apart, or as
	// many halves as r_rtShadowSoft asks (u_RtSurface.w): softer, as many taps.
	// The face was drawn guard texels wider than its 90 degrees each side
	// (u_RtSurface.w / 16, r_rtShadowGuard; R_RtFaceScale), so its 90 degrees
	// are the middle of the tile and the taps near an edge read the guard,
	// what the next face sees there; clamped inside the tile, they read the
	// same texels over and over and the shadow smeared along every edge.
	"	float guard  = floor(u_RtSurface.w / 16.0);\n"
	"	float spread = max(u_RtSurface.w - 16.0 * guard, 1.0);\n"
	"	float texel  = 1.0 / texels;\n"
	"	vec2  st     = (faceUV - 0.5) * (1.0 - 2.0 * guard * texel) + 0.5;\n"
	"	st           = clamp(st, vec2((1.0 + 0.5 * spread) * texel), vec2(1.0 - (1.0 + 0.5 * spread) * texel));\n"
	"	vec2  uv     = (origin + st) / tiles;\n"
	"	float d      = 0.5 * spread / (RT_TILES * tileTexels);\n"
	"	return 0.25 * (shadow2D(atlas, vec3(uv + vec2(-d, -d), depth))\n"
	"	             + shadow2D(atlas, vec3(uv + vec2( d, -d), depth))\n"
	"	             + shadow2D(atlas, vec3(uv + vec2(-d,  d), depth))\n"
	"	             + shadow2D(atlas, vec3(uv + vec2( d,  d), depth)));\n"
	"}\n"
	"\n"
	// info: x the static tiles, y the dynamic ones (< 0: none), z near, w far;
	// v: from the light to the point
	"float RtShadow(vec4 info, vec3 v, bool baked)\n"
	"{\n"
	"	vec3  a = abs(v);\n"
	"	float face, fwd, left, up;\n"
	"	if (a.x >= a.y && a.x >= a.z) {\n"
	"		if (v.x > 0.0) { face = 0.0; fwd =  v.x; left =  v.y; up = v.z; }\n"
	"		else           { face = 1.0; fwd = -v.x; left = -v.y; up = v.z; }\n"
	"	} else if (a.y >= a.z) {\n"
	"		if (v.y > 0.0) { face = 2.0; fwd =  v.y; left = -v.x; up = v.z; }\n"
	"		else           { face = 3.0; fwd = -v.y; left =  v.x; up = v.z; }\n"
	"	} else {\n"
	"		if (v.z > 0.0) { face = 4.0; fwd =  v.z; left = -v.y; up = v.x; }\n"
	"		else           { face = 5.0; fwd = -v.z; left =  v.y; up = v.x; }\n"
	"	}\n"
	"	vec2  faceUV = vec2(-left, up) / fwd * 0.5 + 0.5;\n"
	"	float n = RT_ZNEAR, f = info.w;\n"
	"	float depth = ((f + n) / (f - n) - 2.0 * f * n / ((f - n) * fwd)) * 0.5 + 0.5;\n"
	"	float vis = 1.0;\n"
	// the world as the map was compiled, in the same tiles as what stands still
	"	if (baked)\n"
	"		return info.x >= 0.0 ? RtShadowAtlas(u_RtShadowBaked, info.x + face, faceUV, depth, RT_BAKED_TEXELS) : 1.0;\n"
	"	if (info.x >= 0.0) vis *= RtShadowAtlas(u_RtShadowStatic, info.x + face, faceUV, depth, RT_TILE_TEXELS);\n"
	// the moving things' tiles, with the faces drawn in the fraction, a bit each
	"	if (info.y >= 0.0)\n"
	"	{\n"
	"		float mask = floor(fract(info.y) * 64.0 + 0.5);\n"
	"		if (mod(floor(mask / exp2(face)), 2.0) > 0.5)\n"
	"			vis *= RtShadowAtlas(u_RtShadowDynamic, floor(info.y) + face, faceUV, depth, RT_TILE_TEXELS);\n"
	"	}\n"
	"	return vis;\n"
	"}\n"
	"\n"
	// delta: the change from the map's own lighting, what the lights bring now
	// less what they brought where the map was compiled (r_realtimeLighting 1)
	"#if !defined(RT_VERTEX)\n"
	"vec3 RtLight(vec3 P, vec3 N, bool delta);\n"
	"#endif\n"
	// the bodies between P and the light at Lp: info is where the light's run
	// of them starts, with how many entries in the fraction (R_RtLightCapsules),
	// each body a sphere around it, then how many capsules, then those
	"float RtCapsules(vec3 P, vec3 Lp, float info)\n"
	"{\n"
	"	float vis = 1.0;\n"
	"#if defined(RT_CAPSULES)\n"
	"	int   i   = int(floor(info));\n"
	"	int   end = i + int(floor(fract(info) * float(RT_MAX_RUN + 1) + 0.5));\n"
	"	vec3  d1 = Lp - P;\n"
	"	float a  = dot(d1, d1);\n"
	"	float len = sqrt(a);\n"
	"	while (i < end)\n"
	"	{\n"
	"		vec4  S = texelFetch(u_RtCapsules, ivec2(i * 2, 0), 0);\n"
	"		int   n = int(texelFetch(u_RtCapsules, ivec2(i * 2 + 1, 0), 0).x);\n"
	"		float sS = clamp(dot(S.xyz - P, d1) / a, 0.0, 1.0);\n"
	"		if (length(P + d1 * sS - S.xyz) > S.w + 1.0 + 0.06 * sS * len)\n"
	"		{\n"
	"			i += 1 + n;\n"
	"			continue;\n"
	"		}\n"
	"		for (int c = i + 1; c <= i + n; c++)\n"
	"		{\n"
	"			vec4  A = texelFetch(u_RtCapsules, ivec2(c * 2, 0), 0);\n"
	"			vec4  B = texelFetch(u_RtCapsules, ivec2(c * 2 + 1, 0), 0);\n"
	"			vec3  d2 = B.xyz - A.xyz;\n"
	"			vec3  r  = P - A.xyz;\n"
	// a sphere around the capsule and its softest edge, which the ray
	// passes wide of for most of a body's capsules: a few products, not
	// the nearest approach of two segments
	"			vec3  mid = A.xyz + 0.5 * d2 - P;\n"
	"			float sm  = clamp(dot(mid, d1) / a, 0.0, 1.0);\n"
	"			vec3  off = mid - d1 * sm;\n"
	"			float reach = 0.5 * length(d2) + 1.7 * A.w + 1.0;\n"
	"			if (dot(off, off) > reach * reach) continue;\n"
	"			float e  = dot(d2, d2), f = dot(d2, r);\n"
	// P on or in this capsule: the body itself, not in its shadow
	"			float tp = e > 1e-4 ? clamp(f / e, 0.0, 1.0) : 0.0;\n"
	"			if (length(r - d2 * tp) < A.w + 1.0) continue;\n"
	// the nearest the ray from P to the light comes to the capsule's core
	"			float b  = dot(d1, d2), cc = dot(d1, r);\n"
	"			float denom = a * e - b * b;\n"
	"			float s  = denom > 1e-4 ? clamp((b * f - cc * e) / denom, 0.0, 1.0) : 0.0;\n"
	"			float t  = e > 1e-4 ? (b * s + f) / e : 0.0;\n"
	"			if (t < 0.0) { t = 0.0; s = clamp(-cc / a, 0.0, 1.0); }\n"
	"			else if (t > 1.0) { t = 1.0; s = clamp((b - cc) / a, 0.0, 1.0); }\n"
	"			float dist = length(P + d1 * s - A.xyz - d2 * t);\n"
	// softer the farther it is from P, as a lamp that is not a point makes it,
	// but dark in the middle still however thin it is
	"			float soft = min(1.0 + 0.03 * s * len, 0.7 * A.w);\n"
	"			vis *= smoothstep(A.w - soft, A.w + soft, dist);\n"
	"		}\n"
	// all but dark already: the rest of the bodies change nothing seen
	"		if (vis < 0.004) return 0.0;\n"
	"		i += 1 + n;\n"
	"	}\n"
	"#endif\n"
	"	return vis;\n"
	"}\n"
	// How much of the sky a surface at P facing N sees, as an open floor does
	// all of it: each direction it faces, as much as it faces it, if nothing
	// is between them in that direction's depth.
	"vec3 RtSky(vec3 P, vec3 N)\n"
	"{\n"
	"	float sum  = 0.0;\n"
	"	float bias = 1.5 * u_RtSkyColor.w + 2.0;\n"
	"	for (int k = 0; k < RT_SKY_DIRS; k++)\n"
	"	{\n"
	"		vec4  d = u_RtSky[k * 4 + 3];\n"
	"		float w = dot(N, d.xyz);\n"
	// a leaf takes what comes through it from behind
	"		if (w < 0.0) w *= -u_RtSurface.x;\n"
	"		if (w <= 0.0) continue;\n"
	"		vec3  Q = P + (N * sign(dot(N, d.xyz)) + d.xyz) * bias;\n"
	"		vec4  r0 = u_RtSky[k * 4], r1 = u_RtSky[k * 4 + 1], r2 = u_RtSky[k * 4 + 2];\n"
	"		vec3  c = vec3(dot(r0.xyz, Q) + r0.w, dot(r1.xyz, Q) + r1.w, dot(r2.xyz, Q) + r2.w) * 0.5 + 0.5;\n"
	"		float t = float(k);\n"
	"		vec2  tile = vec2(mod(t, RT_SKY_TILES), floor(t / RT_SKY_TILES));\n"
	"		vec2  st = clamp(c.xy, vec2(1.0 / RT_SKY_TEXELS), vec2(1.0 - 1.0 / RT_SKY_TEXELS));\n"
	"		sum += w * shadow2D(u_RtSkyMap, vec3((tile + st) / RT_SKY_TILES, c.z));\n"
	"	}\n"
	"	return u_RtSkyColor.rgb * sum;\n"
	"}\n"
	// the map's lighting and what the lights change of it, never darker than
	// a part of what it was: the lights' model is too bright near a lamp
	"vec3 RtDelta(vec3 base, vec3 delta)\n"
	"{\n"
	"	return max(base + delta, base * u_RtParams.w);\n"
	"}\n"
	"#if !defined(RT_VERTEX)\n"
	// The shine of the lights and the sun (r_rtSpecular, u_RtSurface.z), for
	// a program that has the view and the surface's roughness: it sets
	// rt_specOn, rt_specView (to the eye) and rt_specRough before RtLight,
	// which adds what they bring to rt_spec, as lightall's CalcSpecular has it
	"bool  rt_specOn    = false;\n"
	"vec3  rt_specView  = vec3(0.0, 0.0, 1.0);\n"
	"float rt_specRough = 1.0;\n"
	"vec3  rt_spec      = vec3(0.0);\n"
	// r_gtao: how much of the light from all round reaches the pixel (the
	// sky, the bounce, the probes, the ambient); the caller reads it
	"float rt_ao        = 1.0;\n"
	"float RtAmbientOcclusion()\n"
	"{\n"
	"	return u_RtAO.x > 0.5 ? texture2D(u_RtAOMap, gl_FragCoord.xy * r_FBufScale).r : 1.0;\n"
	"}\n"
	"float RtSpecular(vec3 N, vec3 L)\n"
	"{\n"
	"	vec3  H  = normalize(L + rt_specView);\n"
	"	float NH = clamp(dot(N, H), 0.0, 1.0);\n"
	"	float EH = clamp(dot(rt_specView, H), 0.0, 1.0);\n"
	"	float rr = rt_specRough * rt_specRough;\n"
	"	float rrrr = rr * rr;\n"
	"	float dd = NH * NH * (rrrr - 1.0) + 1.0;\n"
	"	float v  = EH * EH * (rt_specRough + 0.5) + 0.0001;\n"
	"	return u_RtSurface.z * rrrr / (4.0 * dd * dd * v);\n"
	"}\n"
	// What light i gives P facing N, before its shadow (0: none), with which
	// kind it is (0 point, 1 spot, 2 the game's, 3 a lamp's or a lit
	// surface's) and whether it is in the baked lamps (R_RtBakeLamps): its
	// kind sent eight more. Shared with the bake, which works it out the same.
	"float RtAtten(int i, vec3 P, vec3 N, out vec3 toLight, out float d, out float kind, out bool baked)\n"
	"{\n"
	"	vec4  l0 = u_RtLights[i * 4];\n"
	"	vec4  l1 = u_RtLights[i * 4 + 1];\n"
	"	kind  = l0.w;\n"
	"	baked = kind > 7.5;\n"
	"	if (baked) kind -= 8.0;\n"
	"	toLight = l0.xyz - P;\n"
	"	float d2 = dot(toLight, toLight);\n"
	"	float r2 = l1.w * l1.w;\n"
	"	d = sqrt(d2);\n"
	"	if (d2 >= r2) return 0.0;\n"
	"	float ndl = dot(N, toLight) / max(d, 1e-4);\n"
	"	if (ndl < 0.0) ndl *= -u_RtSurface.x;\n"
	"	if (ndl <= 0.0) return 0.0;\n"
	"	float fade = 1.0 - d2 / r2;\n"
	// the compiler's point light: intensity * 7500 / distance squared, never nearer than 16
	"	float att  = ndl * fade * fade / max(d2, 256.0);\n"
	"	if (kind > 2.5)\n"
	"	{\n"
	// a lamp's: never nearer than the glow of its shade (l2.w), and less the
	// way its shade closes it off (l2.xyz, as long as the part it stops
	// there): a shade stops what leaves past its rim, so a little over square
	// to it all of it is stopped (a bell shade's ceiling, near and far).
	// Longer than a whole, the shade is a ring round the lamp (a lampshade
	// open at both ends): what leaves near square to its axis is held back
	"		vec4  l2   = u_RtLights[i * 4 + 2];\n"
	"		float shut = length(l2.xyz);\n"
	"		att = ndl * fade * fade / max(d2, max(l2.w * l2.w, 256.0));\n"
	// a lit surface's (its "near" less than nothing): what it gives
	// falls off with the angle from straight out of it (l2.xyz into it)
	"		if (l2.w < 0.0) att *= max(0.0, dot(toLight, l2.xyz)) / (d * shut);\n"
	"		else if (shut > 1.0) att *= 1.0 - (shut - 1.0) * smoothstep(0.55, 0.2, abs(dot(toLight, l2.xyz)) / (d * shut));\n"
	"		else if (shut > 0.0) att *= 1.0 - shut * smoothstep(-0.05, 0.3, dot(toLight, -l2.xyz) / (d * shut));\n"
	"	}\n"
	"	else if (kind > 0.5 && kind < 1.5)\n"
	"	{\n"
	// the compiler's spot light: a cone, with a 32 unit soft edge
	"		vec4  l2 = u_RtLights[i * 4 + 2];\n"
	"		float along = -dot(toLight, l2.xyz);\n"
	"		if (along <= 0.001) return 0.0;\n"
	"		float radiusAtDist = l2.w * along;\n"
	"		float sampleRadius = length(-toLight - l2.xyz * along);\n"
	"		if (sampleRadius >= radiusAtDist) return 0.0;\n"
	"		att *= clamp((radiusAtDist - sampleRadius) / 32.0, 0.0, 1.0);\n"
	"	}\n"
	"	return att;\n"
	"}\n"
	// off the surface P facing N by a texel and a half of light i's shadow at
	// that distance: from the light to there
	"vec3 RtShadowAt(vec4 l0, vec4 l3, vec3 P, vec3 N, vec3 toLight, float d, float tileTexels)\n"
	"{\n"
	"	float texelWorld = (l3.x >= RT_SMALL_CODE ? 4.0 : 2.0) * d / tileTexels;\n"
	"	return P + N * sign(dot(N, toLight)) * (1.5 * texelWorld + 0.5) - l0.xyz;\n"
	"}\n"
	"vec3 RtLight(vec3 P, vec3 N, bool delta)\n"
	"{\n"
	"	vec3 sum   = vec3(0.0);\n"
	"	rt_ao = delta ? 1.0 : RtAmbientOcclusion();\n"
	"	int  count = int(u_RtParams.x);\n"
	// the baked lamps (u_RtDirect.w, eased in): their light on a lightmap
	// is read from it, all of them, and the few lit live add what they
	// change of it (the sharp shadows, of what moves too, and their shine)
	// (u_RtDirect.w less than nothing: without the normal map's shading, r_rtBakedDebug 1)
	"	float bakedHere = (!delta && u_RtDirect.w != 0.0) ? abs(u_RtDirect.w) : 0.0;\n"
	// a shadow looked up off the surface's own face, not the normal map's:
	// its edge followed the bumps
	"	vec3  shadowN = dot(rt_geomN, rt_geomN) > 0.5 ? rt_geomN : N;\n"
	"	for (int k = 0; k < RT_MAX_LIGHTS; k++)\n"
	"	{\n"
	"		if (k >= count) break;\n"
	"		int   i  = RtListed(k);\n"
	"		vec3  toLight;\n"
	"		float d, kind;\n"
	"		bool  baked;\n"
	"		float att = RtAtten(i, P, N, toLight, d, kind, baked);\n"
	"		if (att <= 0.0) continue;\n"
	"		vec4  l0 = u_RtLights[i * 4];\n"
	"		vec4  l1 = u_RtLights[i * 4 + 1];\n"
	// 2: a light the game adds (a muzzle flash, a blast): never shadowed,
	// and on top of the map's lighting always
	"		bool added = kind > 1.5 && kind < 2.5;\n"
	"		float less = baked ? bakedHere : 0.0;\n"
	// what it would give unshadowed is too little to see: passed over, its
	// shadow not looked up (u_RtAmbient.w, r_rtCutoff; R_RtReach)
	"		vec3 most = l1.rgb * att;\n"
	"		if (max(most.r, max(most.g, most.b)) < u_RtAmbient.w) continue;\n"
	"		vec4 l3 = u_RtLights[i * 4 + 3];\n"
	"		float shine = att;\n"
	"		if (u_RtParams.z > 0.5 && (l3.x >= 0.0 || l3.y >= 0.0))\n"
	"		{\n"
	"			vec3  v = RtShadowAt(l0, l3, P, shadowN, toLight, d, RT_TILE_TEXELS);\n"
	"			float now = RtShadow(l3, v, false);\n"
	"			if (l3.z >= 0.0 && now > 0.0) now *= RtCapsules(P, l0.xyz, l3.z);\n"
	"			shine = att * now;\n"
	// a baked lamp not taken out of its bake (R_RtLiveIn) takes back its
	// baked light worked out here: the lightmap's coarse texels have it
	// otherwise at an edge, but only lamps further off are left to it.
	// Worked out at the lightmap's four texels it cost the main pass 1.4 ms
	// on the card in a busy hall (its registers), baked lamps on or off
	"			if (delta || less > 0.0)\n"
	"				now -= (delta ? 1.0 : less) * RtShadow(l3, RtShadowAt(l0, l3, P, shadowN, toLight, d, RT_BAKED_TEXELS), true);\n"
	"			att *= now;\n"
	"		}\n"
	"		else if (delta && !added) continue;\n"
	"		else att *= 1.0 - less;\n"
	"		sum += l1.rgb * att;\n"
	// the shine of what faces it (a leaf's far side takes none)
	"		if (rt_specOn && !delta && dot(N, toLight) > 0.0)\n"
	"			rt_spec += l1.rgb * shine * RtSpecular(N, toLight / d);\n"
	"	}\n"
	"	if (u_RtSunDir.w > 0.5 && !delta)\n"
	"	{\n"
	"		float ndl = dot(N, u_RtSunDir.xyz);\n"
	"		if (ndl < 0.0) ndl *= -u_RtSurface.x;\n"
	"		if (ndl > 0.0)\n"
	"		{\n"
	"			float vis = 1.0;\n"
	"			if (u_RtSunColor.w > 0.5)\n"
	"				vis = texture2D(u_RtSunShadow, gl_FragCoord.xy * r_FBufScale).r;\n"
	"			sum += u_RtSunColor.rgb * (ndl * vis);\n"
	"			if (rt_specOn && dot(N, u_RtSunDir.xyz) > 0.0)\n"
	"				rt_spec += u_RtSunColor.rgb * (ndl * vis) * RtSpecular(N, u_RtSunDir.xyz);\n"
	"		}\n"
	"	}\n"
	"	if (u_RtSkyColor.w > 0.0 && !delta)\n"
	"	{\n"
	// soft, and the same over a model or a leaf: worked out at its vertexes
	// on a lightmap, worked out for each of its texels once a map
	"		if (u_RtSurface.y > 1.5)\n"
	"			sum += rt_ao * u_RtSkyColor.rgb * (float(RT_SKY_DIRS) * 0.5) * texture2D(u_RtSkyLightmap, rt_skyLM).r;\n"
	"		else\n"
	"#if defined(USE_RTSKY_VERTEX)\n"
	"		if (u_RtSurface.y > 0.5)\n"
	"			sum += rt_ao * var_RtSky;\n"
	"		else\n"
	"#endif\n"
	"		sum += rt_ao * RtSky(P, N);\n"
	"	}\n"
	// what the sun, the sky and the lamps light round it gives back
	"#if defined(RT_BOUNCE)\n"
	"	if (u_RtBounce.w > 0.5 && u_RtSurface.y > 1.5 && !delta)\n"
	// x out under the sky, y deep in a room, by how far in its texel is (alpha)
	"	{\n"
	"		vec4 b = texture2D(u_RtBounceMap, rt_skyLM);\n"
	"		sum += rt_ao * (mix(u_RtBounce.x, u_RtBounce.y, b.a) * b.rgb + u_RtBounce.z * texture2D(u_RtBounceLampMap, rt_skyLM).rgb);\n"
	"	}\n"
	"	else if (u_RtBounce.w > 0.5 && !delta)\n"
	"		sum += rt_ao * u_RtBounce.rgb * max(vec3(0.0), vec3(u_RtProbeR.w + dot(u_RtProbeR.xyz, N), u_RtProbeG.w + dot(u_RtProbeG.xyz, N), u_RtProbeB.w + dot(u_RtProbeB.xyz, N)));\n"
	"#endif\n"
	// the baked lamps' light on this lightmap (R_RtBakeLamps), rgb its scale
	// the baked lamps' light on this lightmap (R_RtBakeLamps), rgb its scale,
	// shaded again for the normal map the way most of it comes from, as
	// much as it comes from one way, and shining that way
	"	if (bakedHere > 0.0)\n"
	"	{\n"
	"		vec3  c   = max(texture2D(u_RtDirectMap, rt_skyLM).rgb - texture2D(u_RtLiveMap, rt_skyLM).rgb, vec3(0.0));\n"
	"		vec3  dl  = texture2D(u_RtDirectDir, rt_skyLM).xyz - texture2D(u_RtLiveDir, rt_skyLM).xyz;\n"
	"		float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
	"		float len = length(dl);\n"
	"		vec3  b   = bakedHere * u_RtDirect.rgb * c;\n"
	"		if (len > 1e-5 && lum > 1e-5 && u_RtDirect.w > 0.0)\n"
	"		{\n"
	"			vec3  L = dl / len;\n"
	"			float k = clamp(len / lum, 0.0, 1.0);\n"
	"			if (dot(rt_geomN, rt_geomN) > 0.5)\n"
	"				b *= mix(1.0, clamp(max(dot(N, L), 0.0) / max(dot(rt_geomN, L), 0.25), 0.0, 2.0), k);\n"
	"			if (rt_specOn && dot(N, L) > 0.0)\n"
	"				rt_spec += b * k * RtSpecular(N, L);\n"
	"		}\n"
	"		sum += b;\n"
	"	}\n"
	// a lamp lit live carves its sharp shadows out of the baked light: never
	// below none
	


	"	sum = max(sum, vec3(0.0));\n"
	"	return sum;\n"
	"}\n"
	"#else\n"
	// dust or smoke at P, lit from every side as it hangs in the light, which
	// it is not in the shadow maps of. Its sun shadow is the sun's widest
	// cascade's at P; without one, the screen's at sunUV, where it is on the
	// screen (that of what is behind it, which over water is the bottom, and
	// changes as things pass behind: smoke flickered dark)
	"vec3 RtLightParticle(vec3 P, vec2 sunUV, bool delta)\n"
	"{\n"
	"	vec3 sum   = vec3(0.0);\n"
	"	int  count = int(u_RtParams.x);\n"
	"	for (int k = 0; k < RT_MAX_LIGHTS; k++)\n"
	"	{\n"
	"		if (k >= count) break;\n"
	"		int   i  = RtListed(k);\n"
	"		vec4  l0 = u_RtLights[i * 4];\n"
	"		vec4  l1 = u_RtLights[i * 4 + 1];\n"
	"		vec3  toLight = l0.xyz - P;\n"
	"		float d2 = dot(toLight, toLight);\n"
	"		float r2 = l1.w * l1.w;\n"
	"		if (d2 >= r2) continue;\n"
	"		float fade = 1.0 - d2 / r2;\n"
	"		float att  = fade * fade / max(d2, 256.0);\n"
	"		float kind = l0.w > 7.5 ? l0.w - 8.0 : l0.w;\n"
	"		bool added = kind > 1.5 && kind < 2.5;\n"
	"		if (kind > 2.5)\n"
	"		{\n"
	"			vec4  l2   = u_RtLights[i * 4 + 2];\n"
	"			float shut = length(l2.xyz);\n"
	"			att = fade * fade / max(d2, max(l2.w * l2.w, 256.0));\n"
	"			if (l2.w < 0.0) att *= max(0.0, dot(toLight, l2.xyz)) * inversesqrt(max(d2, 1.0)) / shut;\n"
	"			else if (shut > 1.0) att *= 1.0 - (shut - 1.0) * smoothstep(0.55, 0.2, abs(dot(toLight, l2.xyz)) * inversesqrt(max(d2, 1.0)) / shut);\n"
	"			else if (shut > 0.0) att *= 1.0 - shut * smoothstep(-0.05, 0.3, dot(toLight, -l2.xyz) * inversesqrt(max(d2, 1.0)) / shut);\n"
	"		}\n"
	"		else if (kind > 0.5 && kind < 1.5)\n"
	"		{\n"
	"			vec4  l2 = u_RtLights[i * 4 + 2];\n"
	"			float along = -dot(toLight, l2.xyz);\n"
	"			if (along <= 0.001) continue;\n"
	"			float radiusAtDist = l2.w * along;\n"
	"			float sampleRadius = length(-toLight - l2.xyz * along);\n"
	"			if (sampleRadius >= radiusAtDist) continue;\n"
	"			att *= clamp((radiusAtDist - sampleRadius) / 32.0, 0.0, 1.0);\n"
	"		}\n"
	"		vec3 most = l1.rgb * att;\n"
	"		if (max(most.r, max(most.g, most.b)) < u_RtAmbient.w) continue;\n"
	"		vec4 l3 = u_RtLights[i * 4 + 3];\n"
	"		if (u_RtParams.z > 0.5 && (l3.x >= 0.0 || l3.y >= 0.0))\n"
	"		{\n"
	"			vec3  v = P - l0.xyz;\n"
	"			float now = RtShadow(l3, v, false);\n"
	"			att *= delta ? now - RtShadow(l3, v, true) : now;\n"
	"		}\n"
	"		else if (delta && !added) continue;\n"
	"		sum += l1.rgb * att;\n"
	"	}\n"
	"	if (u_RtSunDir.w > 0.5 && !delta)\n"
	"	{\n"
	"		float vis = 1.0;\n"
	"		vec4 sp = u_RtSunCascadeMvp * vec4(P, 1.0);\n"
	"		if (u_RtSunColor.w > 0.5 && abs(sp.w) > 0.0)\n"
	"		{\n"
	"			if (all(lessThan(abs(sp.xyz), vec3(abs(sp.w)))))\n"
	"			{\n"
	"				sp.xyz = sp.xyz * (0.5 / sp.w) + vec3(0.5);\n"
	"				vis = shadow2D(u_RtSunCascade, vec3(sp.xy, sp.z - 0.0005));\n"
	"			}\n"
	"		}\n"
	"		else if (u_RtSunColor.w > 0.5)\n"
	"			vis = texture2D(u_RtSunShadow, sunUV).r;\n"
	"		sum += u_RtSunColor.rgb * vis;\n"
	"	}\n"
	// hanging in the air, it is lit as a floor is
	"	if (u_RtSkyColor.w > 0.0 && !delta)\n"
	"		sum += RtSky(P, vec3(0.0, 0.0, 1.0));\n"
	"	return sum;\n"
	"}\n"
	"#endif\n";

/*
Added in OPM: the light in the air (r_volumetric 2, tr_froxel.c) for what is
drawn after the opaque world: dust, smoke, glass, water, the alpha tested
leaves. The air from the eye to the fragment, summed (what it adds, what it
lets through), read from the grid at the fragment's depth (as froxel_fp.glsl's
FROXEL_APPLY reads it at each pixel's), and put on the fragment as its blend
takes it (u_FroxelFog.x): 1 what it lets through of the fragment plus what it
adds (blended over, or drawn whole), 2 only what it lets through (added on top:
the air's own light would be added again each time), 3 a multiply faded as far
as the air hides it, 4 as 1 for a colour already times its alpha (blended
with one and one less its alpha: the air's light only as far as it covers,
or its quad's clear corners took the air's light, dark squares round a
fireball in smoke).
*/
static const char *froxelFog_glsl =
	"uniform sampler2D u_FroxelFogMap;\n"
	"uniform vec4 u_FroxelFog;\n"
	"uniform vec4 u_FroxelFogView;\n"
	"uniform vec4 u_FroxelFogGrid;\n"
	"vec2 FroxelFogUV(vec2 uv, float s)\n"
	"{\n"
	"	vec2 edge = 0.5 / u_FroxelFogGrid.xy;\n"
	"	vec2 tile = vec2(mod(s, 8.0), floor(s / 8.0));\n"
	"	return (tile + clamp(uv, edge, 1.0 - edge)) / 8.0;\n"
	"}\n"
	"vec3 FroxelFog(vec3 c, float alpha)\n"
	"{\n"
	"	if (u_FroxelFog.x < 0.5) return c;\n"
	// w more than 0: the depth to take it at (a flare, drawn flat at the
	// end); less: the depth range squeezed into one part of it (the view
	// weapon's), undone
	"	float zf = u_FroxelFog.w < 0.0 ? min(gl_FragCoord.z * -u_FroxelFog.w, 1.0) : gl_FragCoord.z;\n"
	"	float depth = u_FroxelFog.w > 0.0 ? u_FroxelFog.w : u_FroxelFogView.y / mix(u_FroxelFogView.x, 1.0, zf);\n"
	"	vec2  uv = (gl_FragCoord.xy - u_FroxelFogGrid.zw) * u_FroxelFogView.zw;\n"
	"	float f  = min(log2(depth / 256.0 + 1.0) / u_FroxelFog.z * u_FroxelFog.y, u_FroxelFog.y) - 1.0;\n"
	"	vec4  a, b;\n"
	"	if (f < 0.0)\n"
	"	{\n"
	"		a = vec4(0.0, 0.0, 0.0, 1.0);\n"
	"		b = texture2D(u_FroxelFogMap, FroxelFogUV(uv, 0.0));\n"
	"		f += 1.0;\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		float s = floor(f);\n"
	"		a = texture2D(u_FroxelFogMap, FroxelFogUV(uv, s));\n"
	"		b = texture2D(u_FroxelFogMap, FroxelFogUV(uv, min(s + 1.0, u_FroxelFog.y - 1.0)));\n"
	"		f -= s;\n"
	"	}\n"
	"	vec4 air = mix(a, b, f);\n"
	"	if (u_FroxelFog.x < 1.5) return c * air.a + air.rgb;\n"
	"	if (u_FroxelFog.x < 2.5) return c * air.a;\n"
	"	if (u_FroxelFog.x < 3.5) return mix(vec3(1.0), c, air.a);\n"
	"	return c * air.a + air.rgb * clamp(alpha, 0.0, 1.0);\n"
	"}\n";

static const char *rt_skyBake_vp =
	"attribute vec3 attr_Position;\n"
	"attribute vec3 attr_Normal;\n"
	"attribute vec4 attr_TexCoord0;\n"
	"varying vec3 var_P;\n"
	"varying vec3 var_N;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4(attr_TexCoord0.xy * 2.0 - 1.0, 0.0, 1.0);\n"
	"	var_P = attr_Position;\n"
	"	var_N = attr_Normal;\n"
	"}\n";
static const char *rt_skyBake_fp =
	"varying vec3 var_P;\n"
	"varying vec3 var_N;\n"
	"void main()\n"
	"{\n"
	"	gl_FragColor = vec4(RtSky(var_P, normalize(var_N)).r, 0.0, 0.0, 1.0);\n"
	"}\n";

/*
Added in OPM
A lamp's light on the lightmaps, once a map (R_RtBakeLamps): drawn at the
lightmap coordinates of the surfaces it reaches, added up, light 0 of
u_RtLights with its shadow of the world from the baked atlas, as RtLight
works it out for a pixel.
*/
static const char *rt_lampBake_fp =
	"varying vec3 var_P;\n"
	"varying vec3 var_N;\n"
	"void main()\n"
	"{\n"
	"	vec3  N = normalize(var_N);\n"
	"	vec3  toLight;\n"
	"	float d, kind;\n"
	"	bool  baked;\n"
	"	float att = RtAtten(0, var_P, N, toLight, d, kind, baked);\n"
	"	vec3  c = vec3(0.0);\n"
	"	if (att > 0.0)\n"
	"	{\n"
	"		vec4 l3 = u_RtLights[3];\n"
	"		if (l3.x >= 0.0)\n"
	"			att *= RtShadow(l3, RtShadowAt(u_RtLights[0], l3, var_P, N, toLight, d, RT_BAKED_TEXELS), true);\n"
	"		c = u_RtLights[1].rgb * att;\n"
	"	}\n"
	// a: where a surface is (u_RtParams.w 1, before the lamps), the lamps adding none;
	// u_RtParams.x 2: the way it comes from, as bright as it is, for the normal maps
	"	if (u_RtParams.x > 1.5)\n"
	"		c = (att > 0.0 ? toLight / max(d, 1e-4) : vec3(0.0)) * dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
	"	gl_FragColor = vec4(c, u_RtParams.w);\n"
	"}\n";

/*
Added in OPM
A lamp's light on a page, as the bake leaves it (R_RtBakeFinish), for those
lit live (R_RtLivePass): u_RtParams.x 0, each texel the mean of the surfaces
drawn over it (u_RtDirectDir's r how many), a 1 where any is; 1, a ring of
the texels none covers taking the mean of those round them that are; 2,
added on, times u_RtParams.y.
*/
static const char *rt_bakeFill_fp =
	"void main()\n"
	"{\n"
	"	ivec2 p = ivec2(gl_FragCoord.xy);\n"
	"	vec4  s = texelFetch(u_RtDirectMap, p, 0);\n"
	"	if (u_RtParams.x < 0.5)\n"
	"	{\n"
	"		float n = texelFetch(u_RtDirectDir, p, 0).r;\n"
	"		gl_FragColor = vec4(n > 1.5 ? s.rgb / n : s.rgb, n > 0.5 ? 1.0 : 0.0);\n"
	"	}\n"
	"	else if (u_RtParams.x < 1.5)\n"
	"	{\n"
	"		ivec2 size  = textureSize(u_RtDirectMap, 0);\n"
	"		vec3  sum   = vec3(0.0);\n"
	"		float count = 0.0;\n"
	"		if (s.a >= 0.5) { gl_FragColor = s; return; }\n"
	"		for (int dy = -1; dy <= 1; dy++)\n"
	"		for (int dx = -1; dx <= 1; dx++)\n"
	"		{\n"
	"			ivec2 q = p + ivec2(dx, dy);\n"
	"			if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;\n"
	"			vec4 t = texelFetch(u_RtDirectMap, q, 0);\n"
	"			if (t.a < 0.5) continue;\n"
	"			sum += t.rgb;\n"
	"			count += 1.0;\n"
	"		}\n"
	"		gl_FragColor = count > 0.0 ? vec4(sum / count, 1.0) : s;\n"
	"	}\n"
	"	else\n"
	"		gl_FragColor = vec4(s.rgb * u_RtParams.y, 0.0);\n"
	"}\n";

static void GLSL_GetShaderHeader( GLenum shaderType, const GLchar *extra, char *dest, int size )
{
	float fbufWidthScale, fbufHeightScale;

	dest[0] = '\0';

	// HACK: abuse the GLSL preprocessor to turn GLSL 1.20 shaders into 1.30 ones
	if(glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 30))
	{
		if (qglesMajorVersion >= 3 && glRefConfig.glslMajorVersion >= 3)
			Q_strcat(dest, size, "#version 300 es\n");
		else if (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 50))
			Q_strcat(dest, size, "#version 150\n");
		else
			Q_strcat(dest, size, "#version 130\n");

		// `extra' may contain #extension which must be directly after #version
		if (extra)
		{
			Q_strcat(dest, size, extra);
		}

		if (qglesMajorVersion >= 2)
		{
			Q_strcat(dest, size, "precision mediump float;\n");
			Q_strcat(dest, size, "precision mediump sampler2DShadow;\n");
		}

		if(shaderType == GL_VERTEX_SHADER)
		{
			Q_strcat(dest, size, "#define attribute in\n");
			Q_strcat(dest, size, "#define varying out\n");
		}
		else
		{
			Q_strcat(dest, size, "#define varying in\n");

			Q_strcat(dest, size, "out vec4 out_Color;\n");
			Q_strcat(dest, size, "#define gl_FragColor out_Color\n");
			Q_strcat(dest, size, "#define texture2D texture\n");
			Q_strcat(dest, size, "#define textureCubeLod textureLod\n");
			Q_strcat(dest, size, "#define shadow2D texture\n");
		}
	}
	else
	{
		if (qglesMajorVersion >= 2)
		{
			Q_strcat(dest, size, "#version 100\n");

			if (extra)
			{
				Q_strcat(dest, size, extra);
			}

			Q_strcat(dest, size, "precision mediump float;\n");

			if (glRefConfig.shadowSamplers)
			{
				Q_strcat(dest, size, "precision mediump sampler2DShadow;\n");
				Q_strcat(dest, size, "#define shadow2D(a,b) shadow2DEXT(a,b)\n");
			}
		}
		else
		{
			Q_strcat(dest, size, "#version 120\n");

			if (extra)
			{
				Q_strcat(dest, size, extra);
			}

			Q_strcat(dest, size, "#define shadow2D(a,b) shadow2D(a,b).r\n");
		}
	}

	// HACK: add some macros to avoid extra uniforms and save speed and code maintenance
	//Q_strcat(dest, size,
	//		 va("#ifndef r_SpecularExponent\n#define r_SpecularExponent %f\n#endif\n", r_specularExponent->value));
	//Q_strcat(dest, size,
	//		 va("#ifndef r_SpecularScale\n#define r_SpecularScale %f\n#endif\n", r_specularScale->value));
	//Q_strcat(dest, size,
	//       va("#ifndef r_NormalScale\n#define r_NormalScale %f\n#endif\n", r_normalScale->value));


	Q_strcat(dest, size, "#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n");

	//Q_strcat(dest, size, va("#ifndef MAX_SHADOWMAPS\n#define MAX_SHADOWMAPS %i\n#endif\n", MAX_SHADOWMAPS));

	Q_strcat(dest, size,
					 va("#ifndef deformGen_t\n"
						"#define deformGen_t\n"
						"#define DGEN_WAVE_SIN %i\n"
						"#define DGEN_WAVE_SQUARE %i\n"
						"#define DGEN_WAVE_TRIANGLE %i\n"
						"#define DGEN_WAVE_SAWTOOTH %i\n"
						"#define DGEN_WAVE_INVERSE_SAWTOOTH %i\n"
						"#define DGEN_BULGE %i\n"
						"#define DGEN_MOVE %i\n"
						"#endif\n",
						DGEN_WAVE_SIN,
						DGEN_WAVE_SQUARE,
						DGEN_WAVE_TRIANGLE,
						DGEN_WAVE_SAWTOOTH,
						DGEN_WAVE_INVERSE_SAWTOOTH,
						DGEN_BULGE,
						DGEN_MOVE));

	Q_strcat(dest, size,
					 va("#ifndef tcGen_t\n"
						"#define tcGen_t\n"
						"#define TCGEN_LIGHTMAP %i\n"
						"#define TCGEN_TEXTURE %i\n"
						"#define TCGEN_ENVIRONMENT_MAPPED %i\n"
						"#define TCGEN_FOG %i\n"
						"#define TCGEN_VECTOR %i\n"
						"#define TCGEN_ENVIRONMENT_MAPPED2 %i\n"
						"#endif\n",
						TCGEN_LIGHTMAP,
						TCGEN_TEXTURE,
						TCGEN_ENVIRONMENT_MAPPED,
						TCGEN_FOG,
						TCGEN_VECTOR,
						TCGEN_ENVIRONMENT_MAPPED2));

	Q_strcat(dest, size,
					 va("#ifndef colorGen_t\n"
						"#define colorGen_t\n"
						"#define CGEN_LIGHTING_DIFFUSE %i\n"
						"#define CGEN_DOT %i\n"
						"#define CGEN_ONE_MINUS_DOT %i\n"
						"#endif\n",
						CGEN_LIGHTING_DIFFUSE,
						CGEN_DOT,
						CGEN_ONE_MINUS_DOT));

	Q_strcat(dest, size,
							 va("#ifndef alphaGen_t\n"
								"#define alphaGen_t\n"
								"#define AGEN_LIGHTING_SPECULAR %i\n"
								"#define AGEN_PORTAL %i\n"
								"#define AGEN_SCOORD %i\n"
								"#define AGEN_TCOORD %i\n"
								"#define AGEN_DOT %i\n"
								"#define AGEN_ONE_MINUS_DOT %i\n"
								"#endif\n",
								AGEN_LIGHTING_SPECULAR,
								AGEN_PORTAL,
								AGEN_SCOORD,
								AGEN_TCOORD,
								AGEN_DOT,
								AGEN_ONE_MINUS_DOT));

	fbufWidthScale = 1.0f / ((float)glConfig.vidWidth);
	fbufHeightScale = 1.0f / ((float)glConfig.vidHeight);
	Q_strcat(dest, size,
			 va("#ifndef r_FBufScale\n#define r_FBufScale vec2(%f, %f)\n#endif\n", fbufWidthScale, fbufHeightScale));

	// Added in OPM: the realtime lights, and for the particles lit at their
	// corners (USE_RTLIGHT_PARTICLES), the vertex shader too
	if (extra && ((shaderType == GL_FRAGMENT_SHADER && strstr(extra, "#define USE_RTLIGHT"))
		|| (shaderType == GL_VERTEX_SHADER && (strstr(extra, "#define USE_RTLIGHT_PARTICLES") || strstr(extra, "#define USE_RTSKY_VERTEX")))))
	{
		if (shaderType == GL_VERTEX_SHADER)
			Q_strcat(dest, size, "#define RT_VERTEX\n#define texture2D texture\n#define shadow2D texture\n");
		Q_strcat(dest, size, va("#define RT_MAX_LIGHTS %d\n#define RT_TILES %d.0\n#define RT_TILE_TEXELS %d.0\n#define RT_ZNEAR %f\n",
			RT_MAX_LIGHTS, RT_ATLAS_TILES, r_rtShadowTile->integer, RT_ZNEAR));
		Q_strcat(dest, size, va("#define RT_SMALL_CODE %d.0\n#define RT_SMALL_ROW %d.0\n#define RT_BAKED_TEXELS %d.0\n", RT_SMALL_CODE,
			RT_SMALL_FIRST / RT_ATLAS_TILES, R_RtBakedTile()));
		Q_strcat(dest, size, va("#define RT_SKY_DIRS %d\n#define RT_SKY_TILES %d.0\n#define RT_SKY_TEXELS %d.0\n",
			RT_SKY_DIRS, RT_SKY_TILES, r_rtSkyTile->integer));
		// the bodies' capsules are fetched texel by texel (GLSL 1.30)
		if (glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 30)
			Q_strcat(dest, size, va("#define RT_CAPSULES\n#define RT_MAX_RUN %d\n", RT_MAX_RUN));
		// the bounce (R_RtBounce), only where it is on: its reads cost every lit pixel even off
		if (R_RtBounceCompiled())
			Q_strcat(dest, size, "#define RT_BOUNCE\n");
		Q_strcat(dest, size, rt_glsl);
	}

	// Added in OPM: the light in the air, for what is drawn after it
	if (extra && shaderType == GL_FRAGMENT_SHADER && strstr(extra, "#define USE_FROXEL_FOG"))
		Q_strcat(dest, size, froxelFog_glsl);

	if (r_pbr->integer)
		Q_strcat(dest, size, "#define USE_PBR\n");

	if (r_cubeMapping->integer)
	{
		int cubeMipSize = r_cubemapSize->integer;
		int numRoughnessMips = 0;

		while (cubeMipSize)
		{
			cubeMipSize >>= 1;
			numRoughnessMips++;
		}
		numRoughnessMips = MAX(1, numRoughnessMips - 2);
		Q_strcat(dest, size, va("#define ROUGHNESS_MIPS float(%d)\n", numRoughnessMips));
	}

	// OK we added a lot of stuff but if we do something bad in the GLSL shaders then we want the proper line
	// so we have to reset the line counting
	Q_strcat(dest, size, "#line 0\n");
}

static int GLSL_CompileGPUShader(GLuint program, GLuint *prevShader, const GLchar *buffer, int size, GLenum shaderType)
{
	GLint           compiled;
	GLuint          shader;

	shader = qglCreateShader(shaderType);

	qglShaderSource(shader, 1, (const GLchar **)&buffer, &size);

	// compile shader
	qglCompileShader(shader);

	// check if shader compiled
	qglGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if(!compiled)
	{
		GLSL_PrintLog(shader, GLSL_PRINTLOG_SHADER_SOURCE, qfalse);
		GLSL_PrintLog(shader, GLSL_PRINTLOG_SHADER_INFO, qfalse);
		ri.Error(ERR_DROP, "Couldn't compile shader");
		return 0;
	}

	if (*prevShader)
	{
		qglDetachShader(program, *prevShader);
		qglDeleteShader(*prevShader);
	}

	// attach shader to program
	qglAttachShader(program, shader);

	*prevShader = shader;

	return 1;
}

static int GLSL_LoadGPUShaderText(const char *name, const char *fallback,
	GLenum shaderType, char *dest, int destSize)
{
	char            filename[MAX_QPATH];
	GLchar      *buffer = NULL;
	const GLchar *shaderText = NULL;
	int             size;
	int             result;

	if(shaderType == GL_VERTEX_SHADER)
	{
		Com_sprintf(filename, sizeof(filename), "glsl/%s_vp.glsl", name);
	}
	else
	{
		Com_sprintf(filename, sizeof(filename), "glsl/%s_fp.glsl", name);
	}

	if ( r_externalGLSL->integer ) {
		size = ri.FS_ReadFile(filename, (void **)&buffer);
	} else {
		size = 0;
		buffer = NULL;
	}

	if(!buffer)
	{
		if (fallback)
		{
			ri.Printf(PRINT_DEVELOPER, "...loading built-in '%s'\n", filename);
			shaderText = fallback;
			size = strlen(shaderText);
		}
		else
		{
			ri.Printf(PRINT_DEVELOPER, "couldn't load '%s'\n", filename);
			return 0;
		}
	}
	else
	{
		ri.Printf(PRINT_DEVELOPER, "...loading '%s'\n", filename);
		shaderText = buffer;
	}

	if (size > destSize)
	{
		// Added in OPM: say so, rather than the shader just failing to load
		ri.Printf(PRINT_WARNING, "GLSL: '%s' is %d bytes, %d more than there is room for after its header\n", filename, size, size - destSize);
		result = 0;
	}
	else
	{
		Q_strncpyz(dest, shaderText, size + 1);
		result = 1;
	}

	if (buffer)
	{
		ri.FS_FreeFile(buffer);
	}
	
	return result;
}

static void GLSL_LinkProgram(GLuint program)
{
	GLint           linked;

	qglLinkProgram(program);

	qglGetProgramiv(program, GL_LINK_STATUS, &linked);
	if(!linked)
	{
		GLSL_PrintLog(program, GLSL_PRINTLOG_PROGRAM_INFO, qfalse);
		ri.Error(ERR_DROP, "shaders failed to link");
	}
}

static void GLSL_ShowProgramUniforms(GLuint program)
{
	int             i, count, size;
	GLenum			type;
	char            uniformName[1000];

	// query the number of active uniforms
	qglGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);

	// Loop over each of the active uniforms, and set their value
	for(i = 0; i < count; i++)
	{
		qglGetActiveUniform(program, i, sizeof(uniformName), NULL, &size, &type, uniformName);

		ri.Printf(PRINT_DEVELOPER, "active uniform: '%s'\n", uniformName);
	}
}

static int GLSL_InitGPUShader2(shaderProgram_t * program, const char *name, int attribs, const char *vpCode, const char *fpCode)
{
	ri.Printf(PRINT_DEVELOPER, "------- GPU shader -------\n");

	if(strlen(name) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "GLSL_InitGPUShader2: \"%s\" is too long", name);
	}

	Q_strncpyz(program->name, name, sizeof(program->name));

	program->program = qglCreateProgram();
	program->attribs = attribs;

	if (!(GLSL_CompileGPUShader(program->program, &program->vertexShader, vpCode, strlen(vpCode), GL_VERTEX_SHADER)))
	{
		ri.Printf(PRINT_ALL, "GLSL_InitGPUShader2: Unable to load \"%s\" as GL_VERTEX_SHADER\n", name);
		qglDeleteProgram(program->program);
		return 0;
	}

	if(fpCode)
	{
		if(!(GLSL_CompileGPUShader(program->program, &program->fragmentShader, fpCode, strlen(fpCode), GL_FRAGMENT_SHADER)))
		{
			ri.Printf(PRINT_ALL, "GLSL_InitGPUShader2: Unable to load \"%s\" as GL_FRAGMENT_SHADER\n", name);
			qglDeleteProgram(program->program);
			return 0;
		}
	}

	if(attribs & ATTR_POSITION)
		qglBindAttribLocation(program->program, ATTR_INDEX_POSITION, "attr_Position");

	if(attribs & ATTR_TEXCOORD)
		qglBindAttribLocation(program->program, ATTR_INDEX_TEXCOORD, "attr_TexCoord0");

	if(attribs & ATTR_LIGHTCOORD)
		qglBindAttribLocation(program->program, ATTR_INDEX_LIGHTCOORD, "attr_TexCoord1");

//  if(attribs & ATTR_TEXCOORD2)
//      qglBindAttribLocation(program->program, ATTR_INDEX_TEXCOORD2, "attr_TexCoord2");

//  if(attribs & ATTR_TEXCOORD3)
//      qglBindAttribLocation(program->program, ATTR_INDEX_TEXCOORD3, "attr_TexCoord3");

	if(attribs & ATTR_TANGENT)
		qglBindAttribLocation(program->program, ATTR_INDEX_TANGENT, "attr_Tangent");

	if(attribs & ATTR_NORMAL)
		qglBindAttribLocation(program->program, ATTR_INDEX_NORMAL, "attr_Normal");

	if(attribs & ATTR_COLOR)
		qglBindAttribLocation(program->program, ATTR_INDEX_COLOR, "attr_Color");

	if(attribs & ATTR_PAINTCOLOR)
		qglBindAttribLocation(program->program, ATTR_INDEX_PAINTCOLOR, "attr_PaintColor");

	if(attribs & ATTR_LIGHTDIRECTION)
		qglBindAttribLocation(program->program, ATTR_INDEX_LIGHTDIRECTION, "attr_LightDirection");

	if(attribs & ATTR_BONE_INDEXES)
		qglBindAttribLocation(program->program, ATTR_INDEX_BONE_INDEXES, "attr_BoneIndexes");

	if(attribs & ATTR_BONE_WEIGHTS)
		qglBindAttribLocation(program->program, ATTR_INDEX_BONE_WEIGHTS, "attr_BoneWeights");

	if(attribs & ATTR_POSITION2)
		qglBindAttribLocation(program->program, ATTR_INDEX_POSITION2, "attr_Position2");

	if(attribs & ATTR_NORMAL2)
		qglBindAttribLocation(program->program, ATTR_INDEX_NORMAL2, "attr_Normal2");

	if(attribs & ATTR_TANGENT2)
		qglBindAttribLocation(program->program, ATTR_INDEX_TANGENT2, "attr_Tangent2");

	GLSL_LinkProgram(program->program);

	return 1;
}

static int GLSL_InitGPUShader(shaderProgram_t * program, const char *name,
	int attribs, qboolean fragmentShader, const GLchar *extra, qboolean addHeader,
	const char *fallback_vp, const char *fallback_fp)
{
	// Added in OPM: room for lightall, which with its header outgrew 32000
	// (it failed to load, saying only that it could not); static, as two
	// such buffers are a lot of stack and the shaders are built one at a time
	static char vpCode[65536];
	static char fpCode[65536];
	char *postHeader;
	int size;
	int result;

	size = sizeof(vpCode);
	if (addHeader)
	{
		GLSL_GetShaderHeader(GL_VERTEX_SHADER, extra, vpCode, size);
		postHeader = &vpCode[strlen(vpCode)];
		size -= strlen(vpCode);
	}
	else
	{
		postHeader = &vpCode[0];
	}

	if (!GLSL_LoadGPUShaderText(name, fallback_vp, GL_VERTEX_SHADER, postHeader, size))
	{
		return 0;
	}

	if (fragmentShader)
	{
		size = sizeof(fpCode);
		if (addHeader)
		{
			GLSL_GetShaderHeader(GL_FRAGMENT_SHADER, extra, fpCode, size);
			postHeader = &fpCode[strlen(fpCode)];
			size -= strlen(fpCode);
		}
		else
		{
			postHeader = &fpCode[0];
		}

		if (!GLSL_LoadGPUShaderText(name, fallback_fp, GL_FRAGMENT_SHADER, postHeader, size))
		{
			return 0;
		}
	}

	result = GLSL_InitGPUShader2(program, name, attribs, vpCode, fragmentShader ? fpCode : NULL);

	return result;
}

void GLSL_InitUniforms(shaderProgram_t *program)
{
	int i, size;

	GLint *uniforms = program->uniforms;

	size = 0;
	for (i = 0; i < UNIFORM_COUNT; i++)
	{
		uniforms[i] = qglGetUniformLocation(program->program, uniformsInfo[i].name);

		if (uniforms[i] == -1)
			continue;
		 
		program->uniformBufferOffsets[i] = size;

		switch(uniformsInfo[i].type)
		{
			case GLSL_INT:
				size += sizeof(GLint);
				break;
			case GLSL_FLOAT:
				size += sizeof(GLfloat);
				break;
			case GLSL_FLOAT5:
				size += sizeof(vec_t) * 5;
				break;
			case GLSL_VEC2:
				size += sizeof(vec_t) * 2;
				break;
			case GLSL_VEC3:
				size += sizeof(vec_t) * 3;
				break;
			case GLSL_VEC4:
				size += sizeof(vec_t) * 4;
				break;
			case GLSL_MAT16:
				size += sizeof(vec_t) * 16;
				break;
			case GLSL_MAT16_BONEMATRIX:
				size += sizeof(vec_t) * 16 * glRefConfig.glslMaxAnimatedBones;
				break;
			case GLSL_VEC4_RTLIGHTS:
				size += sizeof(vec4_t) * RT_MAX_LIGHTS * RT_LIGHT_VEC4S;
				break;
			case GLSL_VEC4_RTLIST:
				size += sizeof(vec4_t) * (RT_MAX_LIGHTS / 4);
				break;
			case GLSL_VEC4_RTSKY:
				size += sizeof(vec4_t) * RT_SKY_DIRS * 4;
				break;
			case GLSL_VEC4_SKELLIGHTS:
				size += sizeof(vec4_t) * SKEL_GPU_MAX_LIGHTS * 3;
				break;
			case GLSL_VEC4_SKELDENTS:
				size += sizeof(vec4_t) * SKEL_GPU_MAX_DENTS * 2;
				break;
			case GLSL_VEC4_SKELMORPHS:
				size += sizeof(vec4_t) * (SKEL_GPU_MAX_MORPHS / 4);
				break;
			case GLSL_VEC4_WATERSPOTS:
				size += sizeof(vec4_t) * WATER_MAX_SPOTS;
				break;
			case GLSL_VEC4_SMOKE:
				size += sizeof(vec4_t) * SMOKE_MAX_BLOBS * 3;
				break;
			default:
				break;
		}
	}

	program->uniformBuffer = ri.Malloc(size);
}

void GLSL_FinishGPUShader(shaderProgram_t *program)
{
	GLSL_ShowProgramUniforms(program->program);
	GL_CheckErrors();
}

void GLSL_SetUniformInt(shaderProgram_t *program, int uniformNum, GLint value)
{
	GLint *uniforms = program->uniforms;
	GLint *compare = (GLint *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_INT)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformInt: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (value == *compare)
	{
		return;
	}

	*compare = value;

	qglProgramUniform1iEXT(program->program, uniforms[uniformNum], value);
}

void GLSL_SetUniformFloat(shaderProgram_t *program, int uniformNum, GLfloat value)
{
	GLint *uniforms = program->uniforms;
	GLfloat *compare = (GLfloat *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_FLOAT)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformFloat: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (value == *compare)
	{
		return;
	}

	*compare = value;
	
	qglProgramUniform1fEXT(program->program, uniforms[uniformNum], value);
}

void GLSL_SetUniformVec2(shaderProgram_t *program, int uniformNum, const vec2_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_VEC2)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec2: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (v[0] == compare[0] && v[1] == compare[1])
	{
		return;
	}

	compare[0] = v[0];
	compare[1] = v[1];

	qglProgramUniform2fEXT(program->program, uniforms[uniformNum], v[0], v[1]);
}

void GLSL_SetUniformVec3(shaderProgram_t *program, int uniformNum, const vec3_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_VEC3)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec3: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (VectorCompare(v, compare))
	{
		return;
	}

	VectorCopy(v, compare);

	qglProgramUniform3fEXT(program->program, uniforms[uniformNum], v[0], v[1], v[2]);
}

void GLSL_SetUniformVec4(shaderProgram_t *program, int uniformNum, const vec4_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_VEC4)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec4: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (VectorCompare4(v, compare))
	{
		return;
	}

	VectorCopy4(v, compare);

	qglProgramUniform4fEXT(program->program, uniforms[uniformNum], v[0], v[1], v[2], v[3]);
}

void GLSL_SetUniformFloat5(shaderProgram_t *program, int uniformNum, const vec5_t v)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_FLOAT5)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformFloat5: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (VectorCompare5(v, compare))
	{
		return;
	}

	VectorCopy5(v, compare);

	qglProgramUniform1fvEXT(program->program, uniforms[uniformNum], 5, v);
}

void GLSL_SetUniformMat4(shaderProgram_t *program, int uniformNum, const mat4_t matrix)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1)
		return;

	if (uniformsInfo[uniformNum].type != GLSL_MAT16)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (Mat4Compare(matrix, compare))
	{
		return;
	}

	Mat4Copy(matrix, compare);

	qglProgramUniformMatrix4fvEXT(program->program, uniforms[uniformNum], 1, GL_FALSE, matrix);
}

/*
Added in OPM
The realtime lights, RT_LIGHT_VEC4S vec4s each (tr_rtlight.c). Only the first
count are sent; the shader reads no further than u_RtParams.x.
*/
void GLSL_SetUniformRtLights(shaderProgram_t *program, int uniformNum, const vec4_t *v, int count, int generation)
{
	GLint *uniforms = program->uniforms;

	if (uniforms[uniformNum] == -1 || count <= 0) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_VEC4_RTLIGHTS)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformRtLights: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (count > RT_MAX_LIGHTS) {
		count = RT_MAX_LIGHTS;
	}

	// written once a frame (R_RtRenderShadows): comparing the whole of them
	// at each draw was kilobytes a draw for nothing
	if (program->rtLightsGeneration == generation && program->rtLightsCount == count) {
		return;
	}

	program->rtLightsGeneration = generation;
	program->rtLightsCount      = count;
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], count * RT_LIGHT_VEC4S, &v[0][0]);
}

/*
Added in OPM
The sky's directions (tr.rtSkyRows): the same for the whole of a map, so sent
again only when they are drawn again.
*/
void GLSL_SetUniformRtSky(shaderProgram_t *program, int uniformNum, const vec4_t *v, int generation)
{
	GLint *uniforms = program->uniforms;

	if (uniforms[uniformNum] == -1) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_VEC4_RTSKY)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformRtSky: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (program->rtSkyGeneration == generation) {
		return;
	}

	program->rtSkyGeneration = generation;
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], RT_SKY_DIRS * 4, &v[0][0]);
}

/*
Added in OPM
The lights of a model the vertex program poses (tr_skelgpu.c), vec4s of them;
sent when they are not what the program has.
*/
void GLSL_SetUniformSkelLights(shaderProgram_t *program, int uniformNum, const vec4_t *v, int vec4s)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare  = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1 || vec4s <= 0) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_VEC4_SKELLIGHTS)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformSkelLights: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (vec4s > SKEL_GPU_MAX_LIGHTS * 3) {
		vec4s = SKEL_GPU_MAX_LIGHTS * 3;
	}
	if (!memcmp(compare, v, vec4s * sizeof(vec4_t))) {
		return;
	}
	Com_Memcpy(compare, v, vec4s * sizeof(vec4_t));
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], vec4s, &v[0][0]);
}

/*
Added in OPM
A posed model's gore dents (R_GoreDentsGpu), two vec4s each. Only the vec4s
that hold them are sent, and only when they are not what the program has.
*/
void GLSL_SetUniformSkelDents(shaderProgram_t *program, int uniformNum, const vec4_t *v, int vec4s)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare  = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1 || vec4s <= 0) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_VEC4_SKELDENTS)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformSkelDents: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (vec4s > SKEL_GPU_MAX_DENTS * 2) {
		vec4s = SKEL_GPU_MAX_DENTS * 2;
	}
	if (!memcmp(compare, v, vec4s * sizeof(vec4_t))) {
		return;
	}
	Com_Memcpy(compare, v, vec4s * sizeof(vec4_t));
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], vec4s, &v[0][0]);
}

/*
Added in OPM
A posed model's morph targets' weights, four a vec4. Only the vec4s that hold
them are sent, and only when they are not what the program has.
*/
void GLSL_SetUniformSkelMorphs(shaderProgram_t *program, int uniformNum, const vec4_t *v, int vec4s)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare  = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1 || vec4s <= 0) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_VEC4_SKELMORPHS)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformSkelMorphs: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (vec4s > SKEL_GPU_MAX_MORPHS / 4) {
		vec4s = SKEL_GPU_MAX_MORPHS / 4;
	}
	if (!memcmp(compare, v, vec4s * sizeof(vec4_t))) {
		return;
	}
	Com_Memcpy(compare, v, vec4s * sizeof(vec4_t));
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], vec4s, &v[0][0]);
}

/*
Added in OPM
The clouds of blood round bodies in water (tr_water.c), vec4s of them; sent
when they are not what the program has.
*/
void GLSL_SetUniformWaterSpots(shaderProgram_t *program, int uniformNum, const vec4_t *v, int vec4s)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare  = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1 || vec4s <= 0) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_VEC4_WATERSPOTS)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformWaterSpots: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (vec4s > WATER_MAX_SPOTS) {
		vec4s = WATER_MAX_SPOTS;
	}
	if (!memcmp(compare, v, vec4s * sizeof(vec4_t))) {
		return;
	}
	Com_Memcpy(compare, v, vec4s * sizeof(vec4_t));
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], vec4s, &v[0][0]);
}

/*
Added in OPM
The puffs of smoke in the air's grid (tr_froxel.c), three vec4s each; only
those sent.
*/
void GLSL_SetUniformSmoke(shaderProgram_t *program, int uniformNum, const vec4_t *v, int vec4s)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare  = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1 || vec4s <= 0) {
		return;
	}
	if (uniformsInfo[uniformNum].type != GLSL_VEC4_SMOKE) {
		ri.Printf(PRINT_WARNING, "GLSL_SetUniformSmoke: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}
	if (vec4s > SMOKE_MAX_BLOBS * 3) {
		vec4s = SMOKE_MAX_BLOBS * 3;
	}
	if (!memcmp(compare, v, vec4s * sizeof(vec4_t))) {
		return;
	}
	Com_Memcpy(compare, v, vec4s * sizeof(vec4_t));
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], vec4s, &v[0][0]);
}

/*
Added in OPM
The realtime lights a draw may take (R_RtSetUniforms), count indexes, four a
vec4. Only the vec4s that hold them are sent.
*/
void GLSL_SetUniformRtList(shaderProgram_t *program, int uniformNum, const vec4_t *v, int count)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare  = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);
	int    vec4s    = (count + 3) / 4;

	if (uniforms[uniformNum] == -1 || count <= 0) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_VEC4_RTLIST)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformRtList: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (vec4s > RT_MAX_LIGHTS / 4) {
		vec4s = RT_MAX_LIGHTS / 4;
	}

	if (!memcmp(v, compare, vec4s * sizeof(vec4_t))) {
		return;
	}

	Com_Memcpy(compare, v, vec4s * sizeof(vec4_t));
	qglProgramUniform4fvEXT(program->program, uniforms[uniformNum], vec4s, &v[0][0]);
}

void GLSL_SetUniformMat4BoneMatrix(shaderProgram_t *program, int uniformNum, /*const*/ mat4_t *matrix, int numMatricies)
{
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)(program->uniformBuffer + program->uniformBufferOffsets[uniformNum]);

	if (uniforms[uniformNum] == -1) {
		return;
	}

	if (uniformsInfo[uniformNum].type != GLSL_MAT16_BONEMATRIX)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4BoneMatrix: wrong type for uniform %i in program %s\n", uniformNum, program->name);
		return;
	}

	if (numMatricies > glRefConfig.glslMaxAnimatedBones)
	{
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4BoneMatrix: too many matricies (%d/%d) for uniform %i in program %s\n",
				numMatricies, glRefConfig.glslMaxAnimatedBones, uniformNum, program->name);
		return;
	}

	if (!memcmp(matrix, compare, numMatricies * sizeof(mat4_t)))
	{
		return;
	}

	Com_Memcpy(compare, matrix, numMatricies * sizeof(mat4_t));

	qglProgramUniformMatrix4fvEXT(program->program, uniforms[uniformNum], numMatricies, GL_FALSE, &matrix[0][0]);
}

void GLSL_DeleteGPUShader(shaderProgram_t *program)
{
	if(program->program)
	{
		if (program->vertexShader)
		{
			qglDetachShader(program->program, program->vertexShader);
			qglDeleteShader(program->vertexShader);
		}

		if (program->fragmentShader)
		{
			qglDetachShader(program->program, program->fragmentShader);
			qglDeleteShader(program->fragmentShader);
		}

		qglDeleteProgram(program->program);

		if (program->uniformBuffer)
		{
			ri.Free(program->uniformBuffer);
		}

		Com_Memset(program, 0, sizeof(*program));
	}
}

/*
============
GLSL_InitLightallShader

Build one lightall permutation, or return qfalse if that combination cannot
occur. Split out of GLSL_InitGPUShaders so the permutations describing which
material maps a surface carries can be built when one first turns up rather
than all at once -- stock MOH:AA content ships no normal, specular or deluxe
maps, so building every combination up front would spend nearly all of its time
on programs the game never binds.
============
*/
static qboolean GLSL_InitLightallShader(int i)
{
	char extradefines[1024];
	int attribs;

	int lightType = i & LIGHTDEF_LIGHTTYPE_MASK;
	qboolean fastLight = !(r_normalMapping->integer || r_specularMapping->integer);

	// skip impossible combos
	if ((i & LIGHTDEF_USE_PARALLAXMAP) && !r_parallaxMapping->integer)
		return qfalse;

	// The material-map bits say what a stage actually carries, so they only
	// ever appear on a lit stage, and only when the matching cvar allowed the
	// map to be loaded at all.
	if ((i & (LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_SPECULARMAP | LIGHTDEF_USE_DELUXEMAP)) && !lightType)
		return qfalse;

	if ((i & LIGHTDEF_USE_NORMALMAP) && !r_normalMapping->integer)
		return qfalse;

	if ((i & LIGHTDEF_USE_SPECULARMAP) && !r_specularMapping->integer)
		return qfalse;

	if ((i & LIGHTDEF_USE_DELUXEMAP) && (!r_deluxeMapping->integer || lightType != LIGHTDEF_USE_LIGHTMAP))
		return qfalse;

	// parallax height is sampled out of the normal map, so it cannot outlive it
	if ((i & LIGHTDEF_USE_PARALLAXMAP) && !(i & LIGHTDEF_USE_NORMALMAP))
		return qfalse;

	if ((i & LIGHTDEF_USE_SHADOWMAP) && (!lightType || !r_sunlightMode->integer))
		return qfalse;

	if ((i & LIGHTDEF_ENTITY_VERTEX_ANIMATION) && (i & LIGHTDEF_ENTITY_BONE_ANIMATION))
		return qfalse;

	if ((i & LIGHTDEF_ENTITY_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
		return qfalse;

	// Added in OPM: the tufts are world geometry with no material maps; their
	// tangent and lightmap arrays hold where each stands and how it sways
	if ((i & LIGHTDEF_GROUNDCOVER) && (i & (LIGHTDEF_ENTITY_VERTEX_ANIMATION | LIGHTDEF_ENTITY_BONE_ANIMATION
		| LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_SPECULARMAP | LIGHTDEF_USE_DELUXEMAP | LIGHTDEF_USE_PARALLAXMAP)))
		return qfalse;

	// (lit by its vertexes it may be: the tufts bring no light direction, and
	// their colour, all of the light live, gives way to the ambient)
	if ((i & LIGHTDEF_GROUNDCOVER) && lightType == LIGHTDEF_USE_LIGHTMAP)
		return qfalse;

	// Added in OPM: a skeletal model posed from the frame's bones, which
	// brings its own vertexes, and nothing that reads others
	if ((i & LIGHTDEF_SKEL_GPU) && (i & (LIGHTDEF_ENTITY_VERTEX_ANIMATION | LIGHTDEF_ENTITY_BONE_ANIMATION | LIGHTDEF_GROUNDCOVER)))
		return qfalse;

	if ((i & LIGHTDEF_SKEL_GPU) && lightType == LIGHTDEF_USE_LIGHTMAP)
		return qfalse;

	attribs = ATTR_POSITION | ATTR_TEXCOORD | ATTR_COLOR | ATTR_NORMAL;

	extradefines[0] = '\0';

	if (r_dlightMode->integer >= 2)
		Q_strcat(extradefines, 1024, "#define USE_SHADOWMAP\n");

	if (glRefConfig.swizzleNormalmap)
		Q_strcat(extradefines, 1024, "#define SWIZZLE_NORMALMAP\n");

	if (lightType)
	{
		Q_strcat(extradefines, 1024, "#define USE_LIGHT\n");

		if (fastLight)
			Q_strcat(extradefines, 1024, "#define USE_FAST_LIGHT\n");

		switch (lightType)
		{
			case LIGHTDEF_USE_LIGHTMAP:
				Q_strcat(extradefines, 1024, "#define USE_LIGHTMAP\n");
				if ((i & LIGHTDEF_USE_DELUXEMAP) && r_deluxeMapping->integer && !fastLight)
					Q_strcat(extradefines, 1024, "#define USE_DELUXEMAP\n");
				attribs |= ATTR_LIGHTCOORD | ATTR_LIGHTDIRECTION;
				break;
			case LIGHTDEF_USE_LIGHT_VECTOR:
				Q_strcat(extradefines, 1024, "#define USE_LIGHT_VECTOR\n");
				break;
			case LIGHTDEF_USE_LIGHT_VERTEX:
				Q_strcat(extradefines, 1024, "#define USE_LIGHT_VERTEX\n");
				attribs |= ATTR_LIGHTDIRECTION;
				break;
			default:
				break;
		}

		if ((i & LIGHTDEF_USE_NORMALMAP) && r_normalMapping->integer)
		{
			Q_strcat(extradefines, 1024, "#define USE_NORMALMAP\n");

			attribs |= ATTR_TANGENT;

			if ((i & LIGHTDEF_USE_PARALLAXMAP) && !(i & (LIGHTDEF_ENTITY_VERTEX_ANIMATION | LIGHTDEF_ENTITY_BONE_ANIMATION | LIGHTDEF_SKEL_GPU)) && r_parallaxMapping->integer)
			{
				Q_strcat(extradefines, 1024, "#define USE_PARALLAXMAP\n");
				if (r_parallaxMapping->integer > 1)
					Q_strcat(extradefines, 1024, "#define USE_RELIEFMAP\n");

				if (r_parallaxMapShadows->integer)
					Q_strcat(extradefines, 1024, "#define USE_PARALLAXMAP_SHADOWS\n");

				Q_strcat(extradefines, 1024, va("#define r_parallaxMapOffset %f\n", r_parallaxMapOffset->value));
			}
		}

		if ((i & LIGHTDEF_USE_SPECULARMAP) && r_specularMapping->integer)
			Q_strcat(extradefines, 1024, "#define USE_SPECULARMAP\n");

		// Added in OPM: reflections of what is on the screen (R_SsrSetUniforms)
		if (r_ssr->integer)
			Q_strcat(extradefines, 1024, "#define USE_SSR\n");

		if (r_cubeMapping->integer)
		{
			Q_strcat(extradefines, 1024, "#define USE_CUBEMAP\n");
			if (r_cubeMapping->integer == 2)
				Q_strcat(extradefines, 1024, "#define USE_BOX_CUBEMAP_PARALLAX\n");
		}
		else if (r_deluxeSpecular->value > 0.000001f)
		{
			Q_strcat(extradefines, 1024, va("#define r_deluxeSpecular %f\n", r_deluxeSpecular->value));
		}

		switch (r_glossType->integer)
		{
			case 0:
			default:
				Q_strcat(extradefines, 1024, "#define GLOSS_IS_GLOSS\n");
				break;
			case 1:
				Q_strcat(extradefines, 1024, "#define GLOSS_IS_SMOOTHNESS\n");
				break;
			case 2:
				Q_strcat(extradefines, 1024, "#define GLOSS_IS_ROUGHNESS\n");
				break;
			case 3:
				Q_strcat(extradefines, 1024, "#define GLOSS_IS_SHININESS\n");
				break;
		}
	}

	// Added in OPM
	//  With all of the light live (r_realtimeLighting 2) the sun is one of the
	//  realtime lights (RtLight), and reads its screen shadow itself: it no
	//  longer darkens the lightmap. With the direct light live alone (1) it
	//  darkens it as ever.
	if (r_realtimeLighting->integer)
		Q_strcat(extradefines, 1024, "#define USE_RTLIGHT\n");
	if (R_RtVertexSky())
		Q_strcat(extradefines, 1024, "#define USE_RTSKY_VERTEX\n");

	if ((i & LIGHTDEF_USE_SHADOWMAP) && r_realtimeLighting->integer != 2)
	{
		Q_strcat(extradefines, 1024, "#define USE_SHADOWMAP\n");

		if (r_sunlightMode->integer == 1)
			Q_strcat(extradefines, 1024, "#define SHADOWMAP_MODULATE\n");
		else if (r_sunlightMode->integer == 2)
			Q_strcat(extradefines, 1024, "#define USE_PRIMARY_LIGHT\n");
	}

	if (i & LIGHTDEF_USE_TCGEN_AND_TCMOD)
	{
		Q_strcat(extradefines, 1024, "#define USE_TCGEN\n");
		Q_strcat(extradefines, 1024, "#define USE_TCMOD\n");
	}

	if (i & LIGHTDEF_GROUNDCOVER)
	{
		Q_strcat(extradefines, 1024, "#define USE_GROUNDCOVER\n");
		attribs |= ATTR_TANGENT | ATTR_LIGHTCOORD;
	}

	if (i & LIGHTDEF_SKEL_GPU)
	{
		Q_strcat(extradefines, 1024, va("#define USE_SKEL_GPU\n#define USE_MODELMATRIX\n#define SKEL_MAX_LIGHTS %d\n#define SKEL_BONE_ROW %d\n#define SKEL_MAX_DENTS %d\n#define SKEL_MAX_MORPHS %d\n", SKEL_GPU_MAX_LIGHTS, SKEL_GPU_BONE_ROW, SKEL_GPU_MAX_DENTS, SKEL_GPU_MAX_MORPHS));
		attribs |= ATTR_POSITION2 | ATTR_NORMAL2 | ATTR_TANGENT2 | ATTR_BONE_INDEXES
			| ATTR_TANGENT | ATTR_LIGHTCOORD | ATTR_PAINTCOLOR | ATTR_BONE_WEIGHTS | ATTR_LIGHTDIRECTION;
	}

	if (i & LIGHTDEF_ENTITY_VERTEX_ANIMATION)
	{
		Q_strcat(extradefines, 1024, "#define USE_MODELMATRIX\n");

		if (glRefConfig.gpuVertexAnimation)
		{
			Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;

			if ((i & LIGHTDEF_USE_NORMALMAP) && r_normalMapping->integer)
			{
				attribs |= ATTR_TANGENT2;
			}
		}
	}
	else if (i & LIGHTDEF_ENTITY_BONE_ANIMATION)
	{
		Q_strcat(extradefines, 1024, "#define USE_MODELMATRIX\n");
		Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
		attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
	}

	// Added in OPM: the light in the air, for what is drawn after it
	if (r_volumetric->integer == 2 && r_realtimeLighting->integer == 2)
		Q_strcat(extradefines, 1024, "#define USE_FROXEL_FOG\n");

	if (!GLSL_InitGPUShader(&tr.lightallShader[i], "lightall", attribs, qtrue, extradefines, qtrue, fallbackShader_lightall_vp, fallbackShader_lightall_fp))
	{
		ri.Error(ERR_FATAL, "Could not load lightall shader!");
	}

	GLSL_InitUniforms(&tr.lightallShader[i]);

	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_DIFFUSEMAP,  TB_DIFFUSEMAP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_LIGHTMAP,    TB_LIGHTMAP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_NORMALMAP,   TB_NORMALMAP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_DELUXEMAP,   TB_DELUXEMAP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SPECULARMAP, TB_SPECULARMAP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SHADOWMAP,   TB_SHADOWMAP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_CUBEMAP,     TB_CUBEMAP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSHADOWSTATIC,  TMU_RTSHADOW_STATIC);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSHADOWDYNAMIC, TMU_RTSHADOW_DYNAMIC);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSUNSHADOW,     TMU_RTSUNSHADOW);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSUNCASCADE,    TMU_RTSUNCASCADE);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSHADOWBAKED,   TMU_RTSHADOW_BAKED);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTCAPSULES,      TMU_RTCAPSULES);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSKYMAP,        TMU_RTSKY);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSKYLIGHTMAP,   TMU_RTSKYLM);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTBOUNCEMAP,     TMU_RTBOUNCE);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTAOMAP,         TMU_RTAO);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTBOUNCELAMPMAP, TMU_RTBOUNCELAMP);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTDIRECTMAP,     TMU_RTDIRECT);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTDIRECTDIR,     TMU_RTDIRECTDIR);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTLIVEMAP,       TMU_RTLIVE);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTLIVEDIR,       TMU_RTLIVEDIR);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SKELBONES,       TMU_SKELBONES);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SKELMORPHMAP,    TMU_SKELMORPHS);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RAINCOVERMAP,    TMU_RAINCOVER);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RAINREFLECTMAP,  TMU_RAINREFLECT);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SSRCOLORMAP,     TMU_SSRCOLOR);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SSRDEPTHMAP,     TMU_SSRDEPTH);

	GLSL_FinishGPUShader(&tr.lightallShader[i]);


	return qtrue;
}

/*
============
GLSL_GetLightallShader

Hand back a lightall permutation, building it the first time it is asked for.
============
*/
shaderProgram_t *GLSL_GetLightallShader(int index)
{
	if (!tr.lightallShader[index].program)
	{
		GLSL_InitLightallShader(index);

		// The material cvars are all CVAR_LATCH, so a stage's bits and the
		// permutations built for them cannot disagree without a vid_restart
		// rebuilding both. Should some combination still slip through, drop
		// the material maps rather than bind program zero: the result is the
		// pre-change rendering path, not a black screen.
		if (!tr.lightallShader[index].program)
		{
			index &= ~(LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_SPECULARMAP | LIGHTDEF_USE_DELUXEMAP | LIGHTDEF_USE_PARALLAXMAP);

			if (!tr.lightallShader[index].program)
				GLSL_InitLightallShader(index);
		}
	}

	return &tr.lightallShader[index];
}

/*
============
GLSL_InitGenericShader

Added in OPM: one generic permutation. Slim, its twin for the stages that do
not run the realtime lights' loop per pixel (dust and smoke, lit at their
corners, and what the lights leave alone): the loop is left out of its
fragment shader, whose registers alone (never run) took the pixels of a smoke
layer twice as long.
============
*/
static qboolean GLSL_InitGenericShader(shaderProgram_t *sp, int i, qboolean slim)
{
	char extradefines[1024];
	int  attribs;

	if ((i & GENERICDEF_USE_VERTEX_ANIMATION) && (i & GENERICDEF_USE_BONE_ANIMATION))
		return qfalse;

	if ((i & GENERICDEF_USE_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
		return qfalse;

	attribs = ATTR_POSITION | ATTR_TEXCOORD | ATTR_LIGHTCOORD | ATTR_NORMAL | ATTR_COLOR;
	extradefines[0] = '\0';

	if (i & GENERICDEF_USE_DEFORM_VERTEXES)
		Q_strcat(extradefines, 1024, "#define USE_DEFORM_VERTEXES\n");

	if (i & GENERICDEF_USE_TCGEN_AND_TCMOD)
	{
		Q_strcat(extradefines, 1024, "#define USE_TCGEN\n");
		Q_strcat(extradefines, 1024, "#define USE_TCMOD\n");
	}

	if (i & GENERICDEF_USE_VERTEX_ANIMATION)
	{
		if (!glRefConfig.gpuVertexAnimation)
			return qfalse;

		Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
		attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
	}
	else if (i & GENERICDEF_USE_BONE_ANIMATION)
	{
		Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
		attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
	}

	if (i & GENERICDEF_USE_FOG)
		Q_strcat(extradefines, 1024, "#define USE_FOG\n");

	if (i & GENERICDEF_USE_RGBAGEN)
		Q_strcat(extradefines, 1024, "#define USE_RGBAGEN\n");

	// Added in OPM
	if (r_realtimeLighting->integer)
		Q_strcat(extradefines, 1024, "#define USE_RTLIGHT\n");
	if (R_RtVertexParticles())
		Q_strcat(extradefines, 1024, "#define USE_RTLIGHT_PARTICLES\n");
	if (R_RtVertexSky())
		Q_strcat(extradefines, 1024, "#define USE_RTSKY_VERTEX\n");
	if (r_softParticles->value > 0)
		Q_strcat(extradefines, 1024, "#define USE_SOFTPARTICLES\n");
	if (slim)
		Q_strcat(extradefines, 1024, "#define RT_NO_PIXEL_LOOP\n");
	if (r_volumetric->integer == 2 && r_realtimeLighting->integer == 2)
		Q_strcat(extradefines, 1024, "#define USE_FROXEL_FOG\n");

	if (!GLSL_InitGPUShader(sp, "generic", attribs, qtrue, extradefines, qtrue, fallbackShader_generic_vp, fallbackShader_generic_fp))
	{
		ri.Error(ERR_FATAL, "Could not load generic shader!");
	}

	GLSL_InitUniforms(sp);

	GLSL_SetUniformInt(sp, UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);
	GLSL_SetUniformInt(sp, UNIFORM_LIGHTMAP,   TB_LIGHTMAP);
	GLSL_SetUniformInt(sp, UNIFORM_RTSHADOWSTATIC,  TMU_RTSHADOW_STATIC);
	GLSL_SetUniformInt(sp, UNIFORM_RTSHADOWDYNAMIC, TMU_RTSHADOW_DYNAMIC);
	GLSL_SetUniformInt(sp, UNIFORM_RTSUNSHADOW,     TMU_RTSUNSHADOW);
	GLSL_SetUniformInt(sp, UNIFORM_RTSUNCASCADE,    TMU_RTSUNCASCADE);
	GLSL_SetUniformInt(sp, UNIFORM_RTSHADOWBAKED,   TMU_RTSHADOW_BAKED);
	GLSL_SetUniformInt(sp, UNIFORM_RTCAPSULES,      TMU_RTCAPSULES);
	GLSL_SetUniformInt(sp, UNIFORM_RTSKYMAP,        TMU_RTSKY);
	GLSL_SetUniformInt(sp, UNIFORM_RTSKYLIGHTMAP,   TMU_RTSKYLM);
	GLSL_SetUniformInt(sp, UNIFORM_RAINCOVERMAP,    TMU_RAINCOVER);
	GLSL_SetUniformInt(sp, UNIFORM_RAINREFLECTMAP,  TMU_RAINREFLECT);
	GLSL_SetUniformInt(sp, UNIFORM_RTBOUNCEMAP,     TMU_RTBOUNCE);
	GLSL_SetUniformInt(sp, UNIFORM_RTAOMAP,         TMU_RTAO);
	GLSL_SetUniformInt(sp, UNIFORM_RTBOUNCELAMPMAP, TMU_RTBOUNCELAMP);
	GLSL_SetUniformInt(sp, UNIFORM_RTDIRECTMAP,     TMU_RTDIRECT);
	GLSL_SetUniformInt(sp, UNIFORM_RTDIRECTDIR,     TMU_RTDIRECTDIR);
	GLSL_SetUniformInt(sp, UNIFORM_RTLIVEMAP,       TMU_RTLIVE);
	GLSL_SetUniformInt(sp, UNIFORM_RTLIVEDIR,       TMU_RTLIVEDIR);
	GLSL_SetUniformInt(sp, UNIFORM_SCREENDEPTHMAP,  TMU_SOFTDEPTH);

	GLSL_FinishGPUShader(sp);

	return qtrue;
}

/*
============
GLSL_GetGenericSlimShader

The slim twin of a generic permutation (GLSL_InitGenericShader), built the
first time it is asked for.
============
*/
shaderProgram_t *GLSL_GetGenericSlimShader(const shaderProgram_t *generic)
{
	const int i = (int)(generic - tr.genericShader);
	shaderProgram_t *sp;

	if (i < 0 || i >= GENERICDEF_COUNT)
		return (shaderProgram_t *)generic;

	sp = &tr.genericSlimShader[i];
	if (!sp->program && !GLSL_InitGenericShader(sp, i, qtrue))
		return (shaderProgram_t *)generic;

	return sp;
}

void GLSL_InitGPUShaders(void)
{
	int             startTime, endTime;
	int i;
	char extradefines[1024];
	int attribs;
	int numGenShaders = 0, numLightShaders = 0, numEtcShaders = 0;

	ri.Printf(PRINT_ALL, "------- GLSL_InitGPUShaders -------\n");

	R_IssuePendingRenderCommands();

	startTime = ri.Milliseconds();

	// OpenGL ES may not have enough attributes to fit ones used for vertex animation
	if ( glRefConfig.maxVertexAttribs > ATTR_INDEX_NORMAL2 ) {
		ri.Printf(PRINT_ALL, "Using GPU vertex animation\n");
		glRefConfig.gpuVertexAnimation = qtrue;
	} else {
		ri.Printf(PRINT_ALL, "Using CPU vertex animation\n");
		glRefConfig.gpuVertexAnimation = qfalse;
	}

	for (i = 0; i < GENERICDEF_COUNT; i++)
	{
		if (GLSL_InitGenericShader(&tr.genericShader[i], i, qfalse))
			numGenShaders++;

		// the slim twins now, not as the first muzzle flash is drawn (a
		// compile mid-game is a hitch); those for animated models, which
		// would double the time this takes, are built if ever drawn
		if (r_realtimeLighting->integer && !(i & (GENERICDEF_USE_VERTEX_ANIMATION | GENERICDEF_USE_BONE_ANIMATION))
			&& GLSL_InitGenericShader(&tr.genericSlimShader[i], i, qtrue))
			numGenShaders++;
	}


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.textureColorShader, "texturecolor", attribs, qtrue, extradefines, qtrue, fallbackShader_texturecolor_vp, fallbackShader_texturecolor_fp))
	{
		ri.Error(ERR_FATAL, "Could not load texturecolor shader!");
	}
	
	GLSL_InitUniforms(&tr.textureColorShader);

	GLSL_SetUniformInt(&tr.textureColorShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.textureColorShader);

	// Added in OPM: the sky a lightmap texel sees, once a map (R_RtBakeSkyLightmaps)
	if (r_realtimeLighting->integer == 2)
	{
		if (!GLSL_InitGPUShader(&tr.rtSkyBakeShader, "rtskybake", ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD,
			qtrue, "#define USE_RTLIGHT\n", qtrue, rt_skyBake_vp, rt_skyBake_fp))
		{
			ri.Printf(PRINT_WARNING, "realtime lighting: no program for the sky's lightmaps\n");
		}
		else
		{
			GLSL_InitUniforms(&tr.rtSkyBakeShader);
			GLSL_SetUniformInt(&tr.rtSkyBakeShader, UNIFORM_RTSKYMAP, TMU_RTSKY);
			GLSL_FinishGPUShader(&tr.rtSkyBakeShader);
			numEtcShaders++;
		}

		// and the lamps' light on them (R_RtBakeLamps)
		if (!GLSL_InitGPUShader(&tr.rtLampBakeShader, "rtlampbake", ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD,
			qtrue, "#define USE_RTLIGHT\n", qtrue, rt_skyBake_vp, rt_lampBake_fp))
		{
			ri.Printf(PRINT_WARNING, "realtime lighting: no program for the baked lamps\n");
		}
		else
		{
			GLSL_InitUniforms(&tr.rtLampBakeShader);
			GLSL_SetUniformInt(&tr.rtLampBakeShader, UNIFORM_RTSHADOWBAKED, TMU_RTSHADOW_BAKED);
			GLSL_FinishGPUShader(&tr.rtLampBakeShader);
			numEtcShaders++;
		}

		// and that of those lit live, the texels no surface covers filled (R_RtLivePass)
		if (!GLSL_InitGPUShader(&tr.rtBakeFillShader, "rtbakefill", ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD,
			qtrue, "#define USE_RTLIGHT\n", qtrue, rt_skyBake_vp, rt_bakeFill_fp))
		{
			ri.Printf(PRINT_WARNING, "realtime lighting: no program for the baked lamps lit live\n");
		}
		else
		{
			GLSL_InitUniforms(&tr.rtBakeFillShader);
			GLSL_SetUniformInt(&tr.rtBakeFillShader, UNIFORM_RTDIRECTMAP, TMU_RTDIRECT);
			GLSL_SetUniformInt(&tr.rtBakeFillShader, UNIFORM_RTDIRECTDIR, TMU_RTDIRECTDIR);
			GLSL_FinishGPUShader(&tr.rtBakeFillShader);
			numEtcShaders++;
		}
	}

	numEtcShaders++;

	for (i = 0; i < FOGDEF_COUNT; i++)
	{
		if ((i & FOGDEF_USE_VERTEX_ANIMATION) && (i & FOGDEF_USE_BONE_ANIMATION))
			continue;

		if ((i & FOGDEF_USE_VERTEX_ANIMATION) && !glRefConfig.gpuVertexAnimation)
			continue;

		if ((i & FOGDEF_USE_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
			continue;

		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (i & FOGDEF_USE_DEFORM_VERTEXES)
			Q_strcat(extradefines, 1024, "#define USE_DEFORM_VERTEXES\n");

		if (i & FOGDEF_USE_VERTEX_ANIMATION)
		{
			Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		}
		else if (i & FOGDEF_USE_BONE_ANIMATION)
		{
			Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if (!GLSL_InitGPUShader(&tr.fogShader[i], "fogpass", attribs, qtrue, extradefines, qtrue, fallbackShader_fogpass_vp, fallbackShader_fogpass_fp))
		{
			ri.Error(ERR_FATAL, "Could not load fogpass shader!");
		}

		GLSL_InitUniforms(&tr.fogShader[i]);
		GLSL_FinishGPUShader(&tr.fogShader[i]);

		numEtcShaders++;
	}


	for (i = 0; i < DLIGHTDEF_COUNT; i++)
	{
		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (i & DLIGHTDEF_USE_DEFORM_VERTEXES)
		{
			Q_strcat(extradefines, 1024, "#define USE_DEFORM_VERTEXES\n");
		}

		// Added in OPM: the light in the air, over what is drawn after it
		if (r_volumetric->integer == 2 && r_realtimeLighting->integer == 2)
			Q_strcat(extradefines, 1024, "#define USE_FROXEL_FOG\n");

		if (!GLSL_InitGPUShader(&tr.dlightShader[i], "dlight", attribs, qtrue, extradefines, qtrue, fallbackShader_dlight_vp, fallbackShader_dlight_fp))
		{
			ri.Error(ERR_FATAL, "Could not load dlight shader!");
		}

		GLSL_InitUniforms(&tr.dlightShader[i]);
		
		GLSL_SetUniformInt(&tr.dlightShader[i], UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);

		GLSL_FinishGPUShader(&tr.dlightShader[i]);

		numEtcShaders++;
	}


	for (i = 0; i < LIGHTDEF_COUNT; i++)
	{
		// Pre-build every permutation a surface with no material maps can
		// need, which covers all of stock MOH:AA. Anything carrying a normal,
		// specular or deluxe map is left to GLSL_GetLightallShader.
		if (i & (LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_SPECULARMAP | LIGHTDEF_USE_DELUXEMAP | LIGHTDEF_GROUNDCOVER | LIGHTDEF_SKEL_GPU))
			continue;

		if (!GLSL_InitLightallShader(i))
			continue;

		numLightShaders++;
	}

	for (i = 0; i < SHADOWMAPDEF_COUNT; i++)
	{
		if ((i & SHADOWMAPDEF_USE_VERTEX_ANIMATION) && (i & SHADOWMAPDEF_USE_BONE_ANIMATION))
			continue;

		if ((i & SHADOWMAPDEF_USE_VERTEX_ANIMATION) && !glRefConfig.gpuVertexAnimation)
			continue;

		if ((i & SHADOWMAPDEF_USE_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
			continue;

		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;

		extradefines[0] = '\0';

		if (i & SHADOWMAPDEF_USE_VERTEX_ANIMATION)
		{
			Q_strcat(extradefines, 1024, "#define USE_VERTEX_ANIMATION\n");
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		}

		if (i & SHADOWMAPDEF_USE_BONE_ANIMATION)
		{
			Q_strcat(extradefines, 1024, va("#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones));
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if (!GLSL_InitGPUShader(&tr.shadowmapShader[i], "shadowfill", attribs, qtrue, extradefines, qtrue, fallbackShader_shadowfill_vp, fallbackShader_shadowfill_fp))
		{
			ri.Error(ERR_FATAL, "Could not load shadowfill shader!");
		}

		GLSL_InitUniforms(&tr.shadowmapShader[i]);
		GLSL_FinishGPUShader(&tr.shadowmapShader[i]);

		numEtcShaders++;
	}

	attribs = ATTR_POSITION | ATTR_NORMAL;
	extradefines[0] = '\0';

	Q_strcat(extradefines, 1024, "#define USE_PCF\n#define USE_DISCARD\n");
	// Added in OPM: the light in the air, over what is drawn after it
	if (r_volumetric->integer == 2 && r_realtimeLighting->integer == 2)
		Q_strcat(extradefines, 1024, "#define USE_FROXEL_FOG\n");

	if (!GLSL_InitGPUShader(&tr.pshadowShader, "pshadow", attribs, qtrue, extradefines, qtrue, fallbackShader_pshadow_vp, fallbackShader_pshadow_fp))
	{
		ri.Error(ERR_FATAL, "Could not load pshadow shader!");
	}
	
	GLSL_InitUniforms(&tr.pshadowShader);

	GLSL_SetUniformInt(&tr.pshadowShader, UNIFORM_SHADOWMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.pshadowShader);

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.down4xShader, "down4x", attribs, qtrue, extradefines, qtrue, fallbackShader_down4x_vp, fallbackShader_down4x_fp))
	{
		ri.Error(ERR_FATAL, "Could not load down4x shader!");
	}
	
	GLSL_InitUniforms(&tr.down4xShader);

	GLSL_SetUniformInt(&tr.down4xShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.down4xShader);

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.bokehShader, "bokeh", attribs, qtrue, extradefines, qtrue, fallbackShader_bokeh_vp, fallbackShader_bokeh_fp))
	{
		ri.Error(ERR_FATAL, "Could not load bokeh shader!");
	}

	GLSL_InitUniforms(&tr.bokehShader);

	GLSL_SetUniformInt(&tr.bokehShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.bokehShader);

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.tonemapShader, "tonemap", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_tonemap_fp))
	{
		ri.Error(ERR_FATAL, "Could not load tonemap shader!");
	}

	GLSL_InitUniforms(&tr.tonemapShader);

	GLSL_SetUniformInt(&tr.tonemapShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);
	GLSL_SetUniformInt(&tr.tonemapShader, UNIFORM_LEVELSMAP,  TB_LEVELSMAP);

	GLSL_FinishGPUShader(&tr.tonemapShader);

	numEtcShaders++;


	for (i = 0; i < 2; i++)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (!i)
			Q_strcat(extradefines, 1024, "#define FIRST_PASS\n");

		if (!GLSL_InitGPUShader(&tr.calclevels4xShader[i], "calclevels4x", attribs, qtrue, extradefines, qtrue, fallbackShader_calclevels4x_vp, fallbackShader_calclevels4x_fp))
		{
			ri.Error(ERR_FATAL, "Could not load calclevels4x shader!");
		}

		GLSL_InitUniforms(&tr.calclevels4xShader[i]);

		GLSL_SetUniformInt(&tr.calclevels4xShader[i], UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

		GLSL_FinishGPUShader(&tr.calclevels4xShader[i]);

		numEtcShaders++;		
	}


	// GLSL 1.10+ or GL_EXT_shadow_samplers extension are required for sampler2DShadow type
	if (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 10)
	    || glRefConfig.shadowSamplers)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (qglesMajorVersion < 3 && glRefConfig.shadowSamplers)
		{
			Q_strcat(extradefines, 1024, "#extension GL_EXT_shadow_samplers : enable\n");
		}

		if (r_shadowFilter->integer >= 1)
			Q_strcat(extradefines, 1024, "#define USE_SHADOW_FILTER\n");

		if (r_shadowFilter->integer >= 2)
			Q_strcat(extradefines, 1024, "#define USE_SHADOW_FILTER2\n");

		if (r_shadowCascadeZFar->integer != 0)
			Q_strcat(extradefines, 1024, "#define USE_SHADOW_CASCADE\n");

		Q_strcat(extradefines, 1024, va("#define r_shadowMapSize %f\n", r_shadowMapSize->value));
		Q_strcat(extradefines, 1024, va("#define r_sunShadowFarSize %f\n", (float)R_SunShadowFarSize()));
		Q_strcat(extradefines, 1024, va("#define r_shadowCascadeZFar %f\n", r_shadowCascadeZFar->value));

		if (!GLSL_InitGPUShader(&tr.shadowmaskShader, "shadowmask", attribs, qtrue, extradefines, qtrue, fallbackShader_shadowmask_vp, fallbackShader_shadowmask_fp))
		{
			ri.Error(ERR_FATAL, "Could not load shadowmask shader!");
		}
	
		GLSL_InitUniforms(&tr.shadowmaskShader);

		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP,  TB_SHADOWMAP);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP2, TB_SHADOWMAP2);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP3, TB_SHADOWMAP3);
		GLSL_SetUniformInt(&tr.shadowmaskShader, UNIFORM_SHADOWMAP4, TB_SHADOWMAP4);

		GLSL_FinishGPUShader(&tr.shadowmaskShader);

		numEtcShaders++;
	}

	// Added in OPM: the light in the air, which reads the realtime lights
	if (r_volumetric->integer == 1 && r_realtimeLighting->integer == 2)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		Q_strncpyz(extradefines, "#define USE_RTLIGHT\n", sizeof(extradefines));

		if (!GLSL_InitGPUShader(&tr.volumetricShader, "volumetric", attribs, qtrue, extradefines, qtrue, fallbackShader_volumetric_vp, fallbackShader_volumetric_fp))
		{
			ri.Error(ERR_FATAL, "Could not load volumetric shader!");
		}

		GLSL_InitUniforms(&tr.volumetricShader);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_SHADOWMAP,  TB_SHADOWMAP);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_SHADOWMAP2, TB_SHADOWMAP2);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_SHADOWMAP3, TB_SHADOWMAP3);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_SHADOWMAP4, TB_SHADOWMAP4);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_RTSHADOWSTATIC,  TMU_RTSHADOW_STATIC);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_RTSHADOWDYNAMIC, TMU_RTSHADOW_DYNAMIC);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_RTSHADOWBAKED,   TMU_RTSHADOW_BAKED);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_RTSUNSHADOW,     TMU_RTSUNSHADOW);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_RTSUNCASCADE,    TMU_RTSUNCASCADE);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_RTCAPSULES,      TMU_RTCAPSULES);
		GLSL_SetUniformInt(&tr.volumetricShader, UNIFORM_RTSKYMAP,        TMU_RTSKY);
		GLSL_FinishGPUShader(&tr.volumetricShader);

		numEtcShaders++;
	}

	// Added in OPM: r_volumetric 2, the light in the air in a grid of cells
	if (r_volumetric->integer == 2 && r_realtimeLighting->integer == 2)
	{
		static const char *passes[4] = { "#define FROXEL_LIGHT\n", "#define FROXEL_INTEGRATE\n", "#define FROXEL_APPLY\n", "#define FROXEL_DEPTH\n" };

		for (i = 0; i < 4; i++)
		{
			shaderProgram_t *sp = &tr.froxelShader[i];

			attribs = ATTR_POSITION | ATTR_TEXCOORD;
			Q_strncpyz(extradefines, "#define USE_RTLIGHT\n", sizeof(extradefines));
			Q_strcat(extradefines, sizeof(extradefines), passes[i]);
			Q_strcat(extradefines, sizeof(extradefines), va("#define WATER_MAX_SPOTS %d\n#define SMOKE_MAX_BLOBS %d\n", WATER_MAX_SPOTS, SMOKE_MAX_BLOBS));

			if (!GLSL_InitGPUShader(sp, "froxel", attribs, qtrue, extradefines, qtrue, fallbackShader_froxel_vp, fallbackShader_froxel_fp))
			{
				ri.Error(ERR_FATAL, "Could not load froxel shader!");
			}

			GLSL_InitUniforms(sp);
			GLSL_SetUniformInt(sp, UNIFORM_SCREENIMAGEMAP, TB_COLORMAP);
			// the light pass's tile depths off TB_LIGHTMAP, which is the
			// sun's third cascade (TB_SHADOWMAP3) there
			GLSL_SetUniformInt(sp, UNIFORM_SCREENDEPTHMAP, i == 0 ? TB_NORMALMAP : TB_LIGHTMAP);
			GLSL_SetUniformInt(sp, UNIFORM_SHADOWMAP,  TB_SHADOWMAP);
			GLSL_SetUniformInt(sp, UNIFORM_SHADOWMAP2, TB_SHADOWMAP2);
			GLSL_SetUniformInt(sp, UNIFORM_SHADOWMAP3, TB_SHADOWMAP3);
			GLSL_SetUniformInt(sp, UNIFORM_SHADOWMAP4, TB_SHADOWMAP4);
			GLSL_SetUniformInt(sp, UNIFORM_RTSHADOWSTATIC,  TMU_RTSHADOW_STATIC);
			GLSL_SetUniformInt(sp, UNIFORM_RTSHADOWDYNAMIC, TMU_RTSHADOW_DYNAMIC);
			GLSL_SetUniformInt(sp, UNIFORM_RTSHADOWBAKED,   TMU_RTSHADOW_BAKED);
			GLSL_SetUniformInt(sp, UNIFORM_RTSUNSHADOW,     TMU_RTSUNSHADOW);
			GLSL_SetUniformInt(sp, UNIFORM_RTSUNCASCADE,    TMU_RTSUNCASCADE);
			GLSL_SetUniformInt(sp, UNIFORM_RTCAPSULES,      TMU_RTCAPSULES);
			GLSL_SetUniformInt(sp, UNIFORM_RTSKYMAP,        TMU_RTSKY);
			GLSL_SetUniformInt(sp, UNIFORM_SMOKENOISE,      TB_SPECULARMAP);
			GLSL_FinishGPUShader(sp);

			numEtcShaders++;
		}
	}


	if (!GLSL_InitGPUShader(&tr.greyscaleShader, "greyscale", attribs, qtrue, extradefines, qtrue,
		fallbackShader_greyscale_vp, fallbackShader_greyscale_fp))
	{
		ri.Error(ERR_FATAL, "Unable to load greyscale shader");
	}

	GLSL_InitUniforms(&tr.greyscaleShader);
	GLSL_SetUniformInt(&tr.greyscaleShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);
	GLSL_FinishGPUShader(&tr.greyscaleShader);

	numEtcShaders++;

	// GLSL 1.10+ or GL_OES_standard_derivatives extension are required for dFdx() and dFdy() GLSL functions
	if (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 10)
	    || glRefConfig.standardDerivatives)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if (qglesMajorVersion < 3 && glRefConfig.standardDerivatives)
		{
			Q_strcat(extradefines, 1024, "#extension GL_OES_standard_derivatives : enable\n");
		}

		if (!GLSL_InitGPUShader(&tr.ssaoShader, "ssao", attribs, qtrue, extradefines, qtrue, fallbackShader_ssao_vp, fallbackShader_ssao_fp))
		{
			ri.Error(ERR_FATAL, "Could not load ssao shader!");
		}

		GLSL_InitUniforms(&tr.ssaoShader);

		GLSL_SetUniformInt(&tr.ssaoShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);

		GLSL_FinishGPUShader(&tr.ssaoShader);

		numEtcShaders++;

		// Added in OPM: r_gtao's pass, with all of the light live
		if (r_gtao->integer && r_realtimeLighting->integer == 2)
		{
			if (!GLSL_InitGPUShader(&tr.gtaoShader, "gtao", attribs, qtrue, extradefines, qtrue, fallbackShader_gtao_vp, fallbackShader_gtao_fp))
			{
				ri.Error(ERR_FATAL, "Could not load gtao shader!");
			}
			GLSL_InitUniforms(&tr.gtaoShader);
			GLSL_SetUniformInt(&tr.gtaoShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);
			GLSL_FinishGPUShader(&tr.gtaoShader);
			numEtcShaders++;
		}


		for (i = 0; i < 4; i++)
		{
			attribs = ATTR_POSITION | ATTR_TEXCOORD;
			extradefines[0] = '\0';

			if (qglesMajorVersion < 3 && glRefConfig.standardDerivatives)
			{
				Q_strcat(extradefines, 1024, "#extension GL_OES_standard_derivatives : enable\n");
			}

			if (i & 1)
				Q_strcat(extradefines, 1024, "#define USE_VERTICAL_BLUR\n");
			else
				Q_strcat(extradefines, 1024, "#define USE_HORIZONTAL_BLUR\n");

			if (!(i & 2))
				Q_strcat(extradefines, 1024, "#define USE_DEPTH\n");


			if (!GLSL_InitGPUShader(&tr.depthBlurShader[i], "depthBlur", attribs, qtrue, extradefines, qtrue, fallbackShader_depthblur_vp, fallbackShader_depthblur_fp))
			{
				ri.Error(ERR_FATAL, "Could not load depthBlur shader!");
			}
		
			GLSL_InitUniforms(&tr.depthBlurShader[i]);

			GLSL_SetUniformInt(&tr.depthBlurShader[i], UNIFORM_SCREENIMAGEMAP, TB_COLORMAP);
			GLSL_SetUniformInt(&tr.depthBlurShader[i], UNIFORM_SCREENDEPTHMAP, TB_LIGHTMAP);

			GLSL_FinishGPUShader(&tr.depthBlurShader[i]);

			numEtcShaders++;
		}
	}

	// Added in OPM: water (tr_water.c). Without it water keeps its own stages.
	if (r_water->integer && glRefConfig.framebufferObject)
	{
		attribs = ATTR_POSITION | ATTR_NORMAL;
		extradefines[0] = '\0';

		Q_strcat(extradefines, 1024, va("#define WATER_MAX_SPOTS %d\n", WATER_MAX_SPOTS));
		Q_strcat(extradefines, 1024, va("#define WATER_RIPPLE_SIZE %d\n", WATER_RIPPLE_SIZE));
		if (r_volumetric->integer == 2 && r_realtimeLighting->integer == 2)
			Q_strcat(extradefines, 1024, "#define USE_FROXEL_FOG\n");

		if (GLSL_InitGPUShader(&tr.waterShader, "water", attribs, qtrue, extradefines, qtrue, fallbackShader_water_vp, fallbackShader_water_fp))
		{
			GLSL_InitUniforms(&tr.waterShader);

			GLSL_SetUniformInt(&tr.waterShader, UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);
			GLSL_SetUniformInt(&tr.waterShader, UNIFORM_SCREENIMAGEMAP, TB_LIGHTMAP);
			GLSL_SetUniformInt(&tr.waterShader, UNIFORM_SCREENDEPTHMAP, TMU_SOFTDEPTH);
			GLSL_SetUniformInt(&tr.waterShader, UNIFORM_SPECULARMAP, TB_SPECULARMAP);
			GLSL_SetUniformInt(&tr.waterShader, UNIFORM_CUBEMAP, TB_CUBEMAP);
			GLSL_SetUniformInt(&tr.waterShader, UNIFORM_WATERREFLECTMAP, TB_SHADOWMAP);

			GLSL_FinishGPUShader(&tr.waterShader);

			numEtcShaders++;
		}
		else
		{
			ri.Printf(PRINT_WARNING, "WARNING: the water program did not compile, water keeps its stages\n");
		}

		// what lands on the lens: drawn over the frame, through its copy
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';
		Q_strcat(extradefines, 1024, va("#define WATER_MAX_SPOTS %d\n", WATER_MAX_SPOTS));
		if (GLSL_InitGPUShader(&tr.lensShader, "lens", attribs, qtrue, extradefines, qtrue, fallbackShader_lens_vp, fallbackShader_lens_fp))
		{
			GLSL_InitUniforms(&tr.lensShader);
			GLSL_SetUniformInt(&tr.lensShader, UNIFORM_SCREENIMAGEMAP, TB_COLORMAP);
			GLSL_FinishGPUShader(&tr.lensShader);
			numEtcShaders++;
		}

		// the bloom (RB_Bloom): its levels halved and widened
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';
		if (GLSL_InitGPUShader(&tr.bloomShader, "bloom", attribs, qtrue, extradefines, qtrue, fallbackShader_bloom_vp, fallbackShader_bloom_fp))
		{
			GLSL_InitUniforms(&tr.bloomShader);
			GLSL_SetUniformInt(&tr.bloomShader, UNIFORM_SCREENIMAGEMAP, TB_COLORMAP);
			GLSL_FinishGPUShader(&tr.bloomShader);
			numEtcShaders++;
		}

		// its ripples, a step of them
		if (r_waterRipples->integer)
		{
			attribs = ATTR_POSITION | ATTR_TEXCOORD;
			extradefines[0] = '\0';
			Q_strcat(extradefines, 1024, va("#define WATER_MAX_SPOTS %d\n", WATER_MAX_SPOTS));

			if (GLSL_InitGPUShader(&tr.rippleShader, "ripple", attribs, qtrue, extradefines, qtrue, fallbackShader_ripple_vp, fallbackShader_ripple_fp))
			{
				GLSL_InitUniforms(&tr.rippleShader);
				GLSL_SetUniformInt(&tr.rippleShader, UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);
				GLSL_FinishGPUShader(&tr.rippleShader);
				numEtcShaders++;
			}
		}

		// the wet beach the tide leaves: darkened, then the sky in it
		for (i = 0; i < 2; i++)
		{
			attribs = ATTR_POSITION | ATTR_TEXCOORD;
			extradefines[0] = '\0';
			if (qglesMajorVersion < 3 && glRefConfig.standardDerivatives)
				Q_strcat(extradefines, 1024, "#extension GL_OES_standard_derivatives : enable\n");
			Q_strcat(extradefines, 1024, va("#define WATER_MAX_SPOTS %d\n", WATER_MAX_SPOTS));
			if (i)
				Q_strcat(extradefines, 1024, "#define BEACH_ADD\n");

			if (!GLSL_InitGPUShader(&tr.beachShader[i], "beach", attribs, qtrue, extradefines, qtrue, fallbackShader_beach_vp, fallbackShader_beach_fp))
				break;

			GLSL_InitUniforms(&tr.beachShader[i]);
			GLSL_SetUniformInt(&tr.beachShader[i], UNIFORM_SCREENDEPTHMAP, TB_COLORMAP);
			GLSL_SetUniformInt(&tr.beachShader[i], UNIFORM_SCREENIMAGEMAP, TB_LIGHTMAP);
			GLSL_SetUniformInt(&tr.beachShader[i], UNIFORM_CUBEMAP, TB_CUBEMAP);
			GLSL_SetUniformInt(&tr.beachShader[i], UNIFORM_SPECULARMAP, TB_SPECULARMAP);
			GLSL_FinishGPUShader(&tr.beachShader[i]);
			numEtcShaders++;
		}
	}

#if 0
	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if (!GLSL_InitGPUShader(&tr.testcubeShader, "testcube", attribs, qtrue, extradefines, qtrue, NULL, NULL))
	{
		ri.Error(ERR_FATAL, "Could not load testcube shader!");
	}

	GLSL_InitUniforms(&tr.testcubeShader);

	GLSL_SetUniformInt(&tr.testcubeShader, UNIFORM_TEXTUREMAP, TB_COLORMAP);

	GLSL_FinishGPUShader(&tr.testcubeShader);

	numEtcShaders++;
#endif


	endTime = ri.Milliseconds();

	ri.Printf(PRINT_ALL, "loaded %i GLSL shaders (%i gen %i light %i etc) in %5.2f seconds\n", 
		numGenShaders + numLightShaders + numEtcShaders, numGenShaders, numLightShaders, 
		numEtcShaders, (endTime - startTime) / 1000.0);
}

void GLSL_ShutdownGPUShaders(void)
{
	int i;

	ri.Printf(PRINT_ALL, "------- GLSL_ShutdownGPUShaders -------\n");

	for (i = 0; i < ATTR_INDEX_COUNT && i < glRefConfig.maxVertexAttribs; i++)
		qglDisableVertexAttribArray(i);

	GL_BindNullProgram();

	for ( i = 0; i < GENERICDEF_COUNT; i++)
	{
		GLSL_DeleteGPUShader(&tr.genericShader[i]);
		GLSL_DeleteGPUShader(&tr.genericSlimShader[i]);
	}

	GLSL_DeleteGPUShader(&tr.textureColorShader);
	GLSL_DeleteGPUShader(&tr.rtSkyBakeShader);
	GLSL_DeleteGPUShader(&tr.rtLampBakeShader);
	GLSL_DeleteGPUShader(&tr.rtBakeFillShader);

	for ( i = 0; i < FOGDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.fogShader[i]);

	for ( i = 0; i < DLIGHTDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.dlightShader[i]);

	for ( i = 0; i < LIGHTDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.lightallShader[i]);

	for ( i = 0; i < SHADOWMAPDEF_COUNT; i++)
		GLSL_DeleteGPUShader(&tr.shadowmapShader[i]);

	GLSL_DeleteGPUShader(&tr.pshadowShader);
	GLSL_DeleteGPUShader(&tr.down4xShader);
	GLSL_DeleteGPUShader(&tr.bokehShader);
	GLSL_DeleteGPUShader(&tr.tonemapShader);

	for ( i = 0; i < 2; i++)
		GLSL_DeleteGPUShader(&tr.calclevels4xShader[i]);

	GLSL_DeleteGPUShader(&tr.shadowmaskShader);
	GLSL_DeleteGPUShader(&tr.volumetricShader);
	for (i = 0; i < 4; i++)
		GLSL_DeleteGPUShader(&tr.froxelShader[i]);
	GLSL_DeleteGPUShader(&tr.ssaoShader);
	GLSL_DeleteGPUShader(&tr.gtaoShader);
	GLSL_DeleteGPUShader(&tr.waterShader);
	GLSL_DeleteGPUShader(&tr.rippleShader);
	GLSL_DeleteGPUShader(&tr.lensShader);
	GLSL_DeleteGPUShader(&tr.bloomShader);
	GLSL_DeleteGPUShader(&tr.beachShader[0]);
	GLSL_DeleteGPUShader(&tr.beachShader[1]);

	for ( i = 0; i < 4; i++)
		GLSL_DeleteGPUShader(&tr.depthBlurShader[i]);
}


void GLSL_BindProgram(shaderProgram_t * program)
{
	GLuint programObject = program ? program->program : 0;
	char *name = program ? program->name : "NULL";

	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get a call to va() every frame!
		GLimp_LogComment(va("--- GLSL_BindProgram( %s ) ---\n", name));
	}

	if (GL_UseProgram(programObject))
		backEnd.pc.c_glslShaderBinds++;

	// Added in OPM: r_linearLighting. The world's view reads its textures
	// and colours as linear light, and the tone map shows it; the 2D, and
	// scenes without the world, which are not tone mapped, as ever.
	if (program && program->uniforms[UNIFORM_LINEAR] != -1)
	{
		const qboolean linear = R_RtLinear() && (program == &tr.tonemapShader
			|| (!backEnd.projection2D && !(backEnd.refdef.rdflags & RDF_NOWORLDMODEL)));

		GLSL_SetUniformFloat(program, UNIFORM_LINEAR, linear ? 1.0f : 0.0f);
	}
}


shaderProgram_t *GLSL_GetGenericShaderProgram(int stage)
{
	shaderStage_t *pStage = tess.xstages[stage];
	int shaderAttribs = 0;

	if (tess.fogNum && pStage->adjustColorsForFog)
	{
		shaderAttribs |= GENERICDEF_USE_FOG;
	}

	switch (pStage->rgbGen)
	{
		case CGEN_LIGHTING_DIFFUSE:
		case CGEN_DOT:
		case CGEN_ONE_MINUS_DOT:
			shaderAttribs |= GENERICDEF_USE_RGBAGEN;
			break;
		default:
			break;
	}

	switch (pStage->alphaGen)
	{
		case AGEN_LIGHTING_SPECULAR:
		case AGEN_PORTAL:
		case AGEN_SCOORD:
		case AGEN_TCOORD:
		case AGEN_DOT:
		case AGEN_ONE_MINUS_DOT:
			shaderAttribs |= GENERICDEF_USE_RGBAGEN;
			break;
		default:
			break;
	}

	if (pStage->bundle[0].tcGen != TCGEN_TEXTURE)
	{
		shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
	}

	if (tess.shader->numDeforms && !ShaderRequiresCPUDeforms(tess.shader))
	{
		shaderAttribs |= GENERICDEF_USE_DEFORM_VERTEXES;
	}

	if (glState.vertexAnimation)
	{
		shaderAttribs |= GENERICDEF_USE_VERTEX_ANIMATION;
	}
	else if (glState.boneAnimation)
	{
		shaderAttribs |= GENERICDEF_USE_BONE_ANIMATION;
	}

	if (pStage->bundle[0].numTexMods)
	{
		shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
	}

	// nextBundle's second texture lives in this permutation
	if (pStage->multitextureEnv)
	{
		shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
	}

	return &tr.genericShader[shaderAttribs];
}
