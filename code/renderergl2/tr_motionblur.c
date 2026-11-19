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
// tr_motionblur.c -- Added in OPM: motion blur (r_motionBlur)
//
// Each pixel of the main view of the world is smeared along the way it moved
// on the screen since the last frame, as far as a camera's shutter open
// r_motionBlurShutter of a 60 Hz frame would have seen it move: the same smear
// at any frame rate. How far each pixel moved:
//
//  - the models that moved since the last frame are drawn again over the
//    frame, where they are and where they were (glsl/mbobject_*.glsl,
//    r_motionBlurObjects): the skeletal ones (soldiers, the dead as ragdolls,
//    vehicles, the player's own body) posed from this frame's bones and from
//    the last frame's, kept for each of them here; the brush ones (doors,
//    lifts, what a script moves) by where they were;
//  - anything else, from its depth and the two cameras (glsl/mbvelocity_fp.glsl).
//
// Then the longest smear in each tile of MB_TILE pixels and about it
// (glsl/mbtile_fp.glsl), and each pixel gathers along that and its own by
// depth (glsl/motionblur_fp.glsl): what moves smears over what is behind it,
// not only over itself. The view weapon is left sharp. Runs on the frame as
// lit, after r_taa's blend and before the glow and tone map. No smear on a
// map load, a jump of the camera or after a frame left out.

#include "tr_local.h"
#include "../tiki/tiki_mesh.h"

// defined in tr_fbo.c, not in a header
FBO_t *FBO_Create(const char *name, int width, int height);

cvar_t *r_motionBlur;
cvar_t *r_motionBlurShutter;
cvar_t *r_motionBlurMax;
cvar_t *r_motionBlurObjects;
static cvar_t *r_motionBlurDebug;

static struct {
	int    frame;        // tr.frameCount of the last view seen, 0 when none
	int    box[4];
	double viewProj[16]; // the camera then, without r_taa's nudge
	vec3_t origin;
	vec3_t forward;
	float  time;         // backEnd.refdef.floatTime then

	// the main view's surfaces, as its opaque world was done (RB_MotionBlurOpaqueDone)
	int            markFrame;
	drawSurf_t    *surfs;
	int            numSurfs;
	trRefEntity_t *entities;
	int            numEntities;
	float          markProj[16]; // the projection the depth was drawn with (r_taa's nudge in it)

	// the model being drawn (RB_MotionBlurDrawTess)
	mat4_t drawMvp, drawNow, drawWas;
	vec4_t drawParams, drawDepth;
} mb;

/*
=============================================================================

The skeletal models' last poses, a frame ago

=============================================================================
*/

#define MB_HISTORY 256

typedef struct {
	int              entityNumber; // ENTITYNUM_NONE: told by where it is
	const dtiki_t   *tiki;
	qhandle_t        hModel;       // a brush model's (doors), with no tiki
	int              frame;        // tr.frameCount it was drawn in
	int              taken;        // the frame a model took it for its own
	float            scale;
	float            transform[16]; // the model to the world
	vec3_t           origin;
	int              numBones;
	int              maxBones;
	skelBoneCache_t *bones;
} mbHistory_t;

static mbHistory_t mbHistory[MB_HISTORY];

// this frame's models: the refentity, its bones, the pose it had, where it is
static int    mbNumModels;
static int    mbModel[MAX_REFENTITIES];
static int    mbModelBones[MAX_REFENTITIES];
static int    mbModelHistory[MAX_REFENTITIES];
static byte   mbModelMoved[MAX_REFENTITIES];
static int    mbNumMoving;
static float  mbModelTransform[MAX_REFENTITIES][16];
static float  mbModelView[MAX_REFENTITIES][16];
static int    mbSeenFrame[MAX_REFENTITIES + 1];
static short  mbSlot[MAX_REFENTITIES + 1];

// the last frame's bones where this frame's are (tr.motionPrevBoneImage)
static skelBoneCache_t mbPrevBones[MAX_SKELBONES];

static void R_MbFreeHistory(void)
{
	int i;

	for (i = 0; i < MB_HISTORY; i++) {
		if (mbHistory[i].bones) {
			ri.Free(mbHistory[i].bones);
		}
	}
	Com_Memset(mbHistory, 0, sizeof(mbHistory));
}

