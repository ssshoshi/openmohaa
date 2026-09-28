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
// DESCRIPTION:
// Covering the sides of props that were never meant to be seen.
//
// A crate against a wall, or a table on the floor, was built with nodraw or
// caulk on the faces nobody could see, and the compiler made no surface for
// them. Once the prop is knocked over those faces are holes. Here each such
// face of a brush model or a piece of brushwork furniture is rebuilt from its
// brush, given the texture of the brush's other sides, projected the way that
// texture runs on the piece's faces that are drawn, and drawn as a poly lit by
// the light grid -- only once the prop has moved from where it stood.
//
// A brush that is caulk or nodraw on every side is left alone: it stands in for
// something drawn another way, and covering it would draw a panel. So is a
// face thinner than PF_FILL_MIN_WIDTH: the edges of a card like a table's legs
// (a unit-thick slab carrying a masked texture on its two sides), where a
// cover would draw a post or a bar where there is meant to be nothing, and
// where a hole could hardly be seen.

#include "cg_physics_local.h"
#include "../physics/phys_furniture.h"

#include <map>
#include <vector>

#define PF_FILL_MAX_POINTS 32
#define PF_FILL_MIN_WIDTH  2.5f

typedef struct {
    qhandle_t shader;
    vec3_t    normal;
    int       first, count; // in pf_points, five floats a point: xyz, st
} pfFace_t;

typedef struct {
    std::vector<pfFace_t> faces;
} pfFill_t;

static std::vector<float>    pf_points;
static std::map<int, pfFill_t> pf_modelFills;     // by inline model
static std::vector<pfFill_t> pf_furnitureFills; // by piece of furniture

// Where each brush entity was first seen, and whether it has moved since.
typedef struct {
    int      model;
    vec3_t   origin, angles;
    qboolean moved;
} pfSeen_t;

static pfSeen_t pf_seen[MAX_GENTITIES];

//=============================================================
// Building
//=============================================================

typedef struct {
    const dshader_t    *shaders;
    const dplane_t     *planes;
    const dbrushside_t *sides;
    const dbrush_t     *brushes;
    const dsurface_t   *surfaces;
    const drawVert_t   *verts;
    int                 numShaders, numPlanes, numSides, numBrushes, numSurfaces, numVerts;
} pfBsp_t;

// How a texture runs over faces facing one way: s and t as linear in the two
// world axes across it.
typedef struct {
    qboolean valid;
    int      shader; // BSP shader it was taken from
    float    s[3], t[3];
} pfProjection_t;

static qboolean CG_FillHidden(const char *shader)
{
    return (strstr(shader, "nodraw") || strstr(shader, "caulk")) ? qtrue : qfalse;
}

static qboolean CG_FillUnseen(const char *shader)
{
    return !Q_stricmpn(shader, "textures/common/", 16) ? qtrue : qfalse;
}

static int CG_FillAxis(const vec3_t n)
{
    const float x = fabs(n[0]), y = fabs(n[1]), z = fabs(n[2]);

    return (z >= x && z >= y) ? 2 : (x >= y ? 0 : 1);
}

// Solve s = a*u + b*v + c through three points.
static qboolean CG_FillSolve(const float u[3], const float v[3], const float s[3], float out[3])
{
    const float det = u[0] * (v[1] - v[2]) - v[0] * (u[1] - u[2]) + (u[1] * v[2] - u[2] * v[1]);

    if (fabs(det) < 1e-4f) {
        return qfalse;
    }

    out[0] = (s[0] * (v[1] - v[2]) - v[0] * (s[1] - s[2]) + (s[1] * v[2] - s[2] * v[1])) / det;
    out[1] = (u[0] * (s[1] - s[2]) - s[0] * (u[1] - u[2]) + (u[1] * s[2] - u[2] * s[1])) / det;
    out[2] = (u[0] * (v[1] * s[2] - v[2] * s[1]) - v[0] * (u[1] * s[2] - u[2] * s[1]) + s[0] * (u[1] * v[2] - u[2] * v[1])) / det;
    return qtrue;
}

// How each texture runs on a piece: one projection per axis its faces face.
typedef struct {
    pfProjection_t axis[3];
} pfProjections_t;

typedef std::map<int, pfProjections_t> pfProjectionMap;

