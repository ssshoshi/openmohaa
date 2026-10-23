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

/*
=============================================================================

THE EDGES' WEAR

Added in OPM. A map's brushwork meets at edges drawn with a ruler: a corner
of a wall is the one colour up to the line and the other past it. What makes
a real corner look soft is mostly not its shape but what has happened to it:
an outside corner is knocked and rubbed paler, a crease where a wall meets
the floor gathers dirt.

So once a map, each texel of the lightmaps' first page, at r_edgeWearScale
texels to one of theirs, is told how closeness it is to an edge of what it is on,
within r_edgeWearWidth units, and which kind: red an outside corner (the face
past it turns away), green a crease (it turns toward). A face that meets
another in the plane of this one is past an edge of it; one that only comes
closeness, in front or behind, is not. The lighting (lightall_fp.glsl, EdgeWear)
reads it where it reads the lightmap and wears and dirties the texture's
colour by it, through a noise so that it is not a band.

The surfaces' triangles are those the sky and the bounce are worked out over
(R_RtBakeGather), in a hash of cells, each in every cell within the width of
it, so that a texel looks in its own cell alone.

=============================================================================
*/

#include "tr_local.h"

cvar_t *r_edgeWear;
cvar_t *r_edgeWearDirt;
cvar_t *r_edgeWearWidth;
cvar_t *r_edgeWearScale;

#define EW_CELL 64.0f

typedef struct {
	vec3_t   v[3];
	vec3_t   normal;
	vec3_t   mid;
	vec3_t   mins, maxs;
	qboolean terrain;
} ewTri_t;

typedef struct {
	int64_t key;
	int     tri;
} ewCell_t;

static struct {
	char      world[MAX_QPATH];
	float     width;
	int       scale;
	qboolean  tried;
	int       texWidth, texHeight;
	byte     *texels; // kept for a vid_restart, which takes the image
	image_t  *image;
} ew;

void R_EdgeWearRegister(void)
{
	r_edgeWear = ri.Cvar_Get("r_edgeWear", "0", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_edgeWear, "How much the outside corners of walls and boxes are worn paler (0 off; turning it on from 0 needs vid_restart; below 0, shows where: outside corners red, creases green)");
	r_edgeWearDirt = ri.Cvar_Get("r_edgeWearDirt", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_edgeWearDirt, "How much dirt gathers in the creases where walls meet floors and each other (with r_edgeWear)");
	r_edgeWearWidth = ri.Cvar_Get("r_edgeWearWidth", "6", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_edgeWearWidth, "How far from an edge, in units, the wear and the dirt reach (worked out again after a vid_restart)");
	r_edgeWearScale = ri.Cvar_Get("r_edgeWearScale", "4", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_edgeWearScale, "How many texels of the edges' wear to a texel of the lightmaps, a side (1 to 4; worked out again after a vid_restart)");
}

static int64_t R_EdgeWearKey(int x, int y, int z)
{
	return ((int64_t)(x + 32768) << 40) | ((int64_t)(y + 32768) << 20) | (int64_t)(z + 32768);
}

static int R_EdgeWearCompare(const void *a, const void *b)
{
	const int64_t ka = ((const ewCell_t *)a)->key, kb = ((const ewCell_t *)b)->key;

	return ka < kb ? -1 : ka > kb ? 1 : 0;
}

// The point of a triangle nearest p (Ericson, Real-Time Collision Detection 5.1.5)
static void R_EdgeWearClosest(const vec3_t p, const ewTri_t *t, vec3_t out)
{
	vec3_t ab, ac, ap, bp, cp;
	float  d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denom;

	VectorSubtract(t->v[1], t->v[0], ab);
	VectorSubtract(t->v[2], t->v[0], ac);
	VectorSubtract(p, t->v[0], ap);
	d1 = DotProduct(ab, ap);
	d2 = DotProduct(ac, ap);
	if (d1 <= 0 && d2 <= 0) {
		VectorCopy(t->v[0], out);
		return;
	}
	VectorSubtract(p, t->v[1], bp);
	d3 = DotProduct(ab, bp);
	d4 = DotProduct(ac, bp);
	if (d3 >= 0 && d4 <= d3) {
		VectorCopy(t->v[1], out);
		return;
	}
	vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) {
		v = d1 / (d1 - d3);
		VectorMA(t->v[0], v, ab, out);
		return;
	}
	VectorSubtract(p, t->v[2], cp);
	d5 = DotProduct(ab, cp);
	d6 = DotProduct(ac, cp);
	if (d6 >= 0 && d5 <= d6) {
		VectorCopy(t->v[2], out);
		return;
	}
	vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) {
		w = d2 / (d2 - d6);
		VectorMA(t->v[0], w, ac, out);
		return;
	}
	va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		vec3_t bc;

		w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		VectorSubtract(t->v[2], t->v[1], bc);
		VectorMA(t->v[1], w, bc, out);
		return;
	}
	denom = 1.0f / (va + vb + vc);
	v     = vb * denom;
	w     = vc * denom;
	VectorMA(t->v[0], v, ab, out);
	VectorMA(out, w, ac, out);
}

