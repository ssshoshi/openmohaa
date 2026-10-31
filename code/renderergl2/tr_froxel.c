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
//
// Smoke: a sprite the cgame marks RF_VOLUME_SMOKE (an emitter's
// `smokevolume`, or a volumetric smoke source) nearer than the grid's depth
// is put in the air as a puff (R_FroxelTakesSprite) in place of its sprite:
// a ball as wide as the sprite, as thick through its middle as the sprite is
// opaque, broken up by a 3D noise, lit by the sun (shadowed by the smoke
// between) and the lamps, and taking the light out of what is behind it.

#include "tr_local.h"
#include "tr_dsa.h"

cvar_t *r_volumetricFog;
cvar_t *r_volumetricTemporal;
cvar_t *r_volumetricSmoke;
cvar_t *r_volumetricSmokeNoise;

// the smoke's noise: a tiling 3D texture, made once (R_FroxelSmokeNoise)
#define SMOKE_NOISE_SIZE 64
// how much of the light on it smoke gives back
#define SMOKE_ALBEDO 0.6f
static GLuint smokeNoiseTexture;

// the sprites taken as puffs this scene (R_FroxelTakesSprite)
static struct {
	int scene;
	int taken;
} smokeTaken;

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
	r_volumetricSmoke = ri.Cvar_Get("r_volumetricSmoke", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_volumetricSmoke, "With r_volumetric 2: smoke grenades and other smoke with a volume drawn in the air, lit and shadowed, in place of their sprites; how thick (0: sprites)");
	r_volumetricSmokeNoise = ri.Cvar_Get("r_volumetricSmokeNoise", "0.4", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_volumetricSmokeNoise, "With r_volumetric 2: how much the noise breaks up the smoke's puffs, 0 smooth balls, 1 shreds");
	ri.Cmd_AddCommand("foginfo", R_FogInfo_f);
	ri.Cmd_AddCommand("fogreload", R_FogReload_f);
	froxelPrev.frame = -1;
}

void R_FroxelShutdown(void)
{
	if (smokeNoiseTexture) {
		qglDeleteTextures(1, &smokeNoiseTexture);
		smokeNoiseTexture = 0;
	}
	ri.Cmd_RemoveCommand("foginfo");
	ri.Cmd_RemoveCommand("fogreload");
}

/*
=============
R_FroxelTakesSprite

Whether a puff of smoke goes into the air's grid this view, in place of its
sprite. The main view only (a reflection or a portal draws the sprite), with
the grid on, the puff nearer than its depth, and room for it. The answer is
kept on the sprite for the backend (RB_VolumetricFroxels).
=============
*/
qboolean R_FroxelTakesSprite(const refSprite_t *sprite)
{
	refSprite_t *spr = (refSprite_t *)sprite;
	vec3_t       delta;
	const float  range = Q_max(64.0f, r_volumetricRange->value);

	if (r_volumetric->integer != 2 || r_volumetricSmoke->value <= 0 || !tr.froxelDepthFbo || !tr.froxelShader[0].program
		|| !R_RtLitLive() || tr.viewParms.isPortal || tr.viewParms.isPortalSky || (tr.viewParms.flags & VPF_WATERREFLECT)
		|| (tr.refdef.rdflags & RDF_NOWORLDMODEL)) {
		return qfalse;
	}
	if (smokeTaken.scene != tr.sceneCount) {
		smokeTaken.scene = tr.sceneCount;
		smokeTaken.taken = 0;
	}
	if (spr->froxelTaken) {
		return qtrue;
	}
	if (smokeTaken.taken >= SMOKE_MAX_BLOBS) {
		return qfalse;
	}
	// a little short of the grid's far side, so the puff fits in it
	VectorSubtract(spr->origin, tr.viewParms.ori.origin, delta);
	if (VectorLengthSquared(delta) > Square(range * 0.85f)) {
		return qfalse;
	}
	spr->froxelTaken = qtrue;
	smokeTaken.taken++;
	return qtrue;
}

/*
=============
R_FroxelSmokeBlobs

The scene's puffs, as the shader takes them: middle and radius, colour and
density (per unit, at the middle).
=============
*/
static int R_FroxelSmokeBlobs(vec4_t *out)
{
	int i, n = 0;

	for (i = 0; i < backEnd.refdef.num_sprites && n < SMOKE_MAX_BLOBS; i++) {
		const refSprite_t *spr = &backEnd.refdef.sprites[i];
		const model_t     *model;
		float              radius, alpha;

		if (!spr->froxelTaken) {
			continue;
		}
		model = tr.models[spr->hModel > 0 && spr->hModel < tr.numModels ? spr->hModel : 0];
		if (model->type != MOD_SPRITE || !model->d.sprite) {
			continue;
		}
		// a little wider than the sprite: the noise eats its edge
		radius = Q_max(model->d.sprite->origin_x, model->d.sprite->origin_y) * spr->scale * model->d.sprite->scale * 1.15f;
		alpha  = Com_Clamp(0.0f, 0.99f, spr->shaderRGBA[3] / 255.0f);
		if (radius < 1.0f || alpha <= 0.0f) {
			continue;
		}
		VectorCopy(spr->origin, out[n * 2]);
		out[n * 2][3] = radius;
		// smoke is grey: its tint times SMOKE_ALBEDO
		VectorSet(out[n * 2 + 1], spr->shaderRGBA[0] / 255.0f * SMOKE_ALBEDO, spr->shaderRGBA[1] / 255.0f * SMOKE_ALBEDO,
			spr->shaderRGBA[2] / 255.0f * SMOKE_ALBEDO);
		// as opaque straight through its middle as the sprite was: the
		// 1 - q fall-off takes 2/3 of the middle's along a diameter
		out[n * 2 + 1][3] = -logf(1.0f - alpha) / (2.0f * radius * (2.0f / 3.0f)) * r_volumetricSmoke->value;
		n++;
	}
	return n;
}

