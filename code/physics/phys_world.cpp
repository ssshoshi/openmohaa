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
// The map's collision in Jolt's terms, read from the BSP, for either module.
//
// Jolt cannot ask the engine's collision model anything, so the map is built
// again from the same data: every world brush with the contents asked for
// becomes a convex hull (a brush is a convex volume by construction), and
// curved patches and terrain become triangle meshes, split the way the engine
// splits them for its own collision. Everything is static and is grouped into
// one body per grid cell, which keeps the broad phase small.

#include "phys_world.h"

#include <map>

#define PW_CELL_SIZE      1024.0f
#define PW_PLANE_EPSILON  0.05f
#define PW_POINT_EPSILON  0.1f
#define PW_PATCH_STEPS    8
#define PW_CONVEX_RADIUS  0.005f // metres; brushes can be a unit thick

//=============================================================
// Cells
//=============================================================

typedef struct {
    JPH::StaticCompoundShapeSettings hulls;
    JPH::TriangleList                triangles;
    int                              numHulls;
} pwCell_t;

static long long Phys_CellKey(const vec3_t p)
{
    const long long x = (long long)floorf(p[0] / PW_CELL_SIZE) + 1024;
    const long long y = (long long)floorf(p[1] / PW_CELL_SIZE) + 1024;
    const long long z = (long long)floorf(p[2] / PW_CELL_SIZE) + 1024;

    return (x << 42) | (y << 21) | z;
}

//=============================================================
// Brushes
//=============================================================

// The corners of a brush: every point where three of its planes meet that is
// not outside any of the others.
int Phys_BrushCorners(const dplane_t *planes, const int *sides, int numSides, vec3_t *out, int maxOut)
{
    int i, j, k, m, n = 0;

    for (i = 0; i < numSides; i++) {
        const dplane_t *a = &planes[sides[i]];

        for (j = i + 1; j < numSides; j++) {
            const dplane_t *b = &planes[sides[j]];
            vec3_t          bc;

            CrossProduct(b->normal, a->normal, bc);
            if (VectorLengthSquared(bc) < 1e-8f) {
                continue;
            }

            for (k = j + 1; k < numSides; k++) {
                const dplane_t *c = &planes[sides[k]];
                vec3_t          cb, ac, ba, p;
                float           denom;
                qboolean        inside = qtrue;

                CrossProduct(b->normal, c->normal, cb);
                CrossProduct(c->normal, a->normal, ac);
                CrossProduct(a->normal, b->normal, ba);

                denom = DotProduct(a->normal, cb);
                if (fabsf(denom) < 1e-6f) {
                    continue;
                }

                for (m = 0; m < 3; m++) {
                    p[m] = (a->dist * cb[m] + b->dist * ac[m] + c->dist * ba[m]) / denom;
                }

                for (m = 0; m < numSides; m++) {
                    const dplane_t *q = &planes[sides[m]];

                    if (DotProduct(q->normal, p) - q->dist > PW_PLANE_EPSILON) {
                        inside = qfalse;
                        break;
                    }
                }

                if (!inside) {
                    continue;
                }

                for (m = 0; m < n; m++) {
                    if (Distance(out[m], p) < PW_POINT_EPSILON) {
                        break;
                    }
                }

                // VectorCopy is a macro: its destination must not have a side effect.
                if (m == n && n < maxOut) {
                    VectorCopy(p, out[n]);
                    n++;
                }
            }
        }
    }

    return n;
}

