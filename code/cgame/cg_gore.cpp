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

// cg_gore.cpp -- wounds, bleeding and blood pools
//
// Added in OPM, after Soldier of Fortune II's gore. A round that hits a man
// leaves a wound where it went in, and a torn one where it came out, and they
// stay on him: through his animation while he lives, through his fall and his
// ragdoll once he is dead. Wounds bleed, a neck wound pumps, and a pool spreads
// under a corpse.
//
// A wound is not a texture on the model. It is a decal made of the model's own
// triangles: when the round lands, the renderer skins the body as drawn
// (R_GetSkinnedMesh), the shot's line is traced through that mesh, and the
// triangles about the entry are kept, each corner by the surface and vertex it
// is, with the texture coordinates it had then. Every frame after, the same
// vertices are skinned again and the decal is drawn wherever they are now.
//
// The server knows nothing of any of it, so a wound on one client may sit a
// little differently on another; it only ever changes what is seen.

#include "cg_local.h"
#include "cg_gore.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

#define GORE_MAX_BODIES     128
#define GORE_MAX_DRAWN      256  // characters handed to the renderer in one frame
#define GORE_MAX_HITS       64
#define GORE_MAX_BLASTS     8
#define GORE_MAX_DROPS      384
#define GORE_MAX_POOLS      32
#define GORE_MAX_DECAL_FRAGS 320
#define GORE_SKIN_VERTS     8192
#define GORE_SKIN_TRIS      12288

#define GORE_HIT_TTL      250  // ms a hit waits for its body to be drawn
#define GORE_RAY_BACK     20.f // the shot's line is traced from this far before the hit
#define GORE_RAY_LENGTH   80.f
#define GORE_ENTRY_SLACK  16.f // and the way in found up to this far past it
#define GORE_EXIT_DEPTH   40.f // thickest part of a man a round comes out of
#define GORE_EXIT_MIN     3.f  // and thinnest: less is a fold of cloth or a belt
#define GORE_LIFT         0.4f // off the skin, along the vertex normals: the face's
                               // morphs are not skinned, and move it a little
#define GORE_GRAVITY      800.f
#define GORE_POOL_DELAY   1200 // ms dead before the pool starts
#define GORE_SPURT_PERIOD 450
#define GORE_LIGHT_SCALE  0.5f // decal lighting is overbright next to the model's and the world's

typedef enum {
    GK_ENTRY,
    GK_EXIT,
    GK_FRAG,
    GK_RUN,
    GK_BURN,
    GK_NUM
} goreKind_t;

// Wounds come in a few textures each, of different blood and flesh, picked at
// random (tools/content/gore_textures.py draws them). With the HRRTM Blood
// Effects addon, its two blood splats make two more.
#define GORE_VARIANTS     6
#define GORE_OWN_VARIANTS 4

static int gore_numVariants;

// A round through cloth: the blood comes through it from underneath, so the
// stain spreads out from the hole over a second or two, and only then runs.
#define GORE_SOAK_DELAY_MIN 100
#define GORE_SOAK_DELAY_MAX 300
#define GORE_SOAK_MIN       1200
#define GORE_SOAK_MAX       2200
#define GORE_SKIN_FADE      120 // on skin it is there at once

static const char *gore_shaderNames[GK_NUM] = {
    "gore/wound_entry%d", "gore/wound_exit%d", "gore/wound_frag%d", "gore/wound_run", "gore/burn"
};

// A triangle of the body: which surface, and which of its vertices.
typedef struct {
    short          surf;
    unsigned short v[3];
} goreTri_t;

// The part of one triangle a decal covers: the triangle cut to the decal's
// square, each corner a point on the triangle (by its weights on the three
// vertices) and where the texture falls on it. Nothing is drawn outside the
// square, so whatever the edges of a texture come to at a low mip level, a
// big triangle never shows its whole shape.
#define GORE_FRAG_PTS 7

typedef struct {
    goreTri_t tri;
    int       numPts;
    float     bary[GORE_FRAG_PTS][3];
    float     st[GORE_FRAG_PTS][2];
} goreFrag_t;

typedef struct {
    int                     kind;
    int                     variant;
    qboolean                cloth;
    int                     soakStart, soakTime; // spreading out (a run: running down)
    float                   tint[3]; // each wound a little different
    int                     born;
    unsigned int            surfMask;
    std::vector<goreFrag_t> frags;
    std::vector<float>      xyz; // GORE_FRAG_PTS * 3 a fragment, where its corners were last drawn

    // The middle of the wound, for the blood that comes out of it: a point on
    // one triangle.
    goreTri_t anchor;
    float     bary[3];
    vec3_t    centre, normal;
    qboolean  hidden; // its surface is not drawn now (a helmet knocked off)

    int    bleedUntil, nextDrop, spurtUntil, nextSpurt;
    vec3_t light;
    int    lightTime;
} goreDecal_t;

// The surfaces of a model, each as R_GetSkinnedMesh hands it over.
typedef struct {
    int   numSurfs;
    int   numVerts[MAX_MODEL_SURFACES];
    int   numTris[MAX_MODEL_SURFACES];
    float winding; // turns a triangle's (b - a) x (c - a) outward
} goreLayout_t;

typedef struct {
    int                      entityNum; // ENTITYNUM_NONE when free
    dtiki_t                 *tiki;
    int                      lastSeen;
    qboolean                 dead;
    int                      deathTime;
    qboolean                 pooled;
    std::vector<goreDecal_t> decals;

    unsigned int poseHash;
    unsigned int poseMask;
    qboolean     dirty;
} goreBody_t;

typedef struct {
    int         entityNum;
    refEntity_t ref;
    qboolean    dead;
    qboolean    noDraw; // in the scene for hits only (the player's own body seen from inside)
} goreDrawn_t;

typedef struct {
    vec3_t pos, dir; // dir: the way the round was going
    int    large;
    int    time;
} goreHit_t;

typedef struct {
    vec3_t pos;
    int    kind;
    int    time;
} goreBlast_t;

typedef struct {
    qboolean active;
    vec3_t   p, v;
    float    size;
    int      born;
    vec3_t   light;
} goreDrop_t;

typedef struct {
    qboolean active;
    vec3_t   pos, normal;
    float    rot, radius;
    int      born, grow;
} gorePool_t;

static cvar_t *cg_gore;
static cvar_t *cg_gore_maxWounds;
static cvar_t *cg_gore_maxPolys;
static cvar_t *cg_gore_drawDist;
static cvar_t *cg_gore_bleed;
static cvar_t *cg_gore_pools;
static cvar_t *cg_gore_scale;
static cvar_t *cg_gore_debug;
static cvar_t *com_blood_gore;

static qhandle_t gore_shaders[GK_NUM][GORE_VARIANTS];
static qhandle_t gore_dropShader, gore_splatShader, gore_poolShader;

static goreBody_t  gore_bodies[GORE_MAX_BODIES];
static goreDrawn_t gore_drawn[GORE_MAX_DRAWN];
static int         gore_numDrawn;
static goreHit_t   gore_hits[GORE_MAX_HITS];
static int         gore_numHits;
static goreBlast_t gore_blasts[GORE_MAX_BLASTS];
static int         gore_numBlasts;
static goreDrop_t  gore_drops[GORE_MAX_DROPS];
static int         gore_nextDrop;
static gorePool_t  gore_pools[GORE_MAX_POOLS];
static float       gore_splatTokens;

static std::map<dtiki_t *, goreLayout_t> gore_layouts;

static skinnedVert_t gore_verts[GORE_SKIN_VERTS];
static int           gore_tris[GORE_SKIN_TRIS * 3];
static int           gore_numVerts, gore_numTris;
static vec3_t        gore_normals[GORE_SKIN_VERTS]; // smooth, by CG_GoreSmoothNormals
static int           gore_vertOfs[MAX_MODEL_SURFACES]; // -1: not in the buffer
static int           gore_triOfs[MAX_MODEL_SURFACES];

static float GoreRand(float lo, float hi)
{
    return lo + (hi - lo) * random();
}

static qboolean CG_GoreEnabled(void)
{
    return cg_gore && cg_gore->integer && com_blood_gore->integer && cgi.apiversion >= 4 && cgi.R_GetSkinnedMesh
             ? qtrue
             : qfalse;
}

