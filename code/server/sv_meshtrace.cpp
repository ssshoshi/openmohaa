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
// Bullets against a model's real triangles. A solid entity with a TIKI model
// blocks traces with its box, which is bigger than the model and has no holes:
// a car stops a bullet a foot from its bumper and through its open windows.
// In single player a bullet trace (traceDeep) that hits the box of such an
// entity is tried again here against the model's mesh, posed as the game
// poses it, and goes on through if it misses. Characters keep their hit
// location spheres (SV_TraceDeep).

#include "server.h"
#include "../corepp/tiki.h"
#include "../tiki/tiki_skel.h"

#include <vector>

typedef struct {
    int                 serverId;
    dtiki_t            *tiki;
    vec3_t              origin, angles;
    float               scale;
    frameInfo_t         frameInfo[MAX_FRAMEINFOS];
    byte                surfaces[32];
    std::vector<float>  xyz;  // 3 a vertex, world space
    std::vector<int>    tris; // 3 a triangle
    qboolean            valid; // has a mesh
} meshTraceCache_t;

static meshTraceCache_t *sv_meshCache[MAX_GENTITIES];
static cvar_t           *sv_meshtrace;
static cvar_t           *sv_meshtrace_gametype;

// Whether ent's cached mesh is still how it stands.
static qboolean SV_MeshCacheCurrent(const meshTraceCache_t *c, const gentity_t *ent)
{
    return c->serverId == sv.serverId && c->tiki == ent->tiki && VectorCompare(c->origin, ent->s.origin)
            && VectorCompare(c->angles, ent->r.currentAngles) && c->scale == ent->s.scale
            && !memcmp(c->frameInfo, ent->s.frameInfo, sizeof(c->frameInfo))
            && !memcmp(c->surfaces, ent->s.surfaces, sizeof(c->surfaces))
        ? qtrue
        : qfalse;
}

// Skins the entity's model the way the renderer does (RE_GetSkinnedMesh), from
// the bones the game poses (TIKI_Orientation), out to world space.
static void SV_MeshBuild(meshTraceCache_t *c, gentity_t *ent)
{
    dtiki_t                   *tiki = ent->tiki;
    const int                  numBones = tiki->m_boneList.NumChannels();
    std::vector<orientation_t> bones(numBones);
    std::vector<char>          posed(numBones, 0);
    const float                scale = ent->s.scale * tiki->load_scale;
    const byte                *bsurf = ent->s.surfaces;
    vec3_t                     axis[3];
    int                        numVerts = 0;
    int                        surfIndex = 0;

    c->serverId = sv.serverId;
    c->tiki     = tiki;
    VectorCopy(ent->s.origin, c->origin);
    VectorCopy(ent->r.currentAngles, c->angles);
    c->scale = ent->s.scale;
    memcpy(c->frameInfo, ent->s.frameInfo, sizeof(c->frameInfo));
    memcpy(c->surfaces, ent->s.surfaces, sizeof(c->surfaces));
    c->xyz.clear();
    c->tris.clear();
    c->valid = qfalse;

    AnglesToAxis(ent->r.currentAngles, axis);

    for (int mesh = 0; mesh < tiki->numMeshes; mesh++) {
        skelHeaderGame_t  *skel = TIKI_GetSkel(tiki->mesh[mesh]);
        skelSurfaceGame_t *surface;

        if (!skel) {
            return;
        }

        surface = skel->pSurfaces;
        for (int s = 0; s < skel->numSurfaces; s++, surface = surface->pNext, surfIndex++) {
            skeletorVertex_t *vert;

            // hidden, as the renderer treats it
            if (surfIndex < 32 && (bsurf[surfIndex] & 4)) {
                continue;
            }

            for (int i = 0; i < surface->numTriangles * 3; i++) {
                c->tris.push_back(numVerts + surface->pTriangles[i]);
            }

            vert = surface->pVerts;
            for (int i = 0; i < surface->numVerts; i++) {
                skelWeight_t *weight = (skelWeight_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                                        + sizeof(skeletorMorph_t) * vert->numMorphs);
                vec3_t        local;

                VectorClear(local);
                for (int k = 0; k < vert->numWeights; k++, weight++) {
                    int boneNum = mesh > 0 ? tiki->m_boneList.LocalChannel(skel->pBones[weight->boneIndex].channel)
                                           : weight->boneIndex;
                    const orientation_t *o;

                    if (boneNum < 0 || boneNum >= numBones) {
                        continue;
                    }
                    if (!posed[boneNum]) {
                        bones[boneNum]  = ge->TIKI_Orientation(ent, boneNum);
                        posed[boneNum] = 1;
                    }
                    o = &bones[boneNum];

                    // The bone's place (scaled, with load_origin) plus the offset along its axes.
                    for (int j = 0; j < 3; j++) {
                        local[j] += (o->origin[j]
                                     + scale
                                           * (weight->offset[0] * o->axis[0][j] + weight->offset[1] * o->axis[1][j]
                                              + weight->offset[2] * o->axis[2][j]))
                                  * weight->boneWeight;
                    }
                }

                for (int j = 0; j < 3; j++) {
                    c->xyz.push_back(
                        ent->s.origin[j] + local[0] * axis[0][j] + local[1] * axis[1][j] + local[2] * axis[2][j]
                    );
                }

                vert = (skeletorVertex_t *)((byte *)vert + sizeof(skeletorVertex_t)
                                            + sizeof(skeletorMorph_t) * vert->numMorphs
                                            + sizeof(skelWeight_t) * vert->numWeights);
            }
            numVerts += surface->numVerts;
        }
    }

    c->valid = !c->tris.empty() ? qtrue : qfalse;
}

