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
// tr_pyro.c - Added in OPM: simulated explosions drawn as volumes
//
// An explosion simulated offline (tools/pyro: Blender's smoke and fire) is a
// .vol model: its smoke's density and its flame in a grid of cells, frame
// after frame. cgame spawns it as any temp model (originspawn, model
// models/fx/NAME.vol); the renderer keeps every frame in one 3D texture,
// the frames side by side, and draws each one alive after the opaque world
// (RB_PyroVolumes): each pixel marches through the box to the depth of what
// is behind it, the smoke shadowing itself toward the light where it is and
// lit by the fire inside it, the flame glowing. Seen from anywhere, in the
// map's own light, and cut by walls and ground where they are.

#include "tr_local.h"
#include "tr_dsa.h"

#define PYRO_MAGIC   "OPMV"
#define PYRO_VERSION 1
#define PYRO_MAX_FRAMES 128

typedef struct pyroVolume_s {
	int    nx, ny, nz;
	int    numFrames;
	int    tilesX, tilesY;  // the frames in the 3D texture: tilesX across, tilesY up
	vec3_t size;           // the box at scale 1, units
	vec3_t offset;         // its lowest corner from the entity's origin
	float  extinction;     // per unit, where a cell holds 1
	float  fire;           // the flame's brightness
	float  times[PYRO_MAX_FRAMES];
	GLuint texture;        // RG8: sqrt of density, flame
} pyroVolume_t;

static cvar_t *r_pyro;
static cvar_t *r_pyroSteps;
static cvar_t *r_pyroFire;
static cvar_t *r_pyroDensity;
static cvar_t *r_pyroAlbedo;
static cvar_t *r_pyroHalf;
static cvar_t *r_pyroDebug;

static pyroVolume_t *pyroVolumes[64];
static int           numPyroVolumes;

void R_PyroInit(void)
{
	r_pyro = ri.Cvar_Get("r_pyro", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_pyro, "Simulated explosions (.vol models) drawn as volumes, lit by the map and cut by walls (0: not drawn)");
	r_pyroSteps = ri.Cvar_Get("r_pyroSteps", "64", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_pyroSteps, "Steps through an explosion's volume for each pixel: more is finer and slower");
	r_pyroFire = ri.Cvar_Get("r_pyroFire", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_pyroFire, "How brightly the fire of simulated explosions glows");
	r_pyroDensity = ri.Cvar_Get("r_pyroDensity", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_pyroDensity, "How thick the smoke of simulated explosions is");
	r_pyroAlbedo = ri.Cvar_Get("r_pyroAlbedo", "0.025", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_pyroAlbedo, "How much light the smoke of simulated explosions sends back: soot is near black (0.025), dust and steam light (0.3)");
	r_pyroHalf = ri.Cvar_Get("r_pyroHalf", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_pyroHalf, "Simulated explosions marched at half the view's size and blended over it (0: full size, about four times the cost)");
	r_pyroDebug = ri.Cvar_Get("r_pyroDebug", "0", CVAR_CHEAT);
	ri.Cvar_SetDescription(r_pyroDebug, "1: draw each explosion volume's box; 2: print what is drawn; 3: print every volume entity");
}

void R_PyroShutdown(void)
{
	int i;

	for (i = 0; i < numPyroVolumes; i++) {
		if (pyroVolumes[i]->texture) {
			qglDeleteTextures(1, &pyroVolumes[i]->texture);
		}
	}
	numPyroVolumes = 0;
}