void R_MotionBlurRegisterCvars(void)
{
	r_motionBlur = ri.Cvar_Get("r_motionBlur", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_motionBlur, "Motion blur: the view smeared along the way it moves as the camera turns and moves, and as soldiers "
		"and bodies move (the view weapon is left sharp; needs r_postProcess, r_depthPrepass)");
	r_motionBlurShutter = ri.Cvar_Get("r_motionBlurShutter", "0.5", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_motionBlurShutter, 0.0f, 2.0f, qfalse);
	ri.Cvar_SetDescription(r_motionBlurShutter, "How long r_motionBlur's shutter is open, in 60 Hz frames (0.5: a 180 degree shutter at 60 fps); "
		"the smear is the same at any frame rate");
	r_motionBlurMax = ri.Cvar_Get("r_motionBlurMax", "32", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_motionBlurMax, 1.0f, 64.0f, qfalse);
	ri.Cvar_SetDescription(r_motionBlurMax, "The longest smear r_motionBlur makes, in pixels (a fast flick of the view is not smeared further)");
	r_motionBlurObjects = ri.Cvar_Get("r_motionBlurObjects", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_motionBlurObjects, "Whether r_motionBlur smears soldiers and bodies as they move (0: only as the camera moves)");
	r_motionBlurDebug = ri.Cvar_Get("r_motionBlurDebug", "0", CVAR_CHEAT);
	ri.Cvar_SetDescription(r_motionBlurDebug, "1: r_motionBlur shows each pixel's smear as colour (red across, green up, blue: a moving model's own)");
}

qboolean R_MotionBlurWanted(void)
{
	return r_motionBlur->integer && glRefConfig.framebufferObject && r_postProcess->integer
		&& (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 30));
}