// How near p, on self facing normal, is to an outside corner (out[0]) and a
// crease (out[1]): 1 at the edge, 0 the width from it
static void R_EdgeWearAt(const vec3_t p, const ewTri_t *self, const ewTri_t *tris, const ewCell_t *cells, int numCells,
	float width, float out[2])
{
	ewCell_t  find;
	const ewCell_t *at;
	int       lo, hi;

	out[0] = out[1] = 0;
	find.key = R_EdgeWearKey((int)floor(p[0] / EW_CELL), (int)floor(p[1] / EW_CELL), (int)floor(p[2] / EW_CELL));

	// the first of the cell's
	lo = 0;
	hi = numCells;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;

		if (cells[mid].key < find.key) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	for (at = &cells[lo]; at < cells + numCells && at->key == find.key; at++) {
		const ewTri_t *t = &tris[at->tri];
		vec3_t         q, d;
		float          h, dist, side, closeness;

		if (t == self || DotProduct(t->normal, self->normal) > 0.9f || (t->terrain && self->terrain)) {
			continue;
		}
		if (p[0] < t->mins[0] - width || p[0] > t->maxs[0] + width || p[1] < t->mins[1] - width || p[1] > t->maxs[1] + width
			|| p[2] < t->mins[2] - width || p[2] > t->maxs[2] + width) {
			continue;
		}
		R_EdgeWearClosest(p, t, q);
		VectorSubtract(q, p, d);
		// it meets this face's plane there: an edge of it
		h = DotProduct(d, self->normal);
		if (fabs(h) > 1.0f) {
			continue;
		}
		dist = VectorLength(d);
		if (dist >= width) {
			continue;
		}
		// away behind it, or up in front of it
		VectorSubtract(t->mid, q, d);
		side = DotProduct(d, self->normal);
		closeness = 1.0f - dist / width;
		if (side < -0.25f) {
			out[0] = Q_max(out[0], closeness);
		} else if (side > 0.25f) {
			out[1] = Q_max(out[1], closeness);
		}
	}
}

// The texels no surface covered take the average of the covered ones around
// them, a ring at a time, both channels
static void R_EdgeWearDilate(byte *texels, int width, int height, int rings)
{
	byte *next = ri.Malloc(width * height * 4);
	int   n, x, y;

	for (n = 0; n < rings; n++) {
		Com_Memcpy(next, texels, width * height * 4);
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				int sum[2] = { 0, 0 }, count = 0, dx, dy;

				if (texels[(y * width + x) * 4 + 3]) {
					continue;
				}
				for (dy = -1; dy <= 1; dy++) {
					for (dx = -1; dx <= 1; dx++) {
						const int nx = x + dx, ny = y + dy;

						if (nx < 0 || ny < 0 || nx >= width || ny >= height || !texels[(ny * width + nx) * 4 + 3]) {
							continue;
						}
						sum[0] += texels[(ny * width + nx) * 4];
						sum[1] += texels[(ny * width + nx) * 4 + 1];
						count++;
					}
				}
				if (count) {
					next[(y * width + x) * 4]     = sum[0] / count;
					next[(y * width + x) * 4 + 1] = sum[1] / count;
					next[(y * width + x) * 4 + 3] = 255;
				}
			}
		}
		Com_Memcpy(texels, next, width * height * 4);
	}
	ri.Free(next);
}

