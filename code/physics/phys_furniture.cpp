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
// Finding furniture in the world's brushwork.
//
// Many tables, benches and crates were built from brushes in the world rather
// than placed as models, so nothing marks them out. They are found by shape:
//
//  - detail brushes that are solid and not clip, grouped by touching;
//  - a group small enough to be furniture (at most 128 units across and 96
//    high, and not a sliver);
//  - with something to see on it, and nothing on it textured as building
//    (wall, roof, floor, pipe, beam...);
//  - standing on something, and touching no other solid brush except below.
//    Trim, frames, sills and shelves all touch a wall and are left alone.
//
// The table legs that are only drawn (brushes the compiler did not keep for
// collision) are picked up from the surfaces inside the group's box, and are
// given a box of their own in the body.
//
// Rules that say a brush moves (physics.txt, "<map> furniture <brush> moves")
// take it out of the group it would join and group it only with the other
// brushes rules name that it touches; such a group is furniture whatever its
// shape. Touching detail brushes chain far: m3l1b's mine sign is nailed to a
// post that reaches the ceiling beams, and its group is 645 brushes, the whole
// bunker's woodwork. Naming the sign's and the post's brushes frees just them.
// The verdicts say which test turned a group down, and by which brush.

#include "phys_furniture.h"
#include "phys_world.h"

#include <climits>
#include <map>
#include <algorithm>

#define PF_MAX_WIDTH  128.0f
#define PF_MAX_HEIGHT 96.0f
#define PF_MIN_SIZE   8.0f
#define PF_MIN_VOLUME 4096.0f
#define PF_TOUCH      0.5f
#define PF_SUPPORT    1.0f

static const char *pf_building[] = {
    "wall",  "roof", "brick", "stone", "rock",    "concrete", "plaster", "floor", "chimney", "trim",
    "step",  "stair", "window", "door", "pipe",   "beam",     "joist",   "ceiling", "grand", NULL
};

typedef struct {
    int                num;
    int                shader;
    qboolean           visible;
    vec3_t             mins, maxs;
    std::vector<float> corners;
} pfBrush_t;

static const void *PF_Lump(const void *bsp, long len, dheader_t *header, int lumpNum, int elementSize, int *count)
{
    const lump_t *lump = Q_GetLumpByVersion(header, lumpNum);

    *count = 0;
    if (lump->fileofs < 0 || lump->filelen <= 0 || lump->fileofs + lump->filelen > len || elementSize <= 0) {
        return NULL;
    }

    *count = lump->filelen / elementSize;
    return (const byte *)bsp + lump->fileofs;
}

static qboolean PF_Touch(const vec3_t amins, const vec3_t amaxs, const vec3_t bmins, const vec3_t bmaxs, float gap)
{
    int k;

    for (k = 0; k < 3; k++) {
        if (amins[k] > bmaxs[k] + gap || bmins[k] > amaxs[k] + gap) {
            return qfalse;
        }
    }

    return qtrue;
}

static int PF_Find(std::vector<int>& parent, int i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i         = parent[i];
    }
    return i;
}

static qboolean PF_Building(const char *shader)
{
    char lower[64];
    int  i;

    Q_strncpyz(lower, shader, sizeof(lower));
    Q_strlwr(lower);

    for (i = 0; pf_building[i]; i++) {
        if (strstr(lower, pf_building[i])) {
            return qtrue;
        }
    }

    return qfalse;
}

// A box, padded to a unit at least each way.
static void PF_AddBoxHull(physFurniture_t *f, const vec3_t mins, const vec3_t maxs)
{
    std::vector<float> hull;
    vec3_t             lo, hi;
    int                i, k;

    for (k = 0; k < 3; k++) {
        const float mid = (mins[k] + maxs[k]) * 0.5f;

        lo[k] = Q_min(mins[k], mid - 0.5f);
        hi[k] = Q_max(maxs[k], mid + 0.5f);
    }

    for (i = 0; i < 8; i++) {
        hull.push_back((i & 1) ? hi[0] : lo[0]);
        hull.push_back((i & 2) ? hi[1] : lo[1]);
        hull.push_back((i & 4) ? hi[2] : lo[2]);
    }

    f->hulls.push_back(hull);
}

