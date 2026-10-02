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

// cg_marks_models.cpp -- decals on TIKI models
//
// Added in OPM. The renderer only projects decals (bullet holes, blast marks)
// onto the world's and brush models' surfaces, so a crate, a car or a cart had
// none. These are clipped here against the model's own triangles, taken from
// the renderer's skinned mesh (R_GetSkinnedMesh), for:
//
//  - the map's static models that are props (cg_props.cpp): their fragments are
//    kept in the prop's frame and follow it when the physics knocks it about;
//  - solid server entities with a rigid TIKI model (a script_model car): kept in
//    the entity's frame, like the fragments on a brush model.
//
// Only rigid models get them: a mesh carried by one or two bones. On a soldier
// a decal in his first frame's pose would not stay on him.

#include "cg_local.h"
#include "cg_props.h"

#include <map>
#include <vector>

#define MODELMARK_MAX_VERTS 8192
#define MODELMARK_MAX_TRIS  12288
#define MODELMARK_OFFSET    0.5f // off the surface, against z-fighting
#define MODELMARK_BEHIND    48.f // how far behind the hit point: a prop is hit on
                                 // its clip brush, which stands a little off it

typedef struct {
    std::vector<float> xyz;   // 3 a vertex, in the model's own frame (scaled)
    std::vector<int>   tris;  // 3 a triangle
    std::vector<float> plane; // 4 a triangle: outward normal, distance
    vec3_t             mins, maxs;
    float              radius;
    qboolean           rigid;
} modelMarkMesh_t;

static std::map<std::pair<qhandle_t, int>, modelMarkMesh_t> cg_markMeshes;
static char                                                  cg_markMeshesMap[MAX_QPATH];

static skinnedVert_t cg_markVerts[MODELMARK_MAX_VERTS];
static int           cg_markTris[MODELMARK_MAX_TRIS * 3];

// The mesh of a model at a scale, posed as the renderer poses a static model
// (the first frame of its first animation), at the origin. NULL if the renderer
// cannot skin it.
static const modelMarkMesh_t *CG_ModelMarkMesh(qhandle_t hModel, float scale)
{
    std::pair<qhandle_t, int> key(hModel, (int)(scale * 1000.f));
    refEntity_t               ent;
    dtiki_t                  *tiki;
    int                       numVerts, numTris, i, k;
    int                       bones[2] = {-1, -1};
    vec3_t                    centre;
    float                     outward;

    if (Q_stricmp(cg_markMeshesMap, cgs.mapname)) {
        cg_markMeshes.clear();
        Q_strncpyz(cg_markMeshesMap, cgs.mapname, sizeof(cg_markMeshesMap));
    }

    std::map<std::pair<qhandle_t, int>, modelMarkMesh_t>::iterator it = cg_markMeshes.find(key);
    if (it != cg_markMeshes.end()) {
        return it->second.xyz.empty() ? NULL : &it->second;
    }

    modelMarkMesh_t &mesh = cg_markMeshes[key];

    if (cgi.apiversion < 4 || !cgi.R_GetSkinnedMesh || !hModel) {
        return NULL;
    }

    tiki = cgi.R_Model_GetHandle(hModel);
    if (!tiki) {
        return NULL;
    }

    memset(&ent, 0, sizeof(ent));
    ent.reType              = RT_MODEL;
    ent.hModel              = hModel;
    ent.tiki                = tiki;
    ent.scale               = scale;
    ent.entityNumber        = ENTITYNUM_NONE;
    ent.frameInfo[0].index  = 0;
    ent.frameInfo[0].time   = 0.0f;
    ent.frameInfo[0].weight = 1.0f;
    ent.actionWeight        = 1.0f;
    AxisClear(ent.axis);

    numTris  = 0;
    numVerts = cgi.R_GetSkinnedMesh(&ent, cg_markVerts, MODELMARK_MAX_VERTS, cg_markTris, MODELMARK_MAX_TRIS, &numTris);
    if (numVerts <= 0 || numTris <= 0) {
        return NULL;
    }

    mesh.rigid = qtrue;
    ClearBounds(mesh.mins, mesh.maxs);
    mesh.xyz.resize(numVerts * 3);
    for (i = 0; i < numVerts; i++) {
        VectorCopy(cg_markVerts[i].xyz, &mesh.xyz[i * 3]);
        AddPointToBounds(cg_markVerts[i].xyz, mesh.mins, mesh.maxs);

        if (cg_markVerts[i].bone != bones[0] && cg_markVerts[i].bone != bones[1]) {
            if (bones[0] < 0) {
                bones[0] = cg_markVerts[i].bone;
            } else if (bones[1] < 0) {
                bones[1] = cg_markVerts[i].bone;
            } else {
                mesh.rigid = qfalse;
            }
        }
    }
    mesh.radius = Q_max(VectorLength(mesh.mins), VectorLength(mesh.maxs));
    mesh.tris.assign(cg_markTris, cg_markTris + numTris * 3);

    // Which way the triangles face: the winding that points most of them away
    // from the middle of the model is the outside.
    VectorAdd(mesh.mins, mesh.maxs, centre);
    VectorScale(centre, 0.5f, centre);
    outward = 0;
    mesh.plane.resize(numTris * 4);
    for (i = 0; i < numTris; i++) {
        const float *a = &mesh.xyz[mesh.tris[i * 3] * 3];
        const float *b = &mesh.xyz[mesh.tris[i * 3 + 1] * 3];
        const float *c = &mesh.xyz[mesh.tris[i * 3 + 2] * 3];
        vec3_t       e1, e2, n, mid;

        VectorSubtract(b, a, e1);
        VectorSubtract(c, a, e2);
        CrossProduct(e1, e2, n);
        for (k = 0; k < 3; k++) {
            mid[k] = (a[k] + b[k] + c[k]) / 3.f - centre[k];
        }
        outward += DotProduct(n, mid);
        VectorNormalize(n);
        VectorCopy(n, &mesh.plane[i * 4]);
    }
    for (i = 0; i < numTris; i++) {
        float *n = &mesh.plane[i * 4];

        if (outward < 0) {
            VectorInverse(n);
        }
        n[3] = DotProduct(n, &mesh.xyz[mesh.tris[i * 3] * 3]);
    }

    return &mesh;
}

