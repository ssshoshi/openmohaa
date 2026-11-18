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
// tr_taa.c -- Added in OPM: temporal anti-aliasing (r_taa)
//
// The main view of the world is drawn each frame from a camera nudged by a
// different fraction of a pixel (R_TaaJitter, the 2,3 Halton sequence over
// eight frames), and glsl/taa_fp.glsl keeps a running blend of the frames:
// each pixel's place in the last frame is found from its depth (the
// prepass's) and the two cameras, the blend is read there and held to this
// frame's colours round the pixel. It smooths what MSAA and SMAA do not:
// the shimmer inside textures and on their normal maps, specular sparkle,
// thin wires and fences, and the noise of the ambient occlusion and the
// soft shadows.
//
// It runs on the frame as lit (before its glow and tone map), in two frame
// sized images that take turns: the one written this frame is read the next.
// Only the camera's movement is followed (phase 1): what moves by itself is
// held by the colours round it. The blend starts again on a map load, a
// jump of the camera and whenever a frame was left out.

#include "tr_local.h"

// defined in tr_fbo.c, not in a header
FBO_t *FBO_Create(const char *name, int width, int height);

cvar_t *r_taa;
cvar_t *r_taaBlend;
cvar_t *r_taaJitter;
cvar_t *r_taaDebug;
cvar_t *r_taaTransparents;

static struct {
	int     current;     // the image written this frame
	int     frame;       // tr.frameCount of the last blend, 0 when none
	int     box[4];      // the view's box then
	double  viewProj[16]; // the camera then, without its nudge
	vec3_t  origin;
	int     lateFrame; // tr.frameCount when it ran under what is blended (RB_TaaOpaqueDone)
} taa;

void R_TaaRegisterCvars(void)
{
	r_taa = ri.Cvar_Get("r_taa", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_taa, "Temporal anti-aliasing: the view is drawn from a camera nudged a fraction of a pixel each frame "
		"and the frames are blended (needs r_postProcess, r_depthPrepass; smooths texture and specular shimmer that MSAA and SMAA leave)");
	r_taaBlend = ri.Cvar_Get("r_taaBlend", "0.1", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_taaBlend, 0.02f, 1.0f, qfalse);
	ri.Cvar_SetDescription(r_taaBlend, "How much of each new frame r_taa takes in (lower: smoother, slower to catch up)");
	r_taaJitter = ri.Cvar_Get("r_taaJitter", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_taaJitter, "Whether r_taa nudges the camera each frame (0: blend only, for testing)");
	r_taaTransparents = ri.Cvar_Get("r_taaTransparents", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_taaTransparents, "1: r_taa blends the opaque world only, and what is blended over it (smoke, fire, sprites, glass, water, decals) is drawn after, "
		"sharp and still, not smeared by the frames before (needs MSAA off); 0: all of it blended with the frames before");
	r_taaDebug = ri.Cvar_Get("r_taaDebug", "0", CVAR_CHEAT);
	ri.Cvar_SetDescription(r_taaDebug, "1: show how far r_taa found each pixel had moved (red across, green up, blue where it started again); 2: leave the pass out (the view is still nudged), to compare");
}

/*
=================
R_TaaWanted

Whether the TAA images are made: r_taa, frame buffers, and the post process
it runs in.
=================
*/
qboolean R_TaaWanted(void)
{
	return r_taa->integer && glRefConfig.framebufferObject && r_postProcess->integer;
}