static image_t *R_MbImage(const char *name, int width, int height, int format)
{
	image_t *image = R_CreateImage(name, NULL, width, height, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, format);

	qglTextureParameterfEXT(image->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	qglTextureParameterfEXT(image->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return image;
}

void R_MotionBlurCreateImages(int width, int height, int format)
{
	mb.frame     = 0;
	mb.markFrame = 0;
	R_MbFreeHistory();

	if (!R_MotionBlurWanted()) {
		return;
	}

	tr.motionBlurImage = R_CreateImage("*motionBlur", NULL, width, height, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, format);
	tr.motionObjectImage   = R_MbImage("*motionObject", width, height, GL_RGBA16F);
	tr.motionVelocityImage = R_MbImage("*motionVelocity", width, height, GL_RGBA16F);
	tr.motionTileImage[0]  = R_MbImage("*motionTileRows", (width + MB_TILE - 1) / MB_TILE, height, GL_RGBA16F);
	tr.motionTileImage[1]  = R_MbImage("*motionTile", (width + MB_TILE - 1) / MB_TILE, (height + MB_TILE - 1) / MB_TILE, GL_RGBA16F);
	tr.motionTileImage[2]  = R_MbImage("*motionNeighbour", (width + MB_TILE - 1) / MB_TILE, (height + MB_TILE - 1) / MB_TILE, GL_RGBA16F);
	tr.motionPrevBoneImage = R_MbImage("*motionPrevBones", SKEL_GPU_BONE_ROW, (MAX_SKELBONES * 4 + SKEL_GPU_BONE_ROW - 1) / SKEL_GPU_BONE_ROW, GL_RGBA32F);
}

static FBO_t *R_MbFbo(const char *name, image_t *image)
{
	FBO_t *fbo = FBO_Create(name, image->width, image->height);

	FBO_AttachImage(fbo, image, GL_COLOR_ATTACHMENT0, 0);
	R_CheckFBO(fbo);
	return fbo;
}

void R_MotionBlurCreateFbos(void)
{
	if (!tr.motionBlurImage) {
		return;
	}

	tr.motionBlurFbo     = R_MbFbo("_motionBlur", tr.motionBlurImage);
	tr.motionObjectFbo   = R_MbFbo("_motionObject", tr.motionObjectImage);
	tr.motionVelocityFbo = R_MbFbo("_motionVelocity", tr.motionVelocityImage);
	tr.motionTileFbo[0]  = R_MbFbo("_motionTileRows", tr.motionTileImage[0]);
	tr.motionTileFbo[1]  = R_MbFbo("_motionTile", tr.motionTileImage[1]);
	tr.motionTileFbo[2]  = R_MbFbo("_motionNeighbour", tr.motionTileImage[2]);
}

static void R_MbToFloat(const double *in, float *out)
{
	int i;

	for (i = 0; i < 16; i++) {
		out[i] = in[i];
	}
}

static void R_MbMultiply(const double *a, const float *b, float *out)
{
	double bd[16], o[16];
	int    i;

	for (i = 0; i < 16; i++) {
		bd[i] = b[i];
	}
	R_TaaMultiply(a, bd, o);
	R_MbToFloat(o, out);
}

/*
=================
RB_MotionBlurOpaqueDone

Back end, in the main view once its opaque world is drawn: its surfaces and
the projection its depth was drawn with (r_taa takes its nudge out after
this), for the moving models to be drawn again over it in the post process.
=================
*/
void RB_MotionBlurOpaqueDone(drawSurf_t *drawSurfs, int numDrawSurfs)
{
	const viewParms_t *vp = &backEnd.viewParms;

	if (!r_motionBlur->integer || !tr.motionObjectFbo || backEnd.depthFill || backEnd.projection2D || vp->isPortal || vp->isPortalSky
		|| (vp->flags & (VPF_WATERREFLECT | VPF_SHADOWMAP | VPF_DEPTHSHADOW)) || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)
		|| mb.markFrame == tr.frameCount) {
		return;
	}

	mb.markFrame   = tr.frameCount;
	mb.surfs       = drawSurfs;
	mb.numSurfs    = numDrawSurfs;
	mb.entities    = backEnd.refdef.entities;
	mb.numEntities = backEnd.refdef.num_entities;
	Com_Memcpy(mb.markProj, vp->projectionMatrix, sizeof(mb.markProj));
}

// a model's surface of the main view, one of the opaque world: its entity
static int RB_MbSurfaceModel(const drawSurf_t *ds, shader_t **shader)
{
	int      entityNum, fogNum, dlighted, pshadowed;
	qboolean staticModel;

	if (*ds->surface == SF_SPRITE) {
		return -1;
	}
	R_DecomposeSort(ds->sort, &entityNum, shader, &fogNum, &dlighted, &pshadowed, &staticModel);
	if (staticModel || entityNum == REFENTITYNUM_WORLD || entityNum >= mb.numEntities || !*shader
		|| ((*shader)->sort != SS_OPAQUE && (*shader)->sort != SS_PORTAL)) {
		return -1;
	}
	return entityNum;
}

// what a skeletal model is scaled by (its bones are not), 1 for a brush model
static float R_MbScale(const trRefEntity_t *ent)
{
	return ent->e.tiki ? ent->e.tiki->load_scale * ent->e.scale : 1.0f;
}

// the pose a model had a frame ago: the entity's, or the nearest of the same
// model's where it has no number
static int R_MbFindHistory(const trRefEntity_t *ent, int numBones)
{
	float bestDist = 64.0f * 64.0f;
	int   best = -1;
	int   i;

	for (i = 0; i < MB_HISTORY; i++) {
		const mbHistory_t *h = &mbHistory[i];

		if (h->frame != tr.frameCount - 1 || h->taken == tr.frameCount || h->tiki != ent->e.tiki || h->hModel != ent->e.hModel
			|| h->numBones != numBones) {
			continue;
		}
		if (ent->e.entityNumber != ENTITYNUM_NONE) {
			if (h->entityNumber == ent->e.entityNumber) {
				best = i;
				break;
			}
			continue;
		}
		if (h->entityNumber == ENTITYNUM_NONE && DistanceSquared(h->origin, ent->e.origin) < bestDist) {
			bestDist = DistanceSquared(h->origin, ent->e.origin);
			best     = i;
		}
	}
	if (best >= 0) {
		mbHistory[best].taken = tr.frameCount;
	}
	return best;
}

// the models of the main view, their last poses found
static void RB_MbGatherModels(void)
{
	orientationr_t ori;
	shader_t      *shader;
	int            i;

	mbNumModels = 0;
	mbNumMoving = 0;
	for (i = 0; i < mb.numSurfs; i++) {
		const int      entityNum = RB_MbSurfaceModel(&mb.surfs[i], &shader);
		trRefEntity_t *ent;
		int            numBones;

		if (entityNum < 0 || mbSeenFrame[entityNum] == tr.frameCount) {
			continue;
		}
		mbSeenFrame[entityNum] = tr.frameCount;
		mbSlot[entityNum]      = -1;

		ent = &mb.entities[entityNum];
		// the view weapon goes with the camera, and is left sharp
		if (ent->e.reType != RT_MODEL || (ent->e.renderfx & RF_DEPTHHACK)) {
			continue;
		}
		// a skeletal model, or one of the map's brush models (no bones)
		numBones = 0;
		if (ent->e.tiki) {
			numBones = ri.TIKI_GetNumChannels(ent->e.tiki);
			if (numBones <= 0 || ent->e.bonestart < 0 || ent->e.bonestart + numBones > MAX_SKELBONES) {
				continue;
			}
		}

		R_RotateForEntity(ent, &backEnd.viewParms, &ori);
		mbSlot[entityNum]           = mbNumModels;
		mbModel[mbNumModels]        = entityNum;
		mbModelBones[mbNumModels]   = numBones;
		mbModelHistory[mbNumModels] = R_MbFindHistory(ent, numBones);
		Com_Memcpy(mbModelTransform[mbNumModels], ori.transformMatrix, sizeof(mbModelTransform[0]));
		Com_Memcpy(mbModelView[mbNumModels], ori.modelMatrix, sizeof(mbModelView[0]));

		// drawn again only if it moved: what is where it was moves only as
		// the camera does, which its depth tells as well
		{
			const int h = mbModelHistory[mbNumModels];

			mbModelMoved[mbNumModels] = h >= 0
				&& (memcmp(mbHistory[h].transform, ori.transformMatrix, sizeof(mbHistory[h].transform))
					|| mbHistory[h].scale != R_MbScale(ent)
					|| (numBones && memcmp(mbHistory[h].bones, &TIKI_Skel_Bones[ent->e.bonestart], numBones * sizeof(skelBoneCache_t))));
			mbNumMoving += mbModelMoved[mbNumModels];
		}
		mbNumModels++;
	}
}

// the poses kept for the next frame
static void RB_MbKeepModels(void)
{
	int k, i;

	for (k = 0; k < mbNumModels; k++) {
		const trRefEntity_t *ent = &mb.entities[mbModel[k]];
		const int            num = mbModelBones[k];
		mbHistory_t         *h   = mbModelHistory[k] >= 0 ? &mbHistory[mbModelHistory[k]] : NULL;

		// a slot no model has had since the frame before last, else one of
		// the last frame's no model took
		for (i = 0; !h && i < MB_HISTORY; i++) {
			// (frame 0: never used; a brush model's has no tiki)
			if (mbHistory[i].frame < tr.frameCount - 1) {
				h = &mbHistory[i];
			}
		}
		for (i = 0; !h && i < MB_HISTORY; i++) {
			if (mbHistory[i].frame == tr.frameCount - 1 && mbHistory[i].taken != tr.frameCount) {
				h = &mbHistory[i];
			}
		}
		if (!h) {
			continue;
		}

		if (num && h->maxBones < num) {
			if (h->bones) {
				ri.Free(h->bones);
			}
			h->bones    = (skelBoneCache_t *)ri.Malloc(num * sizeof(skelBoneCache_t));
			h->maxBones = num;
		}
		if (num) {
			Com_Memcpy(h->bones, &TIKI_Skel_Bones[ent->e.bonestart], num * sizeof(skelBoneCache_t));
		}
		h->entityNumber = ent->e.entityNumber;
		h->tiki         = ent->e.tiki;
		h->hModel       = ent->e.hModel;
		h->frame        = tr.frameCount;
		h->taken        = tr.frameCount; // not a slot for another model this frame
		h->scale        = R_MbScale(ent);
		h->numBones     = num;
		VectorCopy(ent->e.origin, h->origin);
		Com_Memcpy(h->transform, mbModelTransform[k], sizeof(h->transform));
	}
}

// the last frame's bones to the card, where this frame's are
static void RB_MbSendPrevBones(void)
{
	int lo = MAX_SKELBONES, hi = 0, k, t0, t1;

	for (k = 0; k < mbNumModels; k++) {
		const trRefEntity_t *ent   = &mb.entities[mbModel[k]];
		const int            start = ent->e.bonestart;
		const int            num   = mbModelBones[k];
		const int            h     = mbModelHistory[k];

		if (!mbModelMoved[k] || !num) {
			continue;
		}
		Com_Memcpy(&mbPrevBones[start], h >= 0 ? mbHistory[h].bones : &TIKI_Skel_Bones[start], num * sizeof(skelBoneCache_t));
		lo = MIN(lo, start);
		hi = MAX(hi, start + num);
	}

	if (hi <= lo) {
		return;
	}
	t0 = lo * 4;
	t1 = hi * 4;
	while (t0 < t1) {
		const int row = t0 / SKEL_GPU_BONE_ROW;
		const int x   = t0 - row * SKEL_GPU_BONE_ROW;
		int       w, h;

		if (x || t1 - t0 < SKEL_GPU_BONE_ROW) {
			w = MIN(SKEL_GPU_BONE_ROW - x, t1 - t0);
			h = 1;
		} else {
			w = SKEL_GPU_BONE_ROW;
			h = (t1 - t0) / SKEL_GPU_BONE_ROW;
		}
		qglTextureSubImage2DEXT(tr.motionPrevBoneImage->texnum, GL_TEXTURE_2D, 0, x, row, w, h, GL_RGBA, GL_FLOAT,
			(const float *)mbPrevBones + (size_t)t0 * 4);
		t0 += w * h;
	}
}

/*
=================
RB_MotionBlurDrawTess

From RB_EndSurface while the moving models are drawn (backEnd.motionObjects):
the batch in tess, a model's surface, drawn for how far it moved.
=================
*/
void RB_MotionBlurDrawTess(void)
{
	shaderProgram_t *sp;

	if (tess.skelGpu.active) {
		sp = &tr.mbObjectShader[1];
		GLSL_BindProgram(sp);
		RB_SkelGpuBind();
		GL_BindToTMU(tr.motionPrevBoneImage, TB_COLORMAP);
		GLSL_SetUniformVec4(sp, UNIFORM_SKELPARAMS, tess.skelGpu.params);
		GLSL_SetUniformInt(sp, UNIFORM_SKELNUMDENTS, tess.skelGpu.numDents);
		if (tess.skelGpu.numDents > 0) {
			GLSL_SetUniformSkelDents(sp, UNIFORM_SKELDENTS, tess.skelGpu.dents, tess.skelGpu.numDents * 2);
		}
		GLSL_SetUniformInt(sp, UNIFORM_SKELNUMMORPHS, tess.skelGpu.numMorphs);
		if (tess.skelGpu.numMorphs > 0) {
			GLSL_SetUniformSkelMorphs(sp, UNIFORM_SKELMORPHS, tess.skelGpu.morphs, (tess.skelGpu.numMorphs + 3) / 4);
		}
	} else {
		// a brush model, or a skeletal one posed on the CPU (moved as the
		// whole model moved): from tess, or from where its surfaces bound
		// their vertexes (the world's, the vertex cache's)
		sp = &tr.mbObjectShader[0];
		if (tess.useInternalVao) {
			RB_SkinMaterialize();
			RB_UpdateTessVao(ATTR_POSITION);
		}
		GLSL_BindProgram(sp);
	}

	GL_BindToTMU(tr.renderDepthImage, TB_LIGHTMAP);
	GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, mb.drawMvp);
	GLSL_SetUniformMat4(sp, UNIFORM_MBCURRENT, mb.drawNow);
	GLSL_SetUniformMat4(sp, UNIFORM_MBPREVIOUS, mb.drawWas);
	GLSL_SetUniformVec4(sp, UNIFORM_MBPARAMS, mb.drawParams);
	GLSL_SetUniformVec4(sp, UNIFORM_MBDEPTH, mb.drawDepth);

	R_DrawElements(tess.numIndexes, tess.firstIndex);
}