// Clips poly against the plane, keeping what is in front of it.
static int CG_ModelMarkChop(int numIn, vec3_t *in, vec3_t *out, const vec3_t normal, float dist)
{
    int   i, numOut = 0;
    float d0, d1;

    for (i = 0; i < numIn; i++) {
        const float *p = in[i];
        const float *q = in[(i + 1) % numIn];

        d0 = DotProduct(p, normal) - dist;
        d1 = DotProduct(q, normal) - dist;

        if (d0 >= 0) {
            VectorCopy(p, out[numOut]);
            numOut++;
        }
        if ((d0 >= 0) != (d1 >= 0) && numOut < MAX_VERTS_ON_POLY * 2 - 1) {
            float f = d0 / (d0 - d1);
            out[numOut][0] = p[0] + f * (q[0] - p[0]);
            out[numOut][1] = p[1] + f * (q[1] - p[1]);
            out[numOut][2] = p[2] + f * (q[2] - p[2]);
            numOut++;
        }
    }

    return numOut;
}

// Clips a decal (numPoints world points, projected along projection) against a
// mesh placed at origin with axis, and adds the fragments, in the mesh's frame,
// to the buffers.
static void CG_ModelMarkFragments(
    const modelMarkMesh_t *mesh,
    const vec3_t           origin,
    const vec3_t           axis[3],
    int                    iIndex,
    int                    numPoints,
    const vec3_t          *points,
    const vec3_t           projection,
    int                   *numOutPoints,
    int                    maxPoints,
    vec3_t                *pointBuffer,
    int                   *numOutFragments,
    int                    maxFragments,
    markFragment_t        *fragmentBuffer
)
{
    vec3_t localPoints[MAX_VERTS_ON_POLY];
    vec3_t localProj, projDir, v1, v2, tmp;
    vec3_t normals[MAX_VERTS_ON_POLY + 2];
    float  dists[MAX_VERTS_ON_POLY + 2];
    vec3_t mins, maxs;
    int    numPlanes, i, j, k, t;

    if (numPoints > MAX_VERTS_ON_POLY) {
        numPoints = MAX_VERTS_ON_POLY;
    }

    for (i = 0; i < numPoints; i++) {
        VectorSubtract(points[i], origin, tmp);
        for (k = 0; k < 3; k++) {
            localPoints[i][k] = DotProduct(tmp, axis[k]);
        }
    }
    for (k = 0; k < 3; k++) {
        localProj[k] = DotProduct(projection, axis[k]);
    }
    VectorNormalize2(localProj, projDir);

    ClearBounds(mins, maxs);
    for (i = 0; i < numPoints; i++) {
        AddPointToBounds(localPoints[i], mins, maxs);
        VectorMA(localPoints[i], MODELMARK_BEHIND, projDir, tmp);
        AddPointToBounds(tmp, mins, maxs);
        VectorMA(localPoints[i], -32.f, projDir, tmp);
        AddPointToBounds(tmp, mins, maxs);
    }

    // The sides of the projected polygon, and its near and far ends.
    for (i = 0; i < numPoints; i++) {
        VectorSubtract(localPoints[(i + 1) % numPoints], localPoints[i], v1);
        VectorAdd(localPoints[i], localProj, v2);
        VectorSubtract(localPoints[i], v2, v2);
        CrossProduct(v1, v2, normals[i]);
        VectorNormalize(normals[i]);
        dists[i] = DotProduct(normals[i], localPoints[i]);
    }
    VectorCopy(projDir, normals[numPoints]);
    dists[numPoints] = DotProduct(projDir, localPoints[0]) - 32.f;
    VectorNegate(projDir, normals[numPoints + 1]);
    dists[numPoints + 1] = -DotProduct(projDir, localPoints[0]) - MODELMARK_BEHIND;
    numPlanes            = numPoints + 2;

    for (t = 0; t < (int)mesh->tris.size() / 3; t++) {
        const float *n = &mesh->plane[t * 4];
        vec3_t       clip[2][MAX_VERTS_ON_POLY * 2];
        int          numClip, which;
        vec3_t       tmins, tmaxs;

        // only what faces the shot
        if (DotProduct(n, projDir) > -0.1f) {
            continue;
        }

        ClearBounds(tmins, tmaxs);
        for (j = 0; j < 3; j++) {
            const float *v = &mesh->xyz[mesh->tris[t * 3 + j] * 3];
            VectorMA(v, MODELMARK_OFFSET, n, clip[0][j]);
            AddPointToBounds(clip[0][j], tmins, tmaxs);
        }
        if (tmins[0] > maxs[0] || tmaxs[0] < mins[0] || tmins[1] > maxs[1] || tmaxs[1] < mins[1]
            || tmins[2] > maxs[2] || tmaxs[2] < mins[2]) {
            continue;
        }

        numClip = 3;
        which   = 0;
        for (i = 0; i < numPlanes && numClip; i++) {
            numClip = CG_ModelMarkChop(numClip, clip[which], clip[!which], normals[i], dists[i]);
            which   = !which;
        }
        if (numClip < 3) {
            continue;
        }
        if (numClip > MAX_VERTS_ON_POLY) {
            numClip = MAX_VERTS_ON_POLY;
        }
        if (*numOutPoints + numClip > maxPoints || *numOutFragments >= maxFragments) {
            return;
        }

        markFragment_t *mf = &fragmentBuffer[*numOutFragments];
        mf->firstPoint     = *numOutPoints;
        mf->numPoints      = numClip;
        mf->iIndex         = iIndex;
        for (i = 0; i < numClip; i++) {
            VectorCopy(clip[which][i], pointBuffer[*numOutPoints + i]);
        }
        *numOutPoints += numClip;
        (*numOutFragments)++;
    }
}

