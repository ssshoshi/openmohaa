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
// tr_texalign.c -- Added in OPM: textures that carry on round corners (r_texAlign)
//
// Each face of a map's brushwork has its own texture placement, so where two
// walls of the same brick meet at a corner the courses usually jump and the
// bricks are cut off at different places on each side. This slides textures
// sideways as the map loads so that a wall's texture carries on from the
// wall beside it, as if the corner were unfolded flat: courses run round the
// corner, and the normal and height maps (parallax) follow, so the bricks
// read as wrapping the edge. Only the texture coordinates change: no
// geometry, no lighting.
//
// - A wall is the faces of one plane with the same shader placed the same
//   way (the compiler's pieces of it): they always move together.
// - A corner is a vertical edge where two walls with the same shader meet,
//   inside or out, or a seam between two differently placed walls of one
//   plane. It is aligned only when the texture has the same scale and runs
//   the same way across it once unfolded, its courses already agree up the
//   edge, and the texture's other axis runs along the wall: textures only
//   slide along walls, never up or down (a band placed at a height stays).
// - The slides spread from wall to wall over the corners they share,
//   longest first; round a closed loop (a building) the wrap rarely comes
//   out to whole repeats, so one corner keeps a seam, the shortest.
//
// The same corners give the parallax its silhouette (r_parallaxSilhouette):
// at an outside corner the relief of a face is cut away where the ray into
// it goes out past the edge before it meets the relief, so the mortar runs
// out as notches in the outline instead of the wall ending in a straight
// line. A face beside one gets a coordinate across it in its vertices'
// normal[3] (unused: always 0), -1 and 1 at the corners' edges, which the
// shader carries along the ray (lightall_fp.glsl); this works with or
// without r_texAlign, which only lines the notches up on both sides.

#include "tr_local.h"

cvar_t *r_texAlign;
cvar_t *r_parallaxSilhouette;

static int ta_turned, ta_flat, ta_unfold, ta_scaleUp, ta_courses, ta_mirrored;

#define TA_HASH 65536

typedef struct {
	msurface_t      *surf;
	srfBspSurface_t *cv;
	vec3_t           n;
	float            d;
	vec3_t           centroid;
	vec3_t           grad[2];  // how s and t change with position (in the face's plane)
	float            base[2];  // s and t at the origin
	int              wall;     // the wall (union find root through walls[])
	int              silEdges; // its ends at an outside corner: 1 the low end of across, 2 the high one
	vec3_t           across;   // horizontal, in its plane
	float            acrossMin, acrossMax;
} taFace_t;

typedef struct {
	int   parent;
	float offset;   // this wall's slide, relative to its parent (in its sliding component)
	int   comp;     // the component (0: s, 1: t) it slides in: the one running along it
	float area;
} taWall_t;

// a vertical side of a face's outline
typedef struct {
	int    face;
	float  x, y, z0, z1;
	vec3_t inward;  // into the face from the side, in its plane
	int    next;
} taSide_t;

// two walls that meet along a corner
typedef struct {
	int   a, b;
	float delta;   // b's slide minus a's, for the texture to carry on (mod 1)
	float weight;  // how long they meet for
} taLink_t;

static taFace_t *taFaces;
static int       taNumFaces;
static taWall_t *taWalls;
static taSide_t *taSides;
static int       taNumSides, taMaxSides;
static int      *taSideHash;
static taLink_t *taLinks;
static int       taNumLinks, taMaxLinks;

static float TA_Wrap(float x)
{
	x -= floor(x + 0.5f);
	return x; // into [-0.5, 0.5)
}

static float TA_Coord(const taFace_t *f, int c, const vec3_t p)
{
	return DotProduct(f->grad[c], p) + f->base[c];
}

/*
=================
TA_Find

The wall a face's wall is part of, and its slide relative to that root.
=================
*/
static int TA_Find(int w, float *offset)
{
	float total = 0;

	while (taWalls[w].parent != w) {
		total += taWalls[w].offset;
		w = taWalls[w].parent;
	}
	if (offset) {
		*offset = total;
	}
	return w;
}