// the moving models drawn into tr.motionObjectFbo: how far each pixel of them moved
static void RB_MbDrawModels(const ivec4_t box, const double viewProj[16], const vec4_t depth)
{
	trRefEntity_t   *oldEntity = backEnd.currentEntity;
	orientationr_t   oldOri    = backEnd.ori;
	const qboolean   oldFill   = backEnd.depthFill;
	double           proj[16];
	shader_t        *shader;
	int              lastEntity = -1;
	int              i;

	FBO_Bind(tr.motionObjectFbo);
	qglViewport(box[0], box[1], box[2], box[3]);
	qglScissor(box[0], box[1], box[2], box[3]);
	qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	qglClear(GL_COLOR_BUFFER_BIT);
	GL_State(GLS_DEPTHTEST_DISABLE);
	GL_Cull(CT_TWO_SIDED);

	RB_MbSendPrevBones();

	for (i = 0; i < 16; i++) {
		proj[i] = mb.markProj[i];
	}
	VectorCopy4(depth, mb.drawDepth);

	// posed as the main view posed them, their lighting left out
	backEnd.depthFill          = qtrue;
	backEnd.motionObjects      = qtrue;
	backEnd.currentStaticModel = NULL;

	for (i = 0; i < mb.numSurfs; i++) {
		const drawSurf_t *ds        = &mb.surfs[i];
		const int         entityNum = RB_MbSurfaceModel(ds, &shader);
		int               k;

		if (entityNum < 0 || mbSeenFrame[entityNum] != tr.frameCount || (k = mbSlot[entityNum]) < 0 || !mbModelMoved[k]) {
			continue;
		}

		if (entityNum != lastEntity) {
			const trRefEntity_t *ent = &mb.entities[entityNum];
			const int            h   = mbModelHistory[k];

			lastEntity            = entityNum;
			backEnd.currentEntity = &mb.entities[entityNum];
			R_RotateForEntity(backEnd.currentEntity, &backEnd.viewParms, &backEnd.ori);

			R_MbMultiply(proj, mbModelView[k], mb.drawMvp);
			R_MbMultiply(viewProj, mbModelTransform[k], mb.drawNow);
			R_MbMultiply(mb.viewProj, h >= 0 ? mbHistory[h].transform : mbModelTransform[k], mb.drawWas);
			VectorSet4(mb.drawParams, h >= 0 ? mbHistory[h].scale : R_MbScale(ent), 0.0f, box[2], box[3]);
		}

		RB_BeginSurface(shader, 0, 0);
		rb_surfaceTable[*ds->surface](ds->surface);
		RB_EndSurface();
	}

	backEnd.motionObjects = qfalse;
	backEnd.depthFill     = oldFill;
	backEnd.currentEntity = oldEntity;
	backEnd.ori           = oldOri;
}

