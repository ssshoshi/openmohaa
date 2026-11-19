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
// Outside edges also give the parallax its silhouette (r_parallaxSilhouette):
// where a parallax face meets another drawn face at an outside edge
// (vertical corners, wall tops, ledges, sills: each face lies behind the
// other's plane), its relief is cut away where the ray into it goes out past
// the edge before it meets the relief, so the mortar runs out as notches in
// the outline instead of the wall ending in a straight line. Such a face
// gets two coordinates across it, along its horizontal axis in its vertices'
// normal[3] and along its other axis in lightdir[3] (both unused: always 0),
// -1 and 1 at the outside edges, which the shader carries along the ray
// (lightall_fp.glsl). The ray only goes out past an edge whose other face
// faces away, so only the outline is cut. It works with or without
// r_texAlign, which only lines the notches up on both sides of a corner.

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
	vec3_t           silAxis[2];   // in its plane: horizontal (or x on a floor), and the other
	float            silMin[2], silMax[2];
	float            silLen[2][2]; // how long its ends (low, high) along each axis are at an outside edge
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

// a side of a face's outline, for r_parallaxSilhouette
typedef struct {
	int    face;
	vec3_t dir;     // along it, one way for every side on the same line
	vec3_t line;    // the point of its line nearest the origin
	float  t0, t1;  // where it runs along dir
	vec3_t inward;  // into the face from the side, in its plane
	int    next;
} taEdge_t;

static unsigned TA_HashLine(const vec3_t line, const vec3_t dir)
{
	int i;
	unsigned h = 0;

	for (i = 0; i < 3; i++) {
		h = h * 2654435761u ^ (unsigned)(int)floor(line[i] + 0.5f) * 73856093u ^ (unsigned)(int)floor(dir[i] * 16.0f + 0.5f) * 19349663u;
	}
	return h & (TA_HASH - 1);
}

static void TA_SilhouetteAxes(taFace_t *f)
{
	static const vec3_t up = { 0, 0, 1 }, east = { 1, 0, 0 };
	int                 c, i;

	CrossProduct(up, f->n, f->silAxis[0]);
	if (VectorNormalize(f->silAxis[0]) < 0.1f) {
		// a floor or a ceiling
		VectorMA(east, -DotProduct(east, f->n), f->n, f->silAxis[0]);
		VectorNormalize(f->silAxis[0]);
	}
	CrossProduct(f->n, f->silAxis[0], f->silAxis[1]);
	VectorNormalize(f->silAxis[1]);
	for (c = 0; c < 2; c++) {
		f->silMin[c] = f->silMax[c] = DotProduct(f->silAxis[c], f->cv->verts[0].xyz);
		for (i = 1; i < f->cv->numVerts; i++) {
			float x = DotProduct(f->silAxis[c], f->cv->verts[i].xyz);
			f->silMin[c] = Q_min(f->silMin[c], x);
			f->silMax[c] = Q_max(f->silMax[c], x);
		}
	}
}

/*
=================
TA_SilhouetteEdge

Counts how much of an end of the face is at an outside edge (open: the
length of it no face of its plane carries on past). An edge not square to
one of its axes, or not at an end, gives nothing.
=================
*/
static void TA_SilhouetteEdge(taFace_t *f, const taEdge_t *e, float t0, float t1, float open)
{
	vec3_t perp, mid;
	int    c;

	CrossProduct(f->n, e->dir, perp);
	VectorMA(e->line, 0.5f * (t0 + t1), e->dir, mid);
	for (c = 0; c < 2; c++) {
		float x;

		if (fabs(DotProduct(perp, f->silAxis[c])) < 0.995f || f->silMax[c] - f->silMin[c] < 1.0f) {
			continue;
		}
		x = DotProduct(f->silAxis[c], mid);
		if (fabs(x - f->silMin[c]) < 0.5f) {
			f->silLen[c][0] += open;
		} else if (fabs(x - f->silMax[c]) < 0.5f) {
			f->silLen[c][1] += open;
		}
	}
}