void CG_GoreInit(void)
{
    cg_gore           = cgi.Cvar_Get("cg_gore", "1", CVAR_ARCHIVE);
    cg_gore_maxWounds = cgi.Cvar_Get("cg_gore_maxWounds", "40", CVAR_ARCHIVE);
    cg_gore_maxPolys  = cgi.Cvar_Get("cg_gore_maxPolys", "2500", CVAR_ARCHIVE);
    cg_gore_drawDist  = cgi.Cvar_Get("cg_gore_drawDist", "3000", CVAR_ARCHIVE);
    cg_gore_bleed     = cgi.Cvar_Get("cg_gore_bleed", "1", CVAR_ARCHIVE);
    cg_gore_pools     = cgi.Cvar_Get("cg_gore_pools", "1", CVAR_ARCHIVE);
    cg_gore_scale     = cgi.Cvar_Get("cg_gore_scale", "1", CVAR_ARCHIVE);
    cg_gore_debug     = cgi.Cvar_Get("cg_gore_debug", "0", 0);
    com_blood_gore    = cgi.Cvar_Get("com_blood", "1", 0);
}

void CG_GoreClear(void)
{
    int i;

    for (i = 0; i < GORE_MAX_BODIES; i++) {
        gore_bodies[i].entityNum = ENTITYNUM_NONE;
        gore_bodies[i].decals.clear();
    }

    for (i = 0; i < GK_NUM; i++) {
        int v;

        for (v = 0; v < GORE_VARIANTS; v++) {
            gore_shaders[i][v] = cgi.R_RegisterShader(va(gore_shaderNames[i], v + 1));
        }
    }
    gore_numVariants = cgi.FS_ReadFile("textures/effects/blood_splat.tga", NULL, qtrue) > 0
                         && cgi.FS_ReadFile("textures/effects/blood_splat2.tga", NULL, qtrue) > 0
                         ? GORE_VARIANTS
                         : GORE_OWN_VARIANTS;
    gore_dropShader  = cgi.R_RegisterShader("gore/drop");
    gore_splatShader = cgi.R_RegisterShader("gore/splat");
    gore_poolShader  = cgi.R_RegisterShader("gore/pool");

    memset(gore_drops, 0, sizeof(gore_drops));
    memset(gore_pools, 0, sizeof(gore_pools));
    gore_layouts.clear();
    gore_numDrawn  = 0;
    gore_numHits   = 0;
    gore_numBlasts = 0;
}

//=============================================================
// Bodies
//=============================================================

static goreBody_t *CG_GoreFindBody(int entityNum)
{
    int i;

    for (i = 0; i < GORE_MAX_BODIES; i++) {
        if (gore_bodies[i].entityNum == entityNum) {
            return &gore_bodies[i];
        }
    }

    return NULL;
}

static void CG_GoreFreeBody(goreBody_t *body)
{
    body->entityNum = ENTITYNUM_NONE;
    body->decals.clear();
}

// A body for an entity, the one it has or a new one. When every body is
// taken, the one seen longest ago goes.
static goreBody_t *CG_GoreBody(const goreDrawn_t *drawn)
{
    goreBody_t *body = CG_GoreFindBody(drawn->entityNum);
    goreBody_t *oldest = NULL;
    int         i;

    if (body) {
        return body;
    }

    for (i = 0; i < GORE_MAX_BODIES; i++) {
        if (gore_bodies[i].entityNum == ENTITYNUM_NONE) {
            body = &gore_bodies[i];
            break;
        }

        if (!oldest || gore_bodies[i].lastSeen < oldest->lastSeen) {
            oldest = &gore_bodies[i];
        }
    }

    if (!body) {
        body = oldest;
        CG_GoreFreeBody(body);
    }

    body->entityNum = drawn->entityNum;
    body->tiki      = drawn->ref.tiki;
    body->lastSeen  = cg.time;
    body->dead      = drawn->dead;
    body->deathTime = cg.time;
    body->pooled    = qfalse;
    body->dirty     = qtrue;
    return body;
}

void CG_GoreTransfer(int fromEntity, int toEntity)
{
    goreBody_t *from, *to;

    if (fromEntity == toEntity || fromEntity == ENTITYNUM_NONE) {
        return;
    }

    from = CG_GoreFindBody(fromEntity);
    if (!from) {
        return;
    }

    to = CG_GoreFindBody(toEntity);
    if (to) {
        CG_GoreFreeBody(to);
    }

    from->entityNum = toEntity;
    from->dirty     = qtrue;
}

void CG_GoreAddEntity(centity_t *cent, const refEntity_t *model, qboolean dead)
{
    goreDrawn_t *drawn;
    goreBody_t  *body;

    if (!CG_GoreEnabled() || !model->tiki || !model->tiki->a || !model->tiki->a->bIsCharacter) {
        return;
    }

    if (model->renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK)) {
        return;
    }

    if (gore_numDrawn >= GORE_MAX_DRAWN) {
        return;
    }

    drawn            = &gore_drawn[gore_numDrawn++];
    drawn->entityNum = cent->currentState.number;
    drawn->ref       = *model;
    drawn->dead      = dead;
    drawn->noDraw    = (model->renderfx & RF_THIRD_PERSON) && !cg.renderingThirdPerson ? qtrue : qfalse;

    body = CG_GoreFindBody(drawn->entityNum);
    if (!body) {
        return;
    }

    // The slot went to someone else: another model, or the dead man is up
    // again (a respawn), or a living one has been away too long to be the
    // same man.
    if (body->tiki != model->tiki || (body->dead && !dead) || (!dead && cg.time - body->lastSeen > 10000)) {
        CG_GoreFreeBody(body);
        return;
    }

    if (dead && !body->dead) {
        body->deathTime = cg.time;
    }

    body->dead     = dead;
    body->lastSeen = cg.time;
}

//=============================================================
// Skinning
//=============================================================

static void CG_GoreTriNormal(const float *a, const float *b, const float *c, float winding, vec3_t out)
{
    vec3_t e1, e2;

    VectorSubtract(b, a, e1);
    VectorSubtract(c, a, e2);
    CrossProduct(e1, e2, out);
    VectorScale(out, winding, out);
    VectorNormalize(out);
}

// The layout of a model, measured the first time one of it is hit: each
// surface skinned on its own, which gives the counts, then all of it, which
// gives the way the triangles wind.
static const goreLayout_t *CG_GoreLayout(const refEntity_t *model)
{
    std::map<dtiki_t *, goreLayout_t>::iterator it = gore_layouts.find(model->tiki);
    goreLayout_t                               *lay;
    refEntity_t                                 ent;
    vec3_t                                      mid;
    float                                       facing;
    int                                         s, k, total, n, numTris;

    if (it != gore_layouts.end()) {
        return it->second.numSurfs ? &it->second : NULL;
    }

    lay = &gore_layouts[model->tiki];
    memset(lay, 0, sizeof(*lay));

    ent   = *model;
    total = 0;
    for (s = 0; s < MAX_MODEL_SURFACES; s++) {
        for (k = 0; k < MAX_MODEL_SURFACES; k++) {
            ent.surfaces[k] = (model->surfaces[k] & ~MDL_SURFACE_NODRAW) | (k == s ? 0 : MDL_SURFACE_NODRAW);
        }

        n = cgi.R_GetSkinnedMesh(&ent, gore_verts, GORE_SKIN_VERTS, gore_tris, GORE_SKIN_TRIS, &numTris);
        lay->numVerts[s] = n;
        lay->numTris[s]  = n ? numTris : 0;
        total += n;
        if (n) {
            lay->numSurfs = s + 1;
        }
    }

    for (k = 0; k < MAX_MODEL_SURFACES; k++) {
        ent.surfaces[k] = model->surfaces[k] & ~MDL_SURFACE_NODRAW;
    }

    n = cgi.R_GetSkinnedMesh(&ent, gore_verts, GORE_SKIN_VERTS, gore_tris, GORE_SKIN_TRIS, &numTris);
    if (!n || n != total) {
        lay->numSurfs = 0;
        return NULL;
    }

    // Outward is the way most of the surface faces away from the middle.
    VectorClear(mid);
    for (k = 0; k < n; k++) {
        VectorAdd(mid, gore_verts[k].xyz, mid);
    }
    VectorScale(mid, 1.0f / n, mid);

    facing = 0;
    for (k = 0; k < numTris; k++) {
        const float *a = gore_verts[gore_tris[k * 3 + 0]].xyz;
        const float *b = gore_verts[gore_tris[k * 3 + 1]].xyz;
        const float *c = gore_verts[gore_tris[k * 3 + 2]].xyz;
        vec3_t       e1, e2, cross, centroid;

        VectorSubtract(b, a, e1);
        VectorSubtract(c, a, e2);
        CrossProduct(e1, e2, cross);
        VectorAdd(a, b, centroid);
        VectorAdd(centroid, c, centroid);
        VectorScale(centroid, 1.0f / 3.0f, centroid);
        VectorSubtract(centroid, mid, centroid);
        facing += DotProduct(cross, centroid);
    }
    lay->winding = facing >= 0 ? 1.0f : -1.0f;

    if (cg_gore_debug->integer) {
        Com_Printf("gore: %s has %d surfaces, %d vertices, winding %+.0f\n", model->tiki->name, lay->numSurfs, n, lay->winding);
    }

    return lay;
}