static void R_EdgeWearBuild(void)
{
	rtBakeList_t list;
	ewTri_t     *tris;
	ewCell_t    *cells = NULL;
	int          numTris = 0, numCells = 0, maxCells = 0, i, k, covered = 0;
	const int    start = ri.Milliseconds();
	const float  width = ew.width;
	const int    w = ew.texWidth, h = ew.texHeight;
	double       worldArea = 0, texelArea = 0;

	Com_Memset(&list, 0, sizeof(list));
	R_RtBakeGather(0, &list);

	tris = ri.Malloc(sizeof(ewTri_t) * Q_max(1, list.num / 3));
	for (i = 0; i + 2 < list.num; i += 3) {
		ewTri_t *t = &tris[numTris];
		vec3_t   e1, e2;
		int      x0, x1, y0, y1, z0, z1;

		for (k = 0; k < 3; k++) {
			VectorCopy(list.verts[i + k].xyz, t->v[k]);
		}
		VectorSubtract(t->v[1], t->v[0], e1);
		VectorSubtract(t->v[2], t->v[0], e2);
		CrossProduct(e1, e2, t->normal);
		if (VectorNormalize(t->normal) < 1e-4f) {
			continue;
		}
		// the way the surface's own normals say it faces (the winding
		// either way round)
		if (DotProduct(t->normal, list.verts[i].normal) < 0) {
			VectorNegate(t->normal, t->normal);
		}
		VectorAdd(t->v[0], t->v[1], t->mid);
		VectorAdd(t->mid, t->v[2], t->mid);
		VectorScale(t->mid, 1.0f / 3.0f, t->mid);
		ClearBounds(t->mins, t->maxs);
		for (k = 0; k < 3; k++) {
			AddPointToBounds(t->v[k], t->mins, t->maxs);
		}
		t->terrain = list.verts[i].terrain;

		// in every cell within the width of it
		x0 = (int)floor((t->mins[0] - width) / EW_CELL);
		x1 = (int)floor((t->maxs[0] + width) / EW_CELL);
		y0 = (int)floor((t->mins[1] - width) / EW_CELL);
		y1 = (int)floor((t->maxs[1] + width) / EW_CELL);
		z0 = (int)floor((t->mins[2] - width) / EW_CELL);
		z1 = (int)floor((t->maxs[2] + width) / EW_CELL);
		{
			int x, y, z;

			for (x = x0; x <= x1; x++) {
				for (y = y0; y <= y1; y++) {
					for (z = z0; z <= z1; z++) {
						if (numCells == maxCells) {
							ewCell_t *grown;

							maxCells = Q_max(65536, maxCells * 2);
							grown    = ri.Malloc(sizeof(ewCell_t) * maxCells);
							if (cells && numCells) {
								Com_Memcpy(grown, cells, sizeof(ewCell_t) * numCells);
							}
							if (numCells) {
								ri.Free(cells);
							}
							cells = grown;
						}
						cells[numCells].key = R_EdgeWearKey(x, y, z);
						cells[numCells].tri = numTris;
						numCells++;
					}
				}
			}
		}
		numTris++;
	}
	if (!numCells) {
		cells = NULL;
	} else {
		qsort(cells, numCells, sizeof(ewCell_t), R_EdgeWearCompare);
	}

	ew.texels = ri.Malloc(w * h * 4);
	Com_Memset(ew.texels, 0, w * h * 4);

	// each triangle's texels, at its lightmap coordinates
	for (i = 0, k = 0; i + 2 < list.num; i += 3) {
		const rtBakeVert_t *v[3] = { &list.verts[i], &list.verts[i + 1], &list.verts[i + 2] };
		const ewTri_t      *self;
		float               x[3], y[3], area;
		int                 x0, x1, y0, y1, cx, cy, j;
		vec3_t              e1, e2, n;

		// (the triangles left out above are left out here)
		VectorSubtract(v[1]->xyz, v[0]->xyz, e1);
		VectorSubtract(v[2]->xyz, v[0]->xyz, e2);
		CrossProduct(e1, e2, n);
		if (VectorLength(n) < 1e-4f) {
			continue;
		}
		self = &tris[k++];

		for (j = 0; j < 3; j++) {
			x[j] = v[j]->lm[0] * w;
			y[j] = v[j]->lm[1] * h;
		}
		area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
		if (fabs(area) < 1e-8f) {
			continue;
		}
		worldArea += 0.5 * VectorLength(n);
		texelArea += 0.5 * fabs(area);

		x0 = Q_max(0, (int)floor(Q_min(x[0], Q_min(x[1], x[2]))));
		x1 = Q_min(w - 1, (int)ceil(Q_max(x[0], Q_max(x[1], x[2]))));
		y0 = Q_max(0, (int)floor(Q_min(y[0], Q_min(y[1], y[2]))));
		y1 = Q_min(h - 1, (int)ceil(Q_max(y[0], Q_max(y[1], y[2]))));
		for (cy = y0; cy <= y1 && cy <= y0 + 8192; cy++) {
			for (cx = x0; cx <= x1 && cx <= x0 + 8192; cx++) {
				const float px = cx + 0.5f, py = cy + 0.5f;
				byte       *texel = &ew.texels[(cy * w + cx) * 4];
				float       b[3], closeness[2];
				vec3_t      p = { 0, 0, 0 };

				b[0] = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
				b[1] = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
				b[2] = 1.0f - b[0] - b[1];
				if (b[0] < -0.01f || b[1] < -0.01f || b[2] < -0.01f || texel[3]) {
					continue;
				}
				for (j = 0; j < 3; j++) {
					VectorMA(p, b[j], v[j]->xyz, p);
				}
				R_EdgeWearAt(p, self, tris, cells, numCells, width, closeness);
				texel[0] = (byte)(closeness[0] * 255.0f + 0.5f);
				texel[1] = (byte)(closeness[1] * 255.0f + 0.5f);
				texel[3] = 255;
				covered++;
			}
		}
	}

	// past the edges of each surface's patch of the page, as the lightmap's
	// own filtering reads past them
	R_EdgeWearDilate(ew.texels, w, h, 2 * ew.scale);

	ri.Printf(PRINT_ALL, "edge wear: %d triangles, %d texels (%d x %d, %.1f units a texel), %.1f s\n", numTris, covered, w, h,
		texelArea > 0 ? sqrt(worldArea / texelArea) : 0.0, (ri.Milliseconds() - start) / 1000.0f);

	if (cells) {
		ri.Free(cells);
	}
	ri.Free(tris);
	if (list.verts) {
		ri.Free(list.verts);
	}
}