// The outline of one face of a brush, for drawing: the corners on its plane,
// in order round the face.
static void Phys_BrushFaceOutline(const physBspOptions_t *opt, const dplane_t *plane, const vec3_t *corners, int numCorners)
{
    vec3_t on[64], centre, u, v;
    float  angle[64];
    int    n = 0, i, j;

    VectorClear(centre);
    for (i = 0; i < numCorners && n < 64; i++) {
        if (fabsf(DotProduct(plane->normal, corners[i]) - plane->dist) < PW_PLANE_EPSILON * 4.0f) {
            VectorCopy(corners[i], on[n]);
            VectorAdd(centre, corners[i], centre);
            n++;
        }
    }

    if (n < 3) {
        return;
    }

    VectorScale(centre, 1.0f / n, centre);
    PerpendicularVector(u, plane->normal);
    CrossProduct(plane->normal, u, v);

    for (i = 0; i < n; i++) {
        vec3_t d;

        VectorSubtract(on[i], centre, d);
        angle[i] = atan2f(DotProduct(d, v), DotProduct(d, u));
    }

    // Few enough to sort by insertion.
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 && angle[j - 1] > angle[j]; j--) {
            vec3_t t;
            float  a = angle[j];

            angle[j]     = angle[j - 1];
            angle[j - 1] = a;
            VectorCopy(on[j], t);
            VectorCopy(on[j - 1], on[j]);
            VectorCopy(t, on[j - 1]);
        }
    }

    opt->outline(opt->ctx, on, n, 0);
}

//=============================================================
// Meshes
//=============================================================

// A triangle facing the given way. Jolt's meshes are one sided by default.
static void Phys_AddTriangle(physBspStats_t *stats, pwCell_t *cell, const vec3_t a, const vec3_t b, const vec3_t c, const vec3_t facing)
{
    vec3_t e1, e2, n;

    VectorSubtract(b, a, e1);
    VectorSubtract(c, a, e2);
    CrossProduct(e1, e2, n);

    if (VectorLengthSquared(n) < 1e-6f) {
        return;
    }

    if (DotProduct(n, facing) < 0.0f) {
        cell->triangles.push_back(JPH::Triangle(
            JPH::Float3(a[0] * PHYS_UNITS_TO_METRES, a[1] * PHYS_UNITS_TO_METRES, a[2] * PHYS_UNITS_TO_METRES),
            JPH::Float3(c[0] * PHYS_UNITS_TO_METRES, c[1] * PHYS_UNITS_TO_METRES, c[2] * PHYS_UNITS_TO_METRES),
            JPH::Float3(b[0] * PHYS_UNITS_TO_METRES, b[1] * PHYS_UNITS_TO_METRES, b[2] * PHYS_UNITS_TO_METRES)
        ));
    } else {
        cell->triangles.push_back(JPH::Triangle(
            JPH::Float3(a[0] * PHYS_UNITS_TO_METRES, a[1] * PHYS_UNITS_TO_METRES, a[2] * PHYS_UNITS_TO_METRES),
            JPH::Float3(b[0] * PHYS_UNITS_TO_METRES, b[1] * PHYS_UNITS_TO_METRES, b[2] * PHYS_UNITS_TO_METRES),
            JPH::Float3(c[0] * PHYS_UNITS_TO_METRES, c[1] * PHYS_UNITS_TO_METRES, c[2] * PHYS_UNITS_TO_METRES)
        ));
    }

    stats->triangles++;
}

static void Phys_PatchPoint(const drawVert_t *cp, int width, int i0, int j0, float s, float t, vec3_t out, vec3_t normal)
{
    const float bs[3] = {(1 - s) * (1 - s), 2 * s * (1 - s), s * s};
    const float bt[3] = {(1 - t) * (1 - t), 2 * t * (1 - t), t * t};
    int         a, b, k;

    VectorClear(out);
    VectorClear(normal);

    for (b = 0; b < 3; b++) {
        for (a = 0; a < 3; a++) {
            const drawVert_t *v = &cp[(j0 + b) * width + i0 + a];
            const float       w = bs[a] * bt[b];

            for (k = 0; k < 3; k++) {
                out[k] += v->xyz[k] * w;
                normal[k] += v->normal[k] * w;
            }
        }
    }
}

//=============================================================
// Building
//=============================================================

