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
// tr_smaa.c -- Added in OPM: SMAA 1x anti-aliasing (r_smaa)
//
// Subpixel Morphological Antialiasing (Jimenez et al. 2012,
// https://github.com/iryoku/smaa, MIT): the edges of the tone mapped frame are
// found by their contrast, each edge pixel is weighed by the shape of the edge
// it is on (looked up in a precomputed area texture), and every pixel is
// blended with the neighbour its weights name. It smooths what multisampling
// cannot: alpha tested fences, grates and foliage, and the edges inside a
// texture, for about half the cost of MSAA 2x; on a plain geometric edge it is
// about as good as MSAA 2x. It works on the frame after the tone map and
// before anything 2D, so the HUD and the menus are left sharp.
//
// It needs r_hdr's tone map pass (the scratch image the tone map writes is its
// input) and GLSL 1.30. The shader is glsl/smaa_lib.glsl with the wrappers
// glsl/smaa_vp.glsl and glsl/smaa_fp.glsl.

#include "tr_local.h"

// defined in tr_fbo.c, not in a header
FBO_t *FBO_Create(const char *name, int width, int height);

#include "../thirdparty/smaa/AreaTex.h"
#include "../thirdparty/smaa/SearchTex.h"

/*
=================
R_SmaaWanted

Whether the SMAA resources are made: r_smaa, the tone map's scratch image to
read, and a GLSL that has textureLod with offsets.
=================
*/
qboolean R_SmaaWanted(void)
{
	if (!r_smaa->integer || !glRefConfig.framebufferObject || !r_hdr->integer) {
		return qfalse;
	}

	return glRefConfig.glslMajorVersion > 1 || glRefConfig.glslMinorVersion >= 30;
}