/*
=================
TA_Open

How much of where an edge's pair meet (t0 to t1) no face of the same plane
carries the face on past the edge: touching detail brushes keep the faces
between them, which make an outside corner with each brush's front face
although the wall goes on, and cutting there would open a slit in it. Only
an opaque face carries a wall on.
=================
*/
static float TA_Open(const taEdge_t *edges, const int *hash, const taEdge_t *e, float t0, float t1)
{
	const taFace_t *f = &taFaces[e->face];
	float           cover[32][2], open = 0, at = t0;
	int             j, k, n = 0;

	for (j = hash[TA_HashLine(e->line, e->dir)]; j != -1 && n < 32; j = edges[j].next) {
		const taEdge_t *ec = &edges[j];
		const taFace_t *fc = &taFaces[ec->face];
		vec3_t          diff;

		if (ec->face == e->face || DotProduct(ec->dir, e->dir) < 0.9999f || DotProduct(fc->n, f->n) < 0.999f || fabs(fc->d - f->d) > 0.5f
			|| DotProduct(ec->inward, e->inward) > -0.9f || fc->surf->shader->sort != SS_OPAQUE
			|| (fc->surf->shader->stages[0]->stateBits & GLS_ATEST_BITS)) {
			continue;
		}
		VectorSubtract(ec->line, e->line, diff);
		if (VectorLength(diff) > 0.1f || Q_min(ec->t1, t1) <= Q_max(ec->t0, t0)) {
			continue;
		}
		cover[n][0] = Q_max(ec->t0, t0);
		cover[n][1] = Q_min(ec->t1, t1);
		n++;
	}
	// the stretches left uncovered, in order along the edge
	for (;;) {
		float next = t1, nextEnd = t1;

		for (k = 0; k < n; k++) {
			if (cover[k][1] > at && cover[k][0] < next) {
				next = Q_max(cover[k][0], at);
				nextEnd = cover[k][1];
			}
		}
		open += next - at;
		if (next >= t1) {
			break;
		}
		// past every stretch that starts by where this one ends
		at = nextEnd;
		for (k = 0; k < n; k++) {
			if (cover[k][0] <= at && cover[k][1] > at) {
				at = cover[k][1];
				k = -1;
			}
		}
		if (at >= t1) {
			break;
		}
	}
	return open;
}

/*
=================
TA_SilhouetteWrite

The coordinates across a face beside outside edges, -1 and 1 at its ends
there. An end that is not (or not nearly all of it: a stretch of it that
carries on into the next face would be cut through) is kept just inside,
at 0, so that the ray going out past it cuts nothing away; the gradient
stays that of the whole width. Returns whether it has any.
=================
*/
static qboolean TA_SilhouetteWrite(taFace_t *f)
{
	float    lo[2], hi[2];
	qboolean any = qfalse;
	int      c, i;

	for (c = 0; c < 2; c++) {
		// how long its ends are: its extent along the other axis
		float    endLen = f->silMax[!c] - f->silMin[!c];
		qboolean atLo = f->silLen[c][0] >= 0.9f * endLen, atHi = f->silLen[c][1] >= 0.9f * endLen;

		lo[c] = atLo ? -1.0f : 0.0f;
		hi[c] = atHi ? 1.0f : 0.0f;
		if (atLo && !atHi) {
			hi[c] = 0.0f;
		} else if (atHi && !atLo) {
			lo[c] = 0.0f;
		}
		any |= atLo || atHi;
		if ((atLo || atHi) && r_parallaxSilhouette->integer == 2) {
			ri.Printf(PRINT_DEVELOPER, "silface %.0f %.0f %.0f n %.2f %.2f %.2f axis%d %.2f %.2f %.2f [%.0f %.0f] ends %.0f %.0f of %.0f %s\n",
				f->centroid[0], f->centroid[1], f->centroid[2], f->n[0], f->n[1], f->n[2], c, f->silAxis[c][0], f->silAxis[c][1], f->silAxis[c][2],
				f->silMin[c], f->silMax[c], f->silLen[c][0], f->silLen[c][1], endLen, f->surf->shader->name);
		}
	}
	if (!any) {
		return qfalse;
	}
	for (i = 0; i < f->cv->numVerts; i++) {
		srfVert_t *v = &f->cv->verts[i];

		for (c = 0; c < 2; c++) {
			float    x = (DotProduct(f->silAxis[c], v->xyz) - f->silMin[c]) / Q_max(f->silMax[c] - f->silMin[c], 1.0f);
			float    w = lo[c] + (hi[c] - lo[c]) * Com_Clamp(0.0f, 1.0f, x);
			int16_t *out = c == 0 ? &v->normal[3] : &v->lightdir[3];

			*out = (int16_t)(w * 32767.0f + (w > 0.0f ? 0.5f : -0.5f));
		}
	}
	f->cv->parallaxSilhouette = qtrue;
	return qtrue;
}