// The nearest triangle the segment start->end crosses, both faces: its
// fraction along the segment and its normal facing start. qfalse if none.
static qboolean SV_MeshRay(const meshTraceCache_t *c, const vec3_t start, const vec3_t end, float *frac, vec3_t normal)
{
    vec3_t dir;
    float  best = 2.0f;

    VectorSubtract(end, start, dir);

    for (size_t t = 0; t + 2 < c->tris.size(); t += 3) {
        const float *v0 = &c->xyz[c->tris[t] * 3];
        const float *v1 = &c->xyz[c->tris[t + 1] * 3];
        const float *v2 = &c->xyz[c->tris[t + 2] * 3];
        vec3_t       e1, e2, p, q, s;
        float        det, inv, u, v, f;

        VectorSubtract(v1, v0, e1);
        VectorSubtract(v2, v0, e2);
        CrossProduct(dir, e2, p);
        det = DotProduct(e1, p);
        if (fabs(det) < 1e-8f) {
            continue;
        }
        inv = 1.0f / det;
        VectorSubtract(start, v0, s);
        u = DotProduct(s, p) * inv;
        if (u < 0 || u > 1) {
            continue;
        }
        CrossProduct(s, e1, q);
        v = DotProduct(dir, q) * inv;
        if (v < 0 || u + v > 1) {
            continue;
        }
        f = DotProduct(e2, q) * inv;
        if (f < 0 || f > 1 || f >= best) {
            continue;
        }

        best = f;
        CrossProduct(e1, e2, normal);
        VectorNormalize(normal);
        if (DotProduct(normal, dir) > 0) {
            VectorNegate(normal, normal);
        }
    }

    if (best > 1.0f) {
        return qfalse;
    }
    *frac = best;
    return qtrue;
}

/*
==================
SV_MeshTraceWanted

Whether a trace that hit touch's box should be tried against its mesh.
==================
*/
qboolean SV_MeshTraceWanted(const gentity_t *touch, const vec3_t mins, const vec3_t maxs, qboolean traceDeep)
{
    if (!sv_meshtrace) {
        sv_meshtrace          = Cvar_Get("sv_meshtrace", "1", 0);
        sv_meshtrace_gametype = Cvar_Get("g_gametype", "0", 0);
    }
    if (!traceDeep || !sv_meshtrace->integer || sv_meshtrace_gametype->integer != 0) {
        return qfalse; // bullets, in single player
    }
    if (!touch->tiki || touch->tiki->a->bIsCharacter || !touch->tiki->numMeshes) {
        return qfalse;
    }
    if (!VectorCompare(mins, vec3_origin) || !VectorCompare(maxs, vec3_origin)) {
        return qfalse; // a point, as bullets are
    }
    return qtrue;
}

/*
==================
SV_MeshTrace

The box trace hit touch; retraces start->end against its mesh. Leaves the
trace as a miss when the mesh isn't hit.
==================
*/
void SV_MeshTrace(trace_t *trace, const vec3_t start, const vec3_t end, gentity_t *touch)
{
    const int         num = touch->s.number;
    meshTraceCache_t *c;
    float             frac;
    vec3_t            normal;

    if (num < 0 || num >= MAX_GENTITIES) {
        return;
    }
    if (!sv_meshCache[num]) {
        sv_meshCache[num] = new meshTraceCache_t();
    }
    c = sv_meshCache[num];
    if (!SV_MeshCacheCurrent(c, touch)) {
        SV_MeshBuild(c, touch);
        if (sv_meshtrace->integer > 1 && c->valid) {
            vec3_t lo = {99999, 99999, 99999}, hi = {-99999, -99999, -99999};

            for (size_t i = 0; i < c->xyz.size(); i += 3) {
                for (int j = 0; j < 3; j++) {
                    lo[j] = Q_min(lo[j], c->xyz[i + j] - touch->s.origin[j]);
                    hi[j] = Q_max(hi[j], c->xyz[i + j] - touch->s.origin[j]);
                }
            }
            Com_Printf(
                "meshtrace: #%d mesh (%.0f %.0f %.0f) (%.0f %.0f %.0f), box (%.0f %.0f %.0f) (%.0f %.0f %.0f)\n",
                num, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                touch->r.mins[0], touch->r.mins[1], touch->r.mins[2], touch->r.maxs[0], touch->r.maxs[1], touch->r.maxs[2]
            );
        }
    }
    if (!c->valid) {
        return; // no mesh to go by: the box stands
    }

    if (sv_meshtrace->integer > 1) {
        const qboolean hit = SV_MeshRay(c, start, end, &frac, normal);
        Com_Printf(
            "meshtrace: #%d box %.3f -> %s %.3f (%d tris)\n",
            num,
            trace->fraction,
            hit ? "hit" : "miss",
            hit ? frac : 1.0f,
            (int)(c->tris.size() / 3)
        );
    }

    if (!SV_MeshRay(c, start, end, &frac, normal)) {
        trace->fraction   = 1.0f;
        trace->startsolid = qfalse;
        trace->allsolid   = qfalse;
        VectorCopy(end, trace->endpos);
        return;
    }

    trace->fraction   = frac;
    trace->startsolid = qfalse;
    trace->allsolid   = qfalse;
    VectorCopy(normal, trace->plane.normal);
    for (int j = 0; j < 3; j++) {
        trace->endpos[j] = start[j] + (end[j] - start[j]) * frac;
    }
    trace->plane.dist = DotProduct(trace->endpos, normal);
}