/*
=================
R_TaaCreateImages
=================
*/
void R_TaaCreateImages(int width, int height, int format)
{
	int i;

	if (!R_TaaWanted()) {
		return;
	}

	for (i = 0; i < 2; i++) {
		tr.taaImage[i] = R_CreateImage(va("*taa%d", i), NULL, width, height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, format);
		// the history is read between its texels (taa_fp.glsl's History)
		qglTextureParameteriEXT(tr.taaImage[i]->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		qglTextureParameteriEXT(tr.taaImage[i]->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	taa.frame = 0;
}

/*
=================
R_TaaCreateFbos
=================
*/
void R_TaaCreateFbos(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (!tr.taaImage[i]) {
			return;
		}
		tr.taaFbo[i] = FBO_Create(va("_taa%d", i), tr.taaImage[i]->width, tr.taaImage[i]->height);
		FBO_AttachImage(tr.taaFbo[i], tr.taaImage[i], GL_COLOR_ATTACHMENT0, 0);
		R_CheckFBO(tr.taaFbo[i]);
	}
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
=================
R_TaaJitter

Front end, once the view's projection is set (R_RenderView): nudges the
main view of the world by this frame's fraction of a pixel, and notes by
how much in the view, for the back end to take it off again. Every other
view (shadow maps, portals, mirrors, water reflections, cubemaps, models in
the menus) is left as it is.
=================
*/
void R_TaaJitter(viewParms_t *parms)
{
	float jx, jy;

	parms->taaJitter[0] = parms->taaJitter[1] = 0.0f;
	parms->taaActive = qfalse;

	if (!tr.taaFbo[0] || !tr.taaFbo[1] || !r_taa->integer || !r_depthPrepass->integer) {
		return;
	}
	if (!parms->taaWanted || parms->isPortal || parms->isMirror || parms->stereoFrame != STEREO_CENTER
		|| (parms->flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW | VPF_ORTHOGRAPHIC | VPF_WATERREFLECT
			| VPF_RTSTATIC | VPF_RTDYNAMIC | VPF_RTBAKED | VPF_RTSKY))
		|| parms->viewportWidth <= 0 || parms->viewportHeight <= 0) {
		return;
	}

	parms->taaActive = qtrue;
	if (!r_taaJitter->integer) {
		return;
	}

	// a pixel either way of the middle, -0.5 to 0.5
	jx = R_Halton((tr.frameCount & 7) + 1, 2) - 0.5f;
	jy = R_Halton((tr.frameCount & 7) + 1, 3) - 0.5f;

	// in clip space, where the view is two across: projectionMatrix[8] and
	// [9] take the eye's depth into x and y, so a change there moves every
	// point across the view by the same share
	parms->taaJitter[0] = 2.0f * jx / parms->viewportWidth;
	parms->taaJitter[1] = 2.0f * jy / parms->viewportHeight;
	parms->projectionMatrix[8] -= parms->taaJitter[0];
	parms->projectionMatrix[9] -= parms->taaJitter[1];
}

// column major, as mat4_t
static void R_TaaMultiply(const double *a, const double *b, double *out)
{
	int r, c;

	for (c = 0; c < 4; c++) {
		for (r = 0; r < 4; r++) {
			out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1]
				+ a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
		}
	}
}

static qboolean R_TaaInverse(const double *m, double *out)
{
	double inv[16], det;
	int    i;

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0.0) {
		return qfalse;
	}

	det = 1.0 / det;
	for (i = 0; i < 16; i++) {
		out[i] = inv[i] * det;
	}

	return qtrue;
}

/*
=================
RB_Taa

Back end, in the post process before the glow (RB_PostProcess): blends the
box of src (the frame as lit) with the frames before, and puts the result
back in src. Does nothing when the view was not nudged for it (R_TaaJitter)
or it has been done once this frame already.
=================
*/
void RB_Taa(FBO_t *src, ivec4_t box)
{
	const viewParms_t *vp = &backEnd.viewParms;
	shaderProgram_t   *sp = &tr.taaShader;
	image_t           *color, *history;
	double             proj[16], view[16], viewProj[16], inv[16], reproject[16];
	mat4_t             reprojectf;
	vec4_t             quadVerts[4], params, area, metrics;
	vec2_t             texCoords[4];
	qboolean           valid;
	float              w, h;
	int                i;

	if (!vp->taaActive) {
		// a frame without it: what was blended is stale now
		if (!(backEnd.refdef.rdflags & RDF_NOWORLDMODEL)) {
			taa.frame = 0;
		}
		return;
	}
	if (!tr.taaFbo[0] || !tr.taaFbo[1] || !sp->program || !src || !src->colorImage[0] || !tr.renderDepthImage) {
		return;
	}
	// (r_taaDebug 2: the pass left out, to measure what it costs)
	if (r_taaDebug->integer == 2) {
		return;
	}
	if (taa.frame == tr.frameCount) {
		// a second view of the world in one frame keeps the blend of the first
		return;
	}

	color = src->colorImage[0];
	if (color->width != tr.taaImage[0]->width || color->height != tr.taaImage[0]->height) {
		return;
	}

	R_GpuTimerBegin(GPUTIMER_TAA);

	// the camera without its nudge
	for (i = 0; i < 16; i++) {
		proj[i] = vp->projectionMatrix[i];
		view[i] = vp->world.modelMatrix[i];
	}
	proj[8] += vp->taaJitter[0];
	proj[9] += vp->taaJitter[1];
	R_TaaMultiply(proj, view, viewProj);

	valid = taa.frame && taa.frame == tr.frameCount - 1 && R_TaaInverse(viewProj, inv)
		&& taa.box[0] == box[0] && taa.box[1] == box[1] && taa.box[2] == box[2] && taa.box[3] == box[3]
		&& Distance(taa.origin, vp->ori.origin) < 192.0f;
	if (valid) {
		R_TaaMultiply(taa.viewProj, inv, reproject);
		for (i = 0; i < 16; i++) {
			reprojectf[i] = reproject[i];
		}
	} else {
		Mat4Identity(reprojectf);
	}

	history = tr.taaImage[!taa.current];
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

	FBO_Bind(tr.taaFbo[taa.current]);
	qglViewport(box[0], box[1], box[2], box[3]);
	qglScissor(box[0], box[1], box[2], box[3]);
	GL_State(GLS_DEPTHTEST_DISABLE);
	GL_Cull(CT_TWO_SIDED);

	GLSL_BindProgram(sp);
	GL_BindToTMU(color, TB_COLORMAP);
	GL_BindToTMU(tr.renderDepthImage, TB_LIGHTMAP);
	GL_BindToTMU(history, TB_NORMALMAP);

	GLSL_SetUniformMat4(sp, UNIFORM_TAAREPROJECT, reprojectf);
	// the view weapon is drawn in the first 0.3 of the depth (RB_RenderDrawSurfList's
	// depth hack): the world is only that near closer than about four units
	VectorSet4(params, Com_Clamp(0.02f, 1.0f, r_taaBlend->value), valid ? 1.0f : 0.0f, 0.3f, r_taaDebug->integer == 1 ? 1.0f : 0.0f);
	GLSL_SetUniformVec4(sp, UNIFORM_TAAPARAMS, params);
	VectorSet4(area, box[0] / w, box[1] / h, box[2] / w, box[3] / h);
	GLSL_SetUniformVec4(sp, UNIFORM_TAABOX, area);
	VectorSet4(metrics, 1.0f / w, 1.0f / h, w, h);
	GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, metrics);

	RB_InstantQuad2(quadVerts, texCoords);


	FBO_FastBlit(tr.taaFbo[taa.current], box, src, box, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	for (i = 0; i < 16; i++) {
		taa.viewProj[i] = viewProj[i];
	}
	for (i = 0; i < 4; i++) {
		taa.box[i] = box[i];
	}
	VectorCopy(vp->ori.origin, taa.origin);
	taa.frame   = tr.frameCount;
	taa.current = !taa.current;

	R_GpuTimerEnd(GPUTIMER_TAA);
}

