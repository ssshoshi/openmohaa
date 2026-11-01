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

// Added in OPM
// tr_lens.c: what is on the lens. Water thrown up by a splash, blood from a
// man shot in front of the camera, dirt from a blast: the cgame says what is
// on it and where (cg_lens.cpp, RE_SetLensSpots), and the finished frame of
// the world's view is drawn again through it (glsl/lens_fp.glsl), last of the
// post-processing, under the 2D.

#include "tr_local.h"

cvar_t *r_lens;

// x, y, size, how much is left; what, seed, how far it has run, 0
static vec4_t lensSpots[WATER_MAX_SPOTS];
static vec4_t lensLook[WATER_MAX_SPOTS];
static int    numLensSpots;

void R_LensRegisterCvars(void)
{
    r_lens = ri.Cvar_Get("r_lens", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_lens, "Draws what lands on the camera's lens: water from splashes, blood, dirt from blasts");
}

void RE_SetLensSpots(const float *spots, int num)
{
    int i;

    numLensSpots = Com_Clamp(0, WATER_MAX_SPOTS, num);
    for (i = 0; i < numLensSpots; i++) {
        VectorCopy4(spots + i * 8, lensSpots[i]);
        VectorCopy4(spots + i * 8 + 4, lensLook[i]);
    }
}

/*
===============
RB_LensDraw

Over the world's view in fbo, its box in it: called from RB_PostProcess.
===============
*/
void RB_LensDraw(FBO_t *fbo, ivec4_t box)
{
    shaderProgram_t *sp = &tr.lensShader;
    vec4_t           quadVerts[4], v;
    vec2_t           texCoords[4];
    float            x0, y0, x1, y1;

    if (!r_lens->integer || !numLensSpots || !sp->program || !tr.waterSceneFbo || !glRefConfig.framebufferBlit
        || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) || backEnd.viewParms.isPortal) {
        return;
    }

    // a copy of the frame to look through
    FBO_FastBlit(fbo, box, tr.waterSceneFbo, box, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    FBO_Bind(fbo);
    qglViewport(box[0], box[1], box[2], box[3]);
    qglScissor(box[0], box[1], box[2], box[3]);

    x0 = box[0] / (float)tr.waterSceneImage->width;
    y0 = box[1] / (float)tr.waterSceneImage->height;
    x1 = (box[0] + box[2]) / (float)tr.waterSceneImage->width;
    y1 = (box[1] + box[3]) / (float)tr.waterSceneImage->height;

    VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
    VectorSet2(texCoords[0], x0, y1);
    VectorSet2(texCoords[1], x1, y1);
    VectorSet2(texCoords[2], x1, y0);
    VectorSet2(texCoords[3], x0, y0);

    GLSL_BindProgram(sp);
    GL_BindToTMU(tr.waterSceneImage, TB_COLORMAP);

    VectorSet4(v, numLensSpots, box[3] > 0 ? box[2] / (float)box[3] : 1.0f, x1 - x0, y1 - y0);
    GLSL_SetUniformVec4(sp, UNIFORM_LENSINFO, v);
    GLSL_SetUniformWaterSpots(sp, UNIFORM_LENSSPOTS, lensSpots, numLensSpots);
    GLSL_SetUniformWaterSpots(sp, UNIFORM_LENSLOOK, lensLook, numLensSpots);

    GL_State(GLS_DEPTHTEST_DISABLE);
    GL_Cull(CT_TWO_SIDED);
    RB_InstantQuad2(quadVerts, texCoords);
}