/*
=================
TA_SetupFace

The face's plane and its texture placement as a gradient over position, the
placement checked against every vertex (a planar face's always holds).
=================
*/
static qboolean TA_SetupFace(taFace_t *f)
{
	srfBspSurface_t *cv = f->cv;
	int              i, j, best[3] = { 0, 0, 0 };
	float            bestArea = 0, m[3][3], inv[3][3], det;
	vec3_t           axisU, axisV, e1, e2, cr;

	if (cv->numVerts < 3 || cv->numIndexes < 3) {
		return qfalse;
	}
	VectorCopy(cv->cullPlane.normal, f->n);
	f->d = cv->cullPlane.dist;

	VectorClear(f->centroid);
	for (i = 0; i < cv->numVerts; i++) {
		VectorAdd(f->centroid, cv->verts[i].xyz, f->centroid);
	}
	VectorScale(f->centroid, 1.0f / cv->numVerts, f->centroid);

	for (i = 0; i < cv->numIndexes; i += 3) {
		const float *a = cv->verts[cv->indexes[i]].xyz, *b = cv->verts[cv->indexes[i + 1]].xyz, *c = cv->verts[cv->indexes[i + 2]].xyz;
		float        area;

		VectorSubtract(b, a, e1);
		VectorSubtract(c, a, e2);
		CrossProduct(e1, e2, cr);
		area = VectorLength(cr);
		if (area > bestArea) {
			bestArea = area;
			best[0] = cv->indexes[i];
			best[1] = cv->indexes[i + 1];
			best[2] = cv->indexes[i + 2];
		}
	}
	if (bestArea < 1.0f) {
		return qfalse;
	}

	PerpendicularVector(axisU, f->n);
	CrossProduct(f->n, axisU, axisV);
	for (i = 0; i < 3; i++) {
		m[i][0] = DotProduct(cv->verts[best[i]].xyz, axisU);
		m[i][1] = DotProduct(cv->verts[best[i]].xyz, axisV);
		m[i][2] = 1.0f;
	}
	det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
		+ m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	if (fabs(det) < 1e-6f) {
		return qfalse;
	}
	inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
	inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
	inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
	inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
	inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
	inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
	inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
	inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
	inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;

	for (j = 0; j < 2; j++) {
		float y[3], coef[3];

		for (i = 0; i < 3; i++) {
			y[i] = cv->verts[best[i]].st[j];
		}
		for (i = 0; i < 3; i++) {
			coef[i] = inv[i][0] * y[0] + inv[i][1] * y[1] + inv[i][2] * y[2];
		}
		VectorScale(axisU, coef[0], f->grad[j]);
		VectorMA(f->grad[j], coef[1], axisV, f->grad[j]);
		f->base[j] = coef[2];
	}

	for (i = 0; i < cv->numVerts; i++) {
		for (j = 0; j < 2; j++) {
			if (fabs(TA_Coord(f, j, cv->verts[i].xyz) - cv->verts[i].st[j]) > 0.01f) {
				return qfalse;
			}
		}
	}
	return qtrue;
}

static qboolean TA_ShaderOk(const shader_t *sh)
{
	return sh && !sh->isSky && !sh->isPortal && !sh->numDeforms && !(sh->surfaceFlags & (SURF_NODRAW | SURF_SKY)) && sh->stages[0];
}

// whether two faces place their texture alike: same shader, plane, gradients, and phase
static qboolean TA_SamePlacement(const taFace_t *a, const taFace_t *b)
{
	int c;

	if (a->surf->shader != b->surf->shader || DotProduct(a->n, b->n) < 0.9999f || fabs(a->d - b->d) > 0.1f) {
		return qfalse;
	}
	for (c = 0; c < 2; c++) {
		vec3_t diff;

		VectorSubtract(a->grad[c], b->grad[c], diff);
		if (VectorLength(diff) > 0.0005f + 0.01f * VectorLength(a->grad[c])) {
			return qfalse;
		}
		if (fabs(TA_Wrap(TA_Coord(a, c, a->centroid) - TA_Coord(b, c, a->centroid))) > 0.01f) {
			return qfalse;
		}
	}
	return qtrue;
}

static unsigned TA_HashXY(float x, float y)
{
	int ix = (int)floor(x * 2.0f + 0.5f), iy = (int)floor(y * 2.0f + 0.5f);

	return ((unsigned)ix * 73856093u ^ (unsigned)iy * 19349663u) & (TA_HASH - 1);
}