// The model of a solid server entity that takes decals, or 0.
static const modelMarkMesh_t *CG_EntityMarkMesh(centity_t *cent)
{
    const modelMarkMesh_t *mesh;
    qhandle_t              hModel;

    if (!cent->currentValid || cent->currentState.eType != ET_MODELANIM || cent->currentState.solid == SOLID_BMODEL
        || !cent->currentState.solid) {
        return NULL;
    }

    hModel = cgs.model_draw[cent->currentState.modelindex];
    mesh   = CG_ModelMarkMesh(hModel, cent->currentState.scale > 0 ? cent->currentState.scale : 1.f);

    return mesh && mesh->rigid ? mesh : NULL;
}

extern "C" qboolean CG_EntityTakesModelMarks(int entnum)
{
    if (entnum < 0 || entnum >= MAX_GENTITIES) {
        return qfalse;
    }

    return CG_EntityMarkMesh(&cg_entities[entnum]) ? qtrue : qfalse;
}

// What a solid entity with a rigid model is made of, for its bullet impacts
// (its box has no surface to tell): by the model's name, or 0 for stone.
extern "C" int CG_ModelSurfaceType(int entnum)
{
    static const struct {
        const char *word;
        int         type;
    } kinds[] = {
        // wood first: "car" is in "cart"
        {"crate",   SURF_WOOD},  {"box",     SURF_WOOD},  {"cart",    SURF_WOOD},  {"wagon",   SURF_WOOD},
        {"table",   SURF_WOOD},  {"chair",   SURF_WOOD},  {"bench",   SURF_WOOD},  {"cabinet", SURF_WOOD},
        {"wood",    SURF_WOOD},  {"dresser", SURF_WOOD},  {"shelf",   SURF_WOOD},  {"vehicle", SURF_METAL},
        {"car",     SURF_METAL}, {"truck",   SURF_METAL}, {"tank",    SURF_METAL}, {"panzer",  SURF_METAL},
        {"jeep",    SURF_METAL}, {"sdkfz",   SURF_METAL}, {"opel",    SURF_METAL}, {"kubel",   SURF_METAL},
        {"barrel",  SURF_METAL}, {"drum",    SURF_METAL}, {"locker",  SURF_METAL}, {"metal",   SURF_METAL},
    };
    char   name[MAX_QPATH];
    size_t i;

    if (entnum < 0 || entnum >= MAX_GENTITIES || !CG_EntityTakesModelMarks(entnum)) {
        return 0;
    }

    Q_strncpyz(name, CG_ConfigString(CS_MODELS + cg_entities[entnum].currentState.modelindex), sizeof(name));
    Q_strlwr(name);

    for (i = 0; i < ARRAY_LEN(kinds); i++) {
        if (strstr(name, kinds[i].word)) {
            return kinds[i].type;
        }
    }

    return 0;
}

