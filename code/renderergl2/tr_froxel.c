/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// tr_froxel.c -- the light in the air in a grid of cells over the view
//
// Added in OPM (r_volumetric 2, glsl/froxel_fp.glsl). The view is cut into
// cells, FROXEL_PIXELS of it a side and FROXEL_SLICES slices of its depth;
// each frame every cell is lit (the sun through its cascades, the lamps lit
// live through their shadows), summed from the eye, and the view is seen
// through the sum at each pixel's depth. What is in the air is the haze
// (r_volumetricDensity) and the map's fog banks, read from fog/<map>.txt, a
// line a bank:
//
//     // a box, its corners, how thick (1: a thousand units of it take
//     // two thirds of the light out), how soft its edges (units), its colour
//     box -2000 -2000 -64  2000 2000 96  4  128  0.8 0.82 0.85
//     // the light of the banks where no sun or lamp reaches them
//     ambient 0.10 0.11 0.13
//
// A bank's density rises from nothing at its faces to all of it its soft
// edge inside; a box as wide as the map with a soft top is ground fog.
// `fogreload` reads the file again, `foginfo` lists the banks.
//
// The cells are lit at another place in each every frame and mixed with what
// they had last frame, found where they were then (r_volumetricTemporal).

#include "tr_local.h"

cvar_t *r_volumetricFog;
cvar_t *r_volumetricTemporal;

#define FOG_MAX_VOLUMES (WATER_MAX_SPOTS / 3)

static struct {
	char   file[MAX_QPATH];
	int    numVolumes;
	vec4_t volumes[FOG_MAX_VOLUMES * 3]; // mins and density, maxs and soft edge, colour
	vec3_t ambient;
	qboolean ambientSet;
} fog;

// the cells as the last frame left them (RB_VolumetricFroxels)
static struct {
	int    frame;   // tr.frameCount it was lit at
	int    current; // which of tr.froxelLightImage it is in
	int    sequence;
	vec3_t origin;
	vec3_t axis[3];
	float  tanX, tanY;
	float  range;
} froxelPrev;

static int R_FogNumbers(char **text, float *out, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		const char *token = COM_ParseExt(text, qfalse);
		if (!token[0]) {
			break;
		}
		out[i] = atof(token);
	}
	return i;
}

static void R_FogReadFile(const char *mapName)
{
	union {
		char *c;
		void *v;
	} buffer;
	char *text;

	fog.numVolumes = 0;
	fog.ambientSet = qfalse;
	Com_sprintf(fog.file, sizeof(fog.file), "fog/%s.txt", mapName);
	if (ri.FS_ReadFile(fog.file, &buffer.v) <= 0) {
		return;
	}

	text = buffer.c;
	for (;;) {
		const char *token = COM_ParseExt(&text, qtrue);
		float       v[11];

		if (!token[0]) {
			break;
		}
		if (!Q_stricmp(token, "box")) {
			const int n = R_FogNumbers(&text, v, 11);

			if (n < 7) {
				ri.Printf(PRINT_WARNING, "%s: a box needs its corners and a density\n", fog.file);
			} else if (fog.numVolumes >= FOG_MAX_VOLUMES) {
				ri.Printf(PRINT_WARNING, "%s: no more than %d banks\n", fog.file, FOG_MAX_VOLUMES);
			} else {
				float *lo = fog.volumes[fog.numVolumes * 3];
				float *hi = fog.volumes[fog.numVolumes * 3 + 1];
				float *c  = fog.volumes[fog.numVolumes * 3 + 2];
				int    k;

				for (k = 0; k < 3; k++) {
					lo[k] = Q_min(v[k], v[k + 3]);
					hi[k] = Q_max(v[k], v[k + 3]);
				}
				lo[3] = Q_max(0.0f, v[6]);
				hi[3] = n > 7 ? Q_max(0.0f, v[7]) : 0.0f;
				VectorSet4(c, 1.0f, 1.0f, 1.0f, 0.0f);
				if (n >= 11) {
					VectorSet(c, Q_max(0.0f, v[8]), Q_max(0.0f, v[9]), Q_max(0.0f, v[10]));
				}
				fog.numVolumes++;
			}
		} else if (!Q_stricmp(token, "ambient")) {
			if (R_FogNumbers(&text, v, 3) == 3) {
				VectorSet(fog.ambient, Q_max(0.0f, v[0]), Q_max(0.0f, v[1]), Q_max(0.0f, v[2]));
				fog.ambientSet = qtrue;
			}
		} else {
			ri.Printf(PRINT_WARNING, "%s: unknown '%s'\n", fog.file, token);
		}
		SkipRestOfLine(&text);
	}
	ri.FS_FreeFile(buffer.v);
}

