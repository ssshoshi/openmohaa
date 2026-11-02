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
// tr_bevel.c -- Added in OPM: rounded edges on the world's brushwork (r_bevel)
//
// Brushwork meets at knife edges: one face lit, the next in shadow, and no
// highlight between. This rounds the convex edges of the world's planar
// faces as the map loads, after the RTCW Castle Operations remaster's
// world-bevel tool (which rewrites the BSP offline). Only what is drawn
// changes: collision, the BSP on disk and the server are untouched.
//
// It works on planes, not faces, because the map compiler cuts a plane's
// face into pieces (at portals, lightmap sizes and other faces) and the two
// sides of an edge are seldom cut at the same places:
//
// - An edge is where two planes of opaque, plane mapped world faces meet at
//   a convex angle (r_bevelMinAngle to 180 minus it), along the stretch
//   where both planes' faces line it with nothing missing on either side.
// - Each end of an edge must be one of: a corner of exactly three such
//   planes whose three edges are all rounded (filled with a piece of
//   sphere), or the edge running square into a wall (a third plane at right
//   angles to both, facing back along it: the rounding simply stops at the
//   wall). Anything else at an end leaves the edge sharp, so no crack opens.
// - Every rounding has the radius r_bevelRadius. Each face piece along an
//   edge is cut back to where the cylinder touches its plane, R tan(t/2) in
//   for an angle t between the planes' normals. The cylinder's axis ends at
//   the corners' sphere centres (R behind all three planes) or on the wall.
//   Strip and sphere vertexes take the normalised blend of the planes'
//   normals, so they meet exactly along their shared arcs.
// - The half of a strip on plane A's side is drawn as part of the A face
//   piece that lines that stretch (its shader, lightmap and plane mapped
//   coordinates carried on); a corner's triangles go to the plane they face
//   most, drawn with that plane's piece at the corner.
//
// Known limits of this version: where the two halves of a strip meet, the
// lightmap and the texture can step (each half carries its own face's); a
// strip's row of vertexes is shared by both halves, but a face piece's cut
// back side only has its own corners, which leaves T junctions along it.

#include "tr_local.h"

// tr_light.c
mnode_t *R_FindPoint( world_t *world, const vec3_t p );

cvar_t *r_bevel;
cvar_t *r_bevelRadius;
cvar_t *r_bevelSegments;
cvar_t *r_bevelMinAngle;

#define BEVEL_MAX_POLY  MAX_FACE_POINTS
#define BEVEL_EPSILON   0.05f
#define BEVEL_ATTRS     8 // st, lightmap, colour
#define BEVEL_HASH      65536
#define BEVEL_MAX_SPLITS 128

typedef struct {
	msurface_t      *surf;
	srfBspSurface_t *cv;
	int              plane;
	vec3_t           n;
	float            d;
	vec3_t           centroid;
	int              numPoly;
	vec3_t           poly[BEVEL_MAX_POLY];
	int              polyVert[BEVEL_MAX_POLY]; // the original vertex at each corner
	int              sideEdge[BEVEL_MAX_POLY]; // the segment side i (poly[i] -> poly[i + 1]) lies on, or -1
	vec3_t           origin, axisU, axisV;
	float            attr[BEVEL_ATTRS][3]; // a u + b v + c
	int              triSign;              // the winding of the face's triangles against n
	qboolean         ok;
	qboolean         touched;
	// once cut back
	int              numClipped;
	vec3_t           clipped[BEVEL_MAX_POLY * 2];
	int              clippedVert[BEVEL_MAX_POLY * 2];
} bevelFace_t;

typedef struct {
	vec3_t   n;
	float    d;
	qboolean eligible; // of faces that can be bevelled
	int      next;
} bevelPlane_t;

// a line faces' sides lie on, and the two planes that meet along it
typedef struct {
	vec3_t   dir;       // along it (a canonical sign)
	vec3_t   origin;    // its point nearest the origin
	int      plane[2];  // -1 until seen
	qboolean many;      // more than two planes have sides on it
	qboolean ok;        // two planes meeting convexly, at an angle in range
	vec3_t   axisOff;   // from the line to the rounding's axis
	float    setback;
	int      firstSide; // bevelSide_t list
	int      next;
} bevelLine_t;

// a face's side along a line, as an interval of the line
typedef struct {
	int   face, side, which; // which: 0 for line->plane[0]'s side, 1 for plane[1]'s
	float t0, t1;
	int   next;
} bevelSide_t;

// a stretch of a line that is rounded
typedef struct {
	int      line;
	float    t0, t1;
	int      end[2]; // corners at t0 and at t1
	qboolean ok;
} bevelSeg_t;

#define CORNER_SPHERE 1
#define CORNER_WALL   2

typedef struct {
	vec3_t   p;
	int      type;
	int      plane[3];
	int      face[3];  // the piece of each plane at it (CORNER_SPHERE)
	int      seg[3];   // CORNER_SPHERE: one for each pair; CORNER_WALL: seg[0]
	vec3_t   center;
	qboolean ok;
	int      next;
} bevelCorner_t;

// a vertex of a world surface; plane -1 for one not of a face it can take
typedef struct {
	vec3_t p;
	int    face;
	int    plane;
	int    next;
} bevelPoint_t;

static bevelFace_t   *faces;
static int            numFaces;
static bevelPlane_t  *planes;
static int            numPlanes, maxPlanes;
static int           *planeHash;
static bevelLine_t   *lines;
static int            numLines, maxLines;
static int           *lineHash;
static bevelSide_t   *sides;
static int            numSides, maxSides;
static bevelSeg_t    *segs;
static int            numSegs, maxSegs;
static bevelCorner_t *corners;
static int            numCorners, maxCorners;
static int           *cornerHash;
static bevelPoint_t  *points;
static int            numPoints, maxPoints;
static int           *pointHash;
static world_t       *bevelWorld;
static float          bevelRadius;

static int stat_cornerPlanes[8], stat_cornerForeign, stat_cornerOpen, stat_cornerWall, stat_cornerNoPiece;

static int R_BevelCell(float x)
{
	return (int)floor(x / 32.0f);
}

