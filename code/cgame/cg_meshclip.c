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
// Bits thrown by effects (a truck's glass coming down) landing on a model's
// own shape, not falling through it: the models' triangles, written offline
// (tools/vehicles/opel_glass.py) into opm/collide/*.txt:
//
//     tiki models/vehicles/opeltruck.tik     (a model that has this shape; any number)
//     tri ax ay az bx by bz cx cy cz         (in the model's units, before its scale)
//
// The snapshot's entities are tried, solid or not (a static vehicle's glass is
// a copy of it that is not solid, fgame g_glass.cpp), and only lines (no
// boxes), for the effects' collision (CG_ClipMoveToModelMeshes).

#include "cg_local.h"

#define MESH_MAX       16
#define MESH_TIKIS     8
#define MESH_CHUNK     32 // triangles to a box, for skipping most of them
#define MESH_CACHE     32

typedef struct {
    vec3_t mins, maxs;
    int    first, count;
} meshChunk_t;

typedef struct {
    char         tikis[MESH_TIKIS][MAX_QPATH];
    int          numTikis;
    float       *tris; // 9 a triangle
    int          numTris;
    meshChunk_t *chunks;
    int          numChunks;
    vec3_t       mins, maxs;
} collideMesh_t;

static collideMesh_t meshes[MESH_MAX];
static int           numMeshes = -1;

static struct {
    dtiki_t *tiki;
    int      mesh;
} meshCache[MESH_CACHE];
static int numMeshCache;

static int CompareTriX(const void *a, const void *b)
{
    const float *ta = (const float *)a, *tb = (const float *)b;
    const float  xa = ta[0] + ta[3] + ta[6], xb = tb[0] + tb[3] + tb[6];
    return xa < xb ? -1 : (xa > xb ? 1 : 0);
}

static void CG_LoadMesh(const char *path)
{
    collideMesh_t *m;
    char          *buf, *text, *tok;
    int            cap, i, k;

    if (numMeshes >= MESH_MAX || cgi.FS_ReadFile(path, (void **)&buf, qtrue) <= 0 || !buf) {
        return;
    }
    m = &meshes[numMeshes];
    memset(m, 0, sizeof(*m));
    cap     = 1024;
    m->tris = (float *)cgi.Malloc(cap * 9 * sizeof(float));

    text = buf;
    while (1) {
        tok = COM_Parse(&text);
        if (!tok[0]) {
            break;
        }
        if (!Q_stricmp(tok, "tiki")) {
            tok = COM_ParseExt(&text, qfalse);
            if (m->numTikis < MESH_TIKIS) {
                Q_strncpyz(m->tikis[m->numTikis++], tok, MAX_QPATH);
            }
        } else if (!Q_stricmp(tok, "tri")) {
            if (m->numTris == cap) {
                float *more = (float *)cgi.Malloc(cap * 2 * 9 * sizeof(float));
                memcpy(more, m->tris, cap * 9 * sizeof(float));
                cgi.Free(m->tris);
                m->tris = more;
                cap *= 2;
            }
            for (k = 0; k < 9; k++) {
                m->tris[m->numTris * 9 + k] = atof(COM_ParseExt(&text, qfalse));
            }
            m->numTris++;
        } else {
            SkipRestOfLine(&text);
        }
    }
    cgi.FS_FreeFile(buf);

    if (!m->numTris || !m->numTikis) {
        cgi.Free(m->tris);
        return;
    }

    // in chunks along x, each with its box
    qsort(m->tris, m->numTris, 9 * sizeof(float), CompareTriX);
    m->numChunks = (m->numTris + MESH_CHUNK - 1) / MESH_CHUNK;
    m->chunks    = (meshChunk_t *)cgi.Malloc(m->numChunks * sizeof(meshChunk_t));
    ClearBounds(m->mins, m->maxs);
    for (i = 0; i < m->numChunks; i++) {
        meshChunk_t *c = &m->chunks[i];
        c->first       = i * MESH_CHUNK;
        c->count       = (i == m->numChunks - 1) ? m->numTris - c->first : MESH_CHUNK;
        ClearBounds(c->mins, c->maxs);
        for (k = 0; k < c->count * 3; k++) {
            AddPointToBounds(&m->tris[c->first * 9 + k * 3], c->mins, c->maxs);
        }
        AddPointToBounds(c->mins, m->mins, m->maxs);
        AddPointToBounds(c->maxs, m->mins, m->maxs);
    }
    cgi.DPrintf("collide: %s, %d triangles\n", path, m->numTris);
    numMeshes++;
}