/*
=================
TA_Silhouettes

Finds the outside edges of parallax faces: sides of two faces' outlines on
one line, each face behind the other's plane. Returns the faces given the
coordinates; counts the edges.
=================
*/
static int TA_Silhouettes(int *numEdges)
{
	taEdge_t *edges;
	int      *hash;
	int       i, j, k, n = 0, maxEdges = 0, faces = 0;

	*numEdges = 0;
	for (i = 0; i < taNumFaces; i++) {
		maxEdges += taFaces[i].cv->numIndexes;
	}
	edges = ri.Hunk_AllocateTempMemory(Q_max(maxEdges, 1) * sizeof(*edges) + TA_HASH * sizeof(int));
	hash = (int *)(edges + Q_max(maxEdges, 1));
	for (i = 0; i < TA_HASH; i++) {
		hash[i] = -1;
	}

	for (i = 0; i < taNumFaces; i++) {
		taFace_t        *f = &taFaces[i];
		srfBspSurface_t *cv = f->cv;

		TA_SilhouetteAxes(f);
		for (j = 0; j < cv->numIndexes; j += 3) {
			for (k = 0; k < 3; k++) {
				int          a = cv->indexes[j + k], b = cv->indexes[j + (k + 1) % 3], l, m, shared = 0;
				const float *pa = cv->verts[a].xyz, *pb = cv->verts[b].xyz;
				taEdge_t    *e;
				float        len, ta, tb;
				unsigned     h;

				// a side of the outline: no other triangle of the face has it
				for (l = 0; l < cv->numIndexes && !shared; l += 3) {
					for (m = 0; m < 3; m++) {
						if (cv->indexes[l + m] == b && cv->indexes[l + (m + 1) % 3] == a) {
							shared = 1;
						}
					}
				}
				if (shared || n >= maxEdges) {
					continue;
				}
				e = &edges[n];
				VectorSubtract(pb, pa, e->dir);
				len = VectorNormalize(e->dir);
				if (len < 1.0f) {
					continue;
				}
				// one way for every side on the line
				for (l = 0; l < 3 && fabs(e->dir[l]) < 1e-4f; l++) {
				}
				if (l < 3 && e->dir[l] < 0) {
					VectorNegate(e->dir, e->dir);
				}
				ta = DotProduct(pa, e->dir);
				tb = DotProduct(pb, e->dir);
				e->t0 = Q_min(ta, tb);
				e->t1 = Q_max(ta, tb);
				VectorMA(pa, -ta, e->dir, e->line);
				CrossProduct(f->n, e->dir, e->inward);
				VectorNormalize(e->inward);
				if (DotProduct(e->inward, f->centroid) - DotProduct(e->inward, pa) < 0) {
					VectorNegate(e->inward, e->inward);
				}
				e->face = i;
				h = TA_HashLine(e->line, e->dir);
				e->next = hash[h];
				hash[h] = n++;
			}
		}
	}

	for (i = 0; i < n; i++) {
		const taEdge_t *ea = &edges[i];
		taFace_t       *fa = &taFaces[ea->face];

		for (j = hash[TA_HashLine(ea->line, ea->dir)]; j != -1; j = edges[j].next) {
			const taEdge_t *eb = &edges[j];
			taFace_t       *fb = &taFaces[eb->face];
			vec3_t          diff;
			float           t0, t1, openA, openB;
			qboolean        pa, pb;

			if (j <= i || eb->face == ea->face || DotProduct(ea->dir, eb->dir) < 0.9999f) {
				continue;
			}
			VectorSubtract(ea->line, eb->line, diff);
			if (VectorLength(diff) > 0.1f) {
				continue;
			}
			t0 = Q_max(ea->t0, eb->t0);
			t1 = Q_min(ea->t1, eb->t1);
			if (t1 - t0 < 1.0f) {
				continue;
			}
			// outside: each face round it lies behind the other's plane
			if (DotProduct(eb->inward, fa->n) > -0.25f || DotProduct(ea->inward, fb->n) > -0.25f) {
				continue;
			}
			openA = TA_ParallaxShader(fa->surf->shader) ? TA_Open(edges, hash, ea, t0, t1) : 0;
			openB = TA_ParallaxShader(fb->surf->shader) ? TA_Open(edges, hash, eb, t0, t1) : 0;
			pa = openA >= 1.0f;
			pb = openB >= 1.0f;
			if (!pa && !pb) {
				continue;
			}
			if (r_parallaxSilhouette->integer == 2) {
				ri.Printf(PRINT_DEVELOPER, "siledge %.0f %.0f %.0f - %.0f %.0f %.0f, faces %.0f %.0f %.0f / %.0f %.0f %.0f\n",
					ea->line[0] + ea->dir[0] * t0, ea->line[1] + ea->dir[1] * t0, ea->line[2] + ea->dir[2] * t0,
					ea->line[0] + ea->dir[0] * t1, ea->line[1] + ea->dir[1] * t1, ea->line[2] + ea->dir[2] * t1,
					fa->centroid[0], fa->centroid[1], fa->centroid[2], fb->centroid[0], fb->centroid[1], fb->centroid[2]);
			}
			if (pa) {
				TA_SilhouetteEdge(fa, ea, t0, t1, openA);
			}
			if (pb) {
				TA_SilhouetteEdge(fb, eb, t0, t1, openB);
			}
			(*numEdges)++;
			if (t1 - t0 >= 96.0f) {
				vec3_t p;

				VectorMA(ea->line, 0.5f * (t0 + t1), ea->dir, p);
				ri.Printf(PRINT_DEVELOPER, "texalign: outside edge at %.0f %.0f %.0f, %.0f long, along %.2f %.2f %.2f, faces %.2f %.2f %.2f / %.2f %.2f %.2f, %s / %s\n",
					p[0], p[1], p[2], t1 - t0, ea->dir[0], ea->dir[1], ea->dir[2], fa->n[0], fa->n[1], fa->n[2], fb->n[0], fb->n[1], fb->n[2],
					fa->surf->shader->name, fb->surf->shader->name);
			}
		}
	}

	for (i = 0; i < taNumFaces; i++) {
		if (TA_SilhouetteWrite(&taFaces[i])) {
			faces++;
		}
	}
	ri.Hunk_FreeTempMemory(edges);
	return faces;
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

	// Added in OPM: the parallax's outside edges (r_parallaxSilhouette)
	if (doSilhouette) {
		silFaces = TA_Silhouettes(&silCorners);
		ri.Printf(PRINT_ALL, "texalign: %d outside edges of parallax textures cut their relief away (%d faces)\n", silCorners, silFaces);
	}
	if (!doAlign) {
		ri.Hunk_FreeTempMemory(taFaces);
		return;
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
	ri.Cvar_SetDescription(r_parallaxSilhouette, "With r_parallaxMapping: at the outside edges of parallax textures (corners, wall tops, "
		"ledges), the relief is cut away where the view ray leaves the face past the edge, so mortar shows as notches in the outline. "
		"2 shows each face's edge coordinates instead (red, green; blue: none)");
}