static unsigned int CG_GoreVisibleMask(const refEntity_t *model, const goreLayout_t *lay)
{
    unsigned int mask = 0;
    int          s;

    for (s = 0; s < lay->numSurfs; s++) {
        if (!(model->surfaces[s] & MDL_SURFACE_NODRAW) && lay->numVerts[s]) {
            mask |= 1u << s;
        }
    }

    return mask;
}

// Skins the surfaces in mask into the scratch buffer.
static qboolean CG_GoreSkin(const refEntity_t *model, const goreLayout_t *lay, unsigned int mask)
{
    refEntity_t ent = *model;
    int         s, verts = 0, tris = 0;

    for (s = 0; s < MAX_MODEL_SURFACES; s++) {
        if (s < lay->numSurfs && (mask & (1u << s))) {
            ent.surfaces[s] &= ~MDL_SURFACE_NODRAW;
            gore_vertOfs[s] = verts;
            gore_triOfs[s]  = tris;
            verts += lay->numVerts[s];
            tris += lay->numTris[s];
        } else {
            ent.surfaces[s] |= MDL_SURFACE_NODRAW;
            gore_vertOfs[s] = -1;
            gore_triOfs[s]  = -1;
        }
    }

    gore_numVerts =
        cgi.R_GetSkinnedMesh(&ent, gore_verts, GORE_SKIN_VERTS, gore_tris, GORE_SKIN_TRIS, &gore_numTris);

    return gore_numVerts == verts && gore_numTris == tris && verts ? qtrue : qfalse;
}

static int CG_GoreTriSurface(int tri, const goreLayout_t *lay)
{
    int s;

    for (s = 0; s < lay->numSurfs; s++) {
        if (gore_triOfs[s] >= 0 && tri >= gore_triOfs[s] && tri < gore_triOfs[s] + lay->numTris[s]) {
            return s;
        }
    }

    return -1;
}

// A decal triangle (or anchor) from a triangle of the scratch buffer.
static qboolean CG_GoreTriFromSkin(int tri, const goreLayout_t *lay, goreTri_t *out)
{
    int s = CG_GoreTriSurface(tri, lay);
    int k;

    if (s < 0) {
        return qfalse;
    }

    out->surf = s;
    for (k = 0; k < 3; k++) {
        out->v[k] = gore_tris[tri * 3 + k] - gore_vertOfs[s];
    }

    return qtrue;
}

static const float *CG_GoreVert(const goreTri_t *tri, int k)
{
    return gore_verts[gore_vertOfs[tri->surf] + tri->v[k]].xyz;
}

// A normal for every vertex in the scratch buffer, the average of the
// triangles about it. A decal is lifted off the skin along these: lifted along
// each triangle's own, neighbouring pieces part at every crease.
static void CG_GoreSmoothNormals(const goreLayout_t *lay)
{
    int i, k;

    memset(gore_normals, 0, sizeof(gore_normals[0]) * gore_numVerts);

    for (i = 0; i < gore_numTris; i++) {
        const int *t = &gore_tris[i * 3];
        vec3_t     e1, e2, n;

        VectorSubtract(gore_verts[t[1]].xyz, gore_verts[t[0]].xyz, e1);
        VectorSubtract(gore_verts[t[2]].xyz, gore_verts[t[0]].xyz, e2);
        CrossProduct(e1, e2, n);
        VectorScale(n, lay->winding, n);

        for (k = 0; k < 3; k++) {
            VectorAdd(gore_normals[t[k]], n, gore_normals[t[k]]);
        }
    }

    for (i = 0; i < gore_numVerts; i++) {
        VectorNormalize(gore_normals[i]);
    }
}

static const float *CG_GoreNormal(const goreTri_t *tri, int k)
{
    return gore_normals[gore_vertOfs[tri->surf] + tri->v[k]];
}

// The pose a body was drawn in, roughly: when it has not changed, neither has
// any wound on it, and a corpse at rest is drawn without skinning it again.
// Rounded first, as a body at rest is never quite still: a sleeping ragdoll's
// bones still shift by a hair from frame to frame.
static unsigned int CG_GorePoseHash(const refEntity_t *ref)
{
    unsigned int h = 2166136261u;
    int          i, k;

#define GORE_HASH_INT(v) (h = (h ^ (unsigned int)(v)) * 16777619u)
#define GORE_HASH_POS(f) GORE_HASH_INT((int)floorf((f) * 8.0f))   // an eighth of a unit
#define GORE_HASH_DIR(f) GORE_HASH_INT((int)floorf((f) * 256.0f)) // a quarter of a degree, about

    for (k = 0; k < 3; k++) {
        GORE_HASH_POS(ref->origin[k]);
        GORE_HASH_DIR(ref->axis[k][0]);
        GORE_HASH_DIR(ref->axis[k][1]);
        GORE_HASH_DIR(ref->axis[k][2]);
    }
    GORE_HASH_DIR(ref->scale);
    GORE_HASH_INT(ref->num_bone_overrides);

    if (ref->bone_override && ref->num_bone_overrides > 0) {
        // The whole skeleton is the ragdoll's: the animation under it no
        // longer shows.
        for (i = 0; i < ref->num_bone_overrides; i++) {
            const boneOverride_t *b = &ref->bone_override[i];

            GORE_HASH_INT(b->boneIndex);
            for (k = 0; k < 3; k++) {
                GORE_HASH_DIR(b->matrix[0][k]);
                GORE_HASH_DIR(b->matrix[1][k]);
                GORE_HASH_DIR(b->matrix[2][k]);
                GORE_HASH_POS(b->matrix[3][k]);
            }
        }
    } else {
        for (i = 0; i < MAX_FRAMEINFOS; i++) {
            GORE_HASH_INT(ref->frameInfo[i].index);
            GORE_HASH_INT((int)floorf(ref->frameInfo[i].time * 1000.0f));
            GORE_HASH_INT((int)floorf(ref->frameInfo[i].weight * 1000.0f));
        }
        GORE_HASH_INT((int)floorf(ref->actionWeight * 1000.0f));
    }

    if (ref->bone_tag && ref->bone_quat) {
        for (i = 0; i < NUM_BONE_CONTROLLERS; i++) {
            GORE_HASH_INT(ref->bone_tag[i]);
            for (k = 0; k < 4; k++) {
                GORE_HASH_DIR(ref->bone_quat[i][k]);
            }
        }
    }

#undef GORE_HASH_INT
#undef GORE_HASH_POS
#undef GORE_HASH_DIR

    return h;
}

//=============================================================
// Laying wounds
//=============================================================

static float CG_GoreRayTri(const vec3_t start, const vec3_t dir, const float *a, const float *b, const float *c, float *bary)
{
    vec3_t e1, e2, p, q, s;
    float  det, inv, u, v, t;

    VectorSubtract(b, a, e1);
    VectorSubtract(c, a, e2);
    CrossProduct(dir, e2, p);
    det = DotProduct(e1, p);
    if (fabs(det) < 1e-8f) {
        return -1;
    }

    inv = 1.0f / det;
    VectorSubtract(start, a, s);
    u = DotProduct(s, p) * inv;
    if (u < 0 || u > 1) {
        return -1;
    }

    CrossProduct(s, e1, q);
    v = DotProduct(dir, q) * inv;
    if (v < 0 || u + v > 1) {
        return -1;
    }

    t = DotProduct(e2, q) * inv;
    if (bary) {
        bary[0] = 1 - u - v;
        bary[1] = u;
        bary[2] = v;
    }

    return t;
}