static unsigned R_BevelHashCell(int x, int y, int z)
{
	return ((unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^ (unsigned)z * 83492791u) & (BEVEL_HASH - 1);
}

static unsigned R_BevelHashPoint(const vec3_t p)
{
	return R_BevelHashCell(R_BevelCell(p[0]), R_BevelCell(p[1]), R_BevelCell(p[2]));
}

static qboolean R_BevelSame(const vec3_t a, const vec3_t b)
{
	return fabs(a[0] - b[0]) < BEVEL_EPSILON && fabs(a[1] - b[1]) < BEVEL_EPSILON && fabs(a[2] - b[2]) < BEVEL_EPSILON;
}

/*
=================
R_BevelPlane

The plane of n, d in the table (the same one for every face on it).
=================
*/
static int R_BevelPlane(const vec3_t n, float d, qboolean eligible)
{
	unsigned h;
	int      i;

	h = ((unsigned)(int)floor(n[0] * 64.0f + 0.5f) * 73856093u ^ (unsigned)(int)floor(n[1] * 64.0f + 0.5f) * 19349663u
		 ^ (unsigned)(int)floor(n[2] * 64.0f + 0.5f) * 83492791u ^ (unsigned)(int)floor(d * 0.5f + 0.5f) * 2654435761u)
		& (BEVEL_HASH - 1);
	for (i = planeHash[h]; i != -1; i = planes[i].next) {
		if (DotProduct(planes[i].n, n) > 0.9999f && fabs(planes[i].d - d) < 0.1f) {
			if (eligible) {
				planes[i].eligible = qtrue;
			}
			return i;
		}
	}
	if (numPlanes >= maxPlanes) {
		return -1;
	}
	VectorCopy(n, planes[numPlanes].n);
	planes[numPlanes].d = d;
	planes[numPlanes].eligible = eligible;
	planes[numPlanes].next = planeHash[h];
	planeHash[h] = numPlanes;
	return numPlanes++;
}

static void R_BevelAddPoint(const vec3_t p, int face, int plane)
{
	unsigned h;

	if (numPoints >= maxPoints) {
		return;
	}
	VectorCopy(p, points[numPoints].p);
	points[numPoints].face = face;
	points[numPoints].plane = plane;
	h = R_BevelHashPoint(p);
	points[numPoints].next = pointHash[h];
	pointHash[h] = numPoints;
	numPoints++;
}

/*
=================
R_BevelShaderOk
=================
*/
static qboolean R_BevelShaderOk(const shader_t *sh)
{
	if (!sh || sh->isSky || sh->isPortal || sh->polygonOffset || sh->numDeforms) {
		return qfalse;
	}
	if (sh->sort != SS_OPAQUE || !sh->stages[0]) {
		return qfalse;
	}
	if (sh->stages[0]->stateBits & GLS_ATEST_BITS) {
		return qfalse;
	}
	if (sh->surfaceFlags & (SURF_NODRAW | SURF_SKY)) {
		return qfalse;
	}
	return qtrue;
}
/*
=================
R_BevelSetupFace

The face's polygon (its triangles' outline, in order, straight runs merged),
its plane and its plane mapping. Returns qfalse for a face that cannot be
bevelled.
=================
*/
static qboolean R_BevelSetupFace(bevelFace_t *f)
{
	srfBspSurface_t *cv = f->cv;
	int             *next, *count;
	int              i, j, start, cur, loopLen, numBoundary, best[3];
	vec3_t           loop[BEVEL_MAX_POLY * 2];
	int              loopVert[BEVEL_MAX_POLY * 2];
	float            bestArea, m[3][3], inv[3][3], det;
	vec3_t           e1, e2, cr;

	if (cv->numVerts < 3 || cv->numVerts > BEVEL_MAX_POLY || cv->numIndexes < 3) {
		return qfalse;
	}

	VectorCopy(cv->cullPlane.normal, f->n);
	f->d = cv->cullPlane.dist;

	// every vertex on the plane and shaded flat
	for (i = 0; i < cv->numVerts; i++) {
		vec3_t vn;

		if (fabs(DotProduct(cv->verts[i].xyz, f->n) - f->d) > 0.1f) {
			return qfalse;
		}
		R_VaoUnpackNormal(vn, cv->verts[i].normal);
		if (DotProduct(vn, f->n) < 0.999f) {
			return qfalse;
		}
	}

	// the outline: directed triangle edges whose reverse is not in the face
	next  = ri.Hunk_AllocateTempMemory(cv->numVerts * sizeof(int) * 2);
	count = next + cv->numVerts;
	for (i = 0; i < cv->numVerts; i++) {
		next[i] = -1;
		count[i] = 0;
	}
	numBoundary = 0;
	for (i = 0; i < cv->numIndexes; i += 3) {
		for (j = 0; j < 3; j++) {
			int a = cv->indexes[i + j], b = cv->indexes[i + (j + 1) % 3], k, shared = 0;

			for (k = 0; k < cv->numIndexes; k += 3) {
				int l;
				for (l = 0; l < 3; l++) {
					if (cv->indexes[k + l] == b && cv->indexes[k + (l + 1) % 3] == a) {
						shared = 1;
					}
				}
			}
			if (!shared) {
				if (next[a] != -1) {
					ri.Hunk_FreeTempMemory(next);
					return qfalse; // not a simple outline
				}
				next[a] = b;
				numBoundary++;
			}
		}
	}

	start = cv->indexes[0];
	while (next[start] == -1) {
		start++;
		if (start >= cv->numVerts) {
			ri.Hunk_FreeTempMemory(next);
			return qfalse;
		}
	}
	loopLen = 0;
	cur = start;
	do {
		if (loopLen >= BEVEL_MAX_POLY * 2 || next[cur] == -1) {
			ri.Hunk_FreeTempMemory(next);
			return qfalse;
		}
		VectorCopy(cv->verts[cur].xyz, loop[loopLen]);
		loopVert[loopLen] = cur;
		loopLen++;
		cur = next[cur];
	} while (cur != start);
	ri.Hunk_FreeTempMemory(next);

	if (loopLen != numBoundary || loopLen < 3) {
		return qfalse; // holes, or more than one piece
	}

	// merge straight runs into one side; every turn the same way (convex)
	f->numPoly = 0;
	for (i = 0; i < loopLen; i++) {
		const float *prev = loop[(i + loopLen - 1) % loopLen], *p = loop[i], *nx = loop[(i + 1) % loopLen];
		float        turn;

		VectorSubtract(p, prev, e1);
		VectorSubtract(nx, p, e2);
		CrossProduct(e1, e2, cr);
		turn = DotProduct(cr, f->n);
		if (fabs(turn) < 0.01f * VectorLength(e1) * VectorLength(e2)) {
			continue; // on a straight run
		}
		if (f->numPoly && ((turn > 0) != (f->triSign > 0))) {
			return qfalse; // concave
		}
		f->triSign = turn > 0 ? 1 : -1;
		if (f->numPoly >= BEVEL_MAX_POLY) {
			return qfalse;
		}
		VectorCopy(p, f->poly[f->numPoly]);
		f->polyVert[f->numPoly] = loopVert[i];
		f->sideEdge[f->numPoly] = -1;
		f->numPoly++;
	}
	if (f->numPoly < 3) {
		return qfalse;
	}

	VectorClear(f->centroid);
	for (i = 0; i < f->numPoly; i++) {
		VectorAdd(f->centroid, f->poly[i], f->centroid);
	}
	VectorScale(f->centroid, 1.0f / f->numPoly, f->centroid);

	// the winding the face's triangles use, against its normal
	{
		const float *a = cv->verts[cv->indexes[0]].xyz, *b = cv->verts[cv->indexes[1]].xyz, *c = cv->verts[cv->indexes[2]].xyz;

		VectorSubtract(b, a, e1);
		VectorSubtract(c, a, e2);
		CrossProduct(e1, e2, cr);
		f->triSign = DotProduct(cr, f->n) > 0 ? 1 : -1;
	}

	// plane coordinates, and the plane mapping fitted to the widest triangle
	PerpendicularVector(f->axisU, f->n);
	CrossProduct(f->n, f->axisU, f->axisV);
	VectorCopy(cv->verts[0].xyz, f->origin);

	bestArea = 0;
	best[0] = best[1] = best[2] = 0;
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

	for (i = 0; i < 3; i++) {
		vec3_t rel;

		VectorSubtract(cv->verts[best[i]].xyz, f->origin, rel);
		m[i][0] = DotProduct(rel, f->axisU);
		m[i][1] = DotProduct(rel, f->axisV);
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

	for (j = 0; j < BEVEL_ATTRS; j++) {
		float y[3];

		for (i = 0; i < 3; i++) {
			const srfVert_t *v = &cv->verts[best[i]];

			if (j < 2) {
				y[i] = v->st[j];
			} else if (j < 4) {
				y[i] = v->lightmap[j - 2];
			} else {
				y[i] = v->color[j - 4] / 65535.0f;
			}
		}
		for (i = 0; i < 3; i++) {
			f->attr[j][i] = inv[i][0] * y[0] + inv[i][1] * y[1] + inv[i][2] * y[2];
		}
	}

	// the mapping must hold at every vertex (texture and lightmap)
	for (i = 0; i < cv->numVerts; i++) {
		vec3_t rel;
		float  u, v;

		VectorSubtract(cv->verts[i].xyz, f->origin, rel);
		u = DotProduct(rel, f->axisU);
		v = DotProduct(rel, f->axisV);
		for (j = 0; j < 4; j++) {
			float want = j < 2 ? cv->verts[i].st[j] : cv->verts[i].lightmap[j - 2];
			float got = f->attr[j][0] * u + f->attr[j][1] * v + f->attr[j][2];

			if (fabs(want - got) > (j < 2 ? 0.01f : 0.0005f)) {
				return qfalse;
			}
		}
	}

	return qtrue;
}

/*
=================
R_BevelVert

A vertex of face f at p, facing normal, with f's plane mapping carried to
where p falls on f's plane.
=================
*/
static void R_BevelVert(const bevelFace_t *f, const vec3_t p, const vec3_t normal, srfVert_t *out)
{
	vec3_t rel;
	vec4_t color;
	float  u, v, val[BEVEL_ATTRS];
	int    j;

	Com_Memset(out, 0, sizeof(*out));
	VectorCopy(p, out->xyz);

	VectorSubtract(p, f->origin, rel);
	u = DotProduct(rel, f->axisU);
	v = DotProduct(rel, f->axisV);
	for (j = 0; j < BEVEL_ATTRS; j++) {
		val[j] = f->attr[j][0] * u + f->attr[j][1] * v + f->attr[j][2];
	}
	out->st[0] = val[0];
	out->st[1] = val[1];
	out->lightmap[0] = val[2];
	out->lightmap[1] = val[3];
	color[0] = Com_Clamp(0, 1, val[4]);
	color[1] = Com_Clamp(0, 1, val[5]);
	color[2] = Com_Clamp(0, 1, val[6]);
	color[3] = Com_Clamp(0, 1, val[7]);
	R_VaoPackColor(out->color, color);
	R_VaoPackNormal(out->normal, (float *)normal);
}

/*
=================
R_BevelLineFor

The line a side from a to b lies on, in the table.
=================
*/
static int R_BevelLineFor(const vec3_t a, const vec3_t b)
{
	vec3_t   dir, origin;
	unsigned h;
	int      i, big = 0;

	VectorSubtract(b, a, dir);
	if (VectorNormalize(dir) < 0.1f) {
		return -1;
	}
	// a canonical sign: the largest component positive
	if (fabs(dir[1]) > fabs(dir[big])) {
		big = 1;
	}
	if (fabs(dir[2]) > fabs(dir[big])) {
		big = 2;
	}
	if (dir[big] < 0) {
		VectorNegate(dir, dir);
	}
	VectorMA(a, -DotProduct(a, dir), dir, origin);

	h = ((unsigned)(int)floor(dir[0] * 256.0f + 0.5f) * 73856093u ^ (unsigned)(int)floor(dir[1] * 256.0f + 0.5f) * 19349663u
		 ^ (unsigned)(int)floor(dir[2] * 256.0f + 0.5f) * 83492791u ^ (unsigned)(int)floor(origin[0] * 2.0f + 0.5f) * 2654435761u
		 ^ (unsigned)(int)floor(origin[1] * 2.0f + 0.5f) * 40503u ^ (unsigned)(int)floor(origin[2] * 2.0f + 0.5f) * 2246822519u)
		& (BEVEL_HASH - 1);
	for (i = lineHash[h]; i != -1; i = lines[i].next) {
		if (DotProduct(lines[i].dir, dir) > 0.99995f && Distance(lines[i].origin, origin) < 0.1f) {
			return i;
		}
	}
	if (numLines >= maxLines) {
		return -1;
	}
	Com_Memset(&lines[numLines], 0, sizeof(lines[numLines]));
	VectorCopy(dir, lines[numLines].dir);
	VectorCopy(origin, lines[numLines].origin);
	lines[numLines].plane[0] = lines[numLines].plane[1] = -1;
	lines[numLines].firstSide = -1;
	lines[numLines].next = lineHash[h];
	lineHash[h] = numLines;
	return numLines++;
}

static float R_BevelT(const bevelLine_t *l, const vec3_t p)
{
	vec3_t rel;

	VectorSubtract(p, l->origin, rel);
	return DotProduct(rel, l->dir);
}

// the rounding's axis at t
static void R_BevelAxis(const bevelLine_t *l, float t, vec3_t out)
{
	VectorMA(l->origin, t, l->dir, out);
	VectorAdd(out, l->axisOff, out);
}

/*
=================
R_BevelInward

The direction into face f from its side s, in its plane.
=================
*/
static void R_BevelInward(const bevelFace_t *f, int s, vec3_t inward)
{
	vec3_t dir;

	VectorSubtract(f->poly[(s + 1) % f->numPoly], f->poly[s], dir);
	VectorNormalize(dir);
	CrossProduct(f->n, dir, inward);
	if (DotProduct(inward, f->centroid) - DotProduct(inward, f->poly[s]) < 0) {
		VectorNegate(inward, inward);
	}
}

/*
=================
R_BevelClip

Face f's polygon cut back from each of its sides on a rounded segment, into
f->clipped. Returns the number of points (0 when nothing is left).
=================
*/
static int R_BevelClip(bevelFace_t *f)
{
	vec3_t a[BEVEL_MAX_POLY * 2], b[BEVEL_MAX_POLY * 2];
	int    av[BEVEL_MAX_POLY * 2], bv[BEVEL_MAX_POLY * 2];
	int    na, nb, i, s;

	na = f->numPoly;
	for (i = 0; i < na; i++) {
		VectorCopy(f->poly[i], a[i]);
		av[i] = f->polyVert[i];
	}

	for (s = 0; s < f->numPoly; s++) {
		const bevelSeg_t *seg;
		vec3_t            inward;
		float             base;

		if (f->sideEdge[s] < 0 || !segs[f->sideEdge[s]].ok) {
			continue;
		}
		seg = &segs[f->sideEdge[s]];
		R_BevelInward(f, s, inward);
		base = DotProduct(inward, f->poly[s]) + lines[seg->line].setback;

		nb = 0;
		for (i = 0; i < na; i++) {
			float *p0 = a[i], *p1 = a[(i + 1) % na];
			float  d0 = DotProduct(inward, p0) - base, d1 = DotProduct(inward, p1) - base;

			if (d0 >= -0.001f) {
				VectorCopy(p0, b[nb]);
				bv[nb++] = av[i];
			}
			if ((d0 >= -0.001f) != (d1 >= -0.001f)) {
				float t = d0 / (d0 - d1);

				VectorLerp(p0, p1, t, b[nb]);
				bv[nb++] = -1;
			}
			if (nb >= BEVEL_MAX_POLY * 2 - 2) {
				f->numClipped = 0;
				return 0;
			}
		}
		if (nb < 3) {
			f->numClipped = 0;
			return 0;
		}
		na = nb;
		for (i = 0; i < na; i++) {
			VectorCopy(b[i], a[i]);
			av[i] = bv[i];
		}
	}

	for (i = 0; i < na; i++) {
		VectorCopy(a[i], f->clipped[i]);
		f->clippedVert[i] = av[i];
	}
	f->numClipped = na;
	return na;
}

/*
=================
R_BevelCutRange

The stretch of f's cut back side s, as t along its line: where the strip
starts on f. Returns qfalse when it is gone.
=================
*/
static qboolean R_BevelCutRange(const bevelFace_t *f, int s, float *tmin, float *tmax)
{
	const bevelSeg_t  *seg = &segs[f->sideEdge[s]];
	const bevelLine_t *l = &lines[seg->line];
	vec3_t             inward;
	float              base;
	int                i, n = 0;

	R_BevelInward(f, s, inward);
	base = DotProduct(inward, f->poly[s]) + l->setback;
	*tmin = 1e30f;
	*tmax = -1e30f;
	for (i = 0; i < f->numClipped; i++) {
		if (fabs(DotProduct(inward, f->clipped[i]) - base) < 0.01f) {
			float t = R_BevelT(l, f->clipped[i]);

			*tmin = Q_min(*tmin, t);
			*tmax = Q_max(*tmax, t);
			n++;
		}
	}
	return n >= 2 && *tmax - *tmin > 0.1f;
}

/*
=================
R_BevelPlanesAt

The planes of the surfaces at p: those with a vertex there, and, from the
leaf just out from the edge, faces p lies on. Returns the count, or -1 when
a surface that is not a planar face is there.
=================
*/
static int R_BevelPlanesAt(const vec3_t p, const vec3_t out, int *found, qboolean *opaque)
{
	int      n = 0, h, i, j;
	mnode_t *leaf;
	vec3_t   probe;

	for (h = pointHash[R_BevelHashPoint(p)]; h != -1; h = points[h].next) {
		if (!R_BevelSame(points[h].p, p)) {
			continue;
		}
		if (points[h].plane < 0) {
			return -1;
		}
		for (j = 0; j < n; j++) {
			if (found[j] == points[h].plane) {
				break;
			}
		}
		if (j == n) {
			if (n == 8) {
				return -1;
			}
			opaque[n] = qtrue;
			found[n++] = points[h].plane;
		}
	}

	// a wall p meets without a vertex there
	VectorMA(p, 0.25f, out, probe);
	leaf = R_FindPoint(bevelWorld, probe);
	if (leaf && leaf->contents != -1) {
		for (i = 0; i < leaf->nummarksurfaces; i++) {
			msurface_t      *surf = bevelWorld->surfaces + bevelWorld->marksurfaces[leaf->firstmarksurface + i];
			srfBspSurface_t *cv;
			int              plane, k;
			qboolean         inside = qfalse;

			if (!surf->data || *surf->data != SF_FACE) {
				continue;
			}
			cv = (srfBspSurface_t *)surf->data;
			if (fabs(DotProduct(cv->cullPlane.normal, p) - cv->cullPlane.dist) > 0.1f) {
				continue;
			}
			// on one of its triangles
			for (k = 0; k + 2 < cv->numIndexes && !inside; k += 3) {
				const float *a = cv->verts[cv->indexes[k]].xyz, *b = cv->verts[cv->indexes[k + 1]].xyz,
							*c = cv->verts[cv->indexes[k + 2]].xyz;
				vec3_t       e0, e1, e2, c0, c1, c2;

				VectorSubtract(b, a, e0);
				VectorSubtract(c, b, e1);
				VectorSubtract(a, c, e2);
				{
					vec3_t pa, pb, pc;

					VectorSubtract(p, a, pa);
					VectorSubtract(p, b, pb);
					VectorSubtract(p, c, pc);
					CrossProduct(e0, pa, c0);
					CrossProduct(e1, pb, c1);
					CrossProduct(e2, pc, c2);
				}
				{
					float s0 = DotProduct(c0, cv->cullPlane.normal) / Q_max(VectorLength(e0), 0.01f);
					float s1 = DotProduct(c1, cv->cullPlane.normal) / Q_max(VectorLength(e1), 0.01f);
					float s2 = DotProduct(c2, cv->cullPlane.normal) / Q_max(VectorLength(e2), 0.01f);

					if ((s0 >= -0.1f && s1 >= -0.1f && s2 >= -0.1f) || (s0 <= 0.1f && s1 <= 0.1f && s2 <= 0.1f)) {
						inside = qtrue;
					}
				}
			}
			if (!inside) {
				continue;
			}
			plane = R_BevelPlane(cv->cullPlane.normal, cv->cullPlane.dist, qfalse);
			if (plane < 0) {
				return -1;
			}
			for (j = 0; j < n; j++) {
				if (found[j] == plane) {
					break;
				}
			}
			if (j == n) {
				if (n == 8) {
					return -1;
				}
				opaque[n] = surf->shader && surf->shader->sort == SS_OPAQUE && !surf->shader->isSky;
				found[n++] = plane;
			}
		}
	}

	return n;
}

static int R_BevelSegBetween(int pa, int pb, const vec3_t p)
{
	int i;

	for (i = 0; i < numSegs; i++) {
		const bevelLine_t *l = &lines[segs[i].line];
		vec3_t             e0, e1;

		if (!((l->plane[0] == pa && l->plane[1] == pb) || (l->plane[0] == pb && l->plane[1] == pa))) {
			continue;
		}
		VectorMA(l->origin, segs[i].t0, l->dir, e0);
		VectorMA(l->origin, segs[i].t1, l->dir, e1);
		if (R_BevelSame(e0, p) || R_BevelSame(e1, p)) {
			return i;
		}
	}
	return -1;
}

static int R_BevelPieceAt(int plane, const vec3_t p)
{
	int h;

	for (h = pointHash[R_BevelHashPoint(p)]; h != -1; h = points[h].next) {
		if (points[h].face >= 0 && points[h].plane == plane && R_BevelSame(points[h].p, p)) {
			return points[h].face;
		}
	}
	return -1;
}

/*
=================
R_BevelCorner

What is at end e (0 at t0, 1 at t1) of segment s: a corner of three
rounded edges, or a wall. -1 for anything else.
=================
*/
static int R_BevelCorner(int s, int e)
{
	bevelSeg_t        *seg = &segs[s];
	const bevelLine_t *l = &lines[seg->line];
	const float       *nA = planes[l->plane[0]].n, *nB = planes[l->plane[1]].n;
	vec3_t             p, out, along;
	int                found[8], n, h, k, other = -1;
	qboolean           opaque[8];
	bevelCorner_t     *c;

	VectorMA(l->origin, e ? seg->t1 : seg->t0, l->dir, p);

	// a corner already made at p that has this segment
	for (h = cornerHash[R_BevelHashPoint(p)]; h != -1; h = corners[h].next) {
		if (!R_BevelSame(corners[h].p, p)) {
			continue;
		}
		for (k = 0; k < 3; k++) {
			if (corners[h].seg[k] == s) {
				return h;
			}
		}
	}

	VectorAdd(nA, nB, out);
	VectorNormalize(out);
	n = R_BevelPlanesAt(p, out, found, opaque);
	if (n < 0) {
		stat_cornerForeign++;
		return -1;
	}
	if (n != 3) {
		stat_cornerPlanes[Q_min(n, 7)]++;
		return -1;
	}
	for (k = 0; k < 3; k++) {
		if (found[k] != l->plane[0] && found[k] != l->plane[1]) {
			other = k;
		}
	}
	if (other < 0 || numCorners >= maxCorners) {
		stat_cornerOpen++;
		return -1;
	}

	c = &corners[numCorners];
	Com_Memset(c, 0, sizeof(*c));
	VectorCopy(p, c->p);
	c->seg[0] = c->seg[1] = c->seg[2] = -1;

	// out along the edge, past this end
	VectorScale(l->dir, e ? 1.0f : -1.0f, along);

	{
		const float *nC = planes[found[other]].n;
		int          sAC = R_BevelSegBetween(l->plane[0], found[other], p);
		int          sBC = R_BevelSegBetween(l->plane[1], found[other], p);

		if (sAC >= 0 && sBC >= 0 && planes[found[other]].eligible) {
			// three rounded edges: a piece of sphere
			float m[3][3], rhs[3], det;

			c->type = CORNER_SPHERE;
			c->plane[0] = l->plane[0];
			c->plane[1] = l->plane[1];
			c->plane[2] = found[other];
			c->seg[0] = s;
			c->seg[1] = sBC;
			c->seg[2] = sAC;
			for (k = 0; k < 3; k++) {
				c->face[k] = R_BevelPieceAt(c->plane[k], p);
				if (c->face[k] < 0) {
					stat_cornerNoPiece++;
					return -1;
				}
				VectorCopy(planes[c->plane[k]].n, m[k]);
				rhs[k] = planes[c->plane[k]].d - bevelRadius;
			}
			det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
				+ m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
			if (fabs(det) < 0.05f) {
				stat_cornerOpen++;
				return -1;
			}
			c->center[0] = (rhs[0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (rhs[1] * m[2][2] - m[1][2] * rhs[2])
							+ m[0][2] * (rhs[1] * m[2][1] - m[1][1] * rhs[2])) / det;
			c->center[1] = (m[0][0] * (rhs[1] * m[2][2] - m[1][2] * rhs[2]) - rhs[0] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
							+ m[0][2] * (m[1][0] * rhs[2] - rhs[1] * m[2][0])) / det;
			c->center[2] = (m[0][0] * (m[1][1] * rhs[2] - rhs[1] * m[2][1]) - m[0][1] * (m[1][0] * rhs[2] - rhs[1] * m[2][0])
							+ rhs[0] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
		} else if (opaque[other] && fabs(DotProduct(nA, nC)) < 0.05f && fabs(DotProduct(nB, nC)) < 0.05f
				   && DotProduct(nC, along) < -0.7f && sAC < 0 && sBC < 0) {
			// square into a wall: the rounding stops on it
			float denom = DotProduct(nC, l->dir), t;

			c->type = CORNER_WALL;
			c->plane[0] = l->plane[0];
			c->plane[1] = l->plane[1];
			c->plane[2] = found[other];
			c->seg[0] = s;
			t = (planes[found[other]].d - DotProduct(nC, l->origin) - DotProduct(nC, l->axisOff)) / denom;
			R_BevelAxis(l, t, c->center);
			stat_cornerWall++;
		} else {
			stat_cornerOpen++;
			return -1;
		}
	}

	c->ok = qtrue;
	h = R_BevelHashPoint(p);
	c->next = cornerHash[h];
	cornerHash[h] = numCorners;
	return numCorners++;
}

/*
=================
R_BevelSplits

Where a segment's strip is cut across: its two ends (where the axis meets
the corners) and every corner of a piece's cut back side along it, of
either plane. Returns the count, sorted.
=================
*/
static int R_BevelSplits(int s, float *out)
{
	const bevelSeg_t  *seg = &segs[s];
	const bevelLine_t *l = &lines[seg->line];
	int                n = 0, i, j, k;
	float              lo = R_BevelT(l, corners[seg->end[0]].center), hi = R_BevelT(l, corners[seg->end[1]].center);

	if (lo > hi) {
		float t = lo;
		lo = hi;
		hi = t;
	}
	out[n++] = lo;
	out[n++] = hi;

	for (i = l->firstSide; i != -1; i = sides[i].next) {
		bevelFace_t *f = &faces[sides[i].face];
		float        tmin, tmax;

		if (f->sideEdge[sides[i].side] != s || !R_BevelCutRange(f, sides[i].side, &tmin, &tmax)) {
			continue;
		}
		for (k = 0; k < 2 && n < BEVEL_MAX_SPLITS; k++) {
			float t = k ? tmax : tmin;

			if (t > lo + 0.05f && t < hi - 0.05f) {
				out[n++] = t;
			}
		}
	}

	// sort, and drop what is the same
	for (i = 1; i < n; i++) {
		float t = out[i];

		for (j = i - 1; j >= 0 && out[j] > t; j--) {
			out[j + 1] = out[j];
		}
		out[j + 1] = t;
	}
	for (i = 1, j = 1; i < n; i++) {
		if (out[i] - out[j - 1] > 0.05f) {
			out[j++] = out[i];
		}
	}
	return j;
}

// the new geometry of one face, built up before it replaces the old
typedef struct {
	srfVert_t *verts;
	glIndex_t *indexes;
	int        numVerts, numIndexes, maxVerts, maxIndexes;
} bevelBuild_t;

static int R_BevelPush(bevelBuild_t *b, const srfVert_t *v)
{
	if (b->numVerts >= b->maxVerts) {
		return -1;
	}
	b->verts[b->numVerts] = *v;
	return b->numVerts++;
}

static void R_BevelTri(bevelBuild_t *b, const bevelFace_t *f, int i0, int i1, int i2, const vec3_t facing)
{
	vec3_t e1, e2, cr;

	if (i0 < 0 || i1 < 0 || i2 < 0 || b->numIndexes + 3 > b->maxIndexes) {
		return;
	}
	VectorSubtract(b->verts[i1].xyz, b->verts[i0].xyz, e1);
	VectorSubtract(b->verts[i2].xyz, b->verts[i0].xyz, e2);
	CrossProduct(e1, e2, cr);
	if (VectorLengthSquared(cr) < 1e-8f) {
		return;
	}
	// the face's own winding, whichever way the points came
	if ((DotProduct(cr, facing) > 0 ? 1 : -1) != f->triSign) {
		int t = i1;
		i1 = i2;
		i2 = t;
	}
	b->indexes[b->numIndexes++] = i0;
	b->indexes[b->numIndexes++] = i1;
	b->indexes[b->numIndexes++] = i2;
}

static void R_BevelBlend(const vec3_t a, float wa, const vec3_t b, float wb, const vec3_t c, float wc, vec3_t out)
{
	VectorScale(a, wa, out);
	VectorMA(out, wb, b, out);
	if (c) {
		VectorMA(out, wc, c, out);
	}
	VectorNormalize(out);
}

/*
=================
R_BevelWorld

Called once the world's surfaces, leaves and submodels are loaded, before
their vertexes' light directions are worked out.
=================
*/
void R_BevelWorld(world_t *world)
{
	int           i, j, k, s, iter, firstSurface, numSurfaces, segments;
	int           startTime = ri.Milliseconds(), bevelled = 0, sphereCorners = 0, wallEnds = 0, facesChanged = 0;
	int           stat_lines = 0, stat_many = 0, stat_concave = 0, stat_angle = 0, stat_uncovered = 0, stat_touched = 0, stat_clip = 0;
	float         minAngle, *splitPool;
	int          *splitStart, *splitCount;
	bevelBuild_t  build;

	Com_Memset(stat_cornerPlanes, 0, sizeof(stat_cornerPlanes));
	stat_cornerForeign = stat_cornerOpen = stat_cornerWall = stat_cornerNoPiece = 0;

	if (!r_bevel->integer || !world || !world->bmodels || world->numBModels < 1 || !world->nodes) {
		return;
	}

	bevelWorld = world;
	bevelRadius = r_bevelRadius->value;
	segments = r_bevelSegments->integer;
	minAngle = r_bevelMinAngle->value;
	if (bevelRadius <= 0.1f) {
		return;
	}
	segments = Com_Clamp(2, 8, segments);
	segments &= ~1; // even, so each side takes half

	firstSurface = world->bmodels[0].firstSurface;
	numSurfaces = world->bmodels[0].numSurfaces;

	maxPlanes = numSurfaces + 64;
	planes = ri.Hunk_AllocateTempMemory(maxPlanes * sizeof(*planes) + BEVEL_HASH * sizeof(int) * 4);
	planeHash = (int *)(planes + maxPlanes);
	lineHash = planeHash + BEVEL_HASH;
	pointHash = lineHash + BEVEL_HASH;
	cornerHash = pointHash + BEVEL_HASH;
	for (i = 0; i < BEVEL_HASH * 4; i++) {
		planeHash[i] = -1;
	}
	numPlanes = 0;

	// the faces it can take
	faces = ri.Hunk_AllocateTempMemory(numSurfaces * sizeof(*faces));
	numFaces = 0;
	for (i = 0; i < numSurfaces; i++) {
		msurface_t  *surf = &world->surfaces[firstSurface + i];
		bevelFace_t *f = &faces[numFaces];

		if (!surf->data || *surf->data != SF_FACE || !R_BevelShaderOk(surf->shader)) {
			continue;
		}
		Com_Memset(f, 0, sizeof(*f));
		f->surf = surf;
		f->cv = (srfBspSurface_t *)surf->data;
		if (R_BevelSetupFace(f)) {
			f->plane = R_BevelPlane(f->n, f->d, qtrue);
			if (f->plane >= 0) {
				f->ok = qtrue;
				numFaces++;
			}
		}
	}

	// every vertex of the world, with its plane (-1: not a planar face)
	maxPoints = 0;
	for (i = 0; i < numSurfaces; i++) {
		msurface_t *surf = &world->surfaces[firstSurface + i];

		if (surf->data && (*surf->data == SF_FACE || *surf->data == SF_GRID || *surf->data == SF_TRIANGLES)) {
			maxPoints += ((srfBspSurface_t *)surf->data)->numVerts;
		}
	}
	points = ri.Hunk_AllocateTempMemory(Q_max(maxPoints, 1) * sizeof(*points));
	numPoints = 0;
	for (i = 0, k = 0; i < numSurfaces; i++) {
		msurface_t      *surf = &world->surfaces[firstSurface + i];
		srfBspSurface_t *cv;
		int              owner = -1, plane = -1;

		if (!surf->data || (*surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES)) {
			continue;
		}
		cv = (srfBspSurface_t *)surf->data;
		if (k < numFaces && faces[k].surf == surf) {
			owner = k;
			plane = faces[k].plane;
			k++;
		} else if (*surf->data == SF_FACE) {
			plane = R_BevelPlane(cv->cullPlane.normal, cv->cullPlane.dist, qfalse);
		}
		for (j = 0; j < cv->numVerts; j++) {
			R_BevelAddPoint(cv->verts[j].xyz, owner, plane);
		}
	}

	// every side onto its line
	maxLines = numFaces * 4 + 16;
	lines = ri.Hunk_AllocateTempMemory(maxLines * sizeof(*lines));
	numLines = 0;
	maxSides = 0;
	for (i = 0; i < numFaces; i++) {
		maxSides += faces[i].numPoly;
	}
	sides = ri.Hunk_AllocateTempMemory(Q_max(maxSides, 1) * sizeof(*sides));
	numSides = 0;
	for (i = 0; i < numFaces; i++) {
		bevelFace_t *f = &faces[i];

		for (s = 0; s < f->numPoly; s++) {
			int          li = R_BevelLineFor(f->poly[s], f->poly[(s + 1) % f->numPoly]);
			bevelLine_t *l;
			bevelSide_t *sd;
			float        ta, tb;

			f->sideEdge[s] = -1;
			if (li < 0 || numSides >= maxSides) {
				continue;
			}
			l = &lines[li];
			if (l->plane[0] < 0) {
				l->plane[0] = f->plane;
			} else if (l->plane[0] != f->plane && l->plane[1] < 0) {
				l->plane[1] = f->plane;
			} else if (l->plane[0] != f->plane && l->plane[1] != f->plane) {
				l->many = qtrue;
			}
			sd = &sides[numSides];
			sd->face = i;
			sd->side = s;
			sd->which = (f->plane == l->plane[0]) ? 0 : 1;
			ta = R_BevelT(l, f->poly[s]);
			tb = R_BevelT(l, f->poly[(s + 1) % f->numPoly]);
			sd->t0 = Q_min(ta, tb);
			sd->t1 = Q_max(ta, tb);
			sd->next = l->firstSide;
			l->firstSide = numSides++;
		}
	}

	// the lines that are edges, and their stretches lined on both sides
	maxSegs = numLines * 2 + 16;
	segs = ri.Hunk_AllocateTempMemory(maxSegs * sizeof(*segs));
	numSegs = 0;
	for (i = 0; i < numLines; i++) {
		bevelLine_t *l = &lines[i];
		const float *nA, *nB;
		float        cosn, angle;
		vec3_t       cA, cB;
		int          haveA = 0, haveB = 0;

		if (l->plane[1] < 0) {
			continue; // one plane only
		}
		stat_lines++;
		if (l->many) {
			stat_many++;
			continue;
		}
		nA = planes[l->plane[0]].n;
		nB = planes[l->plane[1]].n;
		for (j = l->firstSide; j != -1; j = sides[j].next) {
			if (sides[j].which == 0 && !haveA) {
				VectorCopy(faces[sides[j].face].centroid, cA);
				haveA = 1;
			}
			if (sides[j].which == 1 && !haveB) {
				VectorCopy(faces[sides[j].face].centroid, cB);
				haveB = 1;
			}
		}
		// convex: each plane's faces fall away behind the other's plane
		if (DotProduct(nA, cB) - planes[l->plane[0]].d > -0.5f || DotProduct(nB, cA) - planes[l->plane[1]].d > -0.5f) {
			stat_concave++;
			continue;
		}
		cosn = DotProduct(nA, nB);
		angle = RAD2DEG(acos(Com_Clamp(-1, 1, cosn)));
		if (angle < minAngle || angle > 180.0f - minAngle) {
			stat_angle++;
			continue;
		}
		l->ok = qtrue;
		l->setback = bevelRadius * tan(DEG2RAD(angle) * 0.5f);
		VectorAdd(nA, nB, l->axisOff);
		VectorScale(l->axisOff, -bevelRadius / (1.0f + cosn), l->axisOff);

		// each plane's sides merged into stretches; a segment where the two
		// planes' stretches are the same
		{
			float lo[2][64], hi[2][64];
			int   cnt[2] = { 0, 0 }, w, a, b;

			for (w = 0; w < 2; w++) {
				// repeatedly take the lowest side not yet in a stretch
				qboolean used[256];
				int      numW = 0, idx[256];

				for (j = l->firstSide; j != -1; j = sides[j].next) {
					if (sides[j].which == w && numW < 256) {
						idx[numW] = j;
						used[numW] = qfalse;
						numW++;
					}
				}
				for (;;) {
					int   best = -1;
					float t0, t1;

					for (a = 0; a < numW; a++) {
						if (!used[a] && (best < 0 || sides[idx[a]].t0 < sides[idx[best]].t0)) {
							best = a;
						}
					}
					if (best < 0) {
						break;
					}
					used[best] = qtrue;
					t0 = sides[idx[best]].t0;
					t1 = sides[idx[best]].t1;
					// grow it with every side that touches it
					for (;;) {
						qboolean grew = qfalse;

						for (a = 0; a < numW; a++) {
							if (!used[a] && sides[idx[a]].t0 <= t1 + 0.1f && sides[idx[a]].t1 >= t0 - 0.1f) {
								t0 = Q_min(t0, sides[idx[a]].t0);
								t1 = Q_max(t1, sides[idx[a]].t1);
								used[a] = qtrue;
								grew = qtrue;
							}
						}
						if (!grew) {
							break;
						}
					}
					if (cnt[w] < 64) {
						lo[w][cnt[w]] = t0;
						hi[w][cnt[w]] = t1;
						cnt[w]++;
					}
				}
			}
			for (a = 0; a < cnt[0]; a++) {
				for (b = 0; b < cnt[1]; b++) {
					if (fabs(lo[0][a] - lo[1][b]) < 0.1f && fabs(hi[0][a] - hi[1][b]) < 0.1f && numSegs < maxSegs) {
						bevelSeg_t *sg = &segs[numSegs];

						sg->line = i;
						sg->t0 = Q_max(lo[0][a], lo[1][b]);
						sg->t1 = Q_min(hi[0][a], hi[1][b]);
						sg->end[0] = sg->end[1] = -1;
						sg->ok = qtrue;
						// the faces' sides along it
						for (j = l->firstSide; j != -1; j = sides[j].next) {
							if (sides[j].t0 >= sg->t0 - 0.1f && sides[j].t1 <= sg->t1 + 0.1f) {
								faces[sides[j].face].sideEdge[sides[j].side] = numSegs;
							}
						}
						numSegs++;
						break;
					}
				}
				if (b == cnt[1]) {
					stat_uncovered++;
				}
			}
		}
	}

	// nothing of a third plane may touch a segment between its ends
	for (i = 0; i < numSegs; i++) {
		bevelSeg_t        *sg = &segs[i];
		const bevelLine_t *l = &lines[sg->line];
		vec3_t             p0, p1, mins, maxs;
		int                x, y, z;

		VectorMA(l->origin, sg->t0, l->dir, p0);
		VectorMA(l->origin, sg->t1, l->dir, p1);
		for (k = 0; k < 3; k++) {
			mins[k] = Q_min(p0[k], p1[k]) - 1;
			maxs[k] = Q_max(p0[k], p1[k]) + 1;
		}
		for (x = R_BevelCell(mins[0]); x <= R_BevelCell(maxs[0]) && sg->ok; x++) {
			for (y = R_BevelCell(mins[1]); y <= R_BevelCell(maxs[1]) && sg->ok; y++) {
				for (z = R_BevelCell(mins[2]); z <= R_BevelCell(maxs[2]) && sg->ok; z++) {
					int h;

					for (h = pointHash[R_BevelHashCell(x, y, z)]; h != -1; h = points[h].next) {
						vec3_t rel, off;
						float  t;

						if (points[h].plane >= 0 && (points[h].plane == l->plane[0] || points[h].plane == l->plane[1])) {
							continue;
						}
						VectorSubtract(points[h].p, l->origin, rel);
						t = DotProduct(rel, l->dir);
						if (t <= sg->t0 + BEVEL_EPSILON || t >= sg->t1 - BEVEL_EPSILON) {
							continue;
						}
						VectorMA(rel, -t, l->dir, off);
						if (VectorLength(off) < 0.25f) {
							sg->ok = qfalse;
							stat_touched++;
							break;
						}
					}
				}
			}
		}
	}

	// what is at each end
	maxCorners = numSegs * 2 + 16;
	corners = ri.Hunk_AllocateTempMemory(maxCorners * sizeof(*corners));
	numCorners = 0;
	for (i = 0; i < numSegs; i++) {
		if (!segs[i].ok) {
			continue;
		}
		segs[i].end[0] = R_BevelCorner(i, 0);
		segs[i].end[1] = R_BevelCorner(i, 1);
		if (segs[i].end[0] < 0 || segs[i].end[1] < 0) {
			segs[i].ok = qfalse;
		}
	}

	// what fails takes what depends on it with it, until it holds
	for (iter = 0; iter < 32; iter++) {
		qboolean changed = qfalse;

		for (i = 0; i < numCorners; i++) {
			if (!corners[i].ok) {
				continue;
			}
			for (k = 0; k < 3; k++) {
				if (corners[i].seg[k] >= 0 && !segs[corners[i].seg[k]].ok) {
					corners[i].ok = qfalse;
					changed = qtrue;
					break;
				}
			}
		}
		for (i = 0; i < numSegs; i++) {
			if (segs[i].ok && (segs[i].end[0] < 0 || segs[i].end[1] < 0 || !corners[segs[i].end[0]].ok || !corners[segs[i].end[1]].ok)) {
				segs[i].ok = qfalse;
				changed = qtrue;
			}
		}
		// every piece cut back, each of its rounded sides still there
		for (i = 0; i < numFaces; i++) {
			bevelFace_t *f = &faces[i];
			int          any = 0;

			for (s = 0; s < f->numPoly; s++) {
				if (f->sideEdge[s] >= 0 && segs[f->sideEdge[s]].ok) {
					any = 1;
				}
			}
			if (!any) {
				f->numClipped = 0;
				continue;
			}
			if (R_BevelClip(f) < 3) {
				for (s = 0; s < f->numPoly; s++) {
					if (f->sideEdge[s] >= 0 && segs[f->sideEdge[s]].ok) {
						segs[f->sideEdge[s]].ok = qfalse;
						changed = qtrue;
						stat_clip++;
					}
				}
				continue;
			}
			for (s = 0; s < f->numPoly; s++) {
				float tmin, tmax;

				if (f->sideEdge[s] >= 0 && segs[f->sideEdge[s]].ok && !R_BevelCutRange(f, s, &tmin, &tmax)) {
					segs[f->sideEdge[s]].ok = qfalse;
					changed = qtrue;
					stat_clip++;
				}
			}
		}
		if (!changed) {
			break;
		}
	}

	// where each segment's strip is cut across
	splitStart = ri.Hunk_AllocateTempMemory(Q_max(numSegs, 1) * sizeof(int) * 2);
	splitCount = splitStart + Q_max(numSegs, 1);
	splitPool = ri.Hunk_AllocateTempMemory(Q_max(numSegs, 1) * BEVEL_MAX_SPLITS * sizeof(float));
	for (i = 0; i < numSegs; i++) {
		splitStart[i] = i * BEVEL_MAX_SPLITS;
		splitCount[i] = segs[i].ok ? R_BevelSplits(i, splitPool + splitStart[i]) : 0;
		if (segs[i].ok) {
			bevelled++;
			for (j = lines[segs[i].line].firstSide; j != -1; j = sides[j].next) {
				if (faces[sides[j].face].sideEdge[sides[j].side] == i) {
					faces[sides[j].face].touched = qtrue;
				}
			}
		}
	}
	if (r_bevel->integer >= 3) {
		int shown = 0;

		for (i = 0; i < numSegs && shown < 40; i++) {
			const bevelLine_t *l = &lines[segs[i].line];
			vec3_t             p0, p1;

			if (!segs[i].ok) {
				continue;
			}
			VectorMA(l->origin, segs[i].t0, l->dir, p0);
			VectorMA(l->origin, segs[i].t1, l->dir, p1);
			ri.Printf(PRINT_ALL, "bevel: edge %.0f %.0f %.0f to %.0f %.0f %.0f, normals %.2f %.2f %.2f / %.2f %.2f %.2f, ends %s %s\n",
				p0[0], p0[1], p0[2], p1[0], p1[1], p1[2], planes[l->plane[0]].n[0], planes[l->plane[0]].n[1], planes[l->plane[0]].n[2],
				planes[l->plane[1]].n[0], planes[l->plane[1]].n[1], planes[l->plane[1]].n[2],
				corners[segs[i].end[0]].type == CORNER_SPHERE ? "corner" : "wall", corners[segs[i].end[1]].type == CORNER_SPHERE ? "corner" : "wall");
			shown++;
		}
	}
	for (i = 0; i < numCorners; i++) {
		if (corners[i].ok) {
			if (corners[i].type == CORNER_SPHERE) {
				sphereCorners++;
			} else {
				wallEnds++;
			}
		}
	}

	// build: each piece cut back, its stretches of strip, its corner pieces
	build.maxVerts = 8192;
	build.maxIndexes = 8192 * 3;
	build.verts = ri.Hunk_AllocateTempMemory(build.maxVerts * sizeof(srfVert_t));
	build.indexes = ri.Hunk_AllocateTempMemory(build.maxIndexes * sizeof(glIndex_t));

	for (i = 0; i < numFaces; i++) {
		bevelFace_t     *f = &faces[i];
		srfBspSurface_t *cv = f->cv;
		srfVert_t        v;
		int              first;

		if (!f->touched || f->numClipped < 3) {
			continue;
		}
		build.numVerts = build.numIndexes = 0;

		// the piece, cut back: a fan (it is convex); corners it keeps keep
		// their own vertex exactly
		first = build.numVerts;
		for (k = 0; k < f->numClipped; k++) {
			if (f->clippedVert[k] >= 0) {
				v = cv->verts[f->clippedVert[k]];
			} else {
				R_BevelVert(f, f->clipped[k], f->n, &v);
			}
			R_BevelPush(&build, &v);
		}
		for (k = 1; k + 1 < f->numClipped; k++) {
			R_BevelTri(&build, f, first, first + k, first + k + 1, f->n);
		}

		// its stretches of the strips
		for (s = 0; s < f->numPoly; s++) {
			const bevelSeg_t  *sg;
			const bevelLine_t *l;
			const float       *nSelf, *nOther, *splits;
			float              tmin, tmax;
			int                numSplits, x, row;

			if (f->sideEdge[s] < 0 || !segs[f->sideEdge[s]].ok || !R_BevelCutRange(f, s, &tmin, &tmax)) {
				continue;
			}
			sg = &segs[f->sideEdge[s]];
			l = &lines[sg->line];
			nSelf = f->n;
			nOther = planes[l->plane[0] == f->plane ? l->plane[1] : l->plane[0]].n;
			splits = splitPool + splitStart[f->sideEdge[s]];
			numSplits = splitCount[f->sideEdge[s]];

			for (x = 0; x + 1 < numSplits; x++) {
				float  mid = 0.5f * (splits[x] + splits[x + 1]);
				int    base;

				if (mid < tmin || mid > tmax) {
					continue; // another piece's stretch
				}
				base = build.numVerts;
				for (row = 0; row <= segments / 2; row++) {
					vec3_t nrm, a0, a1, p0, p1;
					float  u = (float)row / segments;

					R_BevelBlend(nSelf, 1.0f - u, nOther, u, NULL, 0, nrm);
					R_BevelAxis(l, splits[x], a0);
					R_BevelAxis(l, splits[x + 1], a1);
					VectorMA(a0, bevelRadius, nrm, p0);
					VectorMA(a1, bevelRadius, nrm, p1);
					R_BevelVert(f, p0, nrm, &v);
					R_BevelPush(&build, &v);
					R_BevelVert(f, p1, nrm, &v);
					R_BevelPush(&build, &v);
				}
				for (row = 0; row < segments / 2; row++) {
					int    a = base + row * 2, b = a + 1, c = a + 2, d = a + 3;
					vec3_t nrm;
					float  u = (row + 0.5f) / segments;

					R_BevelBlend(nSelf, 1.0f - u, nOther, u, NULL, 0, nrm);
					R_BevelTri(&build, f, a, b, d, nrm);
					R_BevelTri(&build, f, a, d, c, nrm);
				}
			}
		}

		// its pieces of sphere at the corners it is the piece for
		for (k = 0; k < f->numPoly; k++) {
			const bevelCorner_t *c = NULL;
			int                  me = -1, h, a, b, x, y;
			int                  grid[9][9];
			const float         *nA, *nB, *nC;

			for (h = cornerHash[R_BevelHashPoint(f->poly[k])]; h != -1; h = corners[h].next) {
				if (corners[h].ok && corners[h].type == CORNER_SPHERE && R_BevelSame(corners[h].p, f->poly[k])) {
					for (j = 0; j < 3; j++) {
						if (corners[h].face[j] == i) {
							c = &corners[h];
							me = j;
						}
					}
				}
			}
			if (!c) {
				continue;
			}
			nA = planes[c->plane[0]].n;
			nB = planes[c->plane[1]].n;
			nC = planes[c->plane[2]].n;

			for (a = 0; a <= segments; a++) {
				for (b = 0; b <= segments; b++) {
					grid[a][b] = -1;
				}
			}
			// (a, b): a steps towards plane 1, b towards plane 2, the rest
			// is plane 0's, so a side of the patch is a strip's end
			for (a = 0; a < segments; a++) {
				for (b = 0; a + b < segments; b++) {
					int tri[2][3][2] = { { { a, b }, { a + 1, b }, { a, b + 1 } }, { { a + 1, b }, { a + 1, b + 1 }, { a, b + 1 } } };
					int numTris = (a + b + 1 < segments) ? 2 : 1;

					for (x = 0; x < numTris; x++) {
						vec3_t mid;
						float  wa = 0, wb = 0, wc = 0, dots[3];
						int    owner, idx[3];

						for (y = 0; y < 3; y++) {
							wb += tri[x][y][0];
							wc += tri[x][y][1];
							wa += segments - tri[x][y][0] - tri[x][y][1];
						}
						R_BevelBlend(nA, wa, nB, wb, nC, wc, mid);
						dots[0] = DotProduct(mid, nA);
						dots[1] = DotProduct(mid, nB);
						dots[2] = DotProduct(mid, nC);
						owner = 0;
						if (dots[1] > dots[owner]) {
							owner = 1;
						}
						if (dots[2] > dots[owner]) {
							owner = 2;
						}
						if (owner != me) {
							continue;
						}
						for (y = 0; y < 3; y++) {
							int    ga = tri[x][y][0], gb = tri[x][y][1];
							vec3_t pos, nrm;

							if (grid[ga][gb] < 0) {
								R_BevelBlend(nA, (float)(segments - ga - gb), nB, (float)ga, nC, (float)gb, nrm);
								VectorMA(c->center, bevelRadius, nrm, pos);
								R_BevelVert(f, pos, nrm, &v);
								grid[ga][gb] = R_BevelPush(&build, &v);
							}
							idx[y] = grid[ga][gb];
						}
						R_BevelTri(&build, f, idx[0], idx[1], idx[2], mid);
					}
				}
			}
		}

		if (build.numVerts >= build.maxVerts || build.numIndexes + 3 > build.maxIndexes) {
			continue; // too much for one face: it stays as it was
		}

		// tangents for the new triangles, as ParseFace does
		for (k = 0; k < build.numIndexes; k += 3) {
			srfVert_t *dv[3];

			dv[0] = &build.verts[build.indexes[k]];
			dv[1] = &build.verts[build.indexes[k + 1]];
			dv[2] = &build.verts[build.indexes[k + 2]];
			R_CalcTangentVectors(dv);
		}

		cv->verts = ri.Hunk_Alloc(build.numVerts * sizeof(srfVert_t), h_low);
		cv->indexes = ri.Hunk_Alloc(build.numIndexes * sizeof(glIndex_t), h_low);
		Com_Memcpy(cv->verts, build.verts, build.numVerts * sizeof(srfVert_t));
		Com_Memcpy(cv->indexes, build.indexes, build.numIndexes * sizeof(glIndex_t));
		cv->numVerts = build.numVerts;
		cv->numIndexes = build.numIndexes;

		// part of it no longer faces the plane's way: no plane culling
		f->surf->cullinfo.type &= ~CULLINFO_PLANE;
		for (k = 0; k < cv->numVerts; k++) {
			AddPointToBounds(cv->verts[k].xyz, f->surf->cullinfo.bounds[0], f->surf->cullinfo.bounds[1]);
		}
		facesChanged++;
	}

	ri.Hunk_FreeTempMemory(build.indexes);
	ri.Hunk_FreeTempMemory(build.verts);
	ri.Hunk_FreeTempMemory(splitPool);
	ri.Hunk_FreeTempMemory(splitStart);
	ri.Hunk_FreeTempMemory(corners);
	ri.Hunk_FreeTempMemory(segs);
	ri.Hunk_FreeTempMemory(sides);
	ri.Hunk_FreeTempMemory(lines);
	ri.Hunk_FreeTempMemory(points);
	ri.Hunk_FreeTempMemory(faces);
	ri.Hunk_FreeTempMemory(planes);

	ri.Printf(PRINT_ALL, "bevel: %d edges rounded (%d corners, %d ends at walls), %d faces changed, radius %.1f, in %d ms\n", bevelled,
		sphereCorners, wallEnds, facesChanged, bevelRadius, ri.Milliseconds() - startTime);
	if (r_bevel->integer >= 2) {
		ri.Printf(PRINT_ALL, "bevel: %d faces taken; %d lines between two planes: %d with more planes, %d concave, %d at an angle out of "
			"range, %d stretches lined on one side only; %d segments, %d with something touching, %d lost cutting back\n",
			numFaces, stat_lines, stat_many, stat_concave, stat_angle, stat_uncovered, numSegs, stat_touched, stat_clip);
		ri.Printf(PRINT_ALL, "bevel: ends failed: planes there (0-7) %d %d %d %d %d %d %d %d, not a planar face %d, neither corner nor wall %d, "
			"no piece at a corner %d\n", stat_cornerPlanes[0], stat_cornerPlanes[1], stat_cornerPlanes[2], stat_cornerPlanes[3],
			stat_cornerPlanes[4], stat_cornerPlanes[5], stat_cornerPlanes[6], stat_cornerPlanes[7], stat_cornerForeign, stat_cornerOpen,
			stat_cornerNoPiece);
	}
}

void R_BevelRegisterCvars(void)
{
	r_bevel = ri.Cvar_Get("r_bevel", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_bevel, "Rounds the convex edges of the world's brushwork as maps load (only what is drawn: collision is "
		"unchanged); 2 also prints why edges were left sharp");
	r_bevelRadius = ri.Cvar_Get("r_bevelRadius", "2", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_bevelRadius, "r_bevel: the rounded edges' radius, in world units");
	r_bevelSegments = ri.Cvar_Get("r_bevelSegments", "4", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_bevelSegments, "r_bevel: the strips across a rounded edge (2 to 8, even)");
	r_bevelMinAngle = ri.Cvar_Get("r_bevelMinAngle", "30", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_bevelMinAngle, "r_bevel: edges whose faces turn less than this many degrees, or more than 180 minus it, "
		"stay sharp");
}
