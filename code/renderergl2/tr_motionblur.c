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
// tr_motionblur.c -- Added in OPM: camera motion blur (r_motionBlur)
//
// Each pixel of the main view of the world is smeared along the way it moved
// on the screen since the last frame, found from its depth (the prepass's)
// and the two cameras (glsl/motionblur_fp.glsl), as far as a camera's shutter
// open r_motionBlurShutter of a 60 Hz frame would have seen it move: the
// same smear at any frame rate. Only the camera's movement is followed (what
// moves by itself is not smeared); the view weapon is left sharp. Runs on the
// frame as lit, after r_taa's blend and before the glow and tone map. No
// smear on a map load, a jump of the camera or after a frame left out.

#include "tr_local.h"

// defined in tr_fbo.c, not in a header
FBO_t *FBO_Create(const char *name, int width, int height);

cvar_t *r_motionBlur;
cvar_t *r_motionBlurShutter;
cvar_t *r_motionBlurMax;

static struct {
	int    frame;        // tr.frameCount of the last view seen, 0 when none
	int    box[4];
	double viewProj[16]; // the camera then, without r_taa's nudge
	vec3_t origin;
	vec3_t forward;
	float  time;         // backEnd.refdef.floatTime then
} mb;

void R_MotionBlurRegisterCvars(void)
{
	r_motionBlur = ri.Cvar_Get("r_motionBlur", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_motionBlur, "Camera motion blur: the view smeared along the way it moves as the camera turns and moves "
		"(what moves by itself and the view weapon are left sharp; needs r_postProcess, r_depthPrepass)");
	r_motionBlurShutter = ri.Cvar_Get("r_motionBlurShutter", "0.5", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_motionBlurShutter, 0.0f, 2.0f, qfalse);
	ri.Cvar_SetDescription(r_motionBlurShutter, "How long r_motionBlur's shutter is open, in 60 Hz frames (0.5: a 180 degree shutter at 60 fps); "
		"the smear is the same at any frame rate");
	r_motionBlurMax = ri.Cvar_Get("r_motionBlurMax", "32", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_motionBlurMax, 1.0f, 128.0f, qfalse);
	ri.Cvar_SetDescription(r_motionBlurMax, "The longest smear r_motionBlur makes, in pixels (a fast flick of the view is not smeared further)");
}

qboolean R_MotionBlurWanted(void)
{
	return r_motionBlur->integer && glRefConfig.framebufferObject && r_postProcess->integer;
}

void R_MotionBlurCreateImages(int width, int height, int format)
{
	if (!R_MotionBlurWanted()) {
		return;
	}

	tr.motionBlurImage = R_CreateImage("*motionBlur", NULL, width, height, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, format);
	mb.frame = 0;
}

void R_MotionBlurCreateFbos(void)
{
	if (!tr.motionBlurImage) {
		return;
	}

	tr.motionBlurFbo = FBO_Create("_motionBlur", tr.motionBlurImage->width, tr.motionBlurImage->height);
	FBO_AttachImage(tr.motionBlurFbo, tr.motionBlurImage, GL_COLOR_ATTACHMENT0, 0);
	R_CheckFBO(tr.motionBlurFbo);
}

/*
=================
RB_MotionBlur

Back end, in the post process (RB_PostProcess): smears the box of src (the
frame as lit) along the camera's movement since the last frame, in place.
Once a frame, in the main view of the world.
=================
*/
void RB_MotionBlur(FBO_t *src, ivec4_t box)
{
	const viewParms_t *vp = &backEnd.viewParms;
	shaderProgram_t   *sp = &tr.motionBlurShader;
	image_t           *color;
	double             proj[16], view[16], viewProj[16], inv[16], reproject[16];
	mat4_t             reprojectf;
	vec4_t             quadVerts[4], params, area, metrics;
	vec2_t             texCoords[4];
	qboolean           valid;
	float              w, h, delta, shutter;
	int                i;

	if (!r_motionBlur->integer || !tr.motionBlurFbo || !sp->program || !src || !src->colorImage[0] || !tr.renderDepthImage
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

		FBO_Bind(tr.motionBlurFbo);
		qglViewport(box[0], box[1], box[2], box[3]);
		qglScissor(box[0], box[1], box[2], box[3]);
		GL_State(GLS_DEPTHTEST_DISABLE);
		GL_Cull(CT_TWO_SIDED);

		GLSL_BindProgram(sp);
		GL_BindToTMU(color, TB_COLORMAP);
		GL_BindToTMU(tr.renderDepthImage, TB_LIGHTMAP);

		GLSL_SetUniformMat4(sp, UNIFORM_TAAREPROJECT, reprojectf);
		// the view weapon is drawn in the first 0.3 of the depth (as r_taa has it)
		VectorSet4(params, shutter, r_motionBlurMax->value, 0.3f, 0.0f);
		GLSL_SetUniformVec4(sp, UNIFORM_TAAPARAMS, params);
		VectorSet4(area, box[0] / w, box[1] / h, box[2] / w, box[3] / h);
		GLSL_SetUniformVec4(sp, UNIFORM_TAABOX, area);
		VectorSet4(metrics, 1.0f / w, 1.0f / h, w, h);
		GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, metrics);

		RB_InstantQuad2(quadVerts, texCoords);

		FBO_FastBlit(tr.motionBlurFbo, box, src, box, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		R_GpuTimerEnd(GPUTIMER_MOTIONBLUR);
	}

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