// Whether a surface of the model is clothing rather than skin, by its name.
static qboolean CG_GoreIsCloth(dtiki_t *tiki, int surf)
{
    static const char *skin[] = {"head", "face", "hand", "skin", "neck", "eye", "arm"};
    const char        *name;
    int                i;

    if (!tiki || surf < 0 || surf >= tiki->num_surfaces) {
        return qtrue;
    }

    name = tiki->surfaces[surf].name;
    for (i = 0; i < (int)ARRAY_LEN(skin); i++) {
        if (Q_stristr(name, skin[i])) {
            return qfalse;
        }
    }

    return qtrue;
}

// Cuts a fragment to the texture's square. Returns qfalse if nothing is left.
static qboolean CG_GoreClipFrag(goreFrag_t *frag)
{
    int edge;

    for (edge = 0; edge < 4; edge++) {
        float bary[GORE_FRAG_PTS + 1][3], st[GORE_FRAG_PTS + 1][2];
        int   axis = edge & 1;
        float sign = edge < 2 ? 1.0f : -1.0f; // keep st >= 0, then st <= 1
        float off  = edge < 2 ? 0.0f : 1.0f;
        int   n    = 0;
        int   i, k;

        for (i = 0; i < frag->numPts; i++) {
            int   j  = (i + 1) % frag->numPts;
            float di = sign * (frag->st[i][axis] - off);
            float dj = sign * (frag->st[j][axis] - off);

            if (di >= 0 && n < GORE_FRAG_PTS + 1) {
                VectorCopy(frag->bary[i], bary[n]);
                st[n][0] = frag->st[i][0];
                st[n][1] = frag->st[i][1];
                n++;
            }

            if ((di >= 0) != (dj >= 0) && n < GORE_FRAG_PTS + 1) {
                float f = di / (di - dj);

                for (k = 0; k < 3; k++) {
                    bary[n][k] = frag->bary[i][k] + f * (frag->bary[j][k] - frag->bary[i][k]);
                }
                st[n][0] = frag->st[i][0] + f * (frag->st[j][0] - frag->st[i][0]);
                st[n][1] = frag->st[i][1] + f * (frag->st[j][1] - frag->st[i][1]);
                n++;
            }
        }

        if (n < 3 || n > GORE_FRAG_PTS) {
            return qfalse;
        }

        frag->numPts = n;
        memcpy(frag->bary, bary, sizeof(frag->bary[0]) * n);
        memcpy(frag->st, st, sizeof(frag->st[0]) * n);
    }

    return qtrue;
}

// Lays a decal of kind on the skinned body, its middle at centre on triangle
// anchorTri. up, if given, is where the top of the texture goes (a run of blood
// goes down); otherwise it is turned at random.
static void CG_GoreLay(
    goreBody_t         *body,
    const goreLayout_t *lay,
    int                 kind,
    const vec3_t        centre,
    const vec3_t        normal,
    const vec3_t        up,
    int                 anchorTri,
    const float        *bary,
    float               radius,
    float               down
)
{
    goreDecal_t decal;
    vec3_t      axisUp, axisRight;
    float       hw, hup, hdown, depth;
    int         i, k;

    if (up) {
        VectorMA(up, -DotProduct(up, normal), normal, axisUp);
        if (VectorNormalize(axisUp) < 0.1f) {
            return;
        }
    } else {
        vec3_t perp;

        PerpendicularVector(perp, normal);
        RotatePointAroundVector(axisUp, normal, perp, GoreRand(0, 360));
    }
    CrossProduct(axisUp, normal, axisRight);

    if (kind == GK_RUN) {
        hw    = radius * 0.6f;
        hup   = radius * 0.3f;
        hdown = down;
    } else {
        hw = hup = hdown = radius;
    }
    depth = Q_max(radius * 0.8f, 2.5f);

    memset(&decal.anchor, 0, sizeof(decal.anchor));
    if (!CG_GoreTriFromSkin(anchorTri, lay, &decal.anchor)) {
        return;
    }

    decal.kind     = kind;
    decal.variant  = rand() % gore_numVariants;
    decal.cloth    = CG_GoreIsCloth(body->tiki, decal.anchor.surf);
    if (kind == GK_RUN) {
        // after the wound it comes from (CG_GoreLayRun), down the body
        decal.soakStart = cg.time;
        decal.soakTime  = (int)GoreRand(1500, 3000);
    } else if (decal.cloth && kind != GK_BURN) {
        decal.soakStart = cg.time + (int)GoreRand(GORE_SOAK_DELAY_MIN, GORE_SOAK_DELAY_MAX);
        decal.soakTime  = (int)GoreRand(GORE_SOAK_MIN, GORE_SOAK_MAX);
    } else {
        decal.soakStart = cg.time;
        decal.soakTime  = GORE_SKIN_FADE;
    }
    decal.born     = cg.time;
    {
        // darker or brighter, redder or browner
        float shade = GoreRand(0.8f, 1.15f);

        decal.tint[0] = shade;
        decal.tint[1] = shade * GoreRand(0.85f, 1.15f);
        decal.tint[2] = shade * GoreRand(0.85f, 1.1f);
    }
    decal.surfMask = 0;
    decal.hidden   = qfalse;
    VectorCopy(bary, decal.bary);
    VectorCopy(centre, decal.centre);
    VectorCopy(normal, decal.normal);
    decal.bleedUntil = decal.nextDrop = decal.spurtUntil = decal.nextSpurt = 0;
    decal.lightTime                                                         = 0;

    for (i = 0; i < gore_numTris; i++) {
        const float *p[3];
        float        x[3], y[3], z[3];
        vec3_t       nt;
        goreFrag_t   frag;

        for (k = 0; k < 3; k++) {
            p[k] = gore_verts[gore_tris[i * 3 + k]].xyz;
        }

        // Either way round: a head or a hand often has triangles wound against
        // the rest, and leaving them out cuts holes in the decal. What faces
        // away (the far side of an arm) is kept out by the depth.
        CG_GoreTriNormal(p[0], p[1], p[2], lay->winding, nt);
        if (fabs(DotProduct(nt, normal)) < 0.2f) {
            continue;
        }

        for (k = 0; k < 3; k++) {
            vec3_t d;

            VectorSubtract(p[k], centre, d);
            x[k] = DotProduct(d, axisRight);
            y[k] = DotProduct(d, axisUp);
            z[k] = DotProduct(d, normal);
        }

        if (Q_min(z[0], Q_min(z[1], z[2])) > depth || Q_max(z[0], Q_max(z[1], z[2])) < -depth) {
            continue;
        }
        if (Q_max(x[0], Q_max(x[1], x[2])) < -hw || Q_min(x[0], Q_min(x[1], x[2])) > hw) {
            continue;
        }
        if (Q_max(y[0], Q_max(y[1], y[2])) < -hdown || Q_min(y[0], Q_min(y[1], y[2])) > hup) {
            continue;
        }

        if (!CG_GoreTriFromSkin(i, lay, &frag.tri)) {
            continue;
        }

        frag.numPts = 3;
        for (k = 0; k < 3; k++) {
            frag.st[k][0] = (x[k] + hw) / (2 * hw);
            frag.st[k][1] = (hup - y[k]) / (hup + hdown);
            frag.bary[k][0] = k == 0;
            frag.bary[k][1] = k == 1;
            frag.bary[k][2] = k == 2;
        }

        if (!CG_GoreClipFrag(&frag)) {
            continue;
        }

        decal.frags.push_back(frag);
    }

    // Too many, on a fine mesh: the ones nearest the middle are kept, so what
    // goes is the faint edge and not a scatter of holes.
    if ((int)decal.frags.size() > GORE_MAX_DECAL_FRAGS) {
        std::vector<std::pair<float, int> > byDist;
        std::vector<goreFrag_t>             kept;

        for (i = 0; i < (int)decal.frags.size(); i++) {
            const goreFrag_t &frag = decal.frags[i];
            float             ds   = 0, dt = 0;

            for (k = 0; k < frag.numPts; k++) {
                ds += frag.st[k][0];
                dt += frag.st[k][1];
            }
            ds = ds / frag.numPts - 0.5f;
            dt = dt / frag.numPts - (kind == GK_RUN ? 0.0f : 0.5f);
            byDist.push_back(std::make_pair(ds * ds + dt * dt, i));
        }

        std::sort(byDist.begin(), byDist.end());
        for (i = 0; i < GORE_MAX_DECAL_FRAGS; i++) {
            kept.push_back(decal.frags[byDist[i].second]);
        }
        decal.frags.swap(kept);
    }

    for (i = 0; i < (int)decal.frags.size(); i++) {
        decal.surfMask |= 1u << decal.frags[i].tri.surf;
    }

    if (decal.frags.empty()) {
        return;
    }

    // The oldest goes to make room.
    while ((int)body->decals.size() >= Q_max(cg_gore_maxWounds->integer, 1)) {
        body->decals.erase(body->decals.begin());
    }

    body->decals.push_back(decal);
    body->dirty = qtrue;
}