/*
=================
RB_MotionBlur

Back end, in the post process (RB_PostProcess): smears the box of src (the
frame as lit) along the movement since the last frame, in place. Once a
frame, in the main view of the world.
=================
*/
void RB_MotionBlur(FBO_t *src, ivec4_t box)
{
	const viewParms_t *vp = &backEnd.viewParms;
	shaderProgram_t   *sp;
	image_t           *color;
	double             proj[16], view[16], viewProj[16], inv[16], reproject[16];
	mat4_t             reprojectf;
	vec4_t             quadVerts[4], params, area, metrics, depth, edges;
	vec2_t             texCoords[4];
	qboolean           valid, models;
	float              w, h, delta, shutter;
	int                i, tilesW, tilesH;

	if (!r_motionBlur->integer || !tr.motionBlurFbo || !tr.motionBlurShader.program || !tr.mbVelocityShader.program
		|| !tr.mbTileShader[0].program || !tr.mbTileShader[1].program || !tr.mbTileShader[2].program || !src || !src->colorImage[0] || !tr.renderDepthImage
		|| !r_depthPrepass->integer || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) || vp->isPortal
		|| (vp->flags & (VPF_WATERREFLECT | VPF_SHADOWMAP | VPF_DEPTHSHADOW))) {
		return;
	}
	if (mb.frame == tr.frameCount) {
		// a second view of the world in one frame: smeared once
		return;
	}

	color = src->colorImage[0];
	if (color->width != tr.motionBlurImage->width || color->height != tr.motionBlurImage->height) {
		return;
	}

	// the camera without r_taa's nudge (taaJitter is 0 without it)
	for (i = 0; i < 16; i++) {
		proj[i] = vp->projectionMatrix[i];
		view[i] = vp->world.modelMatrix[i];
	}
	proj[8] += vp->taaJitter[0];
	proj[9] += vp->taaJitter[1];
	R_TaaMultiply(proj, view, viewProj);

	delta = backEnd.refdef.floatTime - mb.time;
	valid = mb.frame && mb.frame == tr.frameCount - 1 && delta > 0.0f && R_TaaInverse(viewProj, inv)
		&& mb.box[0] == box[0] && mb.box[1] == box[1] && mb.box[2] == box[2] && mb.box[3] == box[3]
		&& Distance(mb.origin, vp->ori.origin) < 192.0f && DotProduct(mb.forward, vp->ori.axis[0]) > 0.5f;

	// a shutter open so long, against how long the frame took
	shutter = valid ? (r_motionBlurShutter->value / 60.0f) / delta : 0.0f;

	// the moving models, as the main view drew them this frame
	models = r_motionBlurObjects->integer && mb.markFrame == tr.frameCount && tr.mbObjectShader[0].program
		&& tr.mbObjectShader[1].program && tr.motionPrevBoneImage;
	mbNumModels = 0;
	mbNumMoving = 0;
	if (models) {
		RB_MbGatherModels();
	}

	// nothing to smear: the camera as it was, and no model that could move
	if (valid && !mbNumMoving) {
		for (i = 0; i < 16 && fabs(viewProj[i] - mb.viewProj[i]) < 1e-6; i++) {
		}
		valid = i < 16;
	}

	if (valid && shutter > 0.0f) {
		R_GpuTimerBegin(GPUTIMER_MOTIONBLUR);

		R_TaaMultiply(mb.viewProj, inv, reproject);
		for (i = 0; i < 16; i++) {
			reprojectf[i] = reproject[i];
		}

		w = color->width;
		h = color->height;
		VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
		VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
		VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
		VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
		VectorSet2(texCoords[0], box[0] / w, (box[1] + box[3]) / h);
		VectorSet2(texCoords[1], (box[0] + box[2]) / w, (box[1] + box[3]) / h);
		VectorSet2(texCoords[2], (box[0] + box[2]) / w, box[1] / h);
		VectorSet2(texCoords[3], box[0] / w, box[1] / h);
		VectorSet4(area, box[0] / w, box[1] / h, box[2] / w, box[3] / h);
		VectorSet4(metrics, 1.0f / w, 1.0f / h, w, h);
		VectorSet4(depth, vp->projectionMatrix[10], vp->projectionMatrix[14], 0.0f, 0.0f);
		VectorSet4(edges, box[0], box[1], box[0] + box[2] - 1, box[1] + box[3] - 1);
		tilesW = (box[2] + MB_TILE - 1) / MB_TILE;
		tilesH = (box[3] + MB_TILE - 1) / MB_TILE;

		if (mbNumMoving) {
			RB_MbDrawModels(box, viewProj, depth);
		}

		// each pixel's smear
		sp = &tr.mbVelocityShader;
		FBO_Bind(tr.motionVelocityFbo);
		qglViewport(box[0], box[1], box[2], box[3]);
		qglScissor(box[0], box[1], box[2], box[3]);
		GL_State(GLS_DEPTHTEST_DISABLE);
		GL_Cull(CT_TWO_SIDED);
		GLSL_BindProgram(sp);
		GL_BindToTMU(tr.renderDepthImage, TB_COLORMAP);
		GL_BindToTMU(tr.motionObjectImage, TB_LIGHTMAP);
		GLSL_SetUniformMat4(sp, UNIFORM_TAAREPROJECT, reprojectf);
		// the view weapon is drawn in the first 0.3 of the depth (as r_taa has it)
		VectorSet4(params, shutter, r_motionBlurMax->value, 0.3f, mbNumMoving ? 1.0f : 0.0f);
		GLSL_SetUniformVec4(sp, UNIFORM_TAAPARAMS, params);
		GLSL_SetUniformVec4(sp, UNIFORM_TAABOX, area);
		GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, metrics);
		GLSL_SetUniformVec4(sp, UNIFORM_MBDEPTH, depth);
		RB_InstantQuad2(quadVerts, texCoords);

		// the longest in each tile's rows
		sp = &tr.mbTileShader[0];
		FBO_Bind(tr.motionTileFbo[0]);
		qglViewport(0, 0, tilesW, box[3]);
		qglScissor(0, 0, tilesW, box[3]);
		GLSL_BindProgram(sp);
		GL_BindToTMU(tr.motionVelocityImage, TB_COLORMAP);
		GLSL_SetUniformVec4(sp, UNIFORM_MBPARAMS, edges);
		RB_InstantQuad2(quadVerts, texCoords);

		// in each tile
		sp = &tr.mbTileShader[1];
		FBO_Bind(tr.motionTileFbo[1]);
		qglViewport(0, 0, tilesW, tilesH);
		qglScissor(0, 0, tilesW, tilesH);
		GLSL_BindProgram(sp);
		GL_BindToTMU(tr.motionTileImage[0], TB_COLORMAP);
		VectorSet4(params, 0.0f, 0.0f, tilesW - 1, box[3] - 1);
		GLSL_SetUniformVec4(sp, UNIFORM_MBPARAMS, params);
		RB_InstantQuad2(quadVerts, texCoords);

		// and about it
		sp = &tr.mbTileShader[2];
		FBO_Bind(tr.motionTileFbo[2]);
		GLSL_BindProgram(sp);
		GL_BindToTMU(tr.motionTileImage[1], TB_COLORMAP);
		VectorSet4(params, 0.0f, 0.0f, tilesW - 1, tilesH - 1);
		GLSL_SetUniformVec4(sp, UNIFORM_MBPARAMS, params);
		RB_InstantQuad2(quadVerts, texCoords);

		// gathered
		sp = &tr.motionBlurShader;
		FBO_Bind(tr.motionBlurFbo);
		qglViewport(box[0], box[1], box[2], box[3]);
		qglScissor(box[0], box[1], box[2], box[3]);
		GLSL_BindProgram(sp);
		GL_BindToTMU(color, TB_COLORMAP);
		GL_BindToTMU(tr.motionVelocityImage, TB_LIGHTMAP);
		GL_BindToTMU(tr.motionTileImage[2], TB_NORMALMAP);
		GLSL_SetUniformVec4(sp, UNIFORM_MBPARAMS, edges);
		VectorSet4(params, (float)(tr.frameCount & 63), r_motionBlurDebug->integer ? 1.0f : 0.0f, 0.0f, 0.0f);
		GLSL_SetUniformVec4(sp, UNIFORM_TAAPARAMS, params);
		GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, metrics);
		GL_BindToTMU(tr.motionObjectImage, TB_DELUXEMAP);
		RB_InstantQuad2(quadVerts, texCoords);

		FBO_FastBlit(tr.motionBlurFbo, box, src, box, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		R_GpuTimerEnd(GPUTIMER_MOTIONBLUR);
	}

	// the models' poses for the next frame, smeared this one or not
	RB_MbKeepModels();

	for (i = 0; i < 16; i++) {
		mb.viewProj[i] = viewProj[i];
	}
	for (i = 0; i < 4; i++) {
		mb.box[i] = box[i];
	}
	VectorCopy(vp->ori.origin, mb.origin);
	VectorCopy(vp->ori.axis[0], mb.forward);
	mb.time  = backEnd.refdef.floatTime;
	mb.frame = tr.frameCount;
}