static void CG_LoadMeshes(void)
{
    char **list;
    int    num, i;

    numMeshes = 0;
    list      = cgi.FS_ListFilteredFiles("opm/collide", "txt", NULL, qfalse, &num, qfalse);
    for (i = 0; i < num; i++) {
        CG_LoadMesh(va("opm/collide/%s", list[i]));
    }
    cgi.FS_FreeFileList(list);
}

static int CG_MeshForTiki(dtiki_t *tiki)
{
    const char *name;
    int         i, k, found = -1;

    for (i = 0; i < numMeshCache; i++) {
        if (meshCache[i].tiki == tiki) {
            return meshCache[i].mesh;
        }
    }
    name = cgi.TIKI_Name(tiki);
    for (i = 0; i < numMeshes && found < 0; i++) {
        for (k = 0; k < meshes[i].numTikis; k++) {
            if (name && !Q_stricmp(name, meshes[i].tikis[k])) {
                found = i;
                break;
            }
        }
    }
    if (numMeshCache < MESH_CACHE) {
        meshCache[numMeshCache].tiki = tiki;
        meshCache[numMeshCache].mesh = found;
        numMeshCache++;
    }
    return found;
}

static centity_t *CG_MeshEntity(int i)
{
    return cg.snap && i < cg.snap->numEntities ? &cg_entities[cg.snap->entities[i].number] : NULL;
}

// the line from s along d (0..1) through the box?
static qboolean LineHitsBox(const vec3_t s, const vec3_t d, const vec3_t mins, const vec3_t maxs, float best)
{
    float t0 = 0, t1 = best;
    int   i;

    for (i = 0; i < 3; i++) {
        if (fabs(d[i]) < 1e-6f) {
            if (s[i] < mins[i] || s[i] > maxs[i]) {
                return qfalse;
            }
        } else {
            float a = (mins[i] - s[i]) / d[i], b = (maxs[i] - s[i]) / d[i];
            if (a > b) {
                float t = a;
                a       = b;
                b       = t;
            }
            if (a > t0) {
                t0 = a;
            }
            if (b < t1) {
                t1 = b;
            }
            if (t0 > t1) {
                return qfalse;
            }
        }
    }
    return qtrue;
}