static const void *Phys_Lump(const void *bsp, long len, dheader_t *header, int lumpNum, int elementSize, int *count)
{
    const lump_t *lump = Q_GetLumpByVersion(header, lumpNum);

    *count = 0;
    if (lump->fileofs < 0 || lump->filelen <= 0 || lump->fileofs + lump->filelen > len || elementSize <= 0) {
        return NULL;
    }

    *count = lump->filelen / elementSize;
    return (const byte *)bsp + lump->fileofs;
}

void Phys_BuildBspWorld(
    JPH::PhysicsSystem *system, const void *bsp, long len, const physBspOptions_t *opt, std::vector<JPH::BodyID> *bodies, physBspStats_t *stats
)
{
    dheader_t                       header;
    const dshader_t                *shaders;
    const dplane_t                 *planes;
    const dbrushside_t             *brushSides;
    const dbrush_t                 *brushes;
    const dmodel_t                 *models;
    const dsurface_t               *surfaces;
    const drawVert_t               *verts;
    const cTerraPatch_t            *terrain;
    int                             numShaders, numPlanes, numBrushSides, numBrushes, numModels, numSurfaces;
    int                             numVerts, numTerrain;
    int                             i, j, k;
    std::map<long long, pwCell_t *> cells;

    memset(stats, 0, sizeof(*stats));

    if (!bsp || len < (long)sizeof(dheader_t)) {
        return;
    }

    memcpy(&header, bsp, sizeof(header));
    for (i = 0; i < (int)(sizeof(dheader_t) / 4); i++) {
        ((int *)&header)[i] = LittleLong(((int *)&header)[i]);
    }

    shaders    = (const dshader_t *)Phys_Lump(bsp, len, &header, LUMP_SHADERS, sizeof(dshader_t), &numShaders);
    planes     = (const dplane_t *)Phys_Lump(bsp, len, &header, LUMP_PLANES, sizeof(dplane_t), &numPlanes);
    brushSides = (const dbrushside_t *)Phys_Lump(bsp, len, &header, LUMP_BRUSHSIDES, sizeof(dbrushside_t), &numBrushSides);
    brushes    = (const dbrush_t *)Phys_Lump(bsp, len, &header, LUMP_BRUSHES, sizeof(dbrush_t), &numBrushes);
    models     = (const dmodel_t *)Phys_Lump(bsp, len, &header, LUMP_MODELS, sizeof(dmodel_t), &numModels);
    surfaces   = (const dsurface_t *)Phys_Lump(bsp, len, &header, LUMP_SURFACES, sizeof(dsurface_t), &numSurfaces);
    verts      = (const drawVert_t *)Phys_Lump(bsp, len, &header, LUMP_DRAWVERTS, sizeof(drawVert_t), &numVerts);
    terrain    = (const cTerraPatch_t *)Phys_Lump(bsp, len, &header, LUMP_TERRAIN, sizeof(cTerraPatch_t), &numTerrain);

    //
    // The world model's brushes. Those of the other models are doors, lifts
    // and the like, which move and are handled with the entities that own them.
    //
    if (shaders && planes && brushSides && brushes && models && numModels > 0) {
        const int first = models[0].firstBrush;
        const int count = models[0].numBrushes;

        for (i = first; i < first + count && i < numBrushes; i++) {
            const dbrush_t *brush = &brushes[i];
            int             sides[128];
            int             numSides = 0;
            static vec3_t   corners[1024];
            vec3_t          centre;
            int             numCorners;

            if (brush->shaderNum < 0 || brush->shaderNum >= numShaders
                || !(shaders[brush->shaderNum].contentFlags & opt->contents)) {
                continue;
            }

            for (j = 0; j < brush->numSides && numSides < (int)ARRAY_LEN(sides); j++) {
                const int side = brush->firstSide + j;

                if (side < 0 || side >= numBrushSides || brushSides[side].planeNum < 0
                    || brushSides[side].planeNum >= numPlanes) {
                    continue;
                }
                sides[numSides++] = brushSides[side].planeNum;
            }

            numCorners = Phys_BrushCorners(planes, sides, numSides, corners, ARRAY_LEN(corners));
            if (numCorners < 4) {
                stats->brushFails++;
                continue;
            }

            VectorClear(centre);
            for (j = 0; j < numCorners; j++) {
                VectorAdd(centre, corners[j], centre);
            }
            VectorScale(centre, 1.0f / numCorners, centre);

            if (opt->skipBrush) {
                vec3_t bmins, bmaxs;

                ClearBounds(bmins, bmaxs);
                for (j = 0; j < numCorners; j++) {
                    AddPointToBounds(corners[j], bmins, bmaxs);
                }

                if (opt->skipBrush(opt->ctx, i, shaders[brush->shaderNum].shader, shaders[brush->shaderNum].contentFlags, bmins, bmaxs)) {
                    stats->skipped++;
                    continue;
                }
            }

            {
                JPH::Array<JPH::Vec3> points;

                for (j = 0; j < numCorners; j++) {
                    vec3_t d;

                    VectorSubtract(corners[j], centre, d);
                    points.push_back(PhysToJolt(d));
                }

                JPH::ConvexHullShapeSettings hull(points, PW_CONVEX_RADIUS);
                JPH::ShapeSettings::ShapeResult result = hull.Create();

                if (result.HasError()) {
                    stats->brushFails++;
                    continue;
                }

                // Fences (grates, wire mesh) in bodies of their own, told apart
                // by their user data: solid to props, which should not fall
                // through a window grate, where the engine lets rays through
                // the holes in the texture.
                const qboolean  fence = (shaders[brush->shaderNum].contentFlags & CONTENTS_SOLID) ? qfalse : qtrue;
                const long long key   = Phys_CellKey(centre) | (fence ? (1LL << 62) : 0);
                pwCell_t      *&cell = cells[key];

                if (!cell) {
                    cell           = new pwCell_t;
                    cell->numHulls = 0;
                }
                cell->hulls.AddShape(PhysToJolt(centre), JPH::Quat::sIdentity(), result.Get());
                cell->numHulls++;
            }

            if (opt->outline) {
                for (j = 0; j < numSides; j++) {
                    Phys_BrushFaceOutline(opt, &planes[sides[j]], corners, numCorners);
                }
            }

            stats->brushes++;
        }
    }

    //
    // Curved patches: each 3x3 block of control points is one biquadratic
    // piece, evaluated on a fixed grid. Only those the engine collides with.
    //
    if (surfaces && verts && shaders) {
        for (i = 0; i < numSurfaces; i++) {
            const dsurface_t *surf = &surfaces[i];
            const int         width = surf->patchWidth, height = surf->patchHeight;
            int               pi, pj, a, b;

            if (surf->surfaceType != MST_PATCH || surf->shaderNum < 0 || surf->shaderNum >= numShaders
                || !(shaders[surf->shaderNum].contentFlags & CONTENTS_SOLID) || width < 3 || height < 3
                || surf->firstVert < 0 || surf->firstVert + width * height > numVerts) {
                continue;
            }

            for (pj = 0; pj + 2 < height; pj += 2) {
                for (pi = 0; pi + 2 < width; pi += 2) {
                    vec3_t grid[PW_PATCH_STEPS + 1][PW_PATCH_STEPS + 1];
                    vec3_t norm[PW_PATCH_STEPS + 1][PW_PATCH_STEPS + 1];
                    vec3_t mid, midN;
                    pwCell_t *cell;

                    for (b = 0; b <= PW_PATCH_STEPS; b++) {
                        for (a = 0; a <= PW_PATCH_STEPS; a++) {
                            Phys_PatchPoint(
                                &verts[surf->firstVert],
                                width,
                                pi,
                                pj,
                                (float)a / PW_PATCH_STEPS,
                                (float)b / PW_PATCH_STEPS,
                                grid[b][a],
                                norm[b][a]
                            );
                        }
                    }

                    Phys_PatchPoint(&verts[surf->firstVert], width, pi, pj, 0.5f, 0.5f, mid, midN);
                    {
                        pwCell_t *&c = cells[Phys_CellKey(mid)];
                        if (!c) {
                            c           = new pwCell_t;
                            c->numHulls = 0;
                        }
                        cell = c;
                    }

                    for (b = 0; b < PW_PATCH_STEPS; b++) {
                        for (a = 0; a < PW_PATCH_STEPS; a++) {
                            vec3_t quad[4];

                            Phys_AddTriangle(stats, cell, grid[b][a], grid[b][a + 1], grid[b + 1][a + 1], norm[b][a]);
                            Phys_AddTriangle(stats, cell, grid[b][a], grid[b + 1][a + 1], grid[b + 1][a], norm[b][a]);

                            VectorCopy(grid[b][a], quad[0]);
                            VectorCopy(grid[b][a + 1], quad[1]);
                            VectorCopy(grid[b + 1][a + 1], quad[2]);
                            VectorCopy(grid[b + 1][a], quad[3]);
                            if (opt->outline) {
                                opt->outline(opt->ctx, quad, 4, 1);
                            }
                        }
                    }
                }
            }

            stats->patches++;
        }
    }

    //
    // Terrain: 8x8 squares of 64 units a patch, each two triangles split
    // along the diagonal the engine's collision uses (CM_GenerateTerrainCollide).
    //
    if (terrain) {
        const vec3_t up = {0, 0, 1};

        for (i = 0; i < numTerrain; i++) {
            const cTerraPatch_t *patch = &terrain[i];
            const int            x0 = patch->x << 6, y0 = patch->y << 6, z0 = patch->iBaseHeight;
            vec3_t               mid;
            pwCell_t            *cell;

            VectorSet(mid, x0 + 256.0f, y0 + 256.0f, (float)z0);
            {
                pwCell_t *&c = cells[Phys_CellKey(mid)];
                if (!c) {
                    c           = new pwCell_t;
                    c->numHulls = 0;
                }
                cell = c;
            }

            for (k = 0; k < 8; k++) {
                for (j = 0; j < 8; j++) {
                    vec3_t v1, v2, v3, v4, quad[4];

                    VectorSet(v1, (float)((j << 6) + x0), (float)((k << 6) + y0), (float)(z0 + 2 * patch->heightmap[k * 9 + j]));
                    VectorSet(v2, v1[0] + 64, v1[1], (float)(z0 + 2 * patch->heightmap[k * 9 + j + 1]));
                    VectorSet(v3, v1[0] + 64, v1[1] + 64, (float)(z0 + 2 * patch->heightmap[(k + 1) * 9 + j + 1]));
                    VectorSet(v4, v1[0], v1[1] + 64, (float)(z0 + 2 * patch->heightmap[(k + 1) * 9 + j]));

                    if ((j + k) & 1) {
                        // Split along v2-v4.
                        Phys_AddTriangle(stats, cell, v2, v4, v3, up);
                        Phys_AddTriangle(stats, cell, v4, v2, v1, up);
                    } else {
                        // Split along v1-v3.
                        Phys_AddTriangle(stats, cell, v3, v1, v4, up);
                        Phys_AddTriangle(stats, cell, v1, v3, v2, up);
                    }

                    VectorCopy(v1, quad[0]);
                    VectorCopy(v2, quad[1]);
                    VectorCopy(v3, quad[2]);
                    VectorCopy(v4, quad[3]);
                    if (opt->outline) {
                        opt->outline(opt->ctx, quad, 4, 2);
                    }
                }
            }

            stats->terrain++;
        }
    }

    //
    // One static body for each cell's hulls, and one for its triangles.
    //
    {
        JPH::BodyInterface &bi = system->GetBodyInterface();

        for (auto &it : cells) {
            pwCell_t *cell = it.second;

            if (cell->numHulls) {
                JPH::ShapeSettings::ShapeResult shape = cell->hulls.Create();

                if (shape.HasError()) {
                    stats->brushFails += cell->numHulls;
                } else {
                    JPH::BodyCreationSettings settings(
                        shape.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, PhysLayers::WORLD
                    );
                    settings.mUserData = (it.first & (1LL << 62)) ? PHYS_USERDATA_FENCE : PHYS_USERDATA_WORLD;
                    bodies->push_back(bi.CreateAndAddBody(settings, JPH::EActivation::DontActivate));
                }
            }

            if (!cell->triangles.empty()) {
                JPH::MeshShapeSettings          mesh(cell->triangles);
                JPH::ShapeSettings::ShapeResult shape = mesh.Create();

                if (shape.HasError()) {
                    stats->brushFails++;
                } else {
                    JPH::BodyCreationSettings settings(
                        shape.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, PhysLayers::WORLD
                    );
                    bodies->push_back(bi.CreateAndAddBody(settings, JPH::EActivation::DontActivate));
                }
            }

            delete cell;
        }

        stats->bodies = (int)bodies->size();
        system->OptimizeBroadPhase();
    }

}