/*
=================
TA_Link

What two faces meeting along a vertical side need for the texture to carry
on from a to b, if it can: b's slide minus a's. qfalse when it cannot
(different scale or direction, courses not agreeing up the edge, or no
axis running along both walls).
=================
*/
static qboolean TA_Link(const taFace_t *a, const taSide_t *sa, const taFace_t *b, const taSide_t *sb, const vec3_t p, int *compOut, float *delta)
{
	static const vec3_t up = { 0, 0, 1 };
	int                 h, v;

	// the component running along the walls (horizontal) and the one up them
	h = fabs(DotProduct(a->grad[0], up)) < fabs(DotProduct(a->grad[1], up)) ? 0 : 1;
	v = 1 - h;
	if (fabs(DotProduct(a->grad[h], up)) > 0.02f * VectorLength(a->grad[h]) || fabs(DotProduct(b->grad[h], up)) > 0.02f * VectorLength(b->grad[h])) {
		ta_turned++;
		return qfalse; // the texture is turned on the wall: sliding would move it up
	}
	if (fabs(DotProduct(b->grad[v], up)) < 0.5f * VectorLength(b->grad[v])) {
		ta_flat++;
		return qfalse;
	}
	// unfolded, the same scale across the corner. The same direction (going
	// past the edge out of a is going into b) carries the texture on; the
	// opposite one, which the editor's projection along the world's axes
	// gives many corners, mirrors it at the edge: the two still meet there,
	// mortar to mortar, which reads as a corner stone
	{
		float across = DotProduct(b->grad[h], sb->inward), back = -DotProduct(a->grad[h], sa->inward);

		if (fabs(fabs(across) - fabs(back)) > 0.02f * VectorLength(a->grad[h])) {
			ta_unfold++;
			return qfalse;
		}
		if (across * back < 0) {
			ta_mirrored++;
		}
	}
	if (fabs(DotProduct(b->grad[v], up) - DotProduct(a->grad[v], up)) > 0.02f * VectorLength(a->grad[v])) {
		ta_scaleUp++;
		return qfalse;
	}
	// the courses must already agree up the edge (nothing slides vertically)
	if (fabs(TA_Wrap(TA_Coord(a, v, p) - TA_Coord(b, v, p))) > 0.03f) {
		ta_courses++;
		return qfalse;
	}
	*compOut = h;
	*delta = TA_Wrap(TA_Coord(a, h, p) - TA_Coord(b, h, p));
	return qtrue;
}

static int TA_CompareLinks(const void *x, const void *y)
{
	const taLink_t *a = x, *b = y;

	return a->weight > b->weight ? -1 : (a->weight < b->weight ? 1 : 0);
}

/*
=================
TA_ParallaxShader

Whether the shader's surfaces are drawn with parallax at all.
=================
*/
static qboolean TA_ParallaxShader(const shader_t *sh)
{
	const shaderStage_t *st = sh->stages[0];

	return st && st->glslShaderGroup == tr.lightallShader && (st->glslShaderIndex & LIGHTDEF_USE_PARALLAXMAP) && !(st->stateBits & GLS_ATEST_BITS);
}

/*
=================
TA_SilhouetteSide

Marks the end of the face a side of it is at as an outside corner.
=================
*/
static void TA_SilhouetteSide(taFace_t *f, const taSide_t *sd)
{
	static const vec3_t up = { 0, 0, 1 };
	float               x;
	int                 i;

	if (!f->silEdges) {
		CrossProduct(up, f->n, f->across);
		if (VectorNormalize(f->across) < 0.5f) {
			return;
		}
		f->acrossMin = f->acrossMax = DotProduct(f->across, f->cv->verts[0].xyz);
		for (i = 1; i < f->cv->numVerts; i++) {
			x = DotProduct(f->across, f->cv->verts[i].xyz);
			f->acrossMin = Q_min(f->acrossMin, x);
			f->acrossMax = Q_max(f->acrossMax, x);
		}
		if (f->acrossMax - f->acrossMin < 1.0f) {
			return;
		}
	}
	x = f->across[0] * sd->x + f->across[1] * sd->y;
	f->silEdges |= fabs(x - f->acrossMin) < fabs(x - f->acrossMax) ? 1 : 2;
}