qboolean R_FroxelHasWork(void)
{
	int i;

	if (R_FroxelFogOn()) {
		return qtrue;
	}
	for (i = 0; i < backEnd.refdef.num_sprites; i++) {
		if (backEnd.refdef.sprites[i].froxelTaken) {
			return qtrue;
		}
	}
	return qfalse;
}

// a lattice value, wrapping every period cells
static float R_NoiseLattice(int x, int y, int z, int period)
{
	unsigned int h;

	x = ((x % period) + period) % period;
	y = ((y % period) + period) % period;
	z = ((z % period) + period) % period;
	h = (unsigned int)(x * 73856093) ^ (unsigned int)(y * 19349663) ^ (unsigned int)(z * 83492791);
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

// smooth value noise at p (in cells), wrapping every period cells
static float R_NoiseValue(float px, float py, float pz, int period)
{
	const int   x = (int)floorf(px), y = (int)floorf(py), z = (int)floorf(pz);
	float       fx = px - x, fy = py - y, fz = pz - z;
	float       c[2][2][2], a, b;
	int         i, j, k;

	for (i = 0; i < 2; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++)
				c[i][j][k] = R_NoiseLattice(x + i, y + j, z + k, period);
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	fz = fz * fz * (3.0f - 2.0f * fz);
	a = (c[0][0][0] * (1 - fx) + c[1][0][0] * fx) * (1 - fy) + (c[0][1][0] * (1 - fx) + c[1][1][0] * fx) * fy;
	b = (c[0][0][1] * (1 - fx) + c[1][0][1] * fx) * (1 - fy) + (c[0][1][1] * (1 - fx) + c[1][1][1] * fx) * fy;
	return a * (1 - fz) + b * fz;
}

/*
=============
R_FroxelSmokeNoise

The smoke's noise, made the first time it is wanted: SMOKE_NOISE_SIZE cubed,
tiling, billows (the fold of three octaves of value noise), one channel.
=============
*/
static GLuint R_FroxelSmokeNoise(void)
{
	byte *data;
	int   x, y, z;

	if (smokeNoiseTexture || !qglTexImage3D) {
		return smokeNoiseTexture;
	}

	data = ri.Hunk_AllocateTempMemory(SMOKE_NOISE_SIZE * SMOKE_NOISE_SIZE * SMOKE_NOISE_SIZE);
	for (z = 0; z < SMOKE_NOISE_SIZE; z++) {
		for (y = 0; y < SMOKE_NOISE_SIZE; y++) {
			for (x = 0; x < SMOKE_NOISE_SIZE; x++) {
				float v = 0.0f, amp = 0.5f, sum = 0.0f;
				int   period = 4, o;

				for (o = 0; o < 3; o++, period *= 2, amp *= 0.5f) {
					const float s = (float)period / SMOKE_NOISE_SIZE;
					// billows: the noise folded about its middle, upside down
					v   += amp * (1.0f - fabsf(2.0f * R_NoiseValue(x * s, y * s, z * s, period) - 1.0f));
					sum += amp;
				}
				data[(z * SMOKE_NOISE_SIZE + y) * SMOKE_NOISE_SIZE + x] = (byte)(Com_Clamp(0.0f, 1.0f, v / sum) * 255.0f);
			}
		}
	}

	qglGenTextures(1, &smokeNoiseTexture);
	GL_BindMultiTexture(GL_TEXTURE0 + TB_SPECULARMAP, GL_TEXTURE_3D, smokeNoiseTexture);
	qglTexImage3D(GL_TEXTURE_3D, 0, GL_R8, SMOKE_NOISE_SIZE, SMOKE_NOISE_SIZE, SMOKE_NOISE_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, data);
	qglTextureParameteriEXT(smokeNoiseTexture, GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	qglTextureParameteriEXT(smokeNoiseTexture, GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	qglTextureParameteriEXT(smokeNoiseTexture, GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	qglTextureParameteriEXT(smokeNoiseTexture, GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	qglTextureParameteriEXT(smokeNoiseTexture, GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	ri.Hunk_FreeTempMemory(data);
	return smokeNoiseTexture;
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
	static vec4_t blobs[SMOKE_MAX_BLOBS * 2];
	int numBlobs, sum;
	GLuint noise;
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
	// unlit. In two steps of 4 x 4 each, the first into a quarter tile each
	// (raw depth), its quad over the view's share of the depth image: one
	// fragment a tile reading all of it read one texel at a time, and kept
	// the card waiting (0.55 ms at 2560 x 1440)
	texCoords[0][0] = box[0]; texCoords[0][1] = box[3];
	texCoords[1][0] = box[2]; texCoords[1][1] = box[3];
	texCoords[2][0] = box[2]; texCoords[2][1] = box[1];
	texCoords[3][0] = box[0]; texCoords[3][1] = box[1];
	FBO_Bind(tr.froxelDepthQuarterFbo);
	qglViewport(0, 0, tr.froxelDepthQuarterFbo->width, tr.froxelDepthQuarterFbo->height);
	qglScissor(0, 0, tr.froxelDepthQuarterFbo->width, tr.froxelDepthQuarterFbo->height);
	GLSL_BindProgram(&tr.froxelShader[3]);
	VectorSet4(viewInfo, zmax / zmin, zmax, (box[2] - box[0]) / (froxel[0] * 4.0f), (box[3] - box[1]) / (froxel[1] * 4.0f));
	GLSL_SetUniformVec4(&tr.froxelShader[3], UNIFORM_VIEWINFO, viewInfo);
	VectorSet4(v, range, 0.0f, 0.0f, 0.0f);
	GLSL_SetUniformVec4(&tr.froxelShader[3], UNIFORM_VOLRANGE, v);
	GL_BindToTMU(tr.renderDepthImage, TB_LIGHTMAP);
	RB_InstantQuad2(quadVerts, texCoords);
	texCoords[0][0] = 0; texCoords[0][1] = 1;
	texCoords[1][0] = 1; texCoords[1][1] = 1;
	texCoords[2][0] = 1; texCoords[2][1] = 0;
	texCoords[3][0] = 0; texCoords[3][1] = 0;
	// then the tiles, made forward depths
	FBO_Bind(tr.froxelDepthFbo);
	qglViewport(0, 0, tr.froxelDepthFbo->width, tr.froxelDepthFbo->height);
	qglScissor(0, 0, tr.froxelDepthFbo->width, tr.froxelDepthFbo->height);
	VectorSet4(viewInfo, zmax / zmin, zmax, 1.0f / froxel[0], 1.0f / froxel[1]);
	GLSL_SetUniformVec4(&tr.froxelShader[3], UNIFORM_VIEWINFO, viewInfo);
	VectorSet4(v, range, 0.0f, 0.0f, 1.0f);
	GLSL_SetUniformVec4(&tr.froxelShader[3], UNIFORM_VOLRANGE, v);
	GL_BindToTMU(tr.froxelDepthQuarterImage, TB_LIGHTMAP);
	RB_InstantQuad2(quadVerts, texCoords);

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

	// the puffs of smoke, and their noise
	numBlobs = R_FroxelSmokeBlobs(blobs);
	noise    = numBlobs ? R_FroxelSmokeNoise() : 0;
	if (numBlobs) {
		GLSL_SetUniformSmoke(&tr.froxelShader[0], UNIFORM_SMOKEBLOBS, (const vec4_t *)blobs, numBlobs * 2);
	}
	if (noise) {
		GL_BindMultiTexture(GL_TEXTURE0 + TB_SPECULARMAP, GL_TEXTURE_3D, noise);
	}
	// how many, the noise's scale (a tile 80 units across), its drift, and
	// how far it eats into the puffs (none without it)
	VectorSet4(v, (float)numBlobs, 1.0f / 80.0f, backEnd.refdef.floatTime * 0.08f, noise ? Com_Clamp(0.0f, 1.0f, r_volumetricSmokeNoise->value) : 0.0f);
	GLSL_SetUniformVec4(&tr.froxelShader[0], UNIFORM_SMOKEPARAMS, v);

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

	// summed from the eye, as a prefix scan: the first pass the lit cells
	// and the slice before each, then each pass twice as many slices apart,
	// from one image to the other (log2 FROXEL_SLICES passes of two reads a
	// cell, where summing each cell's column read half the column a cell)
	GLSL_BindProgram(&tr.froxelShader[1]);
	{
		int pass = 0, apart;

		for (apart = 1; apart < FROXEL_SLICES; apart *= 2, pass++) {
			FBO_Bind(tr.froxelSumFbo[pass & 1]);
			GL_BindToTMU(pass ? tr.froxelSumImage[(pass - 1) & 1] : tr.froxelLightImage[lit], TB_COLORMAP);
			VectorSet4(v, (float)apart, pass ? 0.0f : 1.0f, 0.0f, 0.0f);
			GLSL_SetUniformVec4(&tr.froxelShader[1], UNIFORM_FROXELJITTER, v);
			RB_InstantQuad2(quadVerts, texCoords);
		}
		sum = (pass - 1) & 1;
	}

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
	GL_BindToTMU(tr.froxelSumImage[sum], TB_COLORMAP);
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