/*
=============
R_RegisterPyroVolume

A .vol model (tools/pyro/pyro.py make_vol): its frames into one 3D texture.
=============
*/
qhandle_t R_RegisterPyroVolume(const char *name, model_t *mod)
{
	union {
		byte *b;
		void *v;
	} buf;
	int           len, i, frameBytes, need;
	const byte   *p;
	pyroVolume_t *vol;
	byte         *atlas;
	int           header[5];

	if (!qglTexImage3D) {
		return 0;
	}
	len = ri.FS_ReadFile(name, &buf.v);
	if (!buf.b) {
		return 0;
	}
	p = buf.b;
	if (len < 4 + 5 * 4 + 8 * 4 || memcmp(p, PYRO_MAGIC, 4)) {
		ri.Printf(PRINT_WARNING, "R_RegisterPyroVolume: %s is not a volume\n", name);
		ri.FS_FreeFile(buf.v);
		return 0;
	}
	memcpy(header, p + 4, sizeof(header));
	for (i = 0; i < 5; i++) {
		header[i] = LittleLong(header[i]);
	}
	if (header[0] != PYRO_VERSION || header[1] <= 0 || header[2] <= 0 || header[3] <= 0
		|| header[4] <= 0 || header[4] > PYRO_MAX_FRAMES) {
		ri.Printf(PRINT_WARNING, "R_RegisterPyroVolume: %s has version %d or a bad size\n", name, header[0]);
		ri.FS_FreeFile(buf.v);
		return 0;
	}
	vol = ri.Hunk_Alloc(sizeof(*vol), h_low);
	vol->nx = header[1];
	vol->ny = header[2];
	vol->nz = header[3];
	vol->numFrames = header[4];
	p += 4 + sizeof(header);
	{
		float f[8];

		memcpy(f, p, sizeof(f));
		for (i = 0; i < 8; i++) {
			f[i] = LittleFloat(f[i]);
		}
		VectorCopy(f, vol->size);
		VectorCopy(f + 3, vol->offset);
		vol->extinction = f[6];
		vol->fire       = f[7];
		p += sizeof(f);
	}
	frameBytes = vol->nx * vol->ny * vol->nz * 2;
	need = (int)(p - buf.b) + vol->numFrames * 4 + vol->numFrames * frameBytes;
	if (len < need) {
		ri.Printf(PRINT_WARNING, "R_RegisterPyroVolume: %s is cut short (%d of %d bytes)\n", name, len, need);
		ri.FS_FreeFile(buf.v);
		return 0;
	}
	for (i = 0; i < vol->numFrames; i++) {
		float t;

		memcpy(&t, p + i * 4, 4);
		vol->times[i] = LittleFloat(t);
	}
	p += vol->numFrames * 4;

	// the frames side by side, as near square as they go
	vol->tilesX = (int)ceil(sqrt((double)vol->numFrames));
	vol->tilesY = (vol->numFrames + vol->tilesX - 1) / vol->tilesX;
	{
		const int w = vol->nx * vol->tilesX, h = vol->ny * vol->tilesY;
		int       z, y, frame;

		atlas = ri.Hunk_AllocateTempMemory(w * h * vol->nz * 2);
		Com_Memset(atlas, 0, w * h * vol->nz * 2);
		for (frame = 0; frame < vol->numFrames; frame++) {
			const byte *src = p + frame * frameBytes;
			const int   x0 = (frame % vol->tilesX) * vol->nx, y0 = (frame / vol->tilesX) * vol->ny;

			for (z = 0; z < vol->nz; z++) {
				for (y = 0; y < vol->ny; y++) {
					memcpy(atlas + (((size_t)z * h + y0 + y) * w + x0) * 2, src + ((size_t)(z * vol->ny + y) * vol->nx) * 2, vol->nx * 2);
				}
			}
		}
		qglGenTextures(1, &vol->texture);
		GL_TextureImage3D(vol->texture, GL_RG8, w, h, vol->nz, GL_RG, GL_UNSIGNED_BYTE, atlas);
		qglTextureParameteriEXT(vol->texture, GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		qglTextureParameteriEXT(vol->texture, GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		qglTextureParameteriEXT(vol->texture, GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		qglTextureParameteriEXT(vol->texture, GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		qglTextureParameteriEXT(vol->texture, GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		ri.Hunk_FreeTempMemory(atlas);
	}
	ri.FS_FreeFile(buf.v);

	if (numPyroVolumes < (int)ARRAY_LEN(pyroVolumes)) {
		pyroVolumes[numPyroVolumes++] = vol;
	}
	mod->type     = MOD_VOLUME;
	mod->d.volume = vol;
	mod->dataSize = vol->nx * vol->tilesX * vol->ny * vol->tilesY * vol->nz * 2;
	ri.Printf(PRINT_DEVELOPER, "volume %s: %d frames of %dx%dx%d, %.1f s\n", name, vol->numFrames, vol->nx, vol->ny, vol->nz,
		vol->times[vol->numFrames - 1]);
	return mod->index;
}

/*
=============
R_PyroFrame

The two frames either side of t (seconds since it was spawned) and how far
from the first to the second; false once it is over.
=============
*/
static qboolean R_PyroFrame(const pyroVolume_t *vol, float t, int *a, int *b, float *mix)
{
	int i;

	if (t < 0 || t > vol->times[vol->numFrames - 1]) {
		return qfalse;
	}
	for (i = 0; i < vol->numFrames - 1 && vol->times[i + 1] < t; i++) {
	}
	*a = i;
	*b = Q_min(i + 1, vol->numFrames - 1);
	*mix = *b > *a ? (t - vol->times[*a]) / Q_max(0.001f, vol->times[*b] - vol->times[*a]) : 0.0f;
	*mix = Com_Clamp(0.0f, 1.0f, *mix);
	return qtrue;
}

/*
=============
R_PyroScissor

The box's corners on the view: the part of the view its pixels can be in,
or false where it is all behind the eye or off the view. A box the eye is
in, or that reaches behind it, takes the whole view.
=============
*/
static qboolean R_PyroScissor(const vec3_t mins, const vec3_t maxs, int *box)
{
	const viewParms_t *vp = &backEnd.viewParms;
	const float tanX = tan(vp->fovX * M_PI / 360.0f), tanY = tan(vp->fovY * M_PI / 360.0f);
	float x0 = 1, y0 = 1, x1 = -1, y1 = -1;
	int i;

	for (i = 0; i < 8; i++) {
		vec3_t c, d;
		float  fwd, x, y;

		c[0] = (i & 1) ? maxs[0] : mins[0];
		c[1] = (i & 2) ? maxs[1] : mins[1];
		c[2] = (i & 4) ? maxs[2] : mins[2];
		VectorSubtract(c, vp->ori.origin, d);
		fwd = DotProduct(d, vp->ori.axis[0]);
		if (fwd < r_znear->value) {
			box[0] = vp->viewportX;
			box[1] = vp->viewportY;
			box[2] = vp->viewportWidth;
			box[3] = vp->viewportHeight;
			return qtrue;
		}
		x = -DotProduct(d, vp->ori.axis[1]) / (fwd * tanX);
		y = DotProduct(d, vp->ori.axis[2]) / (fwd * tanY);
		x0 = Q_min(x0, x);
		x1 = Q_max(x1, x);
		y0 = Q_min(y0, y);
		y1 = Q_max(y1, y);
	}
	x0 = Q_max(x0, -1.0f);
	y0 = Q_max(y0, -1.0f);
	x1 = Q_min(x1, 1.0f);
	y1 = Q_min(y1, 1.0f);
	if (x0 >= x1 || y0 >= y1) {
		return qfalse;
	}
	box[0] = vp->viewportX + (int)floor((x0 * 0.5f + 0.5f) * vp->viewportWidth);
	box[1] = vp->viewportY + (int)floor((y0 * 0.5f + 0.5f) * vp->viewportHeight);
	box[2] = (int)ceil((x1 - x0) * 0.5f * vp->viewportWidth) + 2;
	box[3] = (int)ceil((y1 - y0) * 0.5f * vp->viewportHeight) + 2;
	return qtrue;
}

/*
=============
RB_PyroVolumes

Every explosion volume alive, over the main view's opaque world (with its
depth) and under what is blended after it. Once a frame.
=============
*/
void RB_PyroVolumes(void)
{
	static int  drawnFrame = -1;
	FBO_t      *fbo = glState.currentFBO;
	shaderProgram_t *sp = &tr.pyroShader;
	vec4_t      quadVerts[4], v;
	vec2_t      texCoords[4];
	qboolean    bound = qfalse;
	int         i, drawn = 0;
	const viewParms_t *vp = &backEnd.viewParms;
	// marched at half size into tr.pyroFbo, then blended over the view
	const qboolean half = r_pyroHalf && r_pyroHalf->integer && tr.pyroFbo && tr.pyroCompositeShader.program;
	const int      div  = half ? 2 : 1;
	int            vx = vp->viewportX / div, vy = vp->viewportY / div;
	int            vw = (vp->viewportWidth + div - 1) / div, vh = (vp->viewportHeight + div - 1) / div;
	int            all[4] = { 0, 0, 0, 0 };  // the boxes' union, full size

	if (!r_pyro || !r_pyro->integer || !sp->program || backEnd.depthFill || backEnd.projection2D || vp->isPortal
		|| vp->isPortalSky || (vp->flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP)) || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)
		|| !glRefConfig.framebufferObject || !tr.renderDepthImage || drawnFrame == tr.frameCount) {
		return;
	}

	for (i = 0; i < backEnd.refdef.num_entities; i++) {
		const trRefEntity_t *ent = &backEnd.refdef.entities[i];
		const model_t       *mod;
		const pyroVolume_t  *vol;
		vec3_t   mins, maxs, ambient, directed, dir;
		int      a, b, box[4];
		float    mix, scale, t, fade;

		if (ent->e.reType != RT_MODEL || ent->e.hModel <= 0 || ent->e.hModel >= tr.numModels) {
			continue;
		}
		mod = tr.models[ent->e.hModel];
		if (mod->type != MOD_VOLUME || !mod->d.volume) {
			continue;
		}
		vol = mod->d.volume;
		// the scene's own time: in the middle of the surfaces floatTime is the
		// time since the entity drawn last was spawned (tr_backend.c), and after
		// a temp model's surfaces this was minus the volume's whole age
		t = backEnd.refdef.time * 0.001 - ent->e.shaderTime;
		if (r_pyroDebug->integer == 3) {
			ri.Printf(PRINT_ALL, "pyro entity %s t %.3f (now %.3f spawned %.3f) scale %.2f alpha %d\n", mod->name, t,
				backEnd.refdef.time * 0.001, ent->e.shaderTime, ent->e.scale, ent->e.shaderRGBA[3]);
		}
		if (!R_PyroFrame(vol, t, &a, &b, &mix)) {
			continue;
		}
		scale = ent->e.scale > 0 ? ent->e.scale : 1.0f;
		VectorMA(ent->e.origin, scale, vol->offset, mins);
		VectorMA(mins, scale, vol->size, maxs);
		if (!R_PyroScissor(mins, maxs, box)) {
			continue;
		}
		// it thins away over its last quarter, as the smoke the effect
		// spawns takes over; and with the temp model's own fade
		fade = Com_Clamp(0.0f, 1.0f, (vol->times[vol->numFrames - 1] - t) / Q_max(0.01f, vol->times[vol->numFrames - 1] * 0.25f));
		fade *= (ent->e.shaderRGBA[3] ? ent->e.shaderRGBA[3] : 255) / 255.0f;

		if (!bound) {
			const float tanX = tan(vp->fovX * M_PI / 360.0f), tanY = tan(vp->fovY * M_PI / 360.0f);

			bound = qtrue;
			R_GpuTimerBegin(GPUTIMER_PYRO);
			if (half) {
				FBO_Bind(tr.pyroFbo);
				qglViewport(vx, vy, vw, vh);
				qglScissor(vx, vy, vw, vh);
				qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				qglClearColor(0, 0, 0, 0);
				qglClear(GL_COLOR_BUFFER_BIT);
				qglColorMask(!backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3]);
			} else {
				FBO_Bind(fbo);
				qglViewport(vx, vy, vw, vh);
			}
			GLSL_BindProgram(sp);
			GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
			GL_Cull(CT_TWO_SIDED);
			// the air in front of it (r_volumetric 2), taken at the depth of
			// what is seen of it (pyro_fp.glsl): as a colour already times
			// its alpha, which is what it is
			R_FroxelSetFog(sp, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
			GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, vp->ori.origin);
			GLSL_SetUniformVec3(sp, UNIFORM_VIEWFORWARD, vp->ori.axis[0]);
			VectorScale(vp->ori.axis[1], tanX, v);
			GLSL_SetUniformVec3(sp, UNIFORM_VIEWLEFT, v);
			VectorScale(vp->ori.axis[2], tanY, v);
			GLSL_SetUniformVec3(sp, UNIFORM_VIEWUP, v);
			// zfar / znear, zfar; the viewport's corner and one over its size
			VectorSet4(v, vp->zFar / r_znear->value, vp->zFar, 0, 0);
			GLSL_SetUniformVec4(sp, UNIFORM_VIEWINFO, v);
			VectorSet4(v, (float)vx, (float)vy, 1.0f / vw, 1.0f / vh);
			GLSL_SetUniformVec4(sp, UNIFORM_PYROVIEW, v);
			VectorSet4(v, 1.0f / tr.renderDepthImage->width, 1.0f / tr.renderDepthImage->height, (float)div, 0);
			GLSL_SetUniformVec4(sp, UNIFORM_PYRODEPTH, v);
			GL_BindToTMU(tr.renderDepthImage, TB_LIGHTMAP);

			VectorSet4(quadVerts[0], -1,  1, 0, 1);
			VectorSet4(quadVerts[1],  1,  1, 0, 1);
			VectorSet4(quadVerts[2],  1, -1, 0, 1);
			VectorSet4(quadVerts[3], -1, -1, 0, 1);
			texCoords[0][0] = 0; texCoords[0][1] = 1;
			texCoords[1][0] = 1; texCoords[1][1] = 1;
			texCoords[2][0] = 1; texCoords[2][1] = 0;
			texCoords[3][0] = 0; texCoords[3][1] = 0;
		}

		if (!drawn) {
			memcpy(all, box, sizeof(all));
		} else {
			const int x1 = Q_max(all[0] + all[2], box[0] + box[2]), y1 = Q_max(all[1] + all[3], box[1] + box[3]);

			all[0] = Q_min(all[0], box[0]);
			all[1] = Q_min(all[1], box[1]);
			all[2] = x1 - all[0];
			all[3] = y1 - all[1];
		}
		qglScissor(box[0] / div, box[1] / div, box[2] / div + 2, box[3] / div + 2);
		GL_BindMultiTexture(GL_TEXTURE0 + TB_SPECULARMAP, GL_TEXTURE_3D, vol->texture);

		VectorCopy(mins, v);
		v[3] = vol->extinction / scale * Q_max(0.0f, r_pyroDensity->value);
		GLSL_SetUniformVec4(sp, UNIFORM_PYROBOX, v);
		VectorScale(vol->size, scale, v);
		v[3] = fade;
		GLSL_SetUniformVec4(sp, UNIFORM_PYROSIZE, v);
		// the frames' corners in the texture, in tiles
		VectorSet4(v, (float)(a % vol->tilesX), (float)(a / vol->tilesX), (float)(b % vol->tilesX), (float)(b / vol->tilesX));
		GLSL_SetUniformVec4(sp, UNIFORM_PYROFRAMES, v);
		VectorSet4(v, mix, (float)vol->tilesX, (float)vol->tilesY, (float)Com_Clamp(8, 256, r_pyroSteps->integer));
		GLSL_SetUniformVec4(sp, UNIFORM_PYROPARAMS, v);
		VectorSet4(v, (float)vol->nx, (float)vol->ny, (float)vol->nz, r_pyroDebug->integer == 1 ? 1.0f : 0.0f);
		GLSL_SetUniformVec4(sp, UNIFORM_PYROCELLS, v);

		// the map's light where it is: the grid's, a sky-ish ambient and
		// the strongest direction
		{
			vec3_t mid;

			VectorAdd(mins, maxs, mid);
			VectorScale(mid, 0.5f, mid);
			mid[2] = mins[2] + scale * vol->size[2] * 0.3f;
			if (!R_LightForPoint(mid, ambient, directed, dir)) {
				VectorSet(ambient, 90, 90, 95);
				VectorSet(directed, 160, 150, 135);
				VectorSet(dir, 0.3f, 0.2f, 0.93f);
			}
		}
		VectorScale(ambient, 1.0f / 255.0f, v);
		v[3] = vol->fire * Q_max(0.0f, r_pyroFire->value);
		GLSL_SetUniformVec4(sp, UNIFORM_PYROAMBIENT, v);
		VectorScale(directed, 1.0f / 255.0f, v);
		v[3] = Com_Clamp(0.0f, 1.0f, r_pyroAlbedo->value);
		GLSL_SetUniformVec4(sp, UNIFORM_PYROSUN, v);
		VectorNormalize(dir);
		VectorCopy(dir, v);
		v[3] = 0;
		GLSL_SetUniformVec4(sp, UNIFORM_PYRODIR, v);

		RB_InstantQuad2(quadVerts, texCoords);
		drawn++;
		if (r_pyroDebug->integer == 2) {
			ri.Printf(PRINT_ALL, "pyro %s t %.2f frames %d-%d %.2f fade %.2f box %d %d %d %d light %.0f %.0f %.0f / %.0f %.0f %.0f\n", mod->name,
				t, a, b, mix, fade, box[0], box[1], box[2], box[3], ambient[0], ambient[1], ambient[2], directed[0], directed[1], directed[2]);
		}
	}

	if (bound) {
		if (half && drawn) {
			shaderProgram_t *cp = &tr.pyroCompositeShader;

			FBO_Bind(fbo);
			qglViewport(vp->viewportX, vp->viewportY, vp->viewportWidth, vp->viewportHeight);
			qglScissor(all[0], all[1], all[2], all[3]);
			GLSL_BindProgram(cp);
			GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
			VectorSet4(v, 0.5f / tr.pyroImage->width, 0.5f / tr.pyroImage->height, 0, 0);
			GLSL_SetUniformVec4(cp, UNIFORM_PYRODEPTH, v);
			GL_BindToTMU(tr.pyroImage, TB_COLORMAP);
			RB_InstantQuad2(quadVerts, texCoords);
		} else if (half) {
			FBO_Bind(fbo);
		}
		R_GpuTimerEnd(GPUTIMER_PYRO);
		qglViewport(vp->viewportX, vp->viewportY, vp->viewportWidth, vp->viewportHeight);
		qglScissor(vp->viewportX, vp->viewportY, vp->viewportWidth, vp->viewportHeight);
	}
	if (drawn) {
		drawnFrame = tr.frameCount;
	}
}