/*
=================
TA_SilhouetteWrite

The coordinate across a face beside an outside corner, -1 and 1 at its
ends there; an end at no corner is kept just inside, at 0, so that the
ray going out past it cuts nothing away (the gradient stays that of the
whole width).
=================
*/
static void TA_SilhouetteWrite(taFace_t *f)
{
	float lo, hi, width = f->acrossMax - f->acrossMin;
	int   i;

	switch (f->silEdges) {
	case 1:
		lo = -1.0f;
		hi = 0.0f;
		break;
	case 2:
		lo = 0.0f;
		hi = 1.0f;
		break;
	default:
		lo = -1.0f;
		hi = 1.0f;
		break;
	}
	for (i = 0; i < f->cv->numVerts; i++) {
		float x = (DotProduct(f->across, f->cv->verts[i].xyz) - f->acrossMin) / width;
		float w = lo + (hi - lo) * Com_Clamp(0.0f, 1.0f, x);

		f->cv->verts[i].normal[3] = (int16_t)(w * 32767.0f + (w > 0.0f ? 0.5f : -0.5f));
	}
	f->cv->parallaxSilhouette = qtrue;
}

/*
=================
R_TexAlignWorld

Called once the world's surfaces are loaded, before anything is drawn.
=================
*/
void R_TexAlignWorld(world_t *world)
{
	int i, j, k, firstSurface, numSurfaces, numWalls = 0, moved = 0, aligned = 0, seams = 0, startTime = ri.Milliseconds();
	int stat_unfold = 0, stat_pairs = 0, silCorners = 0, silFaces = 0;
	qboolean doAlign = r_texAlign->integer != 0, doSilhouette = r_parallaxSilhouette->integer && r_parallaxMapping->integer;

	ta_turned = ta_flat = ta_unfold = ta_scaleUp = ta_courses = ta_mirrored = 0;
	if ((!doAlign && !doSilhouette) || !world || !world->bmodels || world->numBModels < 1) {
		return;
	}
	firstSurface = world->bmodels[0].firstSurface;
	numSurfaces = world->bmodels[0].numSurfaces;

	taFaces = ri.Hunk_AllocateTempMemory(Q_max(numSurfaces, 1) * sizeof(*taFaces));
	taNumFaces = 0;
	for (i = 0; i < numSurfaces; i++) {
		msurface_t *surf = &world->surfaces[firstSurface + i];
		taFace_t   *f = &taFaces[taNumFaces];

		if (!surf->data || *surf->data != SF_FACE || !TA_ShaderOk(surf->shader)) {
			continue;
		}
		Com_Memset(f, 0, sizeof(*f));
		f->surf = surf;
		f->cv = (srfBspSurface_t *)surf->data;
		if (TA_SetupFace(f)) {
			taNumFaces++;
		}
	}

	// walls: faces of one plane and shader placed alike (by plane and shader
	// buckets, so the compare stays local)
	taWalls = ri.Hunk_AllocateTempMemory(Q_max(taNumFaces, 1) * sizeof(*taWalls));
	{
		int *bucket = ri.Hunk_AllocateTempMemory(TA_HASH * sizeof(int) + Q_max(taNumFaces, 1) * sizeof(int));
		int *next = bucket + TA_HASH;

		for (i = 0; i < TA_HASH; i++) {
			bucket[i] = -1;
		}
		for (i = 0; i < taNumFaces; i++) {
			taFace_t *f = &taFaces[i];
			unsigned  h = ((unsigned)(int)floor(f->n[0] * 64 + 0.5f) * 73856093u ^ (unsigned)(int)floor(f->n[1] * 64 + 0.5f) * 19349663u
						  ^ (unsigned)(int)floor(f->n[2] * 64 + 0.5f) * 83492791u ^ (unsigned)(int)floor(f->d * 0.5f + 0.5f) * 2654435761u
						  ^ (unsigned)(size_t)f->surf->shader * 40503u)
						 & (TA_HASH - 1);

			f->wall = -1;
			for (j = bucket[h]; j != -1; j = next[j]) {
				if (TA_SamePlacement(&taFaces[j], f)) {
					f->wall = taFaces[j].wall;
					break;
				}
			}
			if (f->wall < 0) {
				f->wall = numWalls;
				taWalls[numWalls].parent = numWalls;
				taWalls[numWalls].offset = 0;
				taWalls[numWalls].comp = -1;
				taWalls[numWalls].area = 0;
				numWalls++;
			}
			next[i] = bucket[h];
			bucket[h] = i;
		}
		ri.Hunk_FreeTempMemory(bucket);
	}

	// the vertical sides of every face's outline, by where they stand
	taMaxSides = 0;
	for (i = 0; i < taNumFaces; i++) {
		taMaxSides += taFaces[i].cv->numIndexes;
	}
	taSides = ri.Hunk_AllocateTempMemory(Q_max(taMaxSides, 1) * sizeof(*taSides) + TA_HASH * sizeof(int));
	taSideHash = (int *)(taSides + Q_max(taMaxSides, 1));
	for (i = 0; i < TA_HASH; i++) {
		taSideHash[i] = -1;
	}
	taNumSides = 0;
	for (i = 0; i < taNumFaces; i++) {
		taFace_t        *f = &taFaces[i];
		srfBspSurface_t *cv = f->cv;

		for (j = 0; j < cv->numIndexes; j += 3) {
			for (k = 0; k < 3; k++) {
				int          a = cv->indexes[j + k], b = cv->indexes[j + (k + 1) % 3], l, shared = 0;
				const float *pa = cv->verts[a].xyz, *pb = cv->verts[b].xyz;
				taSide_t    *sd;
				vec3_t       dir;
				unsigned     h;

				if (fabs(pa[0] - pb[0]) > 0.05f || fabs(pa[1] - pb[1]) > 0.05f || fabs(pa[2] - pb[2]) < 1.0f) {
					continue; // not vertical
				}
				// a side of the outline: no other triangle of the face has it
				for (l = 0; l < cv->numIndexes && !shared; l += 3) {
					int m;
					for (m = 0; m < 3; m++) {
						if (cv->indexes[l + m] == b && cv->indexes[l + (m + 1) % 3] == a) {
							shared = 1;
						}
					}
				}
				if (shared || taNumSides >= taMaxSides) {
					continue;
				}
				sd = &taSides[taNumSides];
				sd->face = i;
				sd->x = pa[0];
				sd->y = pa[1];
				sd->z0 = Q_min(pa[2], pb[2]);
				sd->z1 = Q_max(pa[2], pb[2]);
				VectorSet(dir, 0, 0, 1);
				CrossProduct(f->n, dir, sd->inward);
				VectorNormalize(sd->inward);
				if (DotProduct(sd->inward, f->centroid) - DotProduct(sd->inward, pa) < 0) {
					VectorNegate(sd->inward, sd->inward);
				}
				h = TA_HashXY(sd->x, sd->y);
				sd->next = taSideHash[h];
				taSideHash[h] = taNumSides++;
			}
		}
	}

	// links: walls meeting along a vertical edge, same shader, different walls
	taMaxLinks = taNumSides * 2 + 16;
	taLinks = ri.Hunk_AllocateTempMemory(taMaxLinks * sizeof(*taLinks));
	taNumLinks = 0;
	for (i = 0; i < taNumSides; i++) {
		const taSide_t *sa = &taSides[i];
		const taFace_t *fa = &taFaces[sa->face];

		for (j = taSideHash[TA_HashXY(sa->x, sa->y)]; j != -1; j = taSides[j].next) {
			const taSide_t *sb = &taSides[j];
			const taFace_t *fb = &taFaces[sb->face];
			float           z0, z1, delta;
			vec3_t          p;
			int             comp;

			if (j <= i || fb->wall == fa->wall || fb->surf->shader != fa->surf->shader || fabs(sa->x - sb->x) > 0.05f || fabs(sa->y - sb->y) > 0.05f) {
				continue;
			}
			z0 = Q_max(sa->z0, sb->z0);
			z1 = Q_min(sa->z1, sb->z1);
			if (z1 - z0 < 1.0f) {
				continue;
			}
			// Added in OPM: an outside corner, where the relief can be cut
			// away (each face round it lies behind the other's plane)
			if (doSilhouette && TA_ParallaxShader(fa->surf->shader) && DotProduct(sb->inward, fa->n) < -0.25f
				&& DotProduct(sa->inward, fb->n) < -0.25f) {
				TA_SilhouetteSide(&taFaces[sa->face], sa);
				TA_SilhouetteSide(&taFaces[sb->face], sb);
				silCorners++;
				if (z1 - z0 >= 96.0f) {
					ri.Printf(PRINT_DEVELOPER, "texalign: outside corner at %.0f %.0f, %.0f to %.0f, %s\n", sa->x, sa->y, z0, z1, fa->surf->shader->name);
				}
			}
			if (!doAlign) {
				continue;
			}
			stat_pairs++;
			VectorSet(p, sa->x, sa->y, 0.5f * (z0 + z1));
			if (!TA_Link(fa, sa, fb, sb, p, &comp, &delta)) {
				stat_unfold++;
				continue;
			}
			if (taWalls[fa->wall].comp < 0) {
				taWalls[fa->wall].comp = comp;
			}
			if (taWalls[fb->wall].comp < 0) {
				taWalls[fb->wall].comp = comp;
			}
			if (taWalls[fa->wall].comp != comp || taWalls[fb->wall].comp != comp || taNumLinks >= taMaxLinks) {
				continue;
			}
			taLinks[taNumLinks].a = fa->wall;
			taLinks[taNumLinks].b = fb->wall;
			taLinks[taNumLinks].delta = delta;
			taLinks[taNumLinks].weight = z1 - z0;
			taNumLinks++;
		}
	}

	// the slides: longest corners first, each joining two groups of walls
	// with the slide that carries the texture on; a corner within a group
	// already joined either agrees (aligned anyway) or keeps its seam
	qsort(taLinks, taNumLinks, sizeof(taLink_t), TA_CompareLinks);
	for (i = 0; i < taNumLinks; i++) {
		const taLink_t *lk = &taLinks[i];
		float           oa, ob;
		int             ra = TA_Find(lk->a, &oa), rb = TA_Find(lk->b, &ob);

		if (ra != rb) {
			// b's root moves so that b - a = delta
			taWalls[rb].parent = ra;
			taWalls[rb].offset = TA_Wrap(oa + lk->delta - ob);
			aligned++;
		} else if (fabs(TA_Wrap(ob - oa - lk->delta)) < 0.01f) {
			aligned++;
		} else {
			seams++;
		}
	}

	for (i = 0; i < taNumFaces; i++) {
		if (taFaces[i].silEdges) {
			TA_SilhouetteWrite(&taFaces[i]);
			silFaces++;
		}
	}

	// move each wall's faces
	for (i = 0; i < taNumFaces; i++) {
		taFace_t *f = &taFaces[i];
		float     offset;
		int       c = taWalls[f->wall].comp;

		if (c < 0) {
			continue;
		}
		TA_Find(f->wall, &offset);
		offset = TA_Wrap(offset);
		if (fabs(offset) < 0.0005f) {
			continue;
		}
		for (j = 0; j < f->cv->numVerts; j++) {
			f->cv->verts[j].st[c] += offset;
		}
		moved++;
	}

	ri.Hunk_FreeTempMemory(taLinks);
	ri.Hunk_FreeTempMemory(taSides);
	ri.Hunk_FreeTempMemory(taWalls);
	ri.Hunk_FreeTempMemory(taFaces);

	if (doSilhouette) {
		ri.Printf(PRINT_ALL, "texalign: %d outside corners of parallax textures cut their relief away (%d faces)\n", silCorners, silFaces);
	}
	if (!doAlign) {
		return;
	}
	ri.Printf(PRINT_ALL, "texalign: %d walls of the same texture meet at %d vertical corners: %d carry the texture on (%d faces slid), "
		"%d keep a seam (round a loop), %d could not, in %d ms\n",
		numWalls, stat_pairs, aligned, moved, seams, stat_unfold, ri.Milliseconds() - startTime);
	ri.Printf(PRINT_ALL, "texalign: %d of the corners mirror the texture (they meet at the edge); could not: %d texture turned on the wall, "
		"%d texture not upright, %d scale across the corner, %d scale up the corner, %d courses at different heights\n", ta_mirrored, ta_turned,
		ta_flat, ta_unfold, ta_scaleUp, ta_courses);
}

void R_TexAlignRegisterCvars(void)
{
	r_texAlign = ri.Cvar_Get("r_texAlign", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_texAlign, "Slides textures along walls as maps load so that, where two walls of the same texture meet at a "
		"vertical corner, the texture (and its normal and height map) carries on round it instead of jumping");
	r_parallaxSilhouette = ri.Cvar_Get("r_parallaxSilhouette", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_parallaxSilhouette, "With r_parallaxMapping: at outside vertical corners of the same parallax texture, the relief "
		"is cut away where the view ray leaves the wall past the corner, so mortar shows as notches in the outline");
}