//=============================================================
// Inline models
//=============================================================

void Phys_ReadInlineModels(const void *bsp, long len, std::vector<physInlineModel_t> *out)
{
    dheader_t           header;
    const dplane_t     *planes;
    const dbrushside_t *brushSides;
    const dbrush_t     *brushes;
    const dmodel_t     *models;
    int                 numPlanes, numBrushSides, numBrushes, numModels;
    int                 i, b, j;

    out->clear();

    if (!bsp || len < (long)sizeof(dheader_t)) {
        return;
    }

    memcpy(&header, bsp, sizeof(header));
    for (i = 0; i < (int)(sizeof(dheader_t) / 4); i++) {
        ((int *)&header)[i] = LittleLong(((int *)&header)[i]);
    }

    planes     = (const dplane_t *)Phys_Lump(bsp, len, &header, LUMP_PLANES, sizeof(dplane_t), &numPlanes);
    brushSides = (const dbrushside_t *)Phys_Lump(bsp, len, &header, LUMP_BRUSHSIDES, sizeof(dbrushside_t), &numBrushSides);
    brushes    = (const dbrush_t *)Phys_Lump(bsp, len, &header, LUMP_BRUSHES, sizeof(dbrush_t), &numBrushes);
    models     = (const dmodel_t *)Phys_Lump(bsp, len, &header, LUMP_MODELS, sizeof(dmodel_t), &numModels);

    if (!planes || !brushSides || !brushes || !models) {
        return;
    }

    out->resize(numModels);

    for (i = 1; i < numModels; i++) {
        physInlineModel_t *m = &(*out)[i];

        ClearBounds(m->mins, m->maxs);

        for (b = models[i].firstBrush; b < models[i].firstBrush + models[i].numBrushes && b < numBrushes; b++) {
            static vec3_t corners[1024];
            int           sides[128];
            int           numSides = 0, numCorners;

            for (j = 0; j < brushes[b].numSides && numSides < (int)ARRAY_LEN(sides); j++) {
                const int side = brushes[b].firstSide + j;

                if (side >= 0 && side < numBrushSides && brushSides[side].planeNum >= 0 && brushSides[side].planeNum < numPlanes) {
                    sides[numSides++] = brushSides[side].planeNum;
                }
            }

            numCorners = Phys_BrushCorners(planes, sides, numSides, corners, ARRAY_LEN(corners));

            for (j = 0; j < numCorners; j++) {
                m->corners.push_back(corners[j][0]);
                m->corners.push_back(corners[j][1]);
                m->corners.push_back(corners[j][2]);
                AddPointToBounds(corners[j], m->mins, m->maxs);
            }
        }
    }
}