/*
=============
R_FogLoadWorld

The map's fog banks, as it loads (or `fogreload`).
=============
*/
void R_FogLoadWorld(const char *mapName)
{
	R_FogReadFile(mapName);
	froxelPrev.frame = -1;
}

static void R_FogInfo_f(void)
{
	int i;

	ri.Printf(PRINT_ALL, "%s: %d bank(s)%s\n", fog.file[0] ? fog.file : "(no map)", fog.numVolumes,
		r_volumetric->integer == 2 ? "" : " (r_volumetric 2 shows them)");
	for (i = 0; i < fog.numVolumes; i++) {
		const float *lo = fog.volumes[i * 3], *hi = fog.volumes[i * 3 + 1], *c = fog.volumes[i * 3 + 2];

		ri.Printf(PRINT_ALL, "  box (%.0f %.0f %.0f) (%.0f %.0f %.0f) density %g soft %g colour %g %g %g\n",
			lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], lo[3], hi[3], c[0], c[1], c[2]);
	}
	if (fog.ambientSet) {
		ri.Printf(PRINT_ALL, "  ambient %g %g %g\n", fog.ambient[0], fog.ambient[1], fog.ambient[2]);
	}
}

static void R_FogReload_f(void)
{
	if (!tr.world) {
		ri.Printf(PRINT_ALL, "No map loaded\n");
		return;
	}
	R_FogLoadWorld(tr.world->baseName);
	R_FogInfo_f();
}

void R_FroxelInit(void)
{
	r_volumetricFog = ri.Cvar_Get("r_volumetricFog", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_volumetricFog, "With r_volumetric 2: how thick the map's fog banks are (fog/<map>.txt), 0 none");
	r_volumetricTemporal = ri.Cvar_Get("r_volumetricTemporal", "0.9", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_volumetricTemporal, "With r_volumetric 2: how much of the light in the air each frame keeps from the last (0 none: noisier, quicker to change)");
	ri.Cmd_AddCommand("foginfo", R_FogInfo_f);
	ri.Cmd_AddCommand("fogreload", R_FogReload_f);
	froxelPrev.frame = -1;
}

void R_FroxelShutdown(void)
{
	ri.Cmd_RemoveCommand("foginfo");
	ri.Cmd_RemoveCommand("fogreload");
}

static float R_Halton(int index, int base)
{
	float f = 1.0f, r = 0.0f;

	while (index > 0) {
		f /= base;
		r += f * (index % base);
		index /= base;
	}
	return r;
}