// Starts a wound bleeding, and pumping if it is in the neck.
static void CG_GoreBleed(goreBody_t *body, const refEntity_t *ref, int anchorTri, int kind)
{
    goreDecal_t &decal = body->decals.back();
    const char  *bone  = NULL;
    int          v     = gore_tris[anchorTri * 3];
    float        scale = body->dead ? 0.35f : 1.0f;

    if (gore_verts[v].bone >= 0) {
        bone = cgi.Tag_NameForNum(ref->tiki, gore_verts[v].bone);
    }

    switch (kind) {
    case GK_ENTRY:
        decal.bleedUntil = cg.time + (int)(GoreRand(6000, 14000) * scale);
        break;
    case GK_EXIT:
        decal.bleedUntil = cg.time + (int)(GoreRand(10000, 20000) * scale);
        break;
    case GK_FRAG:
        decal.bleedUntil = cg.time + (int)(GoreRand(4000, 8000) * scale);
        break;
    default:
        return;
    }
    // not before the blood has come through
    decal.nextDrop = decal.soakStart + decal.soakTime + (int)GoreRand(50, 300);

    if (bone && Q_stristr(bone, "neck") && (!body->dead || cg.time - body->deathTime < 2000)) {
        decal.spurtUntil = cg.time + (int)GoreRand(4000, 6000);
        decal.nextSpurt  = cg.time;
        decal.bleedUntil = Q_max(decal.bleedUntil, decal.spurtUntil + 8000);
    }

    if (cg_gore_debug->integer) {
        Com_Printf("gore: centre %.1f %.1f %.1f normal %.2f %.2f %.2f, %d tris\n", decal.centre[0], decal.centre[1],
            decal.centre[2], decal.normal[0], decal.normal[1], decal.normal[2], (int)decal.frags.size());
        Com_Printf("gore: %s wound on %s (surface %s), entity %d, bone %s%s\n",
            kind == GK_EXIT ? "exit" : (kind == GK_FRAG ? "shrapnel" : "entry"), decal.cloth ? "cloth" : "skin",
            decal.anchor.surf < body->tiki->num_surfaces ? body->tiki->surfaces[decal.anchor.surf].name : "?",
            body->entityNum,
            bone ? bone : "?",
            decal.spurtUntil ? ", pumping" : "");
    }
}

// A run of blood down from a wound, for a man shot standing.
static void CG_GoreLayRun(goreBody_t *body, const goreLayout_t *lay, const vec3_t centre, const vec3_t normal, int tri, const float *bary, float radius)
{
    static const vec3_t worldUp = {0, 0, 1};

    if (fabs(normal[2]) > 0.8f) {
        return;
    }

    if (body->decals.empty()) {
        return;
    }

    {
        // runs once the wound it comes from has soaked through
        int from = body->decals.back().soakStart + body->decals.back().soakTime;

        CG_GoreLay(body, lay, GK_RUN, centre, normal, worldUp, tri, bary, radius, GoreRand(10, 24) * cg_gore_scale->value);
        if (body->decals.back().kind == GK_RUN && body->decals.back().born == cg.time) {
            body->decals.back().soakStart = from;
        }
    }
}

typedef struct {
    float  tEntry, tExit;
    int    entryTri, exitTri;
    float  entryBary[3], exitBary[3];
    vec3_t entryNormal, exitNormal;
} goreTrace_t;

// The shot's line through the skinned body: the first skin facing it, from
// GORE_RAY_BACK before the reported hit to GORE_ENTRY_SLACK past it, and the
// first facing away beyond that, if the round came out.
static qboolean CG_GoreTraceSkin(const goreLayout_t *lay, const vec3_t start, const vec3_t dir, goreTrace_t *tr)
{
    int i;

    tr->entryTri = tr->exitTri = -1;
    tr->tEntry = tr->tExit = 0;

    for (i = 0; i < gore_numTris; i++) {
        const float *a = gore_verts[gore_tris[i * 3 + 0]].xyz;
        const float *b = gore_verts[gore_tris[i * 3 + 1]].xyz;
        const float *c = gore_verts[gore_tris[i * 3 + 2]].xyz;
        vec3_t       nt;
        float        bc[3];
        float        t = CG_GoreRayTri(start, dir, a, b, c, bc);

        if (t < 0 || t > GORE_RAY_BACK + GORE_ENTRY_SLACK || (tr->entryTri >= 0 && t >= tr->tEntry)) {
            continue;
        }

        CG_GoreTriNormal(a, b, c, lay->winding, nt);
        if (DotProduct(nt, dir) < 0) {
            tr->entryTri = i;
            tr->tEntry   = t;
            VectorCopy(bc, tr->entryBary);
            VectorCopy(nt, tr->entryNormal);
        }
    }

    if (tr->entryTri < 0) {
        return qfalse;
    }

    for (i = 0; i < gore_numTris; i++) {
        const float *a = gore_verts[gore_tris[i * 3 + 0]].xyz;
        const float *b = gore_verts[gore_tris[i * 3 + 1]].xyz;
        const float *c = gore_verts[gore_tris[i * 3 + 2]].xyz;
        vec3_t       nt;
        float        bc[3];
        float        t = CG_GoreRayTri(start, dir, a, b, c, bc);

        if (t < tr->tEntry + GORE_EXIT_MIN || t > tr->tEntry + GORE_EXIT_DEPTH || (tr->exitTri >= 0 && t >= tr->tExit)) {
            continue;
        }

        CG_GoreTriNormal(a, b, c, lay->winding, nt);
        if (DotProduct(nt, dir) > 0) {
            tr->exitTri = i;
            tr->tExit   = t;
            VectorCopy(bc, tr->exitBary);
            VectorCopy(nt, tr->exitNormal);
        }
    }

    return qtrue;
}

// The server hits a man by boxes about his bones, a little fuller than he is,
// so a round can graze the box and pass beside the skin. Such a line is moved
// across, a little and then a little more, until it meets him.
#define GORE_NUM_NUDGES 17

static void CG_GoreNudge(const vec3_t start, const vec3_t dir, int nudge, vec3_t out)
{
    vec3_t right, up;
    float  ang, r;

    VectorCopy(start, out);
    if (!nudge) {
        return;
    }

    PerpendicularVector(right, dir);
    CrossProduct(dir, right, up);
    r   = nudge <= 8 ? 2.0f : 4.0f;
    ang = ((nudge - 1) % 8) * (M_PI / 4);
    VectorMA(out, r * cos(ang), right, out);
    VectorMA(out, r * sin(ang), up, out);
}