// The projections of a piece's drawn faces, by shader and by axis.
static void CG_FillProjections(const pfBsp_t *bsp, const int *surfaces, int numSurfaces, pfProjectionMap *projs)
{
    int i;

    projs->clear();

    for (i = 0; i < numSurfaces; i++) {
        const int         sn = surfaces[i];
        const dsurface_t *s;
        const drawVert_t *v;
        vec3_t            e1, e2, n;
        int               axis, a, b, k;
        float             u[3], w[3], ss[3], tt[3];
        pfProjection_t    p;

        if (sn < 0 || sn >= bsp->numSurfaces) {
            continue;
        }
        s = &bsp->surfaces[sn];
        if (s->surfaceType != MST_PLANAR || s->numVerts < 3 || s->firstVert < 0 || s->firstVert + s->numVerts > bsp->numVerts) {
            continue;
        }
        if (s->shaderNum < 0 || s->shaderNum >= bsp->numShaders || CG_FillUnseen(bsp->shaders[s->shaderNum].shader)) {
            continue;
        }

        v = &bsp->verts[s->firstVert];
        VectorSubtract(v[1].xyz, v[0].xyz, e1);
        VectorSubtract(v[2].xyz, v[0].xyz, e2);
        CrossProduct(e1, e2, n);
        if (VectorNormalize(n) < 1e-3f) {
            continue;
        }

        axis = CG_FillAxis(n);
        if ((*projs)[s->shaderNum].axis[axis].valid) {
            continue;
        }

        a = (axis + 1) % 3;
        b = (axis + 2) % 3;
        for (k = 0; k < 3; k++) {
            u[k]  = v[k].xyz[a];
            w[k]  = v[k].xyz[b];
            ss[k] = v[k].st[0];
            tt[k] = v[k].st[1];
        }

        if (!CG_FillSolve(u, w, ss, p.s) || !CG_FillSolve(u, w, tt, p.t)) {
            continue;
        }

        p.valid                             = qtrue;
        p.shader                            = s->shaderNum;
        (*projs)[s->shaderNum].axis[axis] = p;
    }
}

// For a face of a brush drawn with shader, facing along axis: the projection
// to texture it by. Exact when the shader has faces that way; else its scale
// from another way; else how anything on the piece runs that way.
static const pfProjection_t *CG_FillProjectionFor(const pfProjectionMap *projs, int shader, int axis, qboolean *exact)
{
    pfProjectionMap::const_iterator it = projs->find(shader);
    int                             k;

    *exact = qtrue;
    if (it != projs->end() && it->second.axis[axis].valid) {
        return &it->second.axis[axis];
    }
    for (it = projs->begin(); it != projs->end(); ++it) {
        if (it->second.axis[axis].valid) {
            return &it->second.axis[axis];
        }
    }

    *exact = qfalse;
    it     = projs->find(shader);
    if (it != projs->end()) {
        for (k = 0; k < 3; k++) {
            if (it->second.axis[k].valid) {
                return &it->second.axis[k];
            }
        }
    }

    return NULL;
}