void Phys_FindFurniture(
    const void *bsp, long len, std::vector<physFurniture_t> *out, const std::set<int> *forced,
    std::vector<physFurnitureCheck_t> *checks, std::vector<physFurnitureBrush_t> *brushList
)
{
    dheader_t           header;
    const dshader_t    *shaders;
    const dplane_t     *planes;
    const dbrushside_t *brushSides;
    const dbrush_t     *brushes;
    const dmodel_t     *models;
    const dsurface_t   *surfaces;
    const drawVert_t   *verts;
    int                 numShaders, numPlanes, numBrushSides, numBrushes, numModels, numSurfaces, numVerts;
    std::vector<pfBrush_t> solids;
    std::vector<int>       candidates, parent;
    std::map<int, std::vector<int>> groups;
    int                    i, j, k;

    out->clear();
    if (checks) {
        checks->clear();
    }
    if (brushList) {
        brushList->clear();
    }

    if (!bsp || len < (long)sizeof(dheader_t)) {
        return;
    }

    memcpy(&header, bsp, sizeof(header));
    for (i = 0; i < (int)(sizeof(dheader_t) / 4); i++) {
        ((int *)&header)[i] = LittleLong(((int *)&header)[i]);
    }

    shaders    = (const dshader_t *)PF_Lump(bsp, len, &header, LUMP_SHADERS, sizeof(dshader_t), &numShaders);
    planes     = (const dplane_t *)PF_Lump(bsp, len, &header, LUMP_PLANES, sizeof(dplane_t), &numPlanes);
    brushSides = (const dbrushside_t *)PF_Lump(bsp, len, &header, LUMP_BRUSHSIDES, sizeof(dbrushside_t), &numBrushSides);
    brushes    = (const dbrush_t *)PF_Lump(bsp, len, &header, LUMP_BRUSHES, sizeof(dbrush_t), &numBrushes);
    models     = (const dmodel_t *)PF_Lump(bsp, len, &header, LUMP_MODELS, sizeof(dmodel_t), &numModels);
    surfaces   = (const dsurface_t *)PF_Lump(bsp, len, &header, LUMP_SURFACES, sizeof(dsurface_t), &numSurfaces);
    verts      = (const drawVert_t *)PF_Lump(bsp, len, &header, LUMP_DRAWVERTS, sizeof(drawVert_t), &numVerts);

    if (!shaders || !planes || !brushSides || !brushes || !models || !surfaces || !verts || numModels < 1) {
        return;
    }

    //
    // The world's solid brushes, other than clip.
    //
    for (i = models[0].firstBrush; i < models[0].firstBrush + models[0].numBrushes && i < numBrushes; i++) {
        static vec3_t corners[1024];
        int           sides[128];
        int           numSides = 0, numCorners;
        pfBrush_t     b;
        char          lower[64];

        if (brushes[i].shaderNum < 0 || brushes[i].shaderNum >= numShaders
            || !(shaders[brushes[i].shaderNum].contentFlags & CONTENTS_SOLID)) {
            continue;
        }

        Q_strncpyz(lower, shaders[brushes[i].shaderNum].shader, sizeof(lower));
        Q_strlwr(lower);
        if (strstr(lower, "clip")) {
            continue;
        }

        for (j = 0; j < brushes[i].numSides && numSides < (int)ARRAY_LEN(sides); j++) {
            const int side = brushes[i].firstSide + j;

            if (side >= 0 && side < numBrushSides && brushSides[side].planeNum >= 0 && brushSides[side].planeNum < numPlanes) {
                sides[numSides++] = brushSides[side].planeNum;
            }
        }

        numCorners = Phys_BrushCorners(planes, sides, numSides, corners, ARRAY_LEN(corners));
        if (numCorners < 4) {
            continue;
        }

        b.num     = i;
        b.shader  = brushes[i].shaderNum;
        b.visible = Q_stricmpn(lower, "textures/common/", 16) ? qtrue : qfalse;
        ClearBounds(b.mins, b.maxs);
        for (j = 0; j < numCorners; j++) {
            AddPointToBounds(corners[j], b.mins, b.maxs);
            b.corners.push_back(corners[j][0]);
            b.corners.push_back(corners[j][1]);
            b.corners.push_back(corners[j][2]);
        }

        if (shaders[b.shader].contentFlags & CONTENTS_DETAIL) {
            candidates.push_back((int)solids.size());
        }
        solids.push_back(b);
    }

    //
    // Detail brushes grouped by touching.
    //
    parent.resize(candidates.size());
    for (i = 0; i < (int)candidates.size(); i++) {
        parent[i] = i;
    }

    for (i = 0; i < (int)candidates.size(); i++) {
        const pfBrush_t *a = &solids[candidates[i]];

        for (j = i + 1; j < (int)candidates.size(); j++) {
            const pfBrush_t *b = &solids[candidates[j]];

            // A forced brush groups only with forced brushes.
            if (forced && (forced->count(a->num) != 0) != (forced->count(b->num) != 0)) {
                continue;
            }
            if (PF_Touch(a->mins, a->maxs, b->mins, b->maxs, PF_TOUCH)) {
                parent[PF_Find(parent, i)] = PF_Find(parent, j);
            }
        }
    }

    for (i = 0; i < (int)candidates.size(); i++) {
        groups[PF_Find(parent, i)].push_back(candidates[i]);
    }

    //
    // Which groups are furniture.
    //
    for (std::map<int, std::vector<int>>::const_iterator it = groups.begin(); it != groups.end(); ++it) {
        const std::vector<int>& members = it->second;
        physFurniture_t         f;
        physFurnitureCheck_t    check;
        vec3_t                  size;
        qboolean                visible = qfalse, support = qfalse;
        int                     building = -1, blocker = -1, blockerShader = -1, firstBrush = INT_MAX;
        std::map<int, int>      shaderUse;
        std::vector<int>        loose;

        f.forced = false;
        ClearBounds(f.mins, f.maxs);
        for (j = 0; j < (int)members.size(); j++) {
            const pfBrush_t *b = &solids[members[j]];

            AddPointToBounds(b->mins, f.mins, f.maxs);
            AddPointToBounds(b->maxs, f.mins, f.maxs);
            firstBrush = Q_min(firstBrush, b->num);
            if (forced && forced->count(b->num)) {
                f.forced = true;
            }

            if (b->visible) {
                visible = qtrue;
                shaderUse[b->shader]++;
                if (building < 0 && PF_Building(shaders[b->shader].shader)) {
                    building = b->shader;
                }
            }
        }

        // Nothing to see: clip, caulk and the like, never furniture and not
        // worth a verdict.
        if (!visible) {
            continue;
        }

        if (brushList) {
            for (j = 0; j < (int)members.size(); j++) {
                const pfBrush_t     *b = &solids[members[j]];
                physFurnitureBrush_t e;

                if (!b->visible) {
                    continue;
                }
                e.num   = b->num;
                e.group = firstBrush;
                VectorCopy(b->mins, e.mins);
                VectorCopy(b->maxs, e.maxs);
                Q_strncpyz(e.shader, shaders[b->shader].shader, sizeof(e.shader));
                brushList->push_back(e);
            }
        }

        check.firstBrush = firstBrush;
        check.numBrushes = (int)members.size();
        check.other      = -1;
        check.furniture  = false;
        check.verdict[0] = 0;
        VectorCopy(f.mins, check.mins);
        VectorCopy(f.maxs, check.maxs);

        VectorSubtract(f.maxs, f.mins, size);
        if (f.forced) {
            // Whatever its shape.
        } else if (building >= 0) {
            Com_sprintf(check.verdict, sizeof(check.verdict), "textured as building (%s)", shaders[building].shader);
        } else if (size[0] > PF_MAX_WIDTH || size[1] > PF_MAX_WIDTH) {
            Com_sprintf(check.verdict, sizeof(check.verdict), "too wide: %.0f x %.0f (at most %.0f)", size[0], size[1], PF_MAX_WIDTH);
        } else if (size[2] > PF_MAX_HEIGHT) {
            Com_sprintf(check.verdict, sizeof(check.verdict), "too tall: %.0f (at most %.0f)", size[2], PF_MAX_HEIGHT);
        } else if (size[0] < PF_MIN_SIZE || size[1] < PF_MIN_SIZE || size[2] < PF_MIN_SIZE
                   || size[0] * size[1] * size[2] < PF_MIN_VOLUME) {
            Com_sprintf(check.verdict, sizeof(check.verdict), "too small: %.0f x %.0f x %.0f", size[0], size[1], size[2]);
        }

        // Standing on something, touching nothing else.
        if (!f.forced && !check.verdict[0]) {
            for (j = 0; j < (int)solids.size() && blocker < 0; j++) {
                const pfBrush_t *o = &solids[j];

                if (!PF_Touch(f.mins, f.maxs, o->mins, o->maxs, PF_SUPPORT)) {
                    continue;
                }

                if (std::find(members.begin(), members.end(), j) != members.end()) {
                    continue;
                }

                if (o->maxs[2] <= f.mins[2] + PF_SUPPORT) {
                    support = qtrue;
                } else {
                    blocker       = o->num;
                    blockerShader = o->shader;
                }
            }

            if (blocker >= 0) {
                check.other = blocker;
                Com_sprintf(check.verdict, sizeof(check.verdict), "touches brush %d (%s) above its foot", blocker,
                    shaders[blockerShader].shader);
            } else if (!support) {
                Com_sprintf(check.verdict, sizeof(check.verdict), "stands on nothing");
            }
        }

        if (check.verdict[0]) {
            if (checks) {
                checks->push_back(check);
            }
            continue;
        }

        //
        // What draws it: the world's surfaces inside its box, other than
        // what lies flat at its foot (the floor).
        //
        for (i = models[0].firstSurface; i < models[0].firstSurface + models[0].numSurfaces && i < numSurfaces; i++) {
            const dsurface_t *s = &surfaces[i];
            vec3_t            smins, smaxs;
            qboolean          inside = qtrue;

            if (s->surfaceType != MST_PLANAR && s->surfaceType != MST_PATCH && s->surfaceType != MST_TRIANGLE_SOUP) {
                continue;
            }
            if (s->firstVert < 0 || s->numVerts <= 0 || s->firstVert + s->numVerts > numVerts) {
                continue;
            }

            ClearBounds(smins, smaxs);
            for (j = 0; j < s->numVerts; j++) {
                AddPointToBounds(verts[s->firstVert + j].xyz, smins, smaxs);
            }

            for (k = 0; k < 3 && inside; k++) {
                if (smins[k] < f.mins[k] - PF_SUPPORT || smaxs[k] > f.maxs[k] + PF_SUPPORT) {
                    inside = qfalse;
                }
            }
            if (!inside || smaxs[2] <= f.mins[2] + PF_TOUCH) {
                continue;
            }

            f.surfaces.push_back(i);

            // Drawn but not inside any brush: a piece the compiler kept no
            // collision for.
            {
                qboolean covered = qfalse;

                for (j = 0; j < (int)members.size() && !covered; j++) {
                    const pfBrush_t *b = &solids[members[j]];

                    covered = qtrue;
                    for (k = 0; k < 3; k++) {
                        if (smins[k] < b->mins[k] - PF_TOUCH || smaxs[k] > b->maxs[k] + PF_TOUCH) {
                            covered = qfalse;
                            break;
                        }
                    }
                }

                if (!covered) {
                    loose.push_back(i);
                }
            }
        }

        if (f.surfaces.empty()) {
            if (checks) {
                Com_sprintf(check.verdict, sizeof(check.verdict), "nothing draws it");
                checks->push_back(check);
            }
            continue;
        }

        //
        // Its shape: every brush, and a box round each group of loose surfaces.
        //
        for (j = 0; j < (int)members.size(); j++) {
            f.brushes.push_back(solids[members[j]].num);
            f.hulls.push_back(solids[members[j]].corners);
        }

        {
            std::vector<int>    looseParent(loose.size());
            std::vector<float>  bounds(loose.size() * 6);
            std::map<int, int>  boxOf;
            std::vector<float>  boxes;

            for (i = 0; i < (int)loose.size(); i++) {
                const dsurface_t *s = &surfaces[loose[i]];
                vec3_t            smins, smaxs;

                ClearBounds(smins, smaxs);
                for (j = 0; j < s->numVerts; j++) {
                    AddPointToBounds(verts[s->firstVert + j].xyz, smins, smaxs);
                }
                for (k = 0; k < 3; k++) {
                    bounds[i * 6 + k]     = smins[k];
                    bounds[i * 6 + 3 + k] = smaxs[k];
                }
                looseParent[i] = i;
            }

            for (i = 0; i < (int)loose.size(); i++) {
                for (j = i + 1; j < (int)loose.size(); j++) {
                    if (PF_Touch(&bounds[i * 6], &bounds[i * 6 + 3], &bounds[j * 6], &bounds[j * 6 + 3], PF_TOUCH)) {
                        looseParent[PF_Find(looseParent, i)] = PF_Find(looseParent, j);
                    }
                }
            }

            for (i = 0; i < (int)loose.size(); i++) {
                const int root = PF_Find(looseParent, i);

                if (boxOf.find(root) == boxOf.end()) {
                    boxOf[root] = (int)boxes.size();
                    for (k = 0; k < 3; k++) {
                        boxes.push_back(bounds[i * 6 + k]);
                    }
                    for (k = 0; k < 3; k++) {
                        boxes.push_back(bounds[i * 6 + 3 + k]);
                    }
                } else {
                    float *box = &boxes[boxOf[root]];

                    for (k = 0; k < 3; k++) {
                        box[k]     = Q_min(box[k], bounds[i * 6 + k]);
                        box[3 + k] = Q_max(box[3 + k], bounds[i * 6 + 3 + k]);
                    }
                }
            }

            for (i = 0; i + 5 < (int)boxes.size(); i += 6) {
                PF_AddBoxHull(&f, &boxes[i], &boxes[i + 3]);
            }
        }

        {
            int best = -1, bestUse = 0;

            for (std::map<int, int>::const_iterator s = shaderUse.begin(); s != shaderUse.end(); ++s) {
                if (s->second > bestUse) {
                    best    = s->first;
                    bestUse = s->second;
                }
            }

            Q_strncpyz(f.shader, best >= 0 ? shaders[best].shader : "", sizeof(f.shader));
        }

        if (checks) {
            check.furniture = true;
            Com_sprintf(check.verdict, sizeof(check.verdict), f.forced ? "furniture: a rule lets it in" : "furniture");
            checks->push_back(check);
        }
        out->push_back(f);
    }
}