// A round into one of the drawn bodies: which, where it went in, and where it
// came out. Returns qfalse if it went into none of them.
static qboolean CG_GorePlaceHit(const goreHit_t *hit)
{
    const goreDrawn_t  *best      = NULL;
    int                 bestNudge = 0;
    float               bestErr   = 0;
    const goreLayout_t *lay;
    goreBody_t         *body;
    goreTrace_t         tr;
    vec3_t              start, ray, entry, exitPos;
    float               scale = cg_gore_scale->value;
    int                 n, nudge;

    VectorMA(hit->pos, -GORE_RAY_BACK, hit->dir, start);

    // The body whose skin the line meets nearest the reported hit, moved
    // across as little as it takes.
    for (n = 0; n < gore_numDrawn; n++) {
        const goreDrawn_t *d = &gore_drawn[n];

        if (Distance(d->ref.origin, hit->pos) > 128) {
            continue;
        }

        lay = CG_GoreLayout(&d->ref);
        if (!lay || !CG_GoreSkin(&d->ref, lay, CG_GoreVisibleMask(&d->ref, lay))) {
            continue;
        }

        for (nudge = 0; nudge < GORE_NUM_NUDGES; nudge++) {
            float err;

            CG_GoreNudge(start, hit->dir, nudge, ray);
            if (!CG_GoreTraceSkin(lay, ray, hit->dir, &tr)) {
                continue;
            }

            err = fabs(tr.tEntry - GORE_RAY_BACK) + (nudge ? (nudge <= 8 ? 20.0f : 40.0f) : 0.0f);
            if (!best || err < bestErr) {
                best      = d;
                bestErr   = err;
                bestNudge = nudge;
            }
            break;
        }
    }

    if (!best) {
        if (cg_gore_debug->integer) {
            Com_Printf(
                "gore: hit at %.0f %.0f %.0f going %.2f %.2f %.2f met no body (%d drawn)\n", hit->pos[0], hit->pos[1],
                hit->pos[2], hit->dir[0], hit->dir[1], hit->dir[2], gore_numDrawn
            );
        }
        return qfalse;
    }

    // Skinned again, as the last one skinned need not be it.
    lay = CG_GoreLayout(&best->ref);
    CG_GoreNudge(start, hit->dir, bestNudge, ray);
    if (!CG_GoreSkin(&best->ref, lay, CG_GoreVisibleMask(&best->ref, lay)) || !CG_GoreTraceSkin(lay, ray, hit->dir, &tr)) {
        return qfalse;
    }

    VectorMA(ray, tr.tEntry, hit->dir, entry);

    body       = CG_GoreBody(best);
    body->dead = best->dead;

    CG_GoreLay(body, lay, GK_ENTRY, entry, tr.entryNormal, NULL, tr.entryTri, tr.entryBary, (hit->large ? 4.2f : 3.6f) * scale, 0);
    if (!body->decals.empty() && body->decals.back().kind == GK_ENTRY && body->decals.back().born == cg.time) {
        CG_GoreBleed(body, &best->ref, tr.entryTri, GK_ENTRY);
        if (random() < (body->dead ? 0.4f : 0.7f)) {
            CG_GoreLayRun(body, lay, entry, tr.entryNormal, tr.entryTri, tr.entryBary, 4.2f * scale);
        }
    }

    if (tr.exitTri >= 0 && random() < (hit->large ? 0.9f : 0.45f)) {
        VectorMA(ray, tr.tExit, hit->dir, exitPos);
        CG_GoreLay(body, lay, GK_EXIT, exitPos, tr.exitNormal, NULL, tr.exitTri, tr.exitBary, (hit->large ? 7.0f : 5.5f) * scale, 0);
        if (!body->decals.empty() && body->decals.back().kind == GK_EXIT && body->decals.back().born == cg.time) {
            CG_GoreBleed(body, &best->ref, tr.exitTri, GK_EXIT);
            if (random() < (body->dead ? 0.4f : 0.9f)) {
                CG_GoreLayRun(body, lay, exitPos, tr.exitNormal, tr.exitTri, tr.exitBary, 6.0f * scale);
            }
        }
    }

    return qtrue;
}

// Shrapnel from a blast close by, on the side of each body facing it, and soot
// on one very close.
static void CG_GorePlaceBlast(const goreBlast_t *blast)
{
    static const float ranges[] = {220, 300, 400, 400};
    float              range    = ranges[blast->kind & 3];
    float              scale    = cg_gore_scale->value;
    int                n;

    for (n = 0; n < gore_numDrawn; n++) {
        const goreDrawn_t  *d = &gore_drawn[n];
        const goreLayout_t *lay;
        goreBody_t         *body = NULL;
        float               dist = Distance(d->ref.origin, blast->pos);
        int                 frags, tries, burnt = 0;

        if (dist > range) {
            continue;
        }

        lay = CG_GoreLayout(&d->ref);
        if (!lay || !CG_GoreSkin(&d->ref, lay, CG_GoreVisibleMask(&d->ref, lay))) {
            continue;
        }

        frags = (int)((1.0f - dist / range) * 7.0f + random());
        if (dist < range * 0.25f) {
            burnt = 1;
        }

        for (tries = 0; tries < 48 && (frags > 0 || burnt); tries++) {
            int          tri = rand() % gore_numTris;
            const float *a   = gore_verts[gore_tris[tri * 3 + 0]].xyz;
            const float *b   = gore_verts[gore_tris[tri * 3 + 1]].xyz;
            const float *c   = gore_verts[gore_tris[tri * 3 + 2]].xyz;
            float        bary[3] = {1.0f / 3, 1.0f / 3, 1.0f / 3};
            vec3_t       centroid, nt, toBlast;

            VectorAdd(a, b, centroid);
            VectorAdd(centroid, c, centroid);
            VectorScale(centroid, 1.0f / 3, centroid);
            CG_GoreTriNormal(a, b, c, lay->winding, nt);
            VectorSubtract(blast->pos, centroid, toBlast);
            VectorNormalize(toBlast);

            if (DotProduct(nt, toBlast) < 0.3f) {
                continue;
            }

            if (!body) {
                body       = CG_GoreBody(d);
                body->dead = d->dead;
            }

            if (burnt) {
                CG_GoreLay(body, lay, GK_BURN, centroid, nt, NULL, tri, bary, GoreRand(7, 11) * scale, 0);
                burnt = 0;
                continue;
            }

            CG_GoreLay(body, lay, GK_FRAG, centroid, nt, NULL, tri, bary, GoreRand(2.5f, 3.5f) * scale, 0);
            if (!body->decals.empty() && body->decals.back().born == cg.time && body->decals.back().kind == GK_FRAG) {
                CG_GoreBleed(body, &d->ref, tri, GK_FRAG);
            }
            frags--;
        }
    }
}

void CG_GoreNoteHit(const vec3_t pos, const vec3_t dir, int large)
{
    goreHit_t *hit;

    if (!CG_GoreEnabled()) {
        return;
    }

    if (gore_numHits >= GORE_MAX_HITS) {
        memmove(gore_hits, gore_hits + 1, sizeof(gore_hits[0]) * (GORE_MAX_HITS - 1));
        gore_numHits--;
    }

    hit = &gore_hits[gore_numHits++];
    VectorCopy(pos, hit->pos);
    VectorCopy(dir, hit->dir);
    VectorNormalize(hit->dir);
    hit->large = large;
    hit->time  = cg.time;
}

void CG_GoreNoteExplosion(const vec3_t pos, int kind)
{
    goreBlast_t *blast;

    if (!CG_GoreEnabled() || gore_numBlasts >= GORE_MAX_BLASTS) {
        return;
    }

    blast = &gore_blasts[gore_numBlasts++];
    VectorCopy(pos, blast->pos);
    blast->kind = kind;
    blast->time = cg.time;
}

//=============================================================
// Blood: drops, splashes and pools
//=============================================================

static void CG_GoreSpawnDrop(const vec3_t pos, const vec3_t vel, float size, const vec3_t light)
{
    goreDrop_t *drop = &gore_drops[gore_nextDrop];

    gore_nextDrop = (gore_nextDrop + 1) % GORE_MAX_DROPS;

    drop->active = qtrue;
    VectorCopy(pos, drop->p);
    VectorCopy(vel, drop->v);
    VectorCopy(light, drop->light);
    drop->size = size;
    drop->born = cg.time;
}

static void CG_GoreStartPool(goreBody_t *body)
{
    const goreDecal_t *source = NULL;
    trace_t            tr;
    vec3_t             start, end;
    int                i, wounds = 0;

    body->pooled = qtrue;

    // Under the worst of the wounds still to be seen.
    for (i = 0; i < (int)body->decals.size(); i++) {
        const goreDecal_t &decal = body->decals[i];

        if (decal.hidden || decal.kind == GK_RUN || decal.kind == GK_BURN) {
            continue;
        }

        wounds++;
        if (!source || decal.kind == GK_EXIT) {
            source = &decal;
        }
    }

    if (!source) {
        return;
    }

    VectorCopy(source->centre, start);
    start[2] += 4;
    VectorCopy(start, end);
    end[2] -= 64;

    CG_Trace(&tr, start, vec3_origin, vec3_origin, end, ENTITYNUM_NONE, MASK_SOLID, qfalse, qfalse, "gore pool");
    if (tr.fraction >= 1.0f || tr.startsolid || tr.plane.normal[2] < 0.7f) {
        return;
    }

    if (CG_PointContents(tr.endpos, 0) & MASK_WATER) {
        return;
    }

    for (i = 0; i < GORE_MAX_POOLS; i++) {
        gorePool_t *pool = &gore_pools[i];

        if (pool->active) {
            continue;
        }

        pool->active = qtrue;
        VectorCopy(tr.endpos, pool->pos);
        VectorCopy(tr.plane.normal, pool->normal);
        pool->rot    = GoreRand(0, 360);
        pool->radius = (12.0f + 3.0f * Q_min(wounds, 8)) * cg_gore_scale->value;
        pool->born   = cg.time;
        pool->grow   = (int)GoreRand(9000, 15000);
        return;
    }
}