/*
=================
R_TaaUnjitter

Takes this frame's nudge back out of a view's projection.
=================
*/
static void R_TaaUnjitter(viewParms_t *vp)
{
	vp->projectionMatrix[8] += vp->taaJitter[0];
	vp->projectionMatrix[9] += vp->taaJitter[1];
	vp->taaJitter[0] = vp->taaJitter[1] = 0.0f;
}

/*
=================
RB_TaaOpaqueDone

Back end, in the main view once its opaque world is drawn (and the air
summed over it, RB_FroxelOpaqueDone), before anything blended: the frames
are blended here, and what is drawn after (smoke, fire, sprites, glass,
water, marks) goes on over the blend with the camera's nudge taken out, so
it is neither smeared by the frames before nor shaken by the nudge. Its
edges are left to SMAA. With MSAA the frame cannot be read here (it is a
multisampled buffer until the post process resolves it): the blend is then
done there, with everything in it (r_taaTransparents 0 does the same).
=================
*/
void RB_TaaOpaqueDone(void)
{
	FBO_t  *fbo = glState.currentFBO;
	ivec4_t box;

	if (!r_taa->integer || !r_taaTransparents->integer || !backEnd.viewParms.taaActive || backEnd.depthFill
		|| backEnd.projection2D || !fbo || !fbo->colorImage[0] || taa.frame == tr.frameCount) {
		return;
	}

	box[0] = backEnd.viewParms.viewportX;
	box[1] = backEnd.viewParms.viewportY;
	box[2] = backEnd.viewParms.viewportWidth;
	box[3] = backEnd.viewParms.viewportHeight;
	RB_Taa(fbo, box);
	if (taa.frame != tr.frameCount) {
		return;
	}
	taa.lateFrame = tr.frameCount;

	R_TaaUnjitter(&backEnd.viewParms);
	FBO_Bind(fbo);
	qglViewport(box[0], box[1], box[2], box[3]);
	qglScissor(box[0], box[1], box[2], box[3]);
	GL_SetProjectionMatrix(backEnd.viewParms.projectionMatrix);
}

/*
=================
RB_TaaLateView

Back end, for a pass of the same view after the blend (the sprites,
RB_SpriteSurfs): drawn without the nudge too.
=================
*/
void RB_TaaLateView(void)
{
	if (taa.lateFrame != tr.frameCount || !backEnd.viewParms.taaActive) {
		return;
	}

	R_TaaUnjitter(&backEnd.viewParms);
	GL_SetProjectionMatrix(backEnd.viewParms.projectionMatrix);
}