static void CG_FillAddFaces(const pfBsp_t *bsp, const int *brushes, int numBrushes, const pfProjectionMap *projs, pfFill_t *fill)
{
    int i, j, k;

    for (i = 0; i < numBrushes; i++) {
        const int       bn = brushes[i];
        const dbrush_t *brush;
        int             planeNums[128], numPlanes = 0;
        static vec3_t   corners[1024];
        int             numCorners, visibleShader = -1, visibleCount = 0;
        std::map<int, int> sideUse;

        if (bn < 0 || bn >= bsp->numBrushes) {
            continue;
        }
        brush = &bsp->brushes[bn];

        // Only a brush that is drawn on some side.
        for (j = 0; j < brush->numSides; j++) {
            const int side = brush->firstSide + j;

            if (side < 0 || side >= bsp->numSides) {
                continue;
            }
            if (bsp->sides[side].shaderNum >= 0 && bsp->sides[side].shaderNum < bsp->numShaders
                && !CG_FillUnseen(bsp->shaders[bsp->sides[side].shaderNum].shader)) {
                const int use = ++sideUse[bsp->sides[side].shaderNum];

                if (use > visibleCount) {
                    visibleShader = bsp->sides[side].shaderNum;
                    visibleCount  = use;
                }
            }
            if (numPlanes < (int)ARRAY_LEN(planeNums) && bsp->sides[side].planeNum >= 0 && bsp->sides[side].planeNum < bsp->numPlanes) {
                planeNums[numPlanes++] = bsp->sides[side].planeNum;
            }
        }

        if (visibleShader < 0) {
            continue;
        }

        numCorners = Phys_BrushCorners(bsp->planes, planeNums, numPlanes, corners, ARRAY_LEN(corners));
        if (numCorners < 4) {
            continue;
        }

        for (j = 0; j < brush->numSides; j++) {
            const int       side = brush->firstSide + j;
            const dplane_t *plane;
            vec3_t          pts[PF_FILL_MAX_POINTS], centre, u, v;
            float           angles[PF_FILL_MAX_POINTS];
            int             n = 0, order[PF_FILL_MAX_POINTS];
            pfFace_t        face;
            const pfProjection_t *pr;
            qboolean        exact;
            float           area = 0.0f, span = 0.0f;
            int             axis, a, b;

            if (side < 0 || side >= bsp->numSides || bsp->sides[side].shaderNum < 0 || bsp->sides[side].shaderNum >= bsp->numShaders
                || bsp->sides[side].planeNum < 0 || bsp->sides[side].planeNum >= bsp->numPlanes) {
                continue;
            }
            if (!CG_FillHidden(bsp->shaders[bsp->sides[side].shaderNum].shader)) {
                continue;
            }

            plane = &bsp->planes[bsp->sides[side].planeNum];

            // The brush's corners on this side.
            for (k = 0; k < numCorners && n < PF_FILL_MAX_POINTS; k++) {
                int m;

                if (fabs(DotProduct(corners[k], plane->normal) - plane->dist) > 0.1f) {
                    continue;
                }
                for (m = 0; m < n; m++) {
                    if (Distance(pts[m], corners[k]) < 0.05f) {
                        break;
                    }
                }
                if (m == n) {
                    VectorCopy(corners[k], pts[n]);
                    n++;
                }
            }
            if (n < 3) {
                continue;
            }

            // In order round the face, clockwise seen from outside, as the
            // engine winds its faces.
            VectorClear(centre);
            for (k = 0; k < n; k++) {
                VectorAdd(centre, pts[k], centre);
            }
            VectorScale(centre, 1.0f / n, centre);
            PerpendicularVector(u, plane->normal);
            CrossProduct(plane->normal, u, v);
            for (k = 0; k < n; k++) {
                vec3_t d;

                VectorSubtract(pts[k], centre, d);
                angles[k] = atan2f(DotProduct(d, v), DotProduct(d, u));
                order[k]  = k;
            }
            for (k = 1; k < n; k++) {
                int m = k;

                while (m > 0 && angles[order[m - 1]] < angles[order[m]]) {
                    const int t  = order[m];
                    order[m]     = order[m - 1];
                    order[m - 1] = t;
                    m--;
                }
            }

            // Too thin to be a hole worth covering (see the top).
            for (k = 1; k + 1 < n; k++) {
                vec3_t e1, e2, c;

                VectorSubtract(pts[order[k]], pts[order[0]], e1);
                VectorSubtract(pts[order[k + 1]], pts[order[0]], e2);
                CrossProduct(e1, e2, c);
                area += 0.5f * VectorLength(c);
            }
            for (k = 0; k < n; k++) {
                int m;

                for (m = k + 1; m < n; m++) {
                    span = Q_max(span, Distance(pts[k], pts[m]));
                }
            }
            if (span <= 0.0f || area / span < PF_FILL_MIN_WIDTH) {
                continue;
            }

            // Its texture: the brush's own, run as it runs on the piece.
            axis = CG_FillAxis(plane->normal);
            pr   = CG_FillProjectionFor(projs, visibleShader, axis, &exact);

            face.shader = cgi.R_RegisterShaderVertexLit(bsp->shaders[visibleShader].shader);
            if (!face.shader) {
                continue;
            }
            VectorCopy(plane->normal, face.normal);
            face.first = (int)(pf_points.size() / 5);
            face.count = n;

            a = (axis + 1) % 3;
            b = (axis + 2) % 3;
            for (k = 0; k < n; k++) {
                const float *p = pts[order[k]];
                float        s, t;

                if (pr && exact) {
                    s = pr->s[0] * p[a] + pr->s[1] * p[b] + pr->s[2];
                    t = pr->t[0] * p[a] + pr->t[1] * p[b] + pr->t[2];
                } else {
                    // Turned from how the piece faces: the same scale, from
                    // wherever it is.
                    const float scaleS = pr ? sqrtf(pr->s[0] * pr->s[0] + pr->s[1] * pr->s[1]) : 1.0f / 128.0f;
                    const float scaleT = pr ? sqrtf(pr->t[0] * pr->t[0] + pr->t[1] * pr->t[1]) : 1.0f / 128.0f;

                    s = p[a] * scaleS;
                    t = p[b] * scaleT;
                }

                pf_points.push_back(p[0]);
                pf_points.push_back(p[1]);
                pf_points.push_back(p[2]);
                pf_points.push_back(s);
                pf_points.push_back(t);
            }

            fill->faces.push_back(face);
        }
    }
}