/*
====================
CG_ClipMoveToModelMeshes

Added in OPM
  A line from start to end against the shapes of the snapshot's models
  that have one (opm/collide): tr takes the nearest hit, its plane facing back
  along the line.
====================
*/
void CG_ClipMoveToModelMeshes(const vec3_t start, const vec3_t end, trace_t *tr)
{
    int i, c, k;

    if (numMeshes < 0) {
        CG_LoadMeshes();
    }
    if (!numMeshes) {
        return;
    }

    for (i = 0; cg.snap && i < cg.snap->numEntities; i++) {
        centity_t     *cent = CG_MeshEntity(i);
        dtiki_t       *tiki;
        collideMesh_t *m;
        vec3_t         axis[3], rel, s, e, d, n;
        float          scale, best = tr->fraction;
        int            hit = -1;

        if (cent->currentState.modelindex <= 0 || cent->currentState.solid == SOLID_BMODEL) {
            continue;
        }
        tiki = cgi.R_Model_GetHandle(cgs.model_draw[cent->currentState.modelindex]);
        if (!tiki) {
            continue;
        }
        k = CG_MeshForTiki(tiki);
        if (k < 0) {
            continue;
        }
        m     = &meshes[k];
        scale = cent->currentState.scale * tiki->load_scale;
        if (scale <= 0) {
            continue;
        }

        // into the model's own space
        AnglesToAxis(cent->lerpAngles, axis);
        VectorSubtract(start, cent->lerpOrigin, rel);
        s[0] = DotProduct(rel, axis[0]) / scale;
        s[1] = DotProduct(rel, axis[1]) / scale;
        s[2] = DotProduct(rel, axis[2]) / scale;
        VectorSubtract(end, cent->lerpOrigin, rel);
        e[0] = DotProduct(rel, axis[0]) / scale;
        e[1] = DotProduct(rel, axis[1]) / scale;
        e[2] = DotProduct(rel, axis[2]) / scale;
        VectorSubtract(e, s, d);

        if (!LineHitsBox(s, d, m->mins, m->maxs, best)) {
            continue;
        }
        for (c = 0; c < m->numChunks; c++) {
            const meshChunk_t *ch = &m->chunks[c];
            if (!LineHitsBox(s, d, ch->mins, ch->maxs, best)) {
                continue;
            }
            for (k = ch->first; k < ch->first + ch->count; k++) {
                // Moller-Trumbore, both sides
                const float *t = &m->tris[k * 9];
                vec3_t       e1, e2, p, q, tv;
                float        det, inv, u, v, f;

                VectorSubtract(t + 3, t, e1);
                VectorSubtract(t + 6, t, e2);
                CrossProduct(d, e2, p);
                det = DotProduct(e1, p);
                if (fabs(det) < 1e-9f) {
                    continue;
                }
                inv = 1.0f / det;
                VectorSubtract(s, t, tv);
                u = DotProduct(tv, p) * inv;
                if (u < 0 || u > 1) {
                    continue;
                }
                CrossProduct(tv, e1, q);
                v = DotProduct(d, q) * inv;
                if (v < 0 || u + v > 1) {
                    continue;
                }
                f = DotProduct(e2, q) * inv;
                if (f > 0 && f < best) {
                    best = f;
                    hit  = k;
                }
            }
        }
        if (hit < 0) {
            continue;
        }

        {
            const float *t = &m->tris[hit * 9];
            vec3_t       e1, e2, ln;
            float        len = VectorLength(d) * scale;

            VectorSubtract(t + 3, t, e1);
            VectorSubtract(t + 6, t, e2);
            CrossProduct(e1, e2, ln);
            VectorNormalize(ln);
            if (DotProduct(ln, d) > 0) {
                VectorNegate(ln, ln);
            }
            // back to the world
            n[0] = ln[0] * axis[0][0] + ln[1] * axis[1][0] + ln[2] * axis[2][0];
            n[1] = ln[0] * axis[0][1] + ln[1] * axis[1][1] + ln[2] * axis[2][1];
            n[2] = ln[0] * axis[0][2] + ln[1] * axis[1][2] + ln[2] * axis[2][2];

            // a little short of the face, so the next step starts out of it
            if (len > 0) {
                best -= 0.25f / len;
                if (best < 0) {
                    best = 0;
                }
            }
            tr->fraction = best;
            for (k = 0; k < 3; k++) {
                tr->endpos[k] = start[k] + (end[k] - start[k]) * best;
            }
            VectorCopy(n, tr->plane.normal);
            tr->plane.dist = DotProduct(n, tr->endpos);
            tr->entityNum  = cent->currentState.number;
            tr->allsolid   = qfalse;
            tr->startsolid = qfalse;
        }
    }
}

/*
====================
CG_ClearModelMeshCache

Added in OPM
  The models of a map are loaded anew: their handles mean other models now.
====================
*/
void CG_ClearModelMeshCache(void)
{
    numMeshCache = 0;
}