/*
=============
R_EdgeWearStep

Once a map, before its first view: the edges round each texel. A vid_restart
takes the image: sent again from what was kept.
=============
*/
void R_EdgeWearStep(void)
{
	if (!r_edgeWear || r_edgeWear->value <= 0 || !tr.world || !tr.lightmaps || tr.numLightmaps <= 0 || !tr.lightmaps[0]) {
		return;
	}

	// a new map, or another width or detail (r_edgeWearWidth, latched: after a vid_restart)
	if (Q_stricmp(ew.world, tr.world->name)
		|| (ew.tried && (ew.width != Q_max(1.0f, r_edgeWearWidth->value) || ew.scale != Com_Clamp(1, 4, r_edgeWearScale->integer)))) {
		if (ew.texels) {
			ri.Free(ew.texels);
		}
		Com_Memset(&ew, 0, sizeof(ew));
		Q_strncpyz(ew.world, tr.world->name, sizeof(ew.world));
	}
	if (!ew.tried) {
		ew.tried     = qtrue;
		ew.width     = Q_max(1.0f, r_edgeWearWidth->value);
		ew.scale     = Com_Clamp(1, 4, r_edgeWearScale->integer);
		ew.texWidth  = Q_min(glConfig.maxTextureSize, tr.lightmaps[0]->width * ew.scale);
		ew.texHeight = Q_min(glConfig.maxTextureSize, tr.lightmaps[0]->height * ew.scale);
		R_EdgeWearBuild();
	}
	if (ew.texels && tr.edgeWearImage != ew.image) {
		tr.edgeWearImage = NULL;
	}
	if (ew.texels && !tr.edgeWearImage) {
		tr.edgeWearImage = R_CreateImage("*edgeWear", ew.texels, ew.texWidth, ew.texHeight, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE | IMGFLAG_CLAMPTOEDGE, GL_RGBA8);
		ew.image = tr.edgeWearImage;
	}
}

/*
=============
R_EdgeWearSetUniforms

For a stage lit by lightmap (NULL: not lit by the first page, or nothing
to wear): on, how much wear, how much dirt.
=============
*/
void R_EdgeWearSetUniforms(shaderProgram_t *sp, const image_t *lightmap)
{
	vec4_t v;

	VectorClear4(v);
	if (lightmap && r_edgeWear->value != 0 && tr.edgeWearImage && tr.lightmaps && lightmap == tr.lightmaps[0]) {
		VectorSet4(v, 1.0f, r_edgeWear->value, r_edgeWearDirt->value, ew.width);
		GL_BindToTMU(tr.edgeWearImage, TMU_EDGEWEAR);
	}
	GLSL_SetUniformVec4(sp, UNIFORM_EDGEWEAR, v);
}