void CG_PhysicsUnloadFills(void)
{
    pf_points.clear();
    pf_modelFills.clear();
    pf_furnitureFills.clear();
    memset(pf_seen, 0, sizeof(pf_seen));
}

static const void *CG_FillLump(const void *data, long len, dheader_t *header, int lumpNum, int elementSize, int *count)
{
    const lump_t *lump = Q_GetLumpByVersion(header, lumpNum);

    *count = 0;
    if (lump->fileofs < 0 || lump->filelen <= 0 || lump->fileofs + lump->filelen > len) {
        return NULL;
    }

    *count = lump->filelen / elementSize;
    return (const byte *)data + lump->fileofs;
}

// At map load, with the BSP in hand and the furniture found.
void CG_PhysicsLoadFills(const void *data, long len)
{
    dheader_t       header;
    pfBsp_t         bsp;
    const dmodel_t *models;
    int             numModels, i, faces = 0;

    CG_PhysicsUnloadFills();

    if (cgi.apiversion < 5 || !cgi.R_RegisterShaderVertexLit || !data || len < (long)sizeof(dheader_t)) {
        return;
    }

    memcpy(&header, data, sizeof(header));
    for (i = 0; i < (int)(sizeof(dheader_t) / 4); i++) {
        ((int *)&header)[i] = LittleLong(((int *)&header)[i]);
    }

    bsp.shaders  = (const dshader_t *)CG_FillLump(data, len, &header, LUMP_SHADERS, sizeof(dshader_t), &bsp.numShaders);
    bsp.planes   = (const dplane_t *)CG_FillLump(data, len, &header, LUMP_PLANES, sizeof(dplane_t), &bsp.numPlanes);
    bsp.sides    = (const dbrushside_t *)CG_FillLump(data, len, &header, LUMP_BRUSHSIDES, sizeof(dbrushside_t), &bsp.numSides);
    bsp.brushes  = (const dbrush_t *)CG_FillLump(data, len, &header, LUMP_BRUSHES, sizeof(dbrush_t), &bsp.numBrushes);
    bsp.surfaces = (const dsurface_t *)CG_FillLump(data, len, &header, LUMP_SURFACES, sizeof(dsurface_t), &bsp.numSurfaces);
    bsp.verts    = (const drawVert_t *)CG_FillLump(data, len, &header, LUMP_DRAWVERTS, sizeof(drawVert_t), &bsp.numVerts);
    models       = (const dmodel_t *)CG_FillLump(data, len, &header, LUMP_MODELS, sizeof(dmodel_t), &numModels);

    if (!bsp.shaders || !bsp.planes || !bsp.sides || !bsp.brushes || !bsp.surfaces || !bsp.verts || !models) {
        return;
    }

    // Every brush model: crates, barrels, and whatever else the server moves.
    for (i = 1; i < numModels; i++) {
        std::vector<int> brushes, surfaces;
        pfProjectionMap  projs;
        pfFill_t         fill;
        int              k;

        for (k = 0; k < models[i].numBrushes; k++) {
            brushes.push_back(models[i].firstBrush + k);
        }
        for (k = 0; k < models[i].numSurfaces; k++) {
            surfaces.push_back(models[i].firstSurface + k);
        }
        if (brushes.empty()) {
            continue;
        }

        CG_FillProjections(&bsp, surfaces.empty() ? NULL : &surfaces[0], (int)surfaces.size(), &projs);
        CG_FillAddFaces(&bsp, &brushes[0], (int)brushes.size(), &projs, &fill);

        if (!fill.faces.empty()) {
            faces += (int)fill.faces.size();
            pf_modelFills[i] = fill;
        }
    }

    // The furniture in the world's brushwork.
    pf_furnitureFills.resize(CG_PhysicsFurnitureCount());
    for (i = 0; i < CG_PhysicsFurnitureCount(); i++) {
        const physFurniture_t *f = CG_PhysicsFurnitureShape(i);
        pfProjectionMap        projs;

        if (!f || f->brushes.empty()) {
            continue;
        }

        CG_FillProjections(&bsp, f->surfaces.empty() ? NULL : &f->surfaces[0], (int)f->surfaces.size(), &projs);
        CG_FillAddFaces(&bsp, &f->brushes[0], (int)f->brushes.size(), &projs, &pf_furnitureFills[i]);
        faces += (int)pf_furnitureFills[i].faces.size();

        if (cg_physics_log->integer > 1) {
            cgi.Printf("  furniture %d: %d hidden faces covered\n", i, (int)pf_furnitureFills[i].faces.size());
        }
    }

    if (cg_physics_log->integer) {
        cgi.Printf(
            "physics: %d hidden faces covered, on %d brush models and the furniture\n", faces, (int)pf_modelFills.size()
        );
    }
}