/*
=============
RB_VolumetricFroxels

Each tile of the view's furthest depth, each cell lit (mixed with last
frame's), summed from the eye, then the view seen through it at each pixel's
depth.
=============
*/
void RB_VolumetricFroxels(FBO_t *dstFbo, ivec4_t dstBox)
{
	vec4_t quadVerts[4], viewInfo, volParams, volRange, froxel, v;
	vec4_t volumes[FOG_MAX_VOLUMES * 3];
	vec2_t texCoords[4];
	vec3_t viewVector;
	vec4_t box;
	const float zmin  = r_znear->value;
	const float zmax  = backEnd.viewParms.zFar;
	const float tanY  = tan(backEnd.viewParms.fovY * M_PI / 360.0f);
	const float tanX  = tan(backEnd.viewParms.fovX * M_PI / 360.0f);
	const float range = Q_max(64.0f, r_volumetricRange->value);
	const float density = Q_max(0.0f, r_volumetricDensity->value) * 0.001f;
	const float fogScale = Q_max(0.0f, r_volumetricFog->value) * 0.001f;
	const qboolean cascades = r_sunlightMode->integer && (backEnd.viewParms.flags & VPF_USESUNLIGHT) && tr.sunShadowDepthImage[3];
	const int numVolumes = fogScale > 0 ? fog.numVolumes : 0;
	// last frame's cells, if they were lit last frame for a grid like this one
	const qboolean history = r_volumetricTemporal->value > 0 && froxelPrev.frame == tr.frameCount - 1 && froxelPrev.range == range;
	const int lit = history ? !froxelPrev.current : froxelPrev.current;
	int i;

	R_GpuTimerBegin(GPUTIMER_VOLUME);

	VectorSet4(quadVerts[0], -1,  1, 0, 1);
	VectorSet4(quadVerts[1],  1,  1, 0, 1);
	VectorSet4(quadVerts[2],  1, -1, 0, 1);
	VectorSet4(quadVerts[3], -1, -1, 0, 1);
	texCoords[0][0] = 0; texCoords[0][1] = 1;
	texCoords[1][0] = 1; texCoords[1][1] = 1;
	texCoords[2][0] = 1; texCoords[2][1] = 0;
	texCoords[3][0] = 0; texCoords[3][1] = 0;

	VectorSet4(froxel, (float)(tr.froxelLightImage[0]->width / FROXEL_TILES), (float)(tr.froxelLightImage[0]->height / (FROXEL_SLICES / FROXEL_TILES)),
		(float)FROXEL_SLICES, log2f(range / 256.0f + 1.0f));
	VectorSet4(viewInfo, zmax / zmin, zmax, 0.0f, 0.0f);
	// the density in units: r_volumetricDensity 1 is a light haze, which
	// takes nothing out of the light through it
	VectorSet4(volParams, density, Q_max(-0.95f, Q_min(r_volumetricAnisotropy->value, 0.95f)),
		Q_max(0.0f, r_volumetricSun->value), Q_max(0.0f, r_volumetricLamps->value));
	VectorSet4(volRange, range, cascades ? 1.0f : 0.0f, 0.0f, 0.0f);

	GL_State(GLS_DEPTHTEST_DISABLE);
	GL_Cull(CT_TWO_SIDED);

	for (i = 0; i < 4; i++)
	{
		shaderProgram_t *sp = &tr.froxelShader[i];

		GLSL_BindProgram(sp);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.ori.origin);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWFORWARD, backEnd.refdef.viewaxis[0]);
		VectorScale(backEnd.refdef.viewaxis[1], tanX, viewVector);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWLEFT, viewVector);
		VectorScale(backEnd.refdef.viewaxis[2], tanY, viewVector);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWUP, viewVector);
		GLSL_SetUniformVec4(sp, UNIFORM_VIEWINFO, viewInfo);
		GLSL_SetUniformVec4(sp, UNIFORM_VOLPARAMS, volParams);
		GLSL_SetUniformVec4(sp, UNIFORM_VOLRANGE, volRange);
		GLSL_SetUniformVec4(sp, UNIFORM_FROXEL, froxel);
	}

	box[0] = backEnd.viewParms.viewportX / (float)glConfig.vidWidth;
	box[1] = backEnd.viewParms.viewportY / (float)glConfig.vidHeight;
	box[2] = box[0] + backEnd.viewParms.viewportWidth / (float)glConfig.vidWidth;
	box[3] = box[1] + backEnd.viewParms.viewportHeight / (float)glConfig.vidHeight;

	// each tile of the view's furthest depth: the cells behind it are left
	// unlit. Its quad takes the view's share of the depth image.
	texCoords[0][0] = box[0]; texCoords[0][1] = box[3];
	texCoords[1][0] = box[2]; texCoords[1][1] = box[3];
	texCoords[2][0] = box[2]; texCoords[2][1] = box[1];
	texCoords[3][0] = box[0]; texCoords[3][1] = box[1];
	FBO_Bind(tr.froxelDepthFbo);
	qglViewport(0, 0, tr.froxelDepthFbo->width, tr.froxelDepthFbo->height);
	qglScissor(0, 0, tr.froxelDepthFbo->width, tr.froxelDepthFbo->height);
	GLSL_BindProgram(&tr.froxelShader[3]);
	VectorSet4(viewInfo, zmax / zmin, zmax, (box[2] - box[0]) / froxel[0], (box[3] - box[1]) / froxel[1]);
	GLSL_SetUniformVec4(&tr.froxelShader[3], UNIFORM_VIEWINFO, viewInfo);
	GL_BindToTMU(tr.renderDepthImage, TB_LIGHTMAP);
	RB_InstantQuad2(quadVerts, texCoords);
	texCoords[0][0] = 0; texCoords[0][1] = 1;
	texCoords[1][0] = 1; texCoords[1][1] = 1;
	texCoords[2][0] = 1; texCoords[2][1] = 0;
	texCoords[3][0] = 0; texCoords[3][1] = 0;

	// each cell lit, into the image last frame's are not in
	FBO_Bind(tr.froxelLightFbo[lit]);
	qglViewport(0, 0, tr.froxelLightFbo[lit]->width, tr.froxelLightFbo[lit]->height);
	qglScissor(0, 0, tr.froxelLightFbo[lit]->width, tr.froxelLightFbo[lit]->height);
	GLSL_BindProgram(&tr.froxelShader[0]);
	if (cascades)
	{
		GL_BindToTMU(tr.sunShadowDepthImage[0], TB_SHADOWMAP);
		GL_BindToTMU(tr.sunShadowDepthImage[1], TB_SHADOWMAP2);
		GL_BindToTMU(tr.sunShadowDepthImage[2], TB_SHADOWMAP3);
		GL_BindToTMU(tr.sunShadowDepthImage[3], TB_SHADOWMAP4);
		GLSL_SetUniformMat4(&tr.froxelShader[0], UNIFORM_SHADOWMVP,  backEnd.refdef.sunShadowMvp[0]);
		GLSL_SetUniformMat4(&tr.froxelShader[0], UNIFORM_SHADOWMVP2, backEnd.refdef.sunShadowMvp[1]);
		GLSL_SetUniformMat4(&tr.froxelShader[0], UNIFORM_SHADOWMVP3, backEnd.refdef.sunShadowMvp[2]);
		GLSL_SetUniformMat4(&tr.froxelShader[0], UNIFORM_SHADOWMVP4, backEnd.refdef.sunShadowMvp[3]);
	}
	else
	{
		// a depth image either way: a shadow sampler on a colour one reads nothing defined
		GL_BindToTMU(tr.rtShadowImage[0], TB_SHADOWMAP);
		GL_BindToTMU(tr.rtShadowImage[0], TB_SHADOWMAP2);
		GL_BindToTMU(tr.rtShadowImage[0], TB_SHADOWMAP3);
		GL_BindToTMU(tr.rtShadowImage[0], TB_SHADOWMAP4);
	}
	R_RtSetVolumeUniforms(&tr.froxelShader[0]);
	GL_BindToTMU(tr.froxelDepthImage, TB_NORMALMAP);
	GL_BindToTMU(tr.froxelLightImage[!lit], TB_COLORMAP);

	// the banks, their density in units
	for (i = 0; i < numVolumes * 3; i++) {
		VectorCopy4(fog.volumes[i], volumes[i]);
		if (i % 3 == 0) {
			volumes[i][3] *= fogScale;
		}
	}
	if (numVolumes) {
		GLSL_SetUniformWaterSpots(&tr.froxelShader[0], UNIFORM_FOGVOLUMES, (const vec4_t *)volumes, numVolumes * 3);
	}
	VectorSet4(v, (float)numVolumes, 0.0f, 0.0f, 0.0f);
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_FOGPARAMS, v);
	// their own light: the file's, or the realtime lights' ambient
	if (fog.ambientSet) {
		VectorCopy(fog.ambient, v);
	} else {
		VectorCopy(tr.rtAmbient, v);
	}
	v[3] = 0.0f;
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_FOGAMBIENT, v);

	// where in each cell this frame (Halton, 2 3 5), and where the cells
	// were last frame
	froxelPrev.sequence = (froxelPrev.sequence + 1) & 63;
	VectorSet4(v, R_Halton(froxelPrev.sequence + 1, 2), R_Halton(froxelPrev.sequence + 1, 3), R_Halton(froxelPrev.sequence + 1, 5), 0.0f);
	if (r_volumetricTemporal->value <= 0) {
		VectorSet4(v, 0.5f, 0.5f, 0.5f, 0.0f);
	}
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_FROXELJITTER, v);
	VectorCopy(froxelPrev.origin, v);
	v[3] = history ? Com_Clamp(0.0f, 0.98f, r_volumetricTemporal->value) : 0.0f;
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_FROXELPREVORIGIN, v);
	VectorCopy(froxelPrev.axis[0], v);
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_FROXELPREVFORWARD, v);
	VectorScale(froxelPrev.axis[1], 1.0f / Q_max(froxelPrev.tanX, 0.001f), v);
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_FROXELPREVLEFT, v);
	VectorScale(froxelPrev.axis[2], 1.0f / Q_max(froxelPrev.tanY, 0.001f), v);
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_FROXELPREVUP, v);
	RB_InstantQuad2(quadVerts, texCoords);

	// kept for the next frame
	froxelPrev.frame   = tr.frameCount;
	froxelPrev.current = lit;
	froxelPrev.range   = range;
	froxelPrev.tanX    = tanX;
	froxelPrev.tanY    = tanY;
	VectorCopy(backEnd.viewParms.ori.origin, froxelPrev.origin);
	VectorCopy(backEnd.refdef.viewaxis[0], froxelPrev.axis[0]);
	VectorCopy(backEnd.refdef.viewaxis[1], froxelPrev.axis[1]);
	VectorCopy(backEnd.refdef.viewaxis[2], froxelPrev.axis[2]);

	// summed from the eye
	FBO_Bind(tr.froxelSumFbo);
	GLSL_BindProgram(&tr.froxelShader[1]);
	GL_BindToTMU(tr.froxelLightImage[lit], TB_COLORMAP);
	RB_InstantQuad2(quadVerts, texCoords);

	// the view through it: times what it lets through, plus what it adds
	texCoords[0][0] = box[0]; texCoords[0][1] = box[3];
	texCoords[1][0] = box[2]; texCoords[1][1] = box[3];
	texCoords[2][0] = box[2]; texCoords[2][1] = box[1];
	texCoords[3][0] = box[0]; texCoords[3][1] = box[1];

	FBO_Bind(dstFbo);
	qglViewport(dstBox[0], dstBox[1], dstBox[2], dstBox[3]);
	qglScissor(dstBox[0], dstBox[1], dstBox[2], dstBox[3]);
	GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_SRC_ALPHA);
	qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	GLSL_BindProgram(&tr.froxelShader[2]);
	GL_BindToTMU(tr.froxelSumImage, TB_COLORMAP);
	GL_BindToTMU(tr.renderDepthImage, TB_LIGHTMAP);
	RB_InstantQuad2(quadVerts, texCoords);
	qglColorMask(!backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3]);

	R_GpuTimerEnd(GPUTIMER_VOLUME);
}

/*
=============
R_FroxelFogOn

Whether the map has fog banks shown: the pass runs for them even with no haze.
=============
*/
qboolean R_FroxelFogOn(void)
{
	return fog.numVolumes > 0 && r_volumetricFog->value > 0;
}
