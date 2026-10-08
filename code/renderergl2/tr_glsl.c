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
extern const char *fallbackShader_ssao_vp;
extern const char *fallbackShader_ssao_fp;
extern const char *fallbackShader_texturecolor_vp;
extern const char *fallbackShader_texturecolor_fp;
extern const char *fallbackShader_tonemap_vp;
extern const char *fallbackShader_tonemap_fp;
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
	{ "u_RtSunDir",        GLSL_VEC4 },
	{ "u_RtSunColor",      GLSL_VEC4 },
	{ "u_RtAmbient",       GLSL_VEC4 },
	{ "u_RtStage",         GLSL_INT },
	{ "u_RtShadowStatic",  GLSL_INT },
	{ "u_RtShadowDynamic", GLSL_INT },
	{ "u_RtSunShadow",     GLSL_INT },
	{ "u_RtShadowBaked",   GLSL_INT },
	{ "u_RtCapsules",      GLSL_INT },
	{ "u_RtList",          GLSL_VEC4_RTLIST },
	{ "u_SoftParticle",    GLSL_VEC4 },
	{ "u_GroundCover",     GLSL_VEC4 },
	{ "u_SkelBones",       GLSL_INT },
	{ "u_SkelParams",      GLSL_VEC4 },
	{ "u_SkelAmbient",     GLSL_VEC4 },
	{ "u_SkelLights",      GLSL_VEC4_SKELLIGHTS }
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
	"uniform sampler2DShadow u_RtShadowBaked;\n"
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
	"float RtShadowAtlas(sampler2DShadow atlas, float tile, vec2 faceUV, float depth)\n"
	"{\n"
	"	vec2  origin = vec2(mod(tile, RT_TILES), floor(tile / RT_TILES));\n"
	"	float texel  = 1.0 / RT_TILE_TEXELS;\n"
	"	vec2  st     = clamp(faceUV, vec2(1.5 * texel), vec2(1.0 - 1.5 * texel));\n"
	"	vec2  uv     = (origin + st) / RT_TILES;\n"
	"	float d      = 0.5 / (RT_TILES * RT_TILE_TEXELS);\n"
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
	"		return info.x >= 0.0 ? RtShadowAtlas(u_RtShadowBaked, info.x + face, faceUV, depth) : 1.0;\n"
	"	if (info.x >= 0.0) vis *= RtShadowAtlas(u_RtShadowStatic, info.x + face, faceUV, depth);\n"
	// the moving things' tiles, with the faces drawn in the fraction, a bit each
	"	if (info.y >= 0.0)\n"
	"	{\n"
	"		float mask = floor(fract(info.y) * 64.0 + 0.5);\n"
	"		if (mod(floor(mask / exp2(face)), 2.0) > 0.5)\n"
	"			vis *= RtShadowAtlas(u_RtShadowDynamic, floor(info.y) + face, faceUV, depth);\n"
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
	// the map's lighting and what the lights change of it, never darker than
	// a part of what it was: the lights' model is too bright near a lamp
	"vec3 RtDelta(vec3 base, vec3 delta)\n"
	"{\n"
	"	return max(base + delta, base * u_RtParams.w);\n"
	"}\n"
	"#if !defined(RT_VERTEX)\n"
	"vec3 RtLight(vec3 P, vec3 N, bool delta)\n"
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
	"		float d   = sqrt(d2);\n"
	"		float ndl = dot(N, toLight) / d;\n"
	"		if (ndl <= 0.0) continue;\n"
	"		float fade = 1.0 - d2 / r2;\n"
	// the compiler's point light: intensity * 7500 / distance squared, never nearer than 16
	"		float att  = ndl * fade * fade / max(d2, 256.0);\n"
	// l0.w: 0 a point light, 1 a spot light, 2 a light the game adds (a muzzle
	// flash, a blast): never shadowed, and on top of the map's lighting always
	"		bool added = l0.w > 1.5;\n"
	"		if (l0.w > 0.5 && !added)\n"
	"		{\n"
	// the compiler's spot light: a cone, with a 32 unit soft edge
	"			vec4  l2 = u_RtLights[i * 4 + 2];\n"
	"			float along = -dot(toLight, l2.xyz);\n"
	"			if (along <= 0.001) continue;\n"
	"			float radiusAtDist = l2.w * along;\n"
	"			float sampleRadius = length(-toLight - l2.xyz * along);\n"
	"			if (sampleRadius >= radiusAtDist) continue;\n"
	"			att *= clamp((radiusAtDist - sampleRadius) / 32.0, 0.0, 1.0);\n"
	"		}\n"
	// what it would give unshadowed is too little to see: passed over, its
	// shadow not looked up (u_RtAmbient.w, r_rtCutoff; R_RtReach)
	"		vec3 most = l1.rgb * att;\n"
	"		if (max(most.r, max(most.g, most.b)) < u_RtAmbient.w) continue;\n"
	"		vec4 l3 = u_RtLights[i * 4 + 3];\n"
	"		if (u_RtParams.z > 0.5 && (l3.x >= 0.0 || l3.y >= 0.0))\n"
	"		{\n"
	// off the surface by a texel and a half of the shadow at that distance
	"			float texelWorld = 2.0 * d / RT_TILE_TEXELS;\n"
	"			vec3  v = P + N * (1.5 * texelWorld + 0.5) - l0.xyz;\n"
	"			float now = RtShadow(l3, v, false);\n"
	"			if (l3.z >= 0.0 && now > 0.0) now *= RtCapsules(P, l0.xyz, l3.z);\n"
	"			att *= delta ? now - RtShadow(l3, v, true) : now;\n"
	"		}\n"
	"		else if (delta && !added) continue;\n"
	"		sum += l1.rgb * att;\n"
	"	}\n"
	"	if (u_RtSunDir.w > 0.5 && !delta)\n"
	"	{\n"
	"		float ndl = dot(N, u_RtSunDir.xyz);\n"
	"		if (ndl > 0.0)\n"
	"		{\n"
	"			float vis = 1.0;\n"
	"			if (u_RtSunColor.w > 0.5)\n"
	"				vis = texture2D(u_RtSunShadow, gl_FragCoord.xy * r_FBufScale).r;\n"
	"			sum += u_RtSunColor.rgb * (ndl * vis);\n"
	"		}\n"
	"	}\n"
	"	return sum;\n"
	"}\n"
	"#else\n"
	// dust or smoke at P, lit from every side as it hangs in the light, which
	// it is not in the shadow maps of; sunUV is where it is on the screen, whose
	// sun shadow (that of what is behind it) it takes
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
	"		bool added = l0.w > 1.5;\n"
	"		if (l0.w > 0.5 && !added)\n"
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
	"		if (u_RtSunColor.w > 0.5)\n"
	"			vis = texture2D(u_RtSunShadow, sunUV).r;\n"
	"		sum += u_RtSunColor.rgb * vis;\n"
	"	}\n"
	"	return sum;\n"
	"}\n"
	"#endif\n";

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
		|| (shaderType == GL_VERTEX_SHADER && strstr(extra, "#define USE_RTLIGHT_PARTICLES"))))
	{
		if (shaderType == GL_VERTEX_SHADER)
			Q_strcat(dest, size, "#define RT_VERTEX\n#define texture2D texture\n#define shadow2D texture\n");
		Q_strcat(dest, size, va("#define RT_MAX_LIGHTS %d\n#define RT_TILES %d.0\n#define RT_TILE_TEXELS %d.0\n#define RT_ZNEAR %f\n",
			RT_MAX_LIGHTS, RT_ATLAS_TILES, r_rtShadowTile->integer, RT_ZNEAR));
		// the bodies' capsules are fetched texel by texel (GLSL 1.30)
		if (glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 30)
			Q_strcat(dest, size, va("#define RT_CAPSULES\n#define RT_MAX_RUN %d\n", RT_MAX_RUN));
		Q_strcat(dest, size, rt_glsl);
	}

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
	char vpCode[32000];
	char fpCode[32000];
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
			case GLSL_VEC4_SKELLIGHTS:
				size += sizeof(vec4_t) * SKEL_GPU_MAX_LIGHTS * 3;
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

	if ((i & LIGHTDEF_GROUNDCOVER) && (lightType == LIGHTDEF_USE_LIGHTMAP || lightType == LIGHTDEF_USE_LIGHT_VERTEX))
		return qfalse;

	// Added in OPM: a skeletal model posed from the frame's bones, which
	// brings its own vertexes, and nothing that reads others
	if ((i & LIGHTDEF_SKEL_GPU) && (i & (LIGHTDEF_ENTITY_VERTEX_ANIMATION | LIGHTDEF_ENTITY_BONE_ANIMATION | LIGHTDEF_GROUNDCOVER
		| LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_SPECULARMAP | LIGHTDEF_USE_DELUXEMAP | LIGHTDEF_USE_PARALLAXMAP)))
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

			if ((i & LIGHTDEF_USE_PARALLAXMAP) && !(i & LIGHTDEF_ENTITY_VERTEX_ANIMATION) && !(i & LIGHTDEF_ENTITY_BONE_ANIMATION) && r_parallaxMapping->integer)
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
		Q_strcat(extradefines, 1024, va("#define USE_SKEL_GPU\n#define USE_MODELMATRIX\n#define SKEL_MAX_LIGHTS %d\n#define SKEL_BONE_ROW %d\n", SKEL_GPU_MAX_LIGHTS, SKEL_GPU_BONE_ROW));
		attribs |= ATTR_POSITION2 | ATTR_NORMAL2 | ATTR_TANGENT2 | ATTR_BONE_INDEXES
			| ATTR_TANGENT | ATTR_LIGHTCOORD | ATTR_PAINTCOLOR | ATTR_BONE_WEIGHTS;
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
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTSHADOWBAKED,   TMU_RTSHADOW_BAKED);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_RTCAPSULES,      TMU_RTCAPSULES);
	GLSL_SetUniformInt(&tr.lightallShader[i], UNIFORM_SKELBONES,       TMU_SKELBONES);

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
		if ((i & GENERICDEF_USE_VERTEX_ANIMATION) && (i & GENERICDEF_USE_BONE_ANIMATION))
			continue;

		if ((i & GENERICDEF_USE_BONE_ANIMATION) && !glRefConfig.glslMaxAnimatedBones)
			continue;

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
				continue;

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
		if (r_softParticles->value > 0)
			Q_strcat(extradefines, 1024, "#define USE_SOFTPARTICLES\n");

		if (!GLSL_InitGPUShader(&tr.genericShader[i], "generic", attribs, qtrue, extradefines, qtrue, fallbackShader_generic_vp, fallbackShader_generic_fp))
		{
			ri.Error(ERR_FATAL, "Could not load generic shader!");
		}

		GLSL_InitUniforms(&tr.genericShader[i]);

		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_LIGHTMAP,   TB_LIGHTMAP);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_RTSHADOWSTATIC,  TMU_RTSHADOW_STATIC);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_RTSHADOWDYNAMIC, TMU_RTSHADOW_DYNAMIC);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_RTSUNSHADOW,     TMU_RTSUNSHADOW);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_RTSHADOWBAKED,   TMU_RTSHADOW_BAKED);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_RTCAPSULES,      TMU_RTCAPSULES);
		GLSL_SetUniformInt(&tr.genericShader[i], UNIFORM_SCREENDEPTHMAP,  TMU_SOFTDEPTH);

		GLSL_FinishGPUShader(&tr.genericShader[i]);

		numGenShaders++;
	}


	attribs = ATTR_POSITION | ATTR_TEXCOORD;

	if (!GLSL_InitGPUShader(&tr.textureColorShader, "texturecolor", attribs, qtrue, extradefines, qtrue, fallbackShader_texturecolor_vp, fallbackShader_texturecolor_fp))
	{
		ri.Error(ERR_FATAL, "Could not load texturecolor shader!");
	}
	
	GLSL_InitUniforms(&tr.textureColorShader);

	GLSL_SetUniformInt(&tr.textureColorShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP);

	GLSL_FinishGPUShader(&tr.textureColorShader);

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
		GLSL_DeleteGPUShader(&tr.genericShader[i]);

	GLSL_DeleteGPUShader(&tr.textureColorShader);

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
	GLSL_DeleteGPUShader(&tr.ssaoShader);

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