//=============================================================
// Drawing
//=============================================================

static void CG_FillDraw(const pfFill_t *fill, const vec3_t origin, const vec3_t axis[3])
{
    polyVert_t verts[PF_FILL_MAX_POINTS];

    for (size_t i = 0; i < fill->faces.size(); i++) {
        const pfFace_t *face = &fill->faces[i];
        vec3_t          normal, centre, light;
        int             k;

        VectorClear(normal);
        for (k = 0; k < 3; k++) {
            VectorMA(normal, face->normal[k], axis[k], normal);
        }

        VectorClear(centre);
        for (k = 0; k < face->count; k++) {
            const float *p = &pf_points[(face->first + k) * 5];
            float       *x = verts[k].xyz;

            VectorCopy(origin, x);
            VectorMA(x, p[0], axis[0], x);
            VectorMA(x, p[1], axis[1], x);
            VectorMA(x, p[2], axis[2], x);
            verts[k].st[0] = p[3];
            verts[k].st[1] = p[4];
            VectorAdd(centre, x, centre);
        }
        VectorScale(centre, 1.0f / face->count, centre);

        // Lit as a decal there would be, from just off the face.
        VectorMA(centre, 2.0f, normal, centre);
        cgi.R_GetLightingForDecal(light, normal, centre);

        for (k = 0; k < face->count; k++) {
            verts[k].modulate[0] = (byte)Q_clamp_float(light[0], 0.0f, 255.0f);
            verts[k].modulate[1] = (byte)Q_clamp_float(light[1], 0.0f, 255.0f);
            verts[k].modulate[2] = (byte)Q_clamp_float(light[2], 0.0f, 255.0f);
            verts[k].modulate[3] = 255;
        }

        cgi.R_AddPolyToScene(face->shader, face->count, verts, 0);
    }
}

void CG_PhysicsDrawFurnitureFill(int index, const vec3_t origin, const vec3_t axis[3])
{
    if (cg_physics_debug->integer == 4) {
        return;
    }

    if (index < 0 || index >= (int)pf_furnitureFills.size() || pf_furnitureFills[index].faces.empty()) {
        return;
    }

    CG_FillDraw(&pf_furnitureFills[index], origin, axis);
}

// Every frame, for each brush entity drawn: its hidden faces, once it has moved
// from where it was first seen.
void CG_PhysicsDrawModelFill(int entnum, int model, const vec3_t origin, const vec3_t angles)
{
    std::map<int, pfFill_t>::const_iterator it;
    pfSeen_t                               *seen;
    vec3_t                                  axis[3];

    if (entnum < 0 || entnum >= MAX_GENTITIES || !cg_physics->integer) {
        return;
    }

    it = pf_modelFills.find(model);
    if (it == pf_modelFills.end()) {
        return;
    }

    // cg_physics_debug 3 turns every brush entity over where it stands (see
    // CG_AddCEntity) and covers it whether or not it has moved; 4 turns them
    // over without covers, to compare.
    if (cg_physics_debug->integer == 3 || cg_physics_debug->integer == 4) {
        if (cg_physics_debug->integer == 3) {
            AnglesToAxis(angles, axis);
            CG_FillDraw(&it->second, origin, axis);
        }
        return;
    }

    seen = &pf_seen[entnum];
    if (seen->model != model) {
        seen->model = model;
        seen->moved = qfalse;
        VectorCopy(origin, seen->origin);
        VectorCopy(angles, seen->angles);
        return;
    }

    if (!seen->moved) {
        if (Distance(origin, seen->origin) < 0.5f && fabs(AngleDelta(angles[0], seen->angles[0])) < 0.5f
            && fabs(AngleDelta(angles[1], seen->angles[1])) < 0.5f && fabs(AngleDelta(angles[2], seen->angles[2])) < 0.5f) {
            return;
        }
        seen->moved = qtrue;
    }

    AnglesToAxis(angles, axis);
    CG_FillDraw(&it->second, origin, axis);
}