extern "C" qboolean CG_PropMarkOrientation(int prop, vec3_t origin, vec3_t axis[3])
{
    if (prop < 0 || prop >= cg_numProps) {
        return qfalse;
    }

    VectorCopy(cg_props[prop].origin, origin);
    AxisCopy(cg_props[prop].axis, axis);
    return qtrue;
}

extern "C" int CG_GetModelMarkFragments(
    int             numPoints,
    const vec3_t   *points,
    const vec3_t    projection,
    const vec3_t    mins,
    const vec3_t    maxs,
    int             numOutPoints,
    int             maxPoints,
    vec3_t         *pointBuffer,
    int             maxFragments,
    markFragment_t *fragmentBuffer
)
{
    int    numFragments = 0;
    int    i;
    vec3_t axis[3];

    if (!cg_modelMarks || !cg_modelMarks->integer) {
        return 0;
    }

    // the map's props
    for (i = 0; i < cg_numProps; i++) {
        const cgProp_t        *p = &cg_props[i];
        const modelMarkMesh_t *mesh;

        if (!p->solid || !p->hModel) {
            continue;
        }
        if (p->absmin[0] > maxs[0] || p->absmax[0] < mins[0] || p->absmin[1] > maxs[1] || p->absmax[1] < mins[1]
            || p->absmin[2] > maxs[2] || p->absmax[2] < mins[2]) {
            continue;
        }

        mesh = CG_ModelMarkMesh(p->hModel, p->scale);
        if (!mesh || !mesh->rigid) {
            continue;
        }

        CG_ModelMarkFragments(
            mesh,
            p->origin,
            p->axis,
            CG_MARK_PROP_INDEX(i),
            numPoints,
            points,
            projection,
            &numOutPoints,
            maxPoints,
            pointBuffer,
            &numFragments,
            maxFragments,
            fragmentBuffer
        );
    }

    // solid entities with a rigid model
    centity_t **solid;
    int         numSolid = CG_GetSolidEntities(&solid);
    for (i = 0; i < numSolid; i++) {
        centity_t             *cent = solid[i];
        const modelMarkMesh_t *mesh = CG_EntityMarkMesh(cent);
        vec3_t                 d;
        int                    k;

        if (!mesh) {
            continue;
        }

        for (k = 0; k < 3; k++) {
            d[k] = Q_max(mins[k] - cent->lerpOrigin[k], 0.f) + Q_max(cent->lerpOrigin[k] - maxs[k], 0.f);
        }
        if (VectorLength(d) > mesh->radius) {
            continue;
        }

        AnglesToAxis(cent->lerpAngles, axis);
        CG_ModelMarkFragments(
            mesh,
            cent->lerpOrigin,
            axis,
            -cent->currentState.number,
            numPoints,
            points,
            projection,
            &numOutPoints,
            maxPoints,
            pointBuffer,
            &numFragments,
            maxFragments,
            fragmentBuffer
        );
    }

    return numFragments;
}