static void CG_GoreUpdatePools(void)
{
    int i;

    for (i = 0; i < GORE_MAX_POOLS; i++) {
        gorePool_t *pool = &gore_pools[i];
        float       f, r;

        if (!pool->active) {
            continue;
        }

        f = (float)(cg.time - pool->born) / pool->grow;
        if (f >= 1.0f || f < 0) {
            // grown: left as a mark like any other
            CG_ImpactMark(
                gore_poolShader, pool->pos, pool->normal, pool->rot, pool->radius, pool->radius, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, 1, qfalse,
                qfalse, qtrue, qfalse, 0.5f, 0.5f
            );
            pool->active = qfalse;
            continue;
        }

        if (Distance(pool->pos, cg.refdef.vieworg) > cg_gore_drawDist->value) {
            continue;
        }

        // spreads quickly, then slows
        r = pool->radius * (0.12f + 0.88f * (1.0f - (1.0f - f) * (1.0f - f)));
        CG_ImpactMark(
            gore_poolShader, pool->pos, pool->normal, pool->rot, r, r, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, 1, qfalse, qtrue, qtrue, qfalse, 0.5f, 0.5f
        );
    }
}

static void CG_GoreUpdateDrops(void)
{
    float dt = cg.frametime * 0.001f;
    int   i;

    gore_splatTokens = Q_min(gore_splatTokens + dt * 12.0f, 12.0f);

    if (dt <= 0) {
        dt = 0;
    }

    for (i = 0; i < GORE_MAX_DROPS; i++) {
        goreDrop_t *drop = &gore_drops[i];
        trace_t     tr;
        vec3_t      next;
        vec3_t      along, side, toView;
        polyVert_t  verts[4];
        float       speed, len;
        int         k;

        if (!drop->active) {
            continue;
        }

        if (cg.time - drop->born > 4000) {
            drop->active = qfalse;
            continue;
        }

        drop->v[2] -= GORE_GRAVITY * dt;
        VectorMA(drop->p, dt, drop->v, next);

        CG_Trace(&tr, drop->p, vec3_origin, vec3_origin, next, ENTITYNUM_NONE, MASK_SOLID | MASK_WATER, qfalse, qfalse, "gore drop");
        if (tr.fraction < 1.0f || tr.startsolid) {
            drop->active = qfalse;

            if (!tr.startsolid && !(tr.contents & MASK_WATER) && gore_splatTokens >= 1.0f) {
                gore_splatTokens -= 1.0f;
                CG_ImpactMark(
                    gore_splatShader, tr.endpos, tr.plane.normal, GoreRand(0, 360), drop->size * GoreRand(1.5f, 2.6f),
                    drop->size * GoreRand(1.5f, 2.6f), GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, 1, qfalse, qfalse, qtrue, qfalse, 0.5f, 0.5f
                );
            }
            continue;
        }

        VectorCopy(next, drop->p);

        // a streak along its way, turned to face the view
        VectorSubtract(cg.refdef.vieworg, drop->p, toView);
        VectorNormalize(toView);
        VectorCopy(drop->v, along);
        VectorMA(along, -DotProduct(along, toView), toView, along);
        speed = VectorNormalize(along);
        if (speed < 1.0f) {
            VectorCopy(cg.refdef.viewaxis[2], along);
        }
        CrossProduct(along, toView, side);
        VectorNormalize(side);
        len = drop->size * (1.0f + speed * 0.012f);

        for (k = 0; k < 4; k++) {
            float a = (k == 0 || k == 3) ? -len : len;
            float s = (k < 2) ? -drop->size : drop->size;

            verts[k].xyz[0] = drop->p[0] + along[0] * a + side[0] * s;
            verts[k].xyz[1] = drop->p[1] + along[1] * a + side[1] * s;
            verts[k].xyz[2] = drop->p[2] + along[2] * a + side[2] * s;
            verts[k].st[0]  = (k < 2) ? 0 : 1;
            verts[k].st[1]  = (k == 0 || k == 3) ? 0 : 1;
            verts[k].modulate[0] = (byte)Q_min(drop->light[0] * GORE_LIGHT_SCALE, 255.f);
            verts[k].modulate[1] = (byte)Q_min(drop->light[1] * GORE_LIGHT_SCALE, 255.f);
            verts[k].modulate[2] = (byte)Q_min(drop->light[2] * GORE_LIGHT_SCALE, 255.f);
            verts[k].modulate[3] = 255;
        }

        cgi.R_AddPolyToScene(gore_dropShader, 4, verts, 0);
    }
}

// Blood out of the wounds of one body: drops while a wound bleeds, and jets
// while a neck wound pumps.
static void CG_GoreBleedBody(goreBody_t *body)
{
    int i, k;

    if (!cg_gore_bleed->integer) {
        return;
    }

    for (i = 0; i < (int)body->decals.size(); i++) {
        goreDecal_t &decal = body->decals[i];
        vec3_t       pos, vel;

        if (decal.hidden || cg.time > decal.bleedUntil) {
            continue;
        }

        VectorMA(decal.centre, 0.5f, decal.normal, pos);

        if (cg.time < decal.spurtUntil && cg.time >= decal.nextSpurt) {
            decal.nextSpurt = cg.time + GORE_SPURT_PERIOD + (int)GoreRand(-80, 80);

            for (k = 0; k < 7; k++) {
                VectorScale(decal.normal, GoreRand(60, 130), vel);
                vel[0] += crandom() * 25;
                vel[1] += crandom() * 25;
                vel[2] += GoreRand(10, 40);
                CG_GoreSpawnDrop(pos, vel, GoreRand(0.5f, 0.9f), decal.light);
            }
        }

        // A dead man's blood goes into his pool, once it has started.
        if (body->pooled || cg.time < decal.nextDrop) {
            continue;
        }

        {
            int   left = decal.bleedUntil - decal.born;
            float f    = left > 0 ? (float)(cg.time - decal.born) / left : 1.0f;

            decal.nextDrop = cg.time + (int)(GoreRand(200, 450) + f * GoreRand(600, 1400));
        }

        VectorScale(decal.normal, GoreRand(3, 12), vel);
        CG_GoreSpawnDrop(pos, vel, GoreRand(0.35f, 0.65f), decal.light);
    }
}

//=============================================================
// Drawing
//=============================================================