static void R_SmaaFilter(image_t *image, int filter)
{
	qglTextureParameteriEXT(image->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	qglTextureParameteriEXT(image->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

/*
=================
R_SmaaCreateImages

The edges and the blend weights, a frame each, and the two lookup textures
(two channel and one channel in the reference, widened to RGBA here).
=================
*/
void R_SmaaCreateImages(int width, int height)
{
	byte *pic;
	int   i;

	if (!R_SmaaWanted()) {
		return;
	}

	tr.smaaEdgesImage = R_CreateImage("*smaaEdges", NULL, width, height, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8);
	tr.smaaBlendImage = R_CreateImage("*smaaBlend", NULL, width, height, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8);
	R_SmaaFilter(tr.smaaEdgesImage, GL_LINEAR);
	R_SmaaFilter(tr.smaaBlendImage, GL_LINEAR);

	pic = ri.Hunk_AllocateTempMemory(AREATEX_WIDTH * AREATEX_HEIGHT * 4);

	for (i = 0; i < AREATEX_WIDTH * AREATEX_HEIGHT; i++) {
		pic[i * 4 + 0] = areaTexBytes[i * 2 + 0];
		pic[i * 4 + 1] = areaTexBytes[i * 2 + 1];
		pic[i * 4 + 2] = 0;
		pic[i * 4 + 3] = 255;
	}
	tr.smaaAreaImage = R_CreateImage("*smaaArea", pic, AREATEX_WIDTH, AREATEX_HEIGHT, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_NOLIGHTSCALE, GL_RGBA8);
	R_SmaaFilter(tr.smaaAreaImage, GL_LINEAR);

	for (i = 0; i < SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT; i++) {
		pic[i * 4 + 0] = searchTexBytes[i];
		pic[i * 4 + 1] = 0;
		pic[i * 4 + 2] = 0;
		pic[i * 4 + 3] = 255;
	}
	tr.smaaSearchImage = R_CreateImage("*smaaSearch", pic, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, IMGTYPE_COLORALPHA,
		IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_NOLIGHTSCALE, GL_RGBA8);
	// the search texture is read at its texel centres, where point and
	// bilinear agree; the reference reads it bilinear
	R_SmaaFilter(tr.smaaSearchImage, GL_LINEAR);

	ri.Hunk_FreeTempMemory(pic);
}

/*
=================
R_SmaaCreateFbos
=================
*/
void R_SmaaCreateFbos(void)
{
	if (!tr.smaaEdgesImage || !tr.smaaBlendImage) {
		return;
	}

	tr.smaaEdgesFbo = FBO_Create("_smaaEdges", tr.smaaEdgesImage->width, tr.smaaEdgesImage->height);
	FBO_AttachImage(tr.smaaEdgesFbo, tr.smaaEdgesImage, GL_COLOR_ATTACHMENT0, 0);
	R_CheckFBO(tr.smaaEdgesFbo);

	tr.smaaBlendFbo = FBO_Create("_smaaBlend", tr.smaaBlendImage->width, tr.smaaBlendImage->height);
	FBO_AttachImage(tr.smaaBlendFbo, tr.smaaBlendImage, GL_COLOR_ATTACHMENT0, 0);
	R_CheckFBO(tr.smaaBlendFbo);
}

/*
=================
RB_SmaaPass

One of the three passes: a quad over the box of dst, reading the same box of
the frame sized images bound.
=================
*/
static void RB_SmaaPass(shaderProgram_t *sp, FBO_t *dst, ivec4_t box, image_t *map0, image_t *map1, image_t *map2, qboolean clear)
{
	vec4_t quadVerts[4], metrics;
	vec2_t texCoords[4];
	float  x0, y0, x1, y1, w, h;

	w  = map0->width;
	h  = map0->height;
	x0 = box[0] / w;
	y0 = box[1] / h;
	x1 = (box[0] + box[2]) / w;
	y1 = (box[1] + box[3]) / h;

	FBO_Bind(dst);
	qglViewport(box[0], box[1], box[2], box[3]);
	qglScissor(box[0], box[1], box[2], box[3]);

	if (clear) {
		// the edge pass discards where there is no edge, and the weights are
		// only written on edges: what is not written must read as none
		qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		qglClear(GL_COLOR_BUFFER_BIT);
	}

	VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet2(texCoords[0], x0, y1);
	VectorSet2(texCoords[1], x1, y1);
	VectorSet2(texCoords[2], x1, y0);
	VectorSet2(texCoords[3], x0, y0);

	GLSL_BindProgram(sp);
	GL_BindToTMU(map0, TB_COLORMAP);
	if (map1) {
		GL_BindToTMU(map1, TB_LIGHTMAP);
	}
	if (map2) {
		GL_BindToTMU(map2, TB_NORMALMAP);
	}

	VectorSet4(metrics, 1.0f / w, 1.0f / h, w, h);
	GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, metrics);

	GL_State(GLS_DEPTHTEST_DISABLE);
	GL_Cull(CT_TWO_SIDED);
	RB_InstantQuad2(quadVerts, texCoords);
}

/*
=================
RB_Smaa

Anti-aliases the box of src (the tone mapped frame) into the same box of dst.
src and dst must differ. Returns qfalse when SMAA is off or not made, leaving
dst untouched.
=================
*/
qboolean RB_Smaa(FBO_t *src, ivec4_t box, FBO_t *dst)
{
	image_t *color;

	if (!r_smaa->integer || !tr.smaaEdgesFbo || !tr.smaaBlendFbo || !src || !dst || src == dst) {
		return qfalse;
	}
	if (!tr.smaaShader[0].program || !tr.smaaShader[1].program || !tr.smaaShader[2].program) {
		return qfalse;
	}

	color = src->colorImage[0];
	if (!color || color->width != tr.smaaEdgesImage->width || color->height != tr.smaaEdgesImage->height) {
		return qfalse;
	}

	RB_SmaaPass(&tr.smaaShader[0], tr.smaaEdgesFbo, box, color, NULL, NULL, qtrue);
	RB_SmaaPass(&tr.smaaShader[1], tr.smaaBlendFbo, box, tr.smaaEdgesImage, tr.smaaAreaImage, tr.smaaSearchImage, qtrue);
	if (r_smaa->integer < 0) {
		// debug: -1 shows the edges, -2 the blend weights
		FBO_FastBlit(r_smaa->integer == -1 ? tr.smaaEdgesFbo : tr.smaaBlendFbo, box, dst, box, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		return qtrue;
	}
	RB_SmaaPass(&tr.smaaShader[2], dst, box, color, tr.smaaBlendImage, NULL, qfalse);

	return qtrue;
}