// Puts every wound of a body where its skin is now. Returns qfalse if the body
// could not be skinned.
static qboolean CG_GorePose(goreBody_t *body, const goreDrawn_t *drawn)
{
    const goreLayout_t *lay = CG_GoreLayout(&drawn->ref);
    unsigned int        visible, mask = 0, hash;
    int                 i, t, k;

    if (!lay) {
        return qfalse;
    }

    visible = CG_GoreVisibleMask(&drawn->ref, lay);
    for (i = 0; i < (int)body->decals.size(); i++) {
        mask |= body->decals[i].surfMask;
    }
    mask &= visible;

    hash = CG_GorePoseHash(&drawn->ref);
    if (!body->dirty && hash == body->poseHash && mask == body->poseMask) {
        return qtrue;
    }

    if (cg_gore_debug->integer > 3) {
        unsigned int all = 0;

        for (i = 0; i < (int)body->decals.size(); i++) {
            all |= body->decals[i].surfMask;
        }
        Com_Printf(
            "gore: entity %d reposed (dirty %d, hash %s, mask %x of %x, visible %x, overrides %d, frame %d %.3f)\n",
            body->entityNum, body->dirty, hash == body->poseHash ? "same" : "changed", mask, all, visible,
            drawn->ref.num_bone_overrides, drawn->ref.frameInfo[0].index, drawn->ref.frameInfo[0].time
        );
    }

    body->poseHash = hash;
    body->poseMask = mask;
    body->dirty    = qfalse;

    if (!mask) {
        for (i = 0; i < (int)body->decals.size(); i++) {
            body->decals[i].hidden = qtrue;
        }
        return qtrue;
    }

    if (!CG_GoreSkin(&drawn->ref, lay, mask)) {
        body->dirty = qtrue;
        return qfalse;
    }
    CG_GoreSmoothNormals(lay);

    for (i = 0; i < (int)body->decals.size(); i++) {
        goreDecal_t &decal = body->decals[i];

        decal.hidden = (decal.surfMask & ~mask) || !(mask & (1u << decal.anchor.surf)) ? qtrue : qfalse;
        if (decal.hidden) {
            continue;
        }

        decal.xyz.resize(decal.frags.size() * GORE_FRAG_PTS * 3);
        for (t = 0; t < (int)decal.frags.size(); t++) {
            const goreFrag_t &frag = decal.frags[t];
            const goreTri_t  &tri  = frag.tri;
            const float     *p[3], *pn[3];

            for (k = 0; k < 3; k++) {
                p[k]  = CG_GoreVert(&tri, k);
                pn[k] = CG_GoreNormal(&tri, k);
            }

            for (k = 0; k < frag.numPts; k++) {
                float *out = &decal.xyz[(t * GORE_FRAG_PTS + k) * 3];
                vec3_t n;
                int    c;

                for (c = 0; c < 3; c++) {
                    n[c] = pn[0][c] * frag.bary[k][0] + pn[1][c] * frag.bary[k][1] + pn[2][c] * frag.bary[k][2];
                }
                VectorNormalize(n);

                for (c = 0; c < 3; c++) {
                    out[c] = p[0][c] * frag.bary[k][0] + p[1][c] * frag.bary[k][1] + p[2][c] * frag.bary[k][2]
                           + n[c] * GORE_LIFT;
                }
            }
        }

        {
            const float *a = CG_GoreVert(&decal.anchor, 0);
            const float *b = CG_GoreVert(&decal.anchor, 1);
            const float *c = CG_GoreVert(&decal.anchor, 2);

            for (k = 0; k < 3; k++) {
                decal.centre[k] = a[k] * decal.bary[0] + b[k] * decal.bary[1] + c[k] * decal.bary[2];
            }
            CG_GoreTriNormal(a, b, c, lay->winding, decal.normal);
        }

        if (!decal.lightTime || cg.time - decal.lightTime > 250) {
            cgi.R_GetLightingForDecal(decal.light, decal.normal, decal.centre);
            decal.lightTime = cg.time;
        }
    }

    return qtrue;
}

static int CG_GoreDrawBody(goreBody_t *body, int budget)
{
    polyVert_t verts[GORE_FRAG_PTS];
    int        used = 0;
    int        i, t, k;

    for (i = 0; i < (int)body->decals.size() && used < budget; i++) {
        goreDecal_t &decal = body->decals[i];
        byte         rgba[4];
        float        f    = (float)(cg.time - decal.soakStart) / Q_max(decal.soakTime, 1);
        float        grow, fade;

        if (decal.hidden || decal.xyz.size() != decal.frags.size() * GORE_FRAG_PTS * 3 || f <= 0) {
            continue;
        }

        // Spreading, quickly and then slowing: the texture is drawn smaller
        // about its middle (a run: from its top) and grows to its size. It is
        // clamped and fades out inside its edge, so the stain simply spreads.
        f = Q_min(f, 1.0f);
        f = 1.0f - (1.0f - f) * (1.0f - f);
        if (decal.kind == GK_RUN) {
            grow = 0.05f + 0.95f * f;
            fade = Q_min(f * 4.0f, 1.0f);
        } else if (decal.soakTime > GORE_SKIN_FADE) {
            grow = 0.15f + 0.85f * f;
            fade = 0.4f + 0.6f * f;
        } else {
            grow = 1.0f;
            fade = f;
        }

        for (k = 0; k < 3; k++) {
            rgba[k] = cg_gore_debug->integer > 2 ? 255
                                                  : (byte)Q_min(decal.light[k] * GORE_LIGHT_SCALE * decal.tint[k], 255.f);
        }
        rgba[3] = (byte)(255 * fade);

        for (t = 0; t < (int)decal.frags.size() && used < budget; t++) {
            const goreFrag_t &frag = decal.frags[t];

            for (k = 0; k < frag.numPts; k++) {
                VectorCopy(&decal.xyz[(t * GORE_FRAG_PTS + k) * 3], verts[k].xyz);
                if (decal.kind == GK_RUN) {
                    verts[k].st[0] = frag.st[k][0];
                    verts[k].st[1] = frag.st[k][1] / grow;
                } else {
                    verts[k].st[0] = 0.5f + (frag.st[k][0] - 0.5f) / grow;
                    verts[k].st[1] = 0.5f + (frag.st[k][1] - 0.5f) / grow;
                }
                memcpy(verts[k].modulate, rgba, 4);
            }

            cgi.R_AddPolyToScene(gore_shaders[decal.kind][decal.variant], frag.numPts, verts, 0);
            used++;
        }
    }

    return used;
}

static double gore_usTotal;
static int    gore_usFrames;

void CG_GoreAddToScene(void)
{
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    int order[GORE_MAX_DRAWN];
    float dist[GORE_MAX_DRAWN];
    int   numOrder = 0;
    int   budget;
    int   i, j;

    if (!CG_GoreEnabled()) {
        gore_numDrawn  = 0;
        gore_numHits   = 0;
        gore_numBlasts = 0;
        return;
    }

    // New wounds, on the bodies as they are this frame. A hit waits a little
    // for its body, which may not have been drawn yet.
    for (i = 0, j = 0; i < gore_numHits; i++) {
        if (CG_GorePlaceHit(&gore_hits[i]) || cg.time - gore_hits[i].time > GORE_HIT_TTL || cg.time < gore_hits[i].time) {
            continue;
        }
        gore_hits[j++] = gore_hits[i];
    }
    gore_numHits = j;

    for (i = 0; i < gore_numBlasts; i++) {
        CG_GorePlaceBlast(&gore_blasts[i]);
    }
    gore_numBlasts = 0;

    // Nearest first, so the budget runs out on the far ones.
    for (i = 0; i < gore_numDrawn; i++) {
        goreDrawn_t *drawn = &gore_drawn[i];
        goreBody_t  *body  = CG_GoreFindBody(drawn->entityNum);
        vec3_t       toBody;
        float        d;

        if (!body || body->decals.empty() || drawn->noDraw) {
            continue;
        }

        VectorSubtract(drawn->ref.origin, cg.refdef.vieworg, toBody);
        d = VectorLength(toBody);
        if (d > cg_gore_drawDist->value || DotProduct(toBody, cg.refdef.viewaxis[0]) < -96.0f) {
            continue;
        }

        for (j = numOrder; j > 0 && dist[j - 1] > d; j--) {
            order[j] = order[j - 1];
            dist[j]  = dist[j - 1];
        }
        order[j] = i;
        dist[j]  = d;
        numOrder++;
    }

    budget = cg_gore_maxPolys->integer;
    for (i = 0; i < numOrder; i++) {
        goreDrawn_t *drawn = &gore_drawn[order[i]];
        goreBody_t  *body  = CG_GoreFindBody(drawn->entityNum);

        if (!CG_GorePose(body, drawn)) {
            continue;
        }

        CG_GoreBleedBody(body);

        if (body->dead && !body->pooled && cg_gore_pools->integer && cg.time - body->deathTime > GORE_POOL_DELAY) {
            CG_GoreStartPool(body);
        }

        if (budget > 0) {
            budget -= CG_GoreDrawBody(body, budget);
        }
    }

    CG_GoreUpdateDrops();
    CG_GoreUpdatePools();

    gore_usTotal += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
    gore_usFrames++;

    if (cg_gore_debug->integer > 1) {
        static int nextReport;

        if (cg.time >= nextReport || cg.time < nextReport - 2000) {
            int drops = 0, hidden = 0, decals = 0;

            nextReport = cg.time + 1000;
            for (i = 0; i < GORE_MAX_DROPS; i++) {
                drops += gore_drops[i].active;
            }
            for (i = 0; i < GORE_MAX_BODIES; i++) {
                for (j = 0; j < (int)gore_bodies[i].decals.size(); j++) {
                    decals++;
                    hidden += gore_bodies[i].decals[j].hidden;
                }
            }
            Com_Printf(
                "gore: %d characters drawn, %d bodies wounded in view, %d wounds (%d hidden), %d polys, %d drops, %.0f us a frame\n",
                gore_numDrawn, numOrder, decals, hidden, cg_gore_maxPolys->integer - budget, drops,
                gore_usFrames ? gore_usTotal / gore_usFrames : 0.0
            );
            gore_usTotal  = 0;
            gore_usFrames = 0;
        }
    }

    gore_numDrawn = 0;
}
