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
#include "cg_physics.h"
#include "cg_physics_ragdoll.h"
#include "cg_ragdoll.h"

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
#define GORE_MAX_GIBS       32
#define GORE_MAX_OVERRIDES  160 // the ragdoll's and the cut bones'
#define GORE_GIB_KEY        MAX_GENTITIES // a gib's body is keyed from here up
#define GORE_GIB_LIGHT_UP   12.f // lit from above its middle: on the ground, the grid there is dark
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
    GK_STUMP, // where a limb came off
    GK_BRAIN, // the inside of a head a round has opened
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
    "gore/wound_entry%d", "gore/wound_exit%d", "gore/wound_frag%d", "gore/wound_run", "gore/burn", "gore/stump",
    "gore/brain"
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

// The parts a man can lose, each cut off at the joint it hangs from.
typedef enum {
    GP_HEAD,
    GP_L_UPPERARM,
    GP_L_FOREARM,
    GP_L_HAND,
    GP_R_UPPERARM,
    GP_R_FOREARM,
    GP_R_HAND,
    GP_L_THIGH,
    GP_L_CALF,
    GP_R_THIGH,
    GP_R_CALF,
    GP_NUM
} gorePart_t;

// Where a round went in, when it is not one of the parts.
#define GORE_ZONE_NONE -1
#define GORE_ZONE_NECK GP_NUM

// The surfaces of a model, each as R_GetSkinnedMesh hands it over.
typedef struct {
    int   numSurfs;
    int   numVerts[MAX_MODEL_SURFACES];
    int   numTris[MAX_MODEL_SURFACES];
    int   firstVert[MAX_MODEL_SURFACES]; // in the whole mesh
    float winding; // turns a triangle's (b - a) x (c - a) outward

    // The vertex each one is welded to, in the whole mesh: the skin is cut
    // along texture seams into vertices that sit on one another, and they
    // must share a normal or a decal parts along the seam.
    std::vector<int> weld;
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

    // Severing and dents (see there)
    qboolean       gib;            // a severed part's own body
    int            pelvis;         // its pelvis bone, -1 if none, -2 not looked for yet
    vec3_t         streakFrom;     // where its last blood streak ended (CG_GoreStreak)
    qboolean       streaking;
    unsigned int   severed;        // parts gone (gorePart_t bits)
    unsigned int   severPending;   // parts to cut off when next drawn
    unsigned int   explodePending; // of those, the ones that go to pieces
    unsigned int   stumpPending;   // stumps still to be given their decal
    vec3_t         stumpAt[GP_NUM], stumpDir[GP_NUM]; // in the world, when cut
    float          stumpCut[GP_NUM][3];               // the joint, in model space before the scale
    // The opening each part came out of, the end of what is left (the end of
    // a sleeve, not the shoulder joint inside it), and the way it faces, in
    // the frame of the bone above; and how wide.
    vec3_t         capAt[GP_NUM], capDir[GP_NUM];
    float          capRadius[GP_NUM];
    int            partHits[GP_NUM]; // rounds into each part of a corpse
    goreDent_t     dents[MAX_GORE_DENTS];
    vec3_t         dentOut[MAX_GORE_DENTS]; // the way each opens, in the head bone's frame
    int            numDents;
    unsigned int   dentPending; // dents whose hollow still needs its decal
    boneOverride_t ovr[GORE_MAX_OVERRIDES];
    int            numOvr;

    // the last round into him, for the one that killed him
    int      lastHitTime, lastHitZone, lastHitLarge;
    vec3_t   lastHitPos, lastHitDir, lastHitExitPos;
    qboolean lastHitExit;
    // and the last blast near him
    int      blastTime, blastKind;
    float    blastNear; // 1 at the blast, 0 at the edge of its reach
    vec3_t   blastPos;
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
    qboolean chunk; // a piece of a head, not a drop of blood
    vec3_t   p, v;
    float    size;
    int      born;
    vec3_t   light;
} goreDrop_t;

// A part cut off, flying and then lying where it fell: a copy of the body with
// every bone but the part's folded into the joint it came away at.
//
// It is a ragdoll of its own: a point at each joint down the part and one at
// its end, kept their lengths apart, falling and catching on the world. Each
// bone of that chain is posed between its two points; every other bone of the
// part rides on the nearest of them, as it was when it came off.
#define GORE_CHAIN_PTS 4

typedef struct {
    qboolean       active;
    int            key; // its body's entity number, GORE_GIB_KEY and up
    int            part;
    refEntity_t    ref;
    boneOverride_t ovr[GORE_MAX_OVERRIDES];
    int            numOvr, numFolded; // the first numFolded are folded into the cut
    goreDent_t     dents[MAX_GORE_DENTS];
    vec3_t         centre; // the middle of the part, in the world
    vec3_t         vel;
    vec3_t         spin; // axis, at its length in degrees a second
    int            born;
    qboolean       resting;

    // the chain
    int    numPts;
    vec3_t pt[GORE_CHAIN_PTS], old[GORE_CHAIN_PTS];
    float  len[GORE_CHAIN_PTS];
    int    carrierOvr[GORE_CHAIN_PTS]; // the bone from each point, in ovr
    vec3_t carrierDir[GORE_CHAIN_PTS]; // which way it pointed, model space
    int    numRiders;
    int    riderOvr[GORE_MAX_OVERRIDES], riderOf[GORE_MAX_OVERRIDES];
    float  riderLocal[GORE_MAX_OVERRIDES][4][3];
    float  stepLeft;
    int    still;
    int    jolt; // its chain in the physics world (CG_JoltChainCreate), or 0
    vec3_t streakFrom; // where its last blood streak ended (CG_GoreStreak)
    qboolean streaking;
} goreGib_t;

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
static cvar_t *cg_gore_dismember;
static cvar_t *cg_gore_maxGibs;
static cvar_t *com_blood_gore;

static qhandle_t gore_shaders[GK_NUM][GORE_VARIANTS];
static qhandle_t gore_dropShader, gore_splatShader, gore_poolShader, gore_chunkShader, gore_streakShader;

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
static goreGib_t   gore_gibs[GORE_MAX_GIBS];
static float       gore_splatTokens;

static std::map<dtiki_t *, goreLayout_t> gore_layouts;

// A model's skeleton as far as cutting it goes: for each part, the bone cut
// off at its joint (the first) and every bone that goes with it.
typedef struct {
    qboolean         valid;
    dtiki_t         *tiki;
    int              root;
    int              numBones;
    int              cut[GP_NUM];
    int              parent[GP_NUM]; // the bone a part hangs from
    std::vector<int> bones[GP_NUM];
    unsigned int     within[GP_NUM]; // the parts that go with each
} goreRig_t;

static std::map<dtiki_t *, goreRig_t> gore_rigs;

static void CG_GoreSpawnDrop(const vec3_t pos, const vec3_t vel, float size, const vec3_t light);
static void CG_GoreKillShot(goreBody_t *body, refEntity_t *model);
static void CG_GoreBlastBody(goreBody_t *body);
static void CG_GoreCorpseHit(goreBody_t *body, refEntity_t *model);
static int  CG_GoreZone(const char *bone);
static void CG_GoreDent(goreBody_t *body, refEntity_t *model, const vec3_t at, const vec3_t out, float radius);
static void CG_GoreHeadHit(goreBody_t *body, refEntity_t *model);
static void CG_GoreChainStart(goreGib_t *gib, const goreRig_t *rig);
static void CG_GoreSpinAxis(vec3_t axis[3], const vec3_t spin, float dt);
static void CG_GoreRegisterBits(void);

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
    cg_gore_dismember = cgi.Cvar_Get("cg_gore_dismember", "1", CVAR_ARCHIVE);
    cg_gore_maxGibs   = cgi.Cvar_Get("cg_gore_maxGibs", "24", CVAR_ARCHIVE);
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
    gore_chunkShader = cgi.R_RegisterShader("gore/chunk");
    gore_streakShader = cgi.R_RegisterShader("gore/streak");
    CG_GoreRegisterBits();

    memset(gore_drops, 0, sizeof(gore_drops));
    memset(gore_pools, 0, sizeof(gore_pools));
    for (i = 0; i < GORE_MAX_GIBS; i++) {
        CG_JoltChainDestroy(gore_gibs[i].jolt);
    }
    memset(gore_gibs, 0, sizeof(gore_gibs));
    gore_rigs.clear();
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

    body->gib            = qfalse;
    body->severed        = 0;
    body->severPending   = 0;
    body->explodePending = 0;
    body->stumpPending   = 0;

    body->numDents       = 0;
    body->dentPending    = 0;
    body->numOvr         = 0;
    body->lastHitTime    = 0;
    body->blastTime      = 0;
    body->pelvis         = -2;
    body->streaking      = qfalse;
    memset(body->partHits, 0, sizeof(body->partHits));
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

// The body an entity has, if it is still the same man: the slot can have gone
// to someone else, another model, or the dead man is up again (a respawn), or
// a living one has been away too long. Notes his death when it comes.
static goreBody_t *CG_GoreBodyFor(int key, refEntity_t *model, qboolean dead)
{
    goreBody_t *body = CG_GoreFindBody(key);

    if (!body) {
        return NULL;
    }

    if (body->tiki != model->tiki || (body->dead && !dead) || (!dead && cg.time - body->lastSeen > 10000)) {
        CG_GoreFreeBody(body);
        return NULL;
    }

    body->lastSeen = cg.time;
    if (dead && !body->dead) {
        body->dead      = qtrue;
        body->deathTime = cg.time;

        // what killed him
        if (body->lastHitTime && cg.time - body->lastHitTime < 500) {
            CG_GoreKillShot(body, model);
        }
        if (body->blastTime && cg.time - body->blastTime < 700) {
            CG_GoreBlastBody(body);
        }
    }

    return body;
}

static void CG_GoreAddEntityKey(int key, const refEntity_t *model, qboolean dead)
{
    goreDrawn_t *drawn;

    if (gore_numDrawn >= GORE_MAX_DRAWN) {
        return;
    }

    drawn            = &gore_drawn[gore_numDrawn++];
    drawn->entityNum = key;
    drawn->ref       = *model;
    drawn->dead      = dead;
    drawn->noDraw    = (model->renderfx & RF_THIRD_PERSON) && !cg.renderingThirdPerson ? qtrue : qfalse;

    CG_GoreBodyFor(key, &drawn->ref, dead);
}

void CG_GoreAddEntity(centity_t *cent, const refEntity_t *model, qboolean dead)
{
    if (!CG_GoreEnabled() || !model->tiki || !model->tiki->a || !model->tiki->a->bIsCharacter) {
        return;
    }

    if (model->renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK)) {
        return;
    }

    CG_GoreAddEntityKey(cent->currentState.number, model, dead);
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
    lay->numSurfs = 0;
    lay->winding  = 1.0f;
    lay->weld.clear();
    memset(lay->numVerts, 0, sizeof(lay->numVerts));
    memset(lay->numTris, 0, sizeof(lay->numTris));
    memset(lay->firstVert, 0, sizeof(lay->firstVert));

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

    {
        std::map<long long, int> seen;
        int                      first = 0, surf;

        for (surf = 0; surf < MAX_MODEL_SURFACES; surf++) {
            lay->firstVert[surf] = first;
            first += lay->numVerts[surf];
        }

        lay->weld.resize(n);
        for (k = 0; k < n; k++) {
            const float *v   = gore_verts[k].xyz;
            long long    key = ((long long)floorf(v[0] * 100.0f) * 73856093LL) ^ ((long long)floorf(v[1] * 100.0f) * 19349663LL)
                          ^ ((long long)floorf(v[2] * 100.0f) * 83492791LL);
            std::map<long long, int>::iterator found = seen.find(key);

            if (found == seen.end()) {
                seen[key]    = k;
                lay->weld[k] = k;
            } else {
                lay->weld[k] = found->second;
            }
        }
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

// The surface a vertex of the whole mesh is on.
static int CG_GoreSurfaceOf(const goreLayout_t *lay, int vert)
{
    int surf;

    for (surf = 0; surf < lay->numSurfs; surf++) {
        if (vert >= lay->firstVert[surf] && vert < lay->firstVert[surf] + lay->numVerts[surf]) {
            return surf;
        }
    }

    return -1;
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

    // Seam vertices take the sum of their welded group: gathered on the one
    // they are welded to (when its surface was skinned too), then handed back.
    {
        int surf, l;

        for (surf = 0; surf < lay->numSurfs; surf++) {
            if (gore_vertOfs[surf] < 0) {
                continue;
            }
            for (l = 0; l < lay->numVerts[surf]; l++) {
                int full = lay->firstVert[surf] + l;
                int to, toSurf, sub;

                if (full >= (int)lay->weld.size() || (to = lay->weld[full]) == full) {
                    continue;
                }
                toSurf = CG_GoreSurfaceOf(lay, to);
                if (toSurf < 0 || gore_vertOfs[toSurf] < 0) {
                    continue;
                }
                sub = gore_vertOfs[toSurf] + (to - lay->firstVert[toSurf]);
                VectorAdd(gore_normals[sub], gore_normals[gore_vertOfs[surf] + l], gore_normals[sub]);
            }
        }

        for (surf = 0; surf < lay->numSurfs; surf++) {
            if (gore_vertOfs[surf] < 0) {
                continue;
            }
            for (l = 0; l < lay->numVerts[surf]; l++) {
                int full = lay->firstVert[surf] + l;
                int to, toSurf;

                if (full >= (int)lay->weld.size() || (to = lay->weld[full]) == full) {
                    continue;
                }
                toSurf = CG_GoreSurfaceOf(lay, to);
                if (toSurf >= 0 && gore_vertOfs[toSurf] >= 0) {
                    VectorCopy(gore_normals[gore_vertOfs[toSurf] + (to - lay->firstVert[toSurf])], gore_normals[gore_vertOfs[surf] + l]);
                }
            }
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

    // the dents change the mesh as much as the pose does
    if ((ref->renderfx & RF_GORE_DENTS) && ref->gore_dents) {
        GORE_HASH_INT(ref->num_gore_dents);
    }

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
    } else if (decal.cloth && kind != GK_BURN && kind != GK_STUMP && kind != GK_BRAIN) {
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

        // folded away with a part that is gone
        {
            vec3_t e1, e2, c;

            VectorSubtract(p[1], p[0], e1);
            VectorSubtract(p[2], p[0], e2);
            CrossProduct(e1, e2, c);
            if (VectorLengthSquared(c) < 0.0001f) {
                continue;
            }
        }

        // Either way round: a head or a hand often has triangles wound against
        // the rest, and leaving them out cuts holes in the decal. What faces
        // away (the far side of an arm) is kept out by the depth.
        CG_GoreTriNormal(p[0], p[1], p[2], lay->winding, nt);
        // a stump or a hollow is laid over whatever is there
        if (fabs(DotProduct(nt, normal)) < (kind == GK_BRAIN ? 0.0f : (kind == GK_STUMP ? 0.15f : 0.2f))) {
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

    // For what it does to him if it kills him, or to a corpse.
    {
        int v = gore_tris[tr.entryTri * 3];

        body->lastHitTime  = cg.time;
        body->lastHitLarge = hit->large;
        body->lastHitZone  = gore_verts[v].bone >= 0 ? CG_GoreZone(cgi.Tag_NameForNum(best->ref.tiki, gore_verts[v].bone))
                                                     : GORE_ZONE_NONE;
        VectorCopy(entry, body->lastHitPos);
        VectorCopy(hit->dir, body->lastHitDir);
        body->lastHitExit = tr.exitTri >= 0 ? qtrue : qfalse;
        if (body->lastHitExit) {
            VectorMA(ray, tr.tExit, hit->dir, body->lastHitExitPos);
        }

        if (!body->gib) {
            refEntity_t ref = best->ref;

            // Every round into a head breaks it, alive or dead.
            if (body->lastHitZone == GP_HEAD) {
                CG_GoreHeadHit(body, &ref);
            }
            if (body->dead) {
                CG_GoreCorpseHit(body, &ref);
            }
        }
    }

    return qtrue;
}

// Lays a decal of kind about a point, on the triangle of the body nearest it.
static qboolean CG_GoreLayNear(goreBody_t *body, const goreDrawn_t *drawn, int kind, const vec3_t at, const vec3_t normal, float radius)
{
    const goreLayout_t *lay = CG_GoreLayout(&drawn->ref);
    float               bary[3] = {1.0f / 3, 1.0f / 3, 1.0f / 3};
    float               bestDist = 0;
    size_t              before = body->decals.size();
    int                 i, best = -1;

    if (!lay || !CG_GoreSkin(&drawn->ref, lay, CG_GoreVisibleMask(&drawn->ref, lay))) {
        return qfalse;
    }

    for (i = 0; i < gore_numTris; i++) {
        const float *a = gore_verts[gore_tris[i * 3 + 0]].xyz;
        const float *b = gore_verts[gore_tris[i * 3 + 1]].xyz;
        const float *c = gore_verts[gore_tris[i * 3 + 2]].xyz;
        vec3_t       mid, e1, e2, n;
        float        d;

        VectorSubtract(b, a, e1);
        VectorSubtract(c, a, e2);
        CrossProduct(e1, e2, n);
        if (VectorLengthSquared(n) < 0.0001f) {
            continue; // folded away
        }

        VectorAdd(a, b, mid);
        VectorAdd(mid, c, mid);
        VectorScale(mid, 1.0f / 3, mid);
        d = DistanceSquared(mid, at);
        if (best < 0 || d < bestDist) {
            best     = i;
            bestDist = d;
        }
    }

    if (best < 0) {
        return qfalse;
    }

    CG_GoreLay(body, lay, kind, at, normal, NULL, best, bary, radius, 0);
    return body->decals.size() > before && body->decals.back().kind == kind ? qtrue : qfalse;
}

//=============================================================
// Severing and dents
//=============================================================
//
// A part comes off by folding its bones into the joint it hangs from: each is
// given a model space transform shrunk to nothing at that joint (bone_override,
// as the ragdoll drives a skeleton), so the skin it carries goes to a point
// and the skin shared with the bone above pinches into a stump. The part
// itself flies off as a second copy of the model with the opposite done to
// it: every bone but the part's folded into the same joint, and the part's
// held in the pose it had.
//
// A head a heavy round has gone through is dented instead (goreDent_t): the
// renderer pushes the skin about the way in and the way out back to a sphere,
// and the hollow is given the inside of a head for a decal.
//
// The bones are the Biped's, by name, which every man in the game has.

// %c: L or R. A name a model does not have is passed over.
static const char *gore_handBones[] = {
    "Bip01 %c Hand", "Bip01 %c Finger0", "Bip01 %c Finger01", "Bip01 %c Finger02", "Bip01 %c Finger1",
    "Bip01 %c Finger11", "Bip01 %c Finger12", "Bip01 %c Finger2", "Bip01 %c Finger21", "Bip01 %c Finger22",
    "Bip01 %c Finger3", "Bip01 %c Finger31", "Bip01 %c Finger32", "Bip01 %c Finger4", "Bip01 %c Finger41",
    "Bip01 %c Finger42", "Bip01 %c Finger0Nub", "Bip01 %c Finger1Nub", "Bip01 %c Finger2Nub", NULL
};
static const char *gore_forearmBones[] = {"Bip01 %c Forearm", "Bip01 %c ForeTwist", "Bip01 %c ForeTwist1", NULL};
// helper Relbow hangs from the right shoulder's helpers, not from the arm
// The shoulder helpers carry the sleeve and the top of the arm on some models
// (the shirt-sleeve ones): they go with it. The right's second is named for
// the left.
static const char *gore_upperarmBones[] = {
    "Bip01 %c UpperArm", "Bip01 %c UpArmTwist", "helper %celbow", "helper %cshoulder", "helper %cshoulder01", NULL
};
static const char *gore_upperarmBonesR[] = {"helper Lshoulder02", NULL};
static const char *gore_calfBones[] = {
    "Bip01 %c Calf", "Bip01 %c Foot", "Bip01 %c Toe0", "Bip01 %c Toe0Nub", "helper %cankle", NULL
};
static const char *gore_thighBones[] = {"Bip01 %c Thigh", "helper %cknee", NULL};
static const char *gore_headBones[]  = {"Bip01 Head", "eyes bone", "Bip01 HeadNub", NULL};

static const char *gore_partNames[GP_NUM] = {
    "head", "left arm", "left forearm", "left hand", "right arm", "right forearm", "right hand",
    "left leg", "left shin", "right leg", "right shin"
};

// How wide the stump of each, in world units.
static const float gore_stumpRadius[GP_NUM] = {4.5f, 4.0f, 3.2f, 2.4f, 4.0f, 3.2f, 2.4f, 5.0f, 4.0f, 5.0f, 4.0f};

static void CG_GoreRigAdd(goreRig_t *rig, int part, const char **names, char side)
{
    int i;

    for (i = 0; names[i]; i++) {
        int bone = cgi.Tag_NumForName(rig->tiki, va(names[i], side, side));

        if (bone < 0) {
            continue;
        }

        if (rig->bones[part].empty()) {
            rig->cut[part] = bone; // the first named: the bone cut off
        }
        rig->bones[part].push_back(bone);
    }
}

static const goreRig_t *CG_GoreRig(dtiki_t *tiki)
{
    std::map<dtiki_t *, goreRig_t>::iterator it = gore_rigs.find(tiki);
    goreRig_t                               *rig;
    int                                      side, p, q;

    if (it != gore_rigs.end()) {
        return it->second.valid ? &it->second : NULL;
    }

    rig       = &gore_rigs[tiki];
    rig->tiki = tiki;
    for (p = 0; p < GP_NUM; p++) {
        rig->cut[p] = -1;
    }

    rig->root = cgi.Tag_NumForName(tiki, "Bip01");
    // Tag_NameForNum is NULL past the last
    for (rig->numBones = 0, p = 0; p < TIKI_MAX_BONES; p++) {
        if (cgi.Tag_NameForNum(tiki, p)) {
            rig->numBones = p + 1;
        }
    }
    CG_GoreRigAdd(rig, GP_HEAD, gore_headBones, ' ');

    // The arm takes the forearm and hand with it, and so on down.
    for (side = 0; side < 2; side++) {
        char c   = side ? 'R' : 'L';
        int  arm = side ? GP_R_UPPERARM : GP_L_UPPERARM;
        int  leg = side ? GP_R_THIGH : GP_L_THIGH;

        CG_GoreRigAdd(rig, arm + 2, gore_handBones, c);
        CG_GoreRigAdd(rig, arm + 1, gore_forearmBones, c);
        CG_GoreRigAdd(rig, arm + 1, gore_handBones, c);
        CG_GoreRigAdd(rig, arm, gore_upperarmBones, c);
        if (side) {
            CG_GoreRigAdd(rig, arm, gore_upperarmBonesR, c);
        }
        CG_GoreRigAdd(rig, arm, gore_forearmBones, c);
        CG_GoreRigAdd(rig, arm, gore_handBones, c);

        CG_GoreRigAdd(rig, leg + 1, gore_calfBones, c);
        CG_GoreRigAdd(rig, leg, gore_thighBones, c);
        CG_GoreRigAdd(rig, leg, gore_calfBones, c);
    }

    {
        static const char *parents[GP_NUM] = {
            "Bip01 Neck", "Bip01 L Clavicle", "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 R Clavicle",
            "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 Pelvis", "Bip01 L Thigh", "Bip01 Pelvis", "Bip01 R Thigh"
        };

        for (p = 0; p < GP_NUM; p++) {
            rig->parent[p] = cgi.Tag_NumForName(tiki, parents[p]);
        }
    }

    for (p = 0; p < GP_NUM; p++) {
        rig->within[p] = 0;
        for (q = 0; q < GP_NUM; q++) {
            if (rig->cut[q] >= 0
                && std::find(rig->bones[p].begin(), rig->bones[p].end(), rig->cut[q]) != rig->bones[p].end()) {
                rig->within[p] |= 1u << q;
            }
        }
    }

    rig->valid = rig->root >= 0 && rig->cut[GP_HEAD] >= 0 ? qtrue : qfalse;
    return rig->valid ? rig : NULL;
}

// Which part a bone belongs to, by its name.
static int CG_GoreZone(const char *bone)
{
    qboolean right;

    if (!bone) {
        return GORE_ZONE_NONE;
    }

    if (Q_stristr(bone, "head") || Q_stristr(bone, "eye")) {
        return GP_HEAD;
    }
    if (Q_stristr(bone, "neck")) {
        return GORE_ZONE_NECK;
    }

    right = Q_stristr(bone, " R ") || Q_stristr(bone, "helper R") ? qtrue : qfalse;
    if (!right && !Q_stristr(bone, " L ") && !Q_stristr(bone, "helper L")) {
        return GORE_ZONE_NONE;
    }

    if (Q_stristr(bone, "hand") || Q_stristr(bone, "finger")) {
        return right ? GP_R_HAND : GP_L_HAND;
    }
    if (Q_stristr(bone, "forearm") || Q_stristr(bone, "foretwist") || Q_stristr(bone, "elbow")) {
        return right ? GP_R_FOREARM : GP_L_FOREARM;
    }
    if (Q_stristr(bone, "upperarm") || Q_stristr(bone, "uparm")) {
        return right ? GP_R_UPPERARM : GP_L_UPPERARM;
    }
    if (Q_stristr(bone, "calf") || Q_stristr(bone, "foot") || Q_stristr(bone, "toe") || Q_stristr(bone, "knee")
        || Q_stristr(bone, "ankle")) {
        return right ? GP_R_CALF : GP_L_CALF;
    }
    if (Q_stristr(bone, "thigh") || Q_stristr(bone, "hip")) {
        return right ? GP_R_THIGH : GP_L_THIGH;
    }

    return GORE_ZONE_NONE;
}

static float CG_GoreModelScale(const refEntity_t *model)
{
    return model->scale * (model->tiki ? model->tiki->load_scale : 1.0f);
}

// A bone as posed for model, in model space before the scale (the space of
// bone_override and goreDent_t), and where it is in the world.
static qboolean CG_GoreBoneFrame(refEntity_t *model, int bone, float m[4][3], vec3_t world)
{
    orientation_t o;
    float         scale = CG_GoreModelScale(model);
    int           k;

    if (bone < 0 || scale <= 0) {
        return qfalse;
    }

    o = cgi.TIKI_Orientation(model, bone);
    if (VectorLengthSquared(o.axis[0]) < 0.25f) {
        return qfalse; // folded already, or no such bone
    }

    for (k = 0; k < 3; k++) {
        VectorCopy(o.axis[k], m[k]);
        m[3][k] = o.origin[k] / scale - model->tiki->load_origin[k];
    }

    if (world) {
        VectorCopy(model->origin, world);
        for (k = 0; k < 3; k++) {
            VectorMA(world, o.origin[k], model->axis[k], world);
        }
    }

    return qtrue;
}

static void CG_GoreModelToWorldDir(const refEntity_t *model, const vec3_t in, vec3_t out)
{
    int k;

    VectorClear(out);
    for (k = 0; k < 3; k++) {
        VectorMA(out, in[k], model->axis[k], out);
    }
}

// A bone folded into point: shrunk almost to nothing rather than to nothing,
// so no normal made from it comes out as zero over zero.
static void CG_GoreFoldInto(boneOverride_t *ovr, int bone, const float point[3])
{
    int k;

    memset(ovr, 0, sizeof(*ovr));
    ovr->boneIndex = bone;
    for (k = 0; k < 3; k++) {
        ovr->matrix[k][k] = 0.001f;
        ovr->matrix[3][k] = point[k];
    }
}

//
// Gibs
//

static void CG_GoreFreeGib(goreGib_t *gib)
{
    goreBody_t *body = gib->key >= 0 ? CG_GoreFindBody(gib->key) : NULL;

    CG_JoltChainDestroy(gib->jolt);
    gib->jolt = 0;

    if (body) {
        CG_GoreFreeBody(body);
    }
    gib->active = qfalse;
}

// The part as it is posed on model, thrown off at vel.
static void CG_GoreSpawnGib(goreBody_t *from, const goreRig_t *rig, refEntity_t *model, int part, const vec3_t vel, const float cut[3])
{
    goreGib_t  *gib    = NULL;
    goreGib_t  *oldest = NULL;
    goreBody_t *body;
    float       m[4][3];
    vec3_t      world;
    int         i, n, used = 0, q;

    for (i = 0; i < GORE_MAX_GIBS && i < Q_max(cg_gore_maxGibs->integer, 1); i++) {
        if (!gore_gibs[i].active) {
            gib = &gore_gibs[i];
            break;
        }
        used++;
        if (!oldest || gore_gibs[i].born < oldest->born) {
            oldest = &gore_gibs[i];
        }
    }

    if (!gib) {
        if (!oldest) {
            return;
        }
        CG_GoreFreeGib(oldest);
        gib = oldest;
    }

    memset(gib, 0, sizeof(*gib));
    gib->key  = GORE_GIB_KEY + (int)(gib - gore_gibs);
    gib->part = part;
    gib->born = cg.time;
    gib->ref  = *model;

    // Everything folds into the cut but the part, held as it is. Every bone
    // by itself: the feet are placed in model space, not from the legs, and
    // would stay where they were if only the root were folded.
    n = 0;
    for (i = 0; i < rig->numBones && n < GORE_MAX_OVERRIDES; i++) {
        if (std::find(rig->bones[part].begin(), rig->bones[part].end(), i) == rig->bones[part].end()) {
            CG_GoreFoldInto(&gib->ovr[n++], i, cut);
        }
    }
    used = n;
    VectorClear(gib->centre);
    for (i = 0; i < (int)rig->bones[part].size() && n < GORE_MAX_OVERRIDES; i++) {
        int bone = rig->bones[part][i];

        if (!CG_GoreBoneFrame(model, bone, m, world)) {
            continue;
        }
        gib->ovr[n].boneIndex = bone;
        memcpy(gib->ovr[n].matrix, m, sizeof(m));
        n++;
        VectorAdd(gib->centre, world, gib->centre);
    }

    // a smaller part of it that was gone already stays gone
    for (q = 0; q < GP_NUM; q++) {
        if (q == part || !(rig->within[part] & (1u << q)) || !(from->severed & (1u << q))) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (std::find(rig->bones[q].begin(), rig->bones[q].end(), gib->ovr[i].boneIndex) != rig->bones[q].end()) {
                CG_GoreFoldInto(&gib->ovr[i], gib->ovr[i].boneIndex, from->stumpCut[q]);
            }
        }
    }

    if (n <= used) {
        gib->active = qfalse;
        return;
    }

    VectorScale(gib->centre, 1.0f / (n - used), gib->centre);

    gib->ref.bone_override      = gib->ovr;
    gib->ref.num_bone_overrides = n;
    gib->ref.entityNumber       = ENTITYNUM_NONE;
    gib->ref.bone_tag           = NULL;
    gib->ref.bone_quat          = NULL;
    gib->ref.renderfx &= ~(RF_THIRD_PERSON | RF_FIRST_PERSON | RF_DEPTHHACK | RF_FIRST_PERSON_BODY);
    gib->ref.renderfx |= RF_LIGHTING_ORIGIN | RF_GORE_DENTS; // the folded triangles left out
    VectorCopy(gib->centre, gib->ref.lightingOrigin);
    gib->ref.lightingOrigin[2] += GORE_GIB_LIGHT_UP;
    gib->ref.gore_dents     = NULL;
    gib->ref.num_gore_dents = 0;

    // a head takes its dents with it
    if (part == GP_HEAD && from->numDents) {
        memcpy(gib->dents, from->dents, sizeof(gib->dents));
        gib->ref.gore_dents     = gib->dents;
        gib->ref.num_gore_dents = from->numDents;
        gib->ref.renderfx |= RF_GORE_DENTS;
    }

    VectorCopy(vel, gib->vel);
    gib->spin[0] = crandom();
    gib->spin[1] = crandom();
    gib->spin[2] = crandom();
    VectorNormalize(gib->spin);
    VectorScale(gib->spin, GoreRand(250, 700), gib->spin);
    gib->numFolded = used;
    gib->numOvr    = n;
    gib->active    = qtrue;
    CG_GoreChainStart(gib, rig);

    // Its own body, with the wounds it had and a stump where it came off.
    body = CG_GoreFindBody(gib->key);
    if (body) {
        CG_GoreFreeBody(body);
    }
    {
        goreDrawn_t drawn;

        drawn.entityNum = gib->key;
        drawn.ref       = gib->ref;
        drawn.dead      = qtrue;
        drawn.noDraw    = qfalse;
        body            = CG_GoreBody(&drawn);
    }
    body->gib    = qtrue;
    body->decals = from->decals;
    body->dirty  = qtrue;
    for (i = 0; i < (int)body->decals.size(); i++) {
        body->decals[i].bleedUntil = Q_min(body->decals[i].bleedUntil, cg.time + 3000);
        body->decals[i].spurtUntil = 0;
    }
    if (part == GP_HEAD) {
        memcpy(body->dents, from->dents, sizeof(body->dents));
        memcpy(body->dentOut, from->dentOut, sizeof(body->dentOut));
        body->numDents = from->numDents;
    }
    VectorCopy(from->stumpAt[part], body->stumpAt[part]);
    VectorNegate(from->stumpDir[part], body->stumpDir[part]);
    body->stumpPending = 1u << part;
}

//
// A part's own ragdoll
//

#define GORE_CHAIN_STEP 0.0166667f // fixed, as the ragdolls step
#define GORE_CHAIN_PAD  1.5f       // how thick a point is against the world

// Model space before the scale (bone_override's) from the world, and back, on
// a gib's own frame: it keeps the body's, and only its bones move.
static void CG_GoreWorldToRaw(const refEntity_t *ref, const vec3_t world, float raw[3])
{
    float  scale = CG_GoreModelScale(ref);
    vec3_t d;
    int    k;

    VectorSubtract(world, ref->origin, d);
    for (k = 0; k < 3; k++) {
        raw[k] = DotProduct(d, ref->axis[k]) / scale - ref->tiki->load_origin[k];
    }
}

static void CG_GoreRawToWorld(const refEntity_t *ref, const float raw[3], vec3_t world)
{
    float scale = CG_GoreModelScale(ref);
    int   k;

    VectorCopy(ref->origin, world);
    for (k = 0; k < 3; k++) {
        VectorMA(world, (raw[k] + ref->tiki->load_origin[k]) * scale, ref->axis[k], world);
    }
}

static void CG_GoreWorldToModelDir(const refEntity_t *ref, const vec3_t in, vec3_t out)
{
    int k;

    for (k = 0; k < 3; k++) {
        out[k] = DotProduct(in, ref->axis[k]);
    }
}

// The bones the chain runs through, and its end, for a part.
static int CG_GoreChainBones(const goreRig_t *rig, int part, int bones[GORE_CHAIN_PTS])
{
    static const char *arm[] = {"Bip01 %c UpperArm", "Bip01 %c Forearm", "Bip01 %c Hand", NULL};
    static const char *leg[] = {"Bip01 %c Thigh", "Bip01 %c Calf", "Bip01 %c Foot", NULL};
    const char       **names;
    char               side;
    int                first, i, n = 0;

    switch (part) {
    case GP_HEAD:
        bones[0] = rig->cut[GP_HEAD];
        return bones[0] >= 0 ? 1 : 0;
    case GP_L_UPPERARM:
    case GP_L_FOREARM:
    case GP_L_HAND:
    case GP_R_UPPERARM:
    case GP_R_FOREARM:
    case GP_R_HAND:
        names = arm;
        side  = part >= GP_R_UPPERARM ? 'R' : 'L';
        first = part - (side == 'R' ? GP_R_UPPERARM : GP_L_UPPERARM);
        break;
    default:
        names = leg;
        side  = part >= GP_R_THIGH ? 'R' : 'L';
        first = part - (side == 'R' ? GP_R_THIGH : GP_L_THIGH);
        break;
    }

    for (i = first; names[i] && n < GORE_CHAIN_PTS - 1; i++) {
        int bone = cgi.Tag_NumForName(rig->tiki, va(names[i], side));

        if (bone >= 0) {
            bones[n++] = bone;
        }
    }

    return n;
}

// How thick a part is at each point of its chain, in world units: from the
// cut to the end, thinner as it goes.
static float CG_GoreChainRadius(int part, int i, int numPts)
{
    float thick;

    if (part == GP_HEAD) {
        return 4.0f;
    }
    thick = part >= GP_L_THIGH ? 3.2f : 2.2f;
    return Q_max(thick * (1.0f - 0.45f * i / Q_max(numPts - 1, 1)), 1.0f);
}

// And how heavy, in kilograms: an arm about four, a leg about ten.
static float CG_GoreChainMass(int part, int numPts)
{
    if (part == GP_HEAD) {
        return 5.0f;
    }
    return (part >= GP_L_THIGH ? 3.5f : 1.4f) * Q_max(numPts - 1, 1);
}

// Sets a gib up as a chain, from the overrides it was given (the part as it
// was when it came off).
static void CG_GoreChainStart(goreGib_t *gib, const goreRig_t *rig)
{
    int    bones[GORE_CHAIN_PTS];
    int    n = CG_GoreChainBones(rig, gib->part, bones);
    int    i, j, k;
    vec3_t tip, end;

    gib->numPts = 0;
    if (n <= 0) {
        return;
    }

    for (i = 0; i < n; i++) {
        gib->carrierOvr[i] = -1;
        for (j = gib->numFolded; j < gib->numOvr; j++) {
            if (gib->ovr[j].boneIndex == bones[i]) {
                gib->carrierOvr[i] = j;
            }
        }
        if (gib->carrierOvr[i] < 0) {
            return;
        }
        CG_GoreRawToWorld(&gib->ref, gib->ovr[gib->carrierOvr[i]].matrix[3], gib->pt[i]);
    }

    // The end: past the last joint, as far as the part's farthest bone goes
    // (the fingers, the toes), or a hand's length along it.
    {
        const float(*m)[3] = gib->ovr[gib->carrierOvr[n - 1]].matrix;
        float far          = 0;

        VectorClear(tip);
        for (j = gib->numFolded; j < gib->numOvr; j++) {
            vec3_t w;
            float  d;

            CG_GoreRawToWorld(&gib->ref, gib->ovr[j].matrix[3], w);
            d = Distance(w, gib->pt[n - 1]);
            if (d > far && (n < 2 || Distance(w, gib->pt[n - 2]) > Distance(gib->pt[n - 1], gib->pt[n - 2]))) {
                far = d;
                VectorCopy(w, tip);
            }
        }
        if (far < 2.0f) {
            vec3_t along;

            CG_GoreModelToWorldDir(&gib->ref, m[0], along);
            VectorNormalize(along);
            VectorMA(gib->pt[n - 1], gib->part == GP_HEAD ? 8.0f : 5.0f, along, tip);
        }
        VectorCopy(tip, gib->pt[n]);
    }
    gib->numPts = n + 1;

    for (i = 0; i < gib->numPts - 1; i++) {
        vec3_t d;

        gib->len[i] = Q_max(Distance(gib->pt[i], gib->pt[i + 1]), 1.0f);
        VectorSubtract(gib->pt[i + 1], gib->pt[i], d);
        CG_GoreWorldToModelDir(&gib->ref, d, gib->carrierDir[i]);
        VectorNormalize(gib->carrierDir[i]);
    }

    // Every other bone of the part rides on the nearest bone of the chain.
    gib->numRiders = 0;
    for (j = 0; j < gib->numOvr; j++) {
        float(*rm)[3] = gib->ovr[j].matrix;
        vec3_t w;
        float  best = 0;
        int    of = -1;

        for (i = 0; i < n; i++) {
            if (gib->carrierOvr[i] == j) {
                break;
            }
        }
        if (i < n) {
            continue;
        }

        // the folded bones ride on the first point, where the cut is
        if (j < gib->numFolded) {
            of = 0;
        } else {
            CG_GoreRawToWorld(&gib->ref, rm[3], w);
            for (i = 0; i < n; i++) {
                vec3_t seg, rel;
                float  t, d;

                VectorSubtract(gib->pt[i + 1], gib->pt[i], seg);
                VectorSubtract(w, gib->pt[i], rel);
                t = Q_bound(0.0f, DotProduct(rel, seg) / Q_max(DotProduct(seg, seg), 0.01f), 1.0f);
                VectorMA(gib->pt[i], t, seg, end);
                d = Distance(end, w);
                if (of < 0 || d < best) {
                    of   = i;
                    best = d;
                }
            }
        }

        {
            const float(*cm)[3] = gib->ovr[gib->carrierOvr[of]].matrix;
            vec3_t rel;

            for (k = 0; k < 3; k++) {
                gib->riderLocal[gib->numRiders][k][0] = DotProduct(rm[k], cm[0]);
                gib->riderLocal[gib->numRiders][k][1] = DotProduct(rm[k], cm[1]);
                gib->riderLocal[gib->numRiders][k][2] = DotProduct(rm[k], cm[2]);
            }
            VectorSubtract(rm[3], cm[3], rel);
            for (k = 0; k < 3; k++) {
                gib->riderLocal[gib->numRiders][3][k] = DotProduct(rel, cm[k]);
            }
        }
        gib->riderOvr[gib->numRiders] = j;
        gib->riderOf[gib->numRiders]  = of;
        gib->numRiders++;
    }

    // Thrown: all of it at vel, and turning about its middle.
    VectorClear(gib->centre);
    for (i = 0; i < gib->numPts; i++) {
        VectorAdd(gib->centre, gib->pt[i], gib->centre);
    }
    VectorScale(gib->centre, 1.0f / gib->numPts, gib->centre);
    {
        vec3_t vels[GORE_CHAIN_PTS];
        float  radii[GORE_CHAIN_PTS];

        for (i = 0; i < gib->numPts; i++) {
            vec3_t rel, turn;

            VectorSubtract(gib->pt[i], gib->centre, rel);
            CrossProduct(gib->spin, rel, turn);
            VectorScale(turn, M_PI / 180.0f, turn);
            VectorAdd(gib->vel, turn, vels[i]);
            VectorMA(gib->pt[i], -GORE_CHAIN_STEP, vels[i], gib->old[i]);
            radii[i] = CG_GoreChainRadius(gib->part, i, gib->numPts);
        }

        // A ragdoll of its own in the physics world, if there is one: it
        // knocks into the world, the props and the bodies, and is pushed by
        // rounds and blasts. The chain below only without it.
        // (thrown a little slower: nothing like the chain's bounce takes the
        // speed off it)
        for (i = 0; i < gib->numPts; i++) {
            VectorScale(vels[i], 0.65f, vels[i]);
        }
        gib->jolt = CG_JoltChainCreate(gib->numPts, gib->pt, vels, radii, CG_GoreChainMass(gib->part, gib->numPts));
    }
}

// One step of the chain.
static void CG_GoreChainStep(goreGib_t *gib)
{
    static const vec3_t mins = {-GORE_CHAIN_PAD, -GORE_CHAIN_PAD, -GORE_CHAIN_PAD};
    static const vec3_t maxs = {GORE_CHAIN_PAD, GORE_CHAIN_PAD, GORE_CHAIN_PAD};
    vec3_t              before[GORE_CHAIN_PTS];
    float               moved = 0;
    int                 i, it;

    for (i = 0; i < gib->numPts; i++) {
        vec3_t v;

        VectorCopy(gib->pt[i], before[i]);
        VectorSubtract(gib->pt[i], gib->old[i], v);
        VectorCopy(gib->pt[i], gib->old[i]);
        VectorMA(gib->pt[i], 0.995f, v, gib->pt[i]);
        gib->pt[i][2] -= GORE_GRAVITY * GORE_CHAIN_STEP * GORE_CHAIN_STEP;
        // never quite still: a leg landing on its foot topples, as it would
        gib->pt[i][0] += crandom() * 0.004f;
        gib->pt[i][1] += crandom() * 0.004f;
    }

    for (it = 0; it < 6; it++) {
        for (i = 0; i < gib->numPts - 1; i++) {
            vec3_t d;
            float  l;

            VectorSubtract(gib->pt[i + 1], gib->pt[i], d);
            l = VectorNormalize(d);
            if (l < 0.001f) {
                continue;
            }
            l = (l - gib->len[i]) * 0.5f;
            VectorMA(gib->pt[i], l, d, gib->pt[i]);
            VectorMA(gib->pt[i + 1], -l, d, gib->pt[i + 1]);
        }
    }

    // Caught on the world: stopped where it meets it, a little of the way it
    // was going kept along the surface.
    for (i = 0; i < gib->numPts; i++) {
        trace_t tr;
        vec3_t  v;

        CG_Trace(&tr, before[i], mins, maxs, gib->pt[i], ENTITYNUM_NONE, MASK_SOLID, qfalse, qfalse, "gore gib");
        if (tr.startsolid || tr.fraction >= 1.0f) {
            continue;
        }

        VectorSubtract(gib->pt[i], gib->old[i], v);
        VectorMA(v, -1.2f * DotProduct(v, tr.plane.normal), tr.plane.normal, v);
        VectorScale(v, 0.5f, v);
        VectorCopy(tr.endpos, gib->pt[i]);
        VectorSubtract(gib->pt[i], v, gib->old[i]);
    }

    for (i = 0; i < gib->numPts; i++) {
        moved = Q_max(moved, Distance(gib->pt[i], gib->old[i]));
    }
    gib->still = moved < 0.03f ? gib->still + 1 : 0;
    if (gib->still > 45) {
        gib->resting = qtrue;
        if (cg_gore_debug->integer) {
            Com_Printf("gore: the %s comes to rest at %.0f %.0f %.0f\n", gore_partNames[gib->part], gib->pt[0][0], gib->pt[0][1], gib->pt[0][2]);
        }
    }
}

// Poses the part from its chain: each bone of the chain turned the least way
// from where it pointed to where its points are now, the rest carried along.
static void CG_GoreChainPose(goreGib_t *gib)
{
    int i, j, k;

    for (i = 0; i < gib->numPts - 1; i++) {
        float(*m)[3] = gib->ovr[gib->carrierOvr[i]].matrix;
        vec3_t d, now, axis;
        float  c;

        VectorSubtract(gib->pt[i + 1], gib->pt[i], d);
        CG_GoreWorldToModelDir(&gib->ref, d, now);
        if (VectorNormalize(now) < 0.001f) {
            continue;
        }

        CrossProduct(gib->carrierDir[i], now, axis);
        c = DotProduct(gib->carrierDir[i], now);
        if (VectorNormalize(axis) > 0.0001f) {
            float deg = acos(Q_bound(-1.0f, c, 1.0f)) * (180.0f / M_PI);

            for (k = 0; k < 3; k++) {
                vec3_t out;

                RotatePointAroundVector(out, axis, m[k], deg);
                VectorCopy(out, m[k]);
            }
        }
        VectorCopy(now, gib->carrierDir[i]);
        CG_GoreWorldToRaw(&gib->ref, gib->pt[i], m[3]);
    }

    for (j = 0; j < gib->numRiders; j++) {
        float(*rm)[3]       = gib->ovr[gib->riderOvr[j]].matrix;
        const float(*cm)[3] = gib->ovr[gib->carrierOvr[gib->riderOf[j]]].matrix;
        const float(*l)[3]  = gib->riderLocal[j];

        for (k = 0; k < 3; k++) {
            int c;

            for (c = 0; c < 3; c++) {
                rm[k][c] = l[k][0] * cm[0][c] + l[k][1] * cm[1][c] + l[k][2] * cm[2][c];
            }
        }
        for (k = 0; k < 3; k++) {
            rm[3][k] = cm[3][k] + l[3][0] * cm[0][k] + l[3][1] * cm[1][k] + l[3][2] * cm[2][k];
        }
    }

    VectorClear(gib->centre);
    for (i = 0; i < gib->numPts; i++) {
        VectorAdd(gib->centre, gib->pt[i], gib->centre);
    }
    VectorScale(gib->centre, 1.0f / gib->numPts, gib->centre);
    VectorCopy(gib->centre, gib->ref.lightingOrigin);
    gib->ref.lightingOrigin[2] += GORE_GIB_LIGHT_UP;
}

static void CG_GoreUpdateChain(goreGib_t *gib, float dt)
{
    int steps = 0;

    if (gib->jolt) {
        if (CG_JoltChainRead(gib->jolt, gib->pt)) {
            CG_GoreChainPose(gib);
            return;
        }
        // its physics world gone (the map changes): the chain from here
        gib->jolt = 0;
        for (steps = 0; steps < gib->numPts; steps++) {
            VectorCopy(gib->pt[steps], gib->old[steps]);
        }
        steps        = 0;
        gib->resting = qfalse;
    }

    if (gib->resting) {
        return;
    }

    gib->stepLeft += dt;
    while (gib->stepLeft >= GORE_CHAIN_STEP && steps < 6 && !gib->resting) {
        CG_GoreChainStep(gib);
        gib->stepLeft -= GORE_CHAIN_STEP;
        steps++;
    }
    if (steps == 6) {
        gib->stepLeft = 0;
    }

    CG_GoreChainPose(gib);
}

//
// Pieces of a head
//

// What a round breaks out of a head: a handful of small pieces of skull, scalp
// and brain, tumbling and bouncing to a stop. The shapes are the game's own
// small debris (a shard, lumps of concrete), sized from their meshes and drawn
// in bone, flesh and brain: each mesh is stretched on its three axes to the
// bit's own shape, a flat plate of skull or a lump.
#define GORE_MAX_BITS  96
#define GORE_BIT_LIFE  20000 // ms a bit lies before it goes
#define GORE_BIT_FADE  1500  // and shrinks away over the last of them

typedef struct {
    qboolean    active, resting;
    refEntity_t ref;
    int         model;
    vec3_t      pos;     // its middle; ref.origin is the mesh's, offset from it
    vec3_t      axis[3]; // which way it is turned
    vec3_t      size;    // how far it reaches each way, along axis
    vec3_t      vel, spin;
    int         born;
} goreBit_t;

typedef enum { GB_BONE, GB_FLESH, GB_BRAIN, GB_NUM } goreBitKind_t;

// which shapes go with which bits: a shard for skull, lumps for the rest
static const struct {
    const char *name;
    qboolean    shard;
} gore_bitModelNames[] = {
    {"models/fx/shard_piece.tik", qtrue  },
    {"models/fx/chunkcrete.tik",  qfalse},
    {"models/fx/concrete1.tik",   qfalse},
    {"models/fx/concrete2.tik",   qfalse},
};
#define GORE_BIT_MODELS ARRAY_LEN(gore_bitModelNames)

static const char *gore_bitShaderNames[GB_NUM] = {"gore/bit_bone", "gore/bit_flesh", "gore/bit_brain"};

static goreBit_t gore_bits[GORE_MAX_BITS];
static int       gore_nextBit;
static qhandle_t gore_bitModels[GORE_BIT_MODELS];
static vec3_t    gore_bitMid[GORE_BIT_MODELS], gore_bitHalf[GORE_BIT_MODELS]; // the mesh's bounds, at scale 1
static qboolean  gore_bitShard[GORE_BIT_MODELS];
static int       gore_numBitModels;
static qhandle_t gore_bitShaders[GB_NUM];
static qhandle_t gore_bitStains[2]; // under a bit where it lies

// A bit's shape posed as it stands: the middle of its mesh and how far it
// reaches from there each way.
static qboolean CG_GoreBitBounds(qhandle_t hModel, vec3_t mid, vec3_t half)
{
    refEntity_t ent;
    vec3_t      mins, maxs;
    int         i, n, numTris = 0;

    memset(&ent, 0, sizeof(ent));
    ent.reType              = RT_MODEL;
    ent.hModel              = hModel;
    ent.tiki                = cgi.R_Model_GetHandle(hModel);
    ent.scale               = 1.0f;
    ent.entityNumber        = ENTITYNUM_NONE;
    ent.frameInfo[0].weight = 1.0f;
    ent.actionWeight        = 1.0f;
    AxisClear(ent.axis);
    if (!ent.tiki) {
        return qfalse;
    }

    n = cgi.R_GetSkinnedMesh(&ent, gore_verts, GORE_SKIN_VERTS, gore_tris, GORE_SKIN_TRIS, &numTris);
    ClearBounds(mins, maxs);
    for (i = 0; i < n; i++) {
        AddPointToBounds(gore_verts[i].xyz, mins, maxs);
    }
    for (i = 0; i < 3; i++) {
        mid[i]  = (mins[i] + maxs[i]) * 0.5f;
        half[i] = (maxs[i] - mins[i]) * 0.5f;
        if (n <= 0 || half[i] < 0.05f) {
            return qfalse;
        }
    }
    return qtrue;
}

static void CG_GoreRegisterBits(void)
{
    int i;

    gore_numBitModels = 0;
    memset(gore_bits, 0, sizeof(gore_bits));
    gore_nextBit = 0;
    if (cgi.apiversion < 4 || !cgi.R_GetSkinnedMesh) {
        return;
    }

    for (i = 0; i < (int)GORE_BIT_MODELS; i++) {
        qhandle_t h = cgi.R_RegisterModel(gore_bitModelNames[i].name);

        if (h && CG_GoreBitBounds(h, gore_bitMid[gore_numBitModels], gore_bitHalf[gore_numBitModels])) {
            gore_bitShard[gore_numBitModels]    = gore_bitModelNames[i].shard;
            gore_bitModels[gore_numBitModels++] = h;
        }
    }
    for (i = 0; i < GB_NUM; i++) {
        gore_bitShaders[i] = cgi.R_RegisterShader(gore_bitShaderNames[i]);
    }
    // HRRTM's blood splats when it is installed (CG_GoreClear has looked)
    for (i = 0; i < 2; i++) {
        gore_bitStains[i] = gore_numVariants == GORE_VARIANTS ? cgi.R_RegisterShader(va("gore/stain%d", i + 1)) : gore_splatShader;
    }
}

// The mesh stretched to the bit's size along its axes, its middle on pos.
static void CG_GoreBitPlace(goreBit_t *bit, float shrink)
{
    int k;

    for (k = 0; k < 3; k++) {
        VectorScale(bit->axis[k], bit->size[k] * shrink / gore_bitHalf[bit->model][k], bit->ref.axis[k]);
    }
    VectorCopy(bit->pos, bit->ref.origin);
    for (k = 0; k < 3; k++) {
        VectorMA(bit->ref.origin, -gore_bitMid[bit->model][k], bit->ref.axis[k], bit->ref.origin);
    }
    VectorCopy(bit->ref.origin, bit->ref.oldorigin);
    VectorCopy(bit->pos, bit->ref.lightingOrigin);
    bit->ref.lightingOrigin[2] += 2.0f;
}

// a shape for a bit: a shard or a lump if there is one, any if not
static int CG_GoreBitModel(qboolean shard)
{
    int pick[GORE_BIT_MODELS], n = 0, i;

    for (i = 0; i < gore_numBitModels; i++) {
        if (gore_bitShard[i] == shard) {
            pick[n++] = i;
        }
    }
    return n ? pick[rand() % n] : rand() % gore_numBitModels;
}

// count bits out of where a dent opens, mostly along out
static void CG_GoreSpawnBits(const vec3_t at, const vec3_t out, int count)
{
    int i, k;

    if (!gore_numBitModels) {
        return;
    }

    for (i = 0; i < count; i++) {
        goreBit_t *bit   = &gore_bits[gore_nextBit];
        int        kind;
        float      size, roll = random();

        gore_nextBit = (gore_nextBit + 1) % GORE_MAX_BITS;

        memset(bit, 0, sizeof(*bit));

        // mostly skull, a flat plate and the biggest; some scalp and a little
        // brain, in lumps
        if (roll < 0.5f) {
            kind = GB_BONE;
            size = GoreRand(0.7f, 1.6f);
            VectorSet(bit->size, size, size * GoreRand(0.5f, 0.9f), size * GoreRand(0.15f, 0.3f));
        } else if (roll < 0.8f) {
            kind = GB_FLESH;
            size = GoreRand(0.5f, 1.2f);
            VectorSet(bit->size, size, size * GoreRand(0.6f, 0.9f), size * GoreRand(0.3f, 0.6f));
        } else {
            kind = GB_BRAIN;
            size = GoreRand(0.4f, 0.9f);
            VectorSet(bit->size, size, size * GoreRand(0.7f, 1.0f), size * GoreRand(0.6f, 0.9f));
        }
        VectorScale(bit->size, cg_gore_scale->value, bit->size);
        bit->model = CG_GoreBitModel(kind == GB_BONE);

        bit->ref.reType              = RT_MODEL;
        bit->ref.hModel              = gore_bitModels[bit->model];
        bit->ref.tiki                = cgi.R_Model_GetHandle(bit->ref.hModel);
        bit->ref.customShader        = gore_bitShaders[kind];
        bit->ref.entityNumber        = ENTITYNUM_NONE;
        bit->ref.frameInfo[0].weight = 1.0f;
        bit->ref.actionWeight        = 1.0f;
        bit->ref.renderfx            = RF_LIGHTING_ORIGIN;
        for (k = 0; k < 4; k++) {
            bit->ref.shaderRGBA[k] = 255;
        }
        bit->ref.scale               = 1.0f;
        bit->ref.nonNormalizedAxes   = qtrue;
        {
            vec3_t angles = {GoreRand(0, 360), GoreRand(0, 360), GoreRand(0, 360)};

            AnglesToAxis(angles, bit->axis);
        }
        for (k = 0; k < 3; k++) {
            bit->pos[k] = at[k] + crandom() * 1.0f;
        }
        CG_GoreBitPlace(bit, 1.0f);

        VectorScale(out, GoreRand(30, 110), bit->vel);
        bit->vel[0] += crandom() * 40;
        bit->vel[1] += crandom() * 40;
        bit->vel[2] += GoreRand(20, 90);
        for (k = 0; k < 3; k++) {
            bit->spin[k] = crandom();
        }
        VectorNormalize(bit->spin);
        VectorScale(bit->spin, GoreRand(360, 1440), bit->spin);

        bit->born   = cg.time;
        bit->active = qtrue;
    }
}

// The bits a dent breaks off, thrown out of it.
static void CG_GoreBreakDent(goreBody_t *from, refEntity_t *model, int dent, int count)
{
    float  m[4][3];
    vec3_t centre, out, at, dir;
    int    k;

    if (!CG_GoreBoneFrame(model, from->dents[dent].boneIndex, m, NULL)) {
        return;
    }
    for (k = 0; k < 3; k++) {
        centre[k] = m[3][k] + from->dents[dent].offset[0] * m[0][k] + from->dents[dent].offset[1] * m[1][k]
                  + from->dents[dent].offset[2] * m[2][k];
        out[k] = from->dents[dent].dir[0] * m[0][k] + from->dents[dent].dir[1] * m[1][k] + from->dents[dent].dir[2] * m[2][k];
    }
    CG_GoreRawToWorld(model, centre, at);
    CG_GoreModelToWorldDir(model, out, dir);
    VectorNormalize(dir);
    CG_GoreSpawnBits(at, dir, count);
    if (cg_gore_debug->integer) {
        Com_Printf("gore: %d bits out of a dent at %.0f %.0f %.0f\n", count, at[0], at[1], at[2]);
    }
}

// Flies, tumbling, bounces to a stop, and in time shrinks away.
static void CG_GoreUpdateBit(goreBit_t *bit, float dt)
{
    static const vec3_t mins = {-0.5f, -0.5f, -0.5f}, maxs = {0.5f, 0.5f, 0.5f};
    trace_t             tr;
    vec3_t              next;
    int                 left = GORE_BIT_LIFE - (cg.time - bit->born);
    float               shrink = left < GORE_BIT_FADE ? Q_max(left, 0) / (float)GORE_BIT_FADE : 1.0f;

    if (bit->resting || dt <= 0) {
        CG_GoreBitPlace(bit, shrink);
        return;
    }

    bit->vel[2] -= GORE_GRAVITY * dt;
    VectorMA(bit->pos, dt, bit->vel, next);
    CG_Trace(&tr, bit->pos, mins, maxs, next, ENTITYNUM_NONE, MASK_SOLID, qfalse, qfalse, "gore bit");
    if (tr.startsolid) {
        bit->resting = qtrue;
        if (cg_gore_debug->integer) {
            Com_Printf("gore: a bit starts in something at %.0f %.0f %.0f\n", bit->pos[0], bit->pos[1], bit->pos[2]);
        }
        CG_GoreBitPlace(bit, shrink);
        return;
    }

    CG_GoreSpinAxis(bit->axis, bit->spin, dt);
    VectorCopy(tr.endpos, bit->pos);

    if (tr.fraction < 1.0f) {
        float into = DotProduct(bit->vel, tr.plane.normal);

        VectorMA(bit->vel, -1.35f * into, tr.plane.normal, bit->vel);
        VectorScale(bit->vel, 0.55f, bit->vel);
        VectorScale(bit->spin, 0.5f, bit->spin);
        if (tr.plane.normal[2] > 0.7f && VectorLength(bit->vel) < 30.0f) {
            // lying flat, its thinnest way up
            VectorCopy(tr.plane.normal, bit->axis[2]);
            VectorMA(bit->axis[0], -DotProduct(bit->axis[0], bit->axis[2]), bit->axis[2], bit->axis[0]);
            if (VectorNormalize(bit->axis[0]) < 0.01f) {
                PerpendicularVector(bit->axis[0], bit->axis[2]);
            }
            CrossProduct(bit->axis[2], bit->axis[0], bit->axis[1]);
            VectorMA(tr.endpos, bit->size[2] + mins[2], bit->axis[2], bit->pos); // the box sits -mins[2] up

            VectorClear(bit->vel);
            VectorClear(bit->spin);
            bit->resting = qtrue;

            // a little blood under it
            {
                float r = Q_max(bit->size[0], bit->size[1]) * GoreRand(1.6f, 2.4f);

                CG_ImpactMark(
                    gore_bitStains[rand() & 1], tr.endpos, tr.plane.normal, GoreRand(0, 360), r, r, GORE_LIGHT_SCALE,
                    GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, 1, qfalse, qfalse, qtrue, qfalse, 0.5f, 0.5f
                );
            }
            if (cg_gore_debug->integer) {
                Com_Printf("gore: a bit comes to rest at %.0f %.0f %.0f\n", bit->pos[0], bit->pos[1], bit->pos[2]);
            }
        }
    }

    CG_GoreBitPlace(bit, shrink);
}

static void CG_GoreAddBits(void)
{
    float dt = Q_min(Q_max(cg.frametime * 0.001f, 0.0f), 0.05f);
    int   i;

    for (i = 0; i < GORE_MAX_BITS; i++) {
        goreBit_t *bit = &gore_bits[i];

        if (!bit->active) {
            continue;
        }
        if (cg.time - bit->born > GORE_BIT_LIFE) {
            bit->active = qfalse;
            continue;
        }

        CG_GoreUpdateBit(bit, dt);
        if (Distance(bit->pos, cg.refdef.vieworg) > cg_gore_drawDist->value * 0.5f) {
            continue;
        }
        cgi.R_AddRefEntityToScene(&bit->ref, ENTITYNUM_NONE);
    }
}

static void CG_GoreSpinAxis(vec3_t axis[3], const vec3_t spin, float dt)
{
    vec3_t dir, out;
    float  deg = VectorLength(spin) * dt;
    int    k;

    if (deg < 0.01f) {
        return;
    }

    VectorScale(spin, 1.0f / VectorLength(spin), dir);
    for (k = 0; k < 3; k++) {
        RotatePointAroundVector(out, dir, axis[k], deg);
        VectorCopy(out, axis[k]);
    }
}

// Flies, tumbling, and bounces to a stop.
static void CG_GoreUpdateGib(goreGib_t *gib, float dt)
{
    static const vec3_t mins = {-2, -2, -2}, maxs = {2, 2, 2};
    trace_t             tr;
    vec3_t              next, offset, dir;
    float               deg;

    if (gib->resting || dt <= 0) {
        return;
    }

    gib->vel[2] -= GORE_GRAVITY * dt;
    VectorMA(gib->centre, dt, gib->vel, next);

    CG_Trace(&tr, gib->centre, mins, maxs, next, ENTITYNUM_NONE, MASK_SOLID, qfalse, qfalse, "gore gib");
    if (tr.startsolid) {
        gib->resting = qtrue;
        if (cg_gore_debug->integer) {
            Com_Printf("gore: the %s starts in something at %.0f %.0f %.0f\n", gore_partNames[gib->part], gib->centre[0], gib->centre[1], gib->centre[2]);
        }
        return;
    }

    // Turned about its middle.
    VectorSubtract(gib->ref.origin, gib->centre, offset);
    deg = VectorLength(gib->spin) * dt;
    if (deg > 0.01f) {
        vec3_t turned;

        VectorScale(gib->spin, 1.0f / VectorLength(gib->spin), dir);
        RotatePointAroundVector(turned, dir, offset, deg);
        VectorCopy(turned, offset);
        CG_GoreSpinAxis(gib->ref.axis, gib->spin, dt);
    }

    VectorCopy(tr.endpos, gib->centre);

    if (tr.fraction < 1.0f) {
        float into = DotProduct(gib->vel, tr.plane.normal);

        // off the surface, a little bounce, and dragged along it
        VectorMA(gib->vel, -1.3f * into, tr.plane.normal, gib->vel);
        VectorScale(gib->vel, 0.6f, gib->vel);
        VectorScale(gib->spin, 0.5f, gib->spin);

        if (tr.plane.normal[2] > 0.7f && VectorLength(gib->vel) < 40.0f) {
            VectorClear(gib->vel);
            VectorClear(gib->spin);
            gib->resting = qtrue;
            if (cg_gore_debug->integer) {
                Com_Printf("gore: the %s comes to rest at %.0f %.0f %.0f\n", gore_partNames[gib->part], gib->centre[0], gib->centre[1], gib->centre[2]);
            }
        }
    }

    VectorAdd(gib->centre, offset, gib->ref.origin);
    VectorCopy(gib->ref.origin, gib->ref.oldorigin);
    VectorCopy(gib->centre, gib->ref.lightingOrigin);
    gib->ref.lightingOrigin[2] += GORE_GIB_LIGHT_UP;
}

static void CG_GoreAddGibs(void)
{
    float dt = Q_max(cg.frametime * 0.001f, 0.0f);
    int   i;

    for (i = 0; i < GORE_MAX_GIBS; i++) {
        goreGib_t *gib = &gore_gibs[i];

        if (!gib->active) {
            continue;
        }

        if (gib->numPts) {
            CG_GoreUpdateChain(gib, Q_min(dt, 0.1f));
        } else {
            CG_GoreUpdateGib(gib, Q_min(dt, 0.05f));
        }

        if (Distance(gib->centre, cg.refdef.vieworg) > cg_gore_drawDist->value) {
            continue;
        }

        cgi.R_AddRefEntityToScene(&gib->ref, ENTITYNUM_NONE);
        CG_GoreAddEntityKey(gib->key, &gib->ref, qtrue);
    }
}

//
// Cutting
//

//
// Parts and bodies pushed about
//

// The closest points of two segments: t along the first, u along the second.
static float CG_GoreSegmentsClosest(const vec3_t a0, const vec3_t a1, const vec3_t b0, const vec3_t b1, float *t, float *u)
{
    vec3_t da, db, r, pa, pb;
    float  aa, bb, ab, ar, br, den;

    VectorSubtract(a1, a0, da);
    VectorSubtract(b1, b0, db);
    VectorSubtract(a0, b0, r);
    aa  = DotProduct(da, da);
    bb  = DotProduct(db, db);
    ab  = DotProduct(da, db);
    ar  = DotProduct(da, r);
    br  = DotProduct(db, r);
    den = aa * bb - ab * ab;

    *t = den > 0.0001f ? Q_bound(0.0f, (ab * br - bb * ar) / den, 1.0f) : 0.0f;
    *u = bb > 0.0001f ? Q_bound(0.0f, (ab * *t + br) / bb, 1.0f) : 0.0f;
    *t = aa > 0.0001f ? Q_bound(0.0f, (ab * *u - ar) / aa, 1.0f) : 0.0f;

    VectorMA(a0, *t, da, pa);
    VectorMA(b0, *u, db, pb);
    return Distance(pa, pb);
}

// A push on a part, in game units a second, at a point of it.
static void CG_GoreGibPush(goreGib_t *gib, const vec3_t point, const vec3_t dv)
{
    int i;

    if (gib->jolt) {
        CG_JoltChainAddVelocity(gib->jolt, point, dv);
        return;
    }

    // the chain: its points nearest the push take the most of it
    for (i = 0; i < gib->numPts; i++) {
        float share = 1.0f / (1.0f + Distance(gib->pt[i], point) / 8.0f);

        VectorMA(gib->old[i], -GORE_CHAIN_STEP * share, dv, gib->old[i]);
    }
    gib->resting = qfalse;
    gib->still   = 0;
}

static void CG_GoreBurst(const vec3_t pos, const vec3_t dir, const vec3_t light, int drops, int chunks);

// A round's path: a part it goes through is knocked along it and bleeds.
void CG_GoreNoteBullet(const vec3_t start, const vec3_t end, int large)
{
    vec3_t dir;
    int    g, i;

    if (!CG_GoreEnabled()) {
        return;
    }

    VectorSubtract(end, start, dir);
    if (VectorNormalize(dir) < 1.0f) {
        return;
    }

    for (g = 0; g < GORE_MAX_GIBS; g++) {
        goreGib_t *gib = &gore_gibs[g];

        for (i = 0; gib->active && i + 1 < gib->numPts; i++) {
            float  t, u, d;
            vec3_t at, dv, light;

            d = CG_GoreSegmentsClosest(start, end, gib->pt[i], gib->pt[i + 1], &t, &u);
            if (d > CG_GoreChainRadius(gib->part, i, gib->numPts) + 1.0f) {
                continue;
            }

            VectorSubtract(gib->pt[i + 1], gib->pt[i], at);
            VectorMA(gib->pt[i], u, at, at);
            VectorScale(dir, large ? 650.0f : 380.0f, dv);
            dv[2] += large ? 140.0f : 80.0f;
            CG_GoreGibPush(gib, at, dv);

            cgi.R_GetLightingForDecal(light, dir, at);
            VectorScale(light, GORE_LIGHT_SCALE, light);
            CG_GoreBurst(at, dir, light, large ? 8 : 4, 0);
            CG_GoreNoteHit(at, dir, large);
            break;
        }
    }
}

// A blast throws the parts lying about it.
static void CG_GoreBlastGibs(const vec3_t pos, int kind)
{
    static const float blasts[4][2] = {
        {220.0f, 520.0f },
        {280.0f, 680.0f },
        {360.0f, 840.0f },
        {440.0f, 1000.0f},
    };
    const float radius = blasts[Q_bound(0, kind, 3)][0];
    const float speed  = blasts[Q_bound(0, kind, 3)][1];
    int         g, i;

    for (g = 0; g < GORE_MAX_GIBS; g++) {
        goreGib_t *gib = &gore_gibs[g];

        for (i = 0; gib->active && i < gib->numPts; i++) {
            vec3_t away;
            float  d;

            VectorSubtract(gib->pt[i], pos, away);
            d = VectorNormalize(away);
            if (d >= radius) {
                continue;
            }
            away[2] += 0.4f;
            VectorNormalize(away);
            VectorScale(away, speed * (1.0f - d / radius) / gib->numPts, away);
            CG_GoreGibPush(gib, gib->pt[i], away);
        }
    }
}

// The way a mark lying on normal has to be turned for its length to run along
// (CG_ImpactMark: its T axis, from a square of the normal turned so far).
static float CG_GoreMarkAngle(const vec3_t normal, const vec3_t along)
{
    vec3_t n, p, a, c;

    VectorNormalize2(normal, n);
    PerpendicularVector(p, n);
    VectorMA(along, -DotProduct(along, n), n, a);
    if (VectorNormalize(a) < 0.001f) {
        return 0.001f;
    }
    CrossProduct(p, a, c);
    return atan2(DotProduct(c, n), DotProduct(p, a)) * (180.0f / M_PI) + 0.001f;
}

// Something bloody dragged along the ground leaves a streak: pos is where it
// is now, a body's pelvis or a part's middle. From where its last streak
// ended to the ground under it now, while it lies on the ground and moves.
static void CG_GoreStreak(const vec3_t pos, vec3_t from, qboolean *streaking, float width)
{
    trace_t tr;
    vec3_t  down, along, mid, light;
    float   len;

    VectorCopy(pos, down);
    down[2] -= 20.0f;
    CG_Trace(&tr, pos, vec3_origin, vec3_origin, down, ENTITYNUM_NONE, MASK_SOLID, qfalse, qfalse, "gore streak");
    if (tr.fraction >= 1.0f || tr.startsolid || tr.plane.normal[2] < 0.6f) {
        *streaking = qfalse;
        return;
    }

    if (!*streaking) {
        VectorCopy(tr.endpos, from);
        *streaking = qtrue;
        return;
    }

    VectorSubtract(tr.endpos, from, along);
    len = VectorNormalize(along);
    if (len < width * 0.75f) {
        return; // still, or barely moving
    }
    if (len > 64.0f) {
        VectorCopy(tr.endpos, from); // thrown, not dragged
        return;
    }

    VectorAdd(from, tr.endpos, mid);
    VectorScale(mid, 0.5f, mid);
    cgi.R_GetLightingForDecal(light, tr.plane.normal, mid);
    CG_ImpactMark(
        gore_streakShader, mid, tr.plane.normal, CG_GoreMarkAngle(tr.plane.normal, along), width * 0.5f,
        (len + width * 0.5f) * 0.5f, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, 1, qfalse, qfalse, qtrue, qfalse,
        0.5f, 0.5f
    );
    VectorCopy(tr.endpos, from);
}

// The bodies and parts sliding along the ground this frame.
static void CG_GoreStreaks(void)
{
    int i;

    for (i = 0; i < gore_numDrawn; i++) {
        goreDrawn_t *drawn = &gore_drawn[i];
        goreBody_t  *body  = CG_GoreFindBody(drawn->entityNum);
        vec3_t       pos;
        float        m[4][3];

        if (!body || !body->dead || body->gib || !drawn->ref.bone_override || !drawn->ref.tiki) {
            continue;
        }
        if (body->pelvis == -2) {
            body->pelvis = cgi.Tag_NumForName(drawn->ref.tiki, "Bip01 Pelvis");
        }
        if (body->pelvis < 0 || !CG_GoreBoneFrame(&drawn->ref, body->pelvis, m, pos)) {
            continue;
        }
        CG_GoreStreak(pos, body->streakFrom, &body->streaking, 9.0f);
    }

    for (i = 0; i < GORE_MAX_GIBS; i++) {
        goreGib_t *gib = &gore_gibs[i];

        if (gib->active && gib->numPts) {
            CG_GoreStreak(gib->centre, gib->streakFrom, &gib->streaking, gib->part >= GP_L_THIGH ? 6.0f : 4.0f);
        }
    }
}

// A burst of blood and bits of a head.
static void CG_GoreBurst(const vec3_t pos, const vec3_t dir, const vec3_t light, int drops, int chunks)
{
    int i;

    for (i = 0; i < drops + chunks; i++) {
        vec3_t vel;

        VectorScale(dir, GoreRand(40, 160), vel);
        vel[0] += crandom() * 120;
        vel[1] += crandom() * 120;
        vel[2] += GoreRand(60, 220);
        CG_GoreSpawnDrop(pos, vel, i < drops ? GoreRand(0.5f, 1.0f) : GoreRand(1.0f, 2.0f), light);
        if (i >= drops) {
            gore_drops[(gore_nextDrop + GORE_MAX_DROPS - 1) % GORE_MAX_DROPS].chunk = qtrue;
        }
    }
}

// Where the part comes out of what is left, measured on the body as it is
// just before the cut: the seam, the vertices that stay on the triangles
// that join them to the part's own. That is the end of a sleeve an arm came
// out of, or the skin about a shoulder, and where the stump goes. Kept in the
// frame of the bone above, as that is what it moves with.
static void CG_GoreFindOpening(goreBody_t *body, const goreRig_t *rig, refEntity_t *model, int part, const vec3_t joint, vec3_t dir)
{
    const goreLayout_t *lay = CG_GoreLayout(model);
    std::vector<char>   inPartVert, seam;
    qboolean            inPart[TIKI_MAX_BONES];
    float               pm[4][3], scale = CG_GoreModelScale(model);
    float               wide = 0;
    vec3_t              opening, raw;
    int                 i, k, count = 0;

    VectorCopy(joint, opening);
    body->capRadius[part] = gore_stumpRadius[part] * cg_gore_scale->value;

    memset(inPart, 0, sizeof(inPart));
    for (i = 0; i < (int)rig->bones[part].size(); i++) {
        if (rig->bones[part][i] < TIKI_MAX_BONES) {
            inPart[rig->bones[part][i]] = qtrue;
        }
    }

    if (lay && CG_GoreSkin(model, lay, CG_GoreVisibleMask(model, lay))) {
        inPartVert.assign(gore_numVerts, 0);
        seam.assign(gore_numVerts, 0);
        for (i = 0; i < gore_numVerts; i++) {
            int b = gore_verts[i].bone;

            inPartVert[i] = b >= 0 && b < TIKI_MAX_BONES && inPart[b] ? 1 : 0;
        }

        for (i = 0; i < gore_numTris; i++) {
            const int *t = &gore_tris[i * 3];
            int        parts = inPartVert[t[0]] + inPartVert[t[1]] + inPartVert[t[2]];

            if (parts == 0 || parts == 3) {
                continue;
            }
            for (k = 0; k < 3; k++) {
                if (!inPartVert[t[k]]) {
                    seam[t[k]] = 1;
                }
            }
        }

        VectorClear(opening);
        for (i = 0; i < gore_numVerts; i++) {
            if (seam[i]) {
                VectorAdd(opening, gore_verts[i].xyz, opening);
                count++;
            }
        }

        if (count) {
            VectorScale(opening, 1.0f / count, opening);
            for (i = 0; i < gore_numVerts; i++) {
                if (seam[i]) {
                    wide = Q_max(wide, Distance(gore_verts[i].xyz, opening));
                }
            }
            body->capRadius[part] = Q_bound(2.0f, wide * 0.9f, 5.5f) * cg_gore_scale->value;
        } else {
            VectorCopy(joint, opening);
        }
    }

    VectorSubtract(opening, joint, dir);
    if (VectorNormalize(dir) < 0.5f) {
        VectorCopy(body->lastHitDir, dir);
    }

    // into the frame of the bone above
    if (!CG_GoreBoneFrame(model, rig->parent[part], pm, NULL) || scale <= 0) {
        memset(pm, 0, sizeof(pm));
        pm[0][0] = pm[1][1] = pm[2][2] = 1;
    }
    for (k = 0; k < 3; k++) {
        vec3_t rel;

        VectorSubtract(opening, model->origin, rel);
        raw[k] = DotProduct(rel, model->axis[k]) / scale - model->tiki->load_origin[k];
    }
    VectorSubtract(raw, pm[3], raw);
    for (k = 0; k < 3; k++) {
        vec3_t dirModel;
        int    j;

        for (j = 0; j < 3; j++) {
            dirModel[j] = DotProduct(dir, model->axis[j]);
        }
        body->capAt[part][k]  = DotProduct(raw, pm[k]);
        body->capDir[part][k] = DotProduct(dirModel, pm[k]);
    }
}

// Cuts a part off, on model as it is about to be drawn.
static void CG_GoreCut(goreBody_t *body, const goreRig_t *rig, refEntity_t *model, int part)
{
    float  m[4][3];
    vec3_t world, dir, vel, light;

    if (rig->cut[part] < 0 || !CG_GoreBoneFrame(model, rig->cut[part], m, world)) {
        return;
    }

    dir[0] = dir[1] = dir[2] = 0;
    CG_GoreFindOpening(body, rig, model, part, world, dir);
    VectorCopy(world, body->stumpAt[part]);
    VectorCopy(dir, body->stumpDir[part]);
    VectorCopy(m[3], body->stumpCut[part]);
    body->stumpPending |= 1u << part;

    // Thrown off by what did it: a blast, or the round.
    if (cg.time - body->blastTime < 1000) {
        VectorSubtract(world, body->blastPos, vel);
        VectorNormalize(vel);
        VectorScale(vel, GoreRand(300, 520), vel);
        vel[2] += GoreRand(150, 300);
    } else {
        VectorScale(body->lastHitDir, GoreRand(110, 200), vel);
        vel[0] += crandom() * 40;
        vel[1] += crandom() * 40;
        vel[2] += GoreRand(70, 150);
    }

    cgi.R_GetLightingForDecal(light, dir, world);
    VectorScale(light, GORE_LIGHT_SCALE, light);

    if (body->explodePending & (1u << part)) {
        CG_GoreBurst(world, dir, light, 40, 14);
        if (part == GP_HEAD) {
            CG_GoreSpawnBits(world, dir, 12 + rand() % 6);
        }
    } else {
        CG_GoreSpawnGib(body, rig, model, part, vel, m[3]);
        CG_GoreBurst(world, dir, light, 16, 0);
    }

    body->severed |= rig->within[part] | (1u << part);

    if (cg_gore_debug->integer) {
        Com_Printf(
            "gore: entity %d loses its %s%s\n", body->entityNum, gore_partNames[part],
            (body->explodePending & (1u << part)) ? " (to pieces)" : ""
        );
    }
}

static void CG_GoreSever(goreBody_t *body, int part, qboolean toPieces)
{
    if (part < 0 || part >= GP_NUM || !cg_gore_dismember->integer || (body->severed & (1u << part)) || body->gib) {
        return;
    }

    body->severPending |= 1u << part;
    if (toPieces) {
        body->explodePending |= 1u << part;
    }
}

// The bones as they are drawn: the ragdoll's, if it has one, and every bone of
// a part that is gone folded into its joint.
static void CG_GoreApply(goreBody_t *body, refEntity_t *model)
{
    static const int order[GP_NUM] = {
        GP_HEAD, GP_L_UPPERARM, GP_L_FOREARM, GP_L_HAND, GP_R_UPPERARM, GP_R_FOREARM, GP_R_HAND,
        GP_L_THIGH, GP_L_CALF, GP_R_THIGH, GP_R_CALF
    };
    const goreRig_t *rig;
    float            folded[TIKI_MAX_BONES][3];
    qboolean         isFolded[TIKI_MAX_BONES];
    int              i, k, p, n;

    if (body->numDents && !(body->severed & (1u << GP_HEAD))) {
        model->gore_dents     = body->dents;
        model->num_gore_dents = body->numDents;
        model->renderfx |= RF_GORE_DENTS;
    }

    if (!(body->severed | body->severPending)) {
        return;
    }

    rig = CG_GoreRig(model->tiki);
    if (!rig) {
        body->severPending = 0;
        return;
    }

    for (p = 0; p < GP_NUM; p++) {
        if (body->severPending & (1u << order[p])) {
            CG_GoreCut(body, rig, model, order[p]);
        }
    }
    body->severPending   = 0;
    body->explodePending = 0;

    // Into the joint as it is now: a live man's limb goes where he moves it.
    memset(isFolded, 0, sizeof(isFolded));
    for (p = 0; p < GP_NUM; p++) {
        int   part = order[p];
        float m[4][3];

        if (!(body->severed & (1u << part)) || rig->cut[part] < 0 || isFolded[rig->cut[part]]) {
            continue;
        }

        if (CG_GoreBoneFrame(model, rig->cut[part], m, NULL)) {
            VectorCopy(m[3], body->stumpCut[part]);
        }

        for (i = 0; i < (int)rig->bones[part].size(); i++) {
            int bone = rig->bones[part][i];

            if (bone >= 0 && bone < TIKI_MAX_BONES && !isFolded[bone]) {
                isFolded[bone] = qtrue;
                VectorCopy(body->stumpCut[part], folded[bone]);
            }
        }
    }

    n = 0;
    for (i = 0; i < model->num_bone_overrides && model->bone_override && n < GORE_MAX_OVERRIDES; i++) {
        int bone = model->bone_override[i].boneIndex;

        if (bone >= 0 && bone < TIKI_MAX_BONES && isFolded[bone]) {
            continue;
        }
        body->ovr[n++] = model->bone_override[i];
    }
    for (k = 0; k < TIKI_MAX_BONES && n < GORE_MAX_OVERRIDES; k++) {
        if (isFolded[k]) {
            CG_GoreFoldInto(&body->ovr[n++], k, folded[k]);
        }
    }

    body->numOvr              = n;
    model->bone_override      = body->ovr;
    model->num_bone_overrides = n;
    model->renderfx |= RF_GORE_DENTS; // the folded triangles left out
    cgi.ForceUpdatePose(model);
}

qboolean CG_GoreHidesAttachment(int parentEntity, int tag)
{
    const goreBody_t *body;
    const goreRig_t  *rig;
    int               p;

    if (!CG_GoreEnabled() || parentEntity < 0 || parentEntity >= ENTITYNUM_NONE) {
        return qfalse;
    }

    body = CG_GoreFindBody(parentEntity);
    if (!body || !body->severed || !(rig = CG_GoreRig(body->tiki))) {
        return qfalse;
    }

    for (p = 0; p < GP_NUM; p++) {
        if ((body->severed & (1u << p))
            && std::find(rig->bones[p].begin(), rig->bones[p].end(), tag) != rig->bones[p].end()) {
            return qtrue;
        }
    }

    return qfalse;
}

//
// Dents
//

// A hollow in the head where a round went in or out: centred a little outside
// the skin, at out, so the sphere carves into it.
static void CG_GoreDent(goreBody_t *body, refEntity_t *model, const vec3_t at, const vec3_t out, float radius)
{
    const goreRig_t *rig = CG_GoreRig(model->tiki);
    float            m[4][3];
    float            scale = CG_GoreModelScale(model);
    vec3_t           centre, local, raw, outLocal, outModel;
    goreDent_t      *dent;
    int              k;

    if (!rig || !cg_gore_dismember->integer || body->numDents >= MAX_GORE_DENTS || (body->severed & (1u << GP_HEAD))) {
        return;
    }

    if (!CG_GoreBoneFrame(model, rig->cut[GP_HEAD], m, NULL) || scale <= 0) {
        return;
    }

    // world to model space before the scale, then into the head bone's frame
    VectorMA(at, radius * 0.45f, out, centre);
    VectorSubtract(centre, model->origin, centre);
    for (k = 0; k < 3; k++) {
        raw[k] = DotProduct(centre, model->axis[k]) / scale - model->tiki->load_origin[k];
    }
    VectorSubtract(raw, m[3], raw);
    for (k = 0; k < 3; k++) {
        outModel[k] = DotProduct(out, model->axis[k]);
    }
    for (k = 0; k < 3; k++) {
        local[k]    = DotProduct(raw, m[k]);
        outLocal[k] = DotProduct(outModel, m[k]);
    }

    dent            = &body->dents[body->numDents];
    dent->boneIndex = rig->cut[GP_HEAD];
    VectorCopy(local, dent->offset);
    VectorCopy(outLocal, dent->dir);
    VectorNormalize(dent->dir);
    dent->radius = radius / scale;
    VectorCopy(outLocal, body->dentOut[body->numDents]);
    body->dentPending |= 1u << body->numDents;
    body->numDents++;
    body->dirty = qtrue;
}

// A round into a head: a hollow where it went in and a bigger one where it
// came out, and the skin of each broken off and thrown. A head broken often
// enough comes apart.
static void CG_GoreHeadHit(goreBody_t *body, refEntity_t *model)
{
    vec3_t back;
    int    before = body->numDents;

    if (!cg_gore_dismember->integer || (body->severed & (1u << GP_HEAD))) {
        return;
    }

    if (body->numDents >= 10) {
        CG_GoreSever(body, GP_HEAD, qtrue);
        return;
    }

    VectorNegate(body->lastHitDir, back);
    CG_GoreDent(body, model, body->lastHitPos, back, body->lastHitLarge ? GoreRand(2.0f, 2.6f) : GoreRand(1.5f, 1.9f));
    if (body->numDents > before) {
        CG_GoreBreakDent(body, model, body->numDents - 1, body->lastHitLarge ? 6 + rand() % 4 : 3 + rand() % 3);
    }

    if (body->lastHitExit) {
        before = body->numDents;
        CG_GoreDent(body, model, body->lastHitExitPos, body->lastHitDir, body->lastHitLarge ? GoreRand(2.6f, 3.2f) : GoreRand(1.9f, 2.4f));
        if (body->numDents > before) {
            CG_GoreBreakDent(body, model, body->numDents - 1, body->lastHitLarge ? 8 + rand() % 5 : 5 + rand() % 4);
        }
    }
}

// What a round did that a man had his last of.
static void CG_GoreKillShot(goreBody_t *body, refEntity_t *model)
{
    float  r = random();
    vec3_t back;

    VectorNegate(body->lastHitDir, back);

    switch (body->lastHitZone) {
    case GP_HEAD:
        // broken where it was hit already (CG_GoreHeadHit); a heavy round
        // that kills may take the rest of it
        if (body->lastHitLarge && r < 0.25f) {
            CG_GoreSever(body, GP_HEAD, qtrue);
        }
        break;
    default:
        // a limb, or the head at the neck, comes off only in a blast
        // (CG_GoreBlastBody)
        break;
    }
}

// A blast close by that killed him, or found him dead.
static void CG_GoreBlastBody(goreBody_t *body)
{
    int   order[GP_NUM], p, cuts = 0;
    float chance = 0.6f * body->blastNear;

    for (p = 0; p < GP_NUM; p++) {
        order[p] = p;
    }
    for (p = GP_NUM - 1; p > 0; p--) {
        int j = rand() % (p + 1), t = order[p];

        order[p] = order[j];
        order[j] = t;
    }

    if (body->blastNear > 0.6f && random() < 0.4f) {
        CG_GoreSever(body, GP_HEAD, qtrue);
        cuts++;
    }

    for (p = 0; p < GP_NUM && cuts < 2; p++) {
        if (order[p] != GP_HEAD && random() < chance) {
            CG_GoreSever(body, order[p], qfalse);
            cuts++;
        }
    }

    body->blastTime = 0;
}

// Rounds into a corpse: a head is opened further each time a heavy round goes
// through it (CG_GoreHeadHit). A limb comes off only in a blast.
static void CG_GoreCorpseHit(goreBody_t *body, refEntity_t *model)
{
    int zone = body->lastHitZone;

    if (zone < 0 || zone >= GP_NUM) {
        return;
    }

    body->partHits[zone] += body->lastHitLarge ? 3 : 1;
}

// Every entity, just before it is drawn.
void CG_GoreModifyEntity(centity_t *cent, refEntity_t *model, qboolean dead)
{
    goreBody_t *body;

    if (!CG_GoreEnabled() || !model->tiki || !model->tiki->a || !model->tiki->a->bIsCharacter
        || (model->renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK))) {
        return;
    }

    body = CG_GoreBodyFor(cent->currentState.number, model, dead);
    if (body) {
        CG_GoreApply(body, model);
    }
}

void CG_GoreSever_f(void)
{
    static const char *names[GP_NUM] = {
        "head", "larm", "lforearm", "lhand", "rarm", "rforearm", "rhand", "lleg", "lshin", "rleg", "rshin"
    };
    goreDrawn_t *best     = NULL;
    float        bestDot  = 0.5f;
    const char  *which    = cgi.Argc() > 1 ? cgi.Argv(1) : "";
    qboolean     toPieces = cgi.Argc() > 2 && !Q_stricmp(cgi.Argv(2), "explode") ? qtrue : qfalse;
    goreBody_t  *body;
    int          i, p;

    // The drawn list is emptied each frame, so the body is found among the
    // entities in front of the view instead.
    for (i = 0; i < MAX_GENTITIES; i++) {
        centity_t *cent = &cg_entities[i];
        vec3_t     to;
        float      d;

        if (!cent->currentValid || i == cg.snap->ps.clientNum) {
            continue;
        }
        VectorSubtract(cent->lerpOrigin, cg.refdef.vieworg, to);
        to[2] += 40;
        if (VectorNormalize(to) > 1500) {
            continue;
        }
        d = DotProduct(to, cg.refdef.viewaxis[0]);
        if (d > bestDot) {
            static goreDrawn_t pick;
            dtiki_t           *tiki = cgi.R_Model_GetHandle(cgs.model_draw[cent->currentState.modelindex]);

            if (!tiki || !tiki->a || !tiki->a->bIsCharacter) {
                continue;
            }
            memset(&pick, 0, sizeof(pick));
            pick.entityNum = i;
            pick.ref.tiki  = tiki;
            pick.dead      = (cent->currentState.eFlags & EF_DEAD) ? qtrue : qfalse;
            best           = &pick;
            bestDot        = d;
        }
    }

    if (!best) {
        Com_Printf("gore_sever: no one in front of you\n");
        return;
    }

    if (!Q_stricmp(which, "headshot")) {
        // a heavy round through the head, along the view, as if it killed him
        centity_t *cent = &cg_entities[best->entityNum];
        vec3_t     pos, dir;

        VectorCopy(cent->lerpOrigin, pos);
        pos[2] += 86; // about the middle of a standing man's head
        VectorSubtract(pos, cg.refdef.vieworg, dir);
        VectorNormalize(dir);
        VectorMA(pos, -4, dir, pos);
        CG_GoreNoteHit(pos, dir, 1);
        return;
    }

    body = CG_GoreBody(best);
    for (p = 0; p < GP_NUM; p++) {
        if (!Q_stricmp(which, "all") || !Q_stricmp(which, names[p])) {
            body->lastHitTime = 0;
            VectorCopy(cg.refdef.viewaxis[0], body->lastHitDir);
            CG_GoreSever(body, p, toPieces);
            if (Q_stricmp(which, "all")) {
                break;
            }
        }
    }

    if (p == GP_NUM && Q_stricmp(which, "all")) {
        Com_Printf("gore_sever <head|larm|lforearm|lhand|rarm|rforearm|rhand|lleg|lshin|rleg|rshin|all> [explode], or headshot\n");
    }
}

void CG_GoreBlast_f(void)
{
    const int kind = cgi.Argc() > 1 ? atoi(cgi.Argv(1)) : 0;
    trace_t   tr;
    vec3_t    end;

    VectorMA(cg.refdef.vieworg, 4096, cg.refdef.viewaxis[0], end);
    CG_Trace(&tr, cg.refdef.vieworg, vec3_origin, vec3_origin, end, cg.snap ? cg.snap->ps.clientNum : ENTITYNUM_NONE, MASK_SHOT, qfalse, qfalse, "gore blast");
    VectorMA(tr.endpos, 8, tr.plane.normal, end);

    CG_RagdollNoteExplosion(end, kind);
    CG_GoreNoteExplosion(end, kind);
    CG_PhysicsNoteExplosion(end, kind);
}

// Where a part's stump is now, on the body as drawn, and which way it faces.
static qboolean CG_GoreStumpFrame(goreBody_t *body, const goreRig_t *rig, refEntity_t *ref, int part, vec3_t at, vec3_t dir)
{
    float  pm[4][3];
    float  scale = CG_GoreModelScale(ref);
    vec3_t local, dirModel;
    int    k;

    if (!CG_GoreBoneFrame(ref, rig->parent[part], pm, NULL)) {
        return qfalse;
    }

    for (k = 0; k < 3; k++) {
        local[k] = (pm[3][k] + body->capAt[part][0] * pm[0][k] + body->capAt[part][1] * pm[1][k]
                    + body->capAt[part][2] * pm[2][k] + ref->tiki->load_origin[k])
                 * scale;
        dirModel[k] = body->capDir[part][0] * pm[0][k] + body->capDir[part][1] * pm[1][k] + body->capDir[part][2] * pm[2][k];
    }

    VectorCopy(ref->origin, at);
    for (k = 0; k < 3; k++) {
        VectorMA(at, local[k], ref->axis[k], at);
    }
    CG_GoreModelToWorldDir(ref, dirModel, dir);
    VectorNormalize(dir);
    return qtrue;
}

// The stumps and hollows still to be given their decals, on the body as drawn.
static void CG_GoreLayPending(goreBody_t *body, goreDrawn_t *drawn)
{
    const goreRig_t *rig;
    int              p;

    if (!(body->stumpPending | body->dentPending)) {
        return;
    }

    for (p = 0; p < GP_NUM; p++) {
        if (body->stumpPending & (1u << p)) {
            vec3_t at, dir;

            VectorCopy(body->stumpAt[p], at);
            VectorCopy(body->stumpDir[p], dir);
            rig = CG_GoreRig(drawn->ref.tiki);
            if (!body->gib && rig) {
                CG_GoreStumpFrame(body, rig, &drawn->ref, p, at, dir);
            }
            qboolean laid = CG_GoreLayNear(body, drawn, GK_STUMP, at, dir, body->gib ? gore_stumpRadius[p] * cg_gore_scale->value : body->capRadius[p]);

            if (cg_gore_debug->integer) {
                Com_Printf(
                    "gore: stump of the %s on entity %d at %.0f %.0f %.0f facing %.2f %.2f %.2f, %.1f wide: %s\n",
                    gore_partNames[p], body->entityNum, at[0], at[1], at[2], dir[0], dir[1], dir[2],
                    body->gib ? gore_stumpRadius[p] : body->capRadius[p],
                    laid ? va("%d pieces", (int)body->decals.back().frags.size()) : "not laid"
                );
            }
            if (laid) {
                goreDecal_t &decal = body->decals.back();

                // it pumps for a while, from a man; it drips, from a part
                decal.bleedUntil = cg.time + (body->gib ? 3000 : 9000);
                decal.nextDrop   = cg.time;
                if (!body->gib) {
                    decal.spurtUntil = cg.time + (int)GoreRand(2500, 4000);
                    decal.nextSpurt  = cg.time;
                }
            }
        }
    }
    body->stumpPending = 0;

    rig = CG_GoreRig(drawn->ref.tiki);
    for (p = 0; p < body->numDents && rig; p++) {
        refEntity_t ref = drawn->ref;
        float       m[4][3];
        vec3_t      centre, out;
        float       scale = CG_GoreModelScale(&ref);
        int         k;

        if (!(body->dentPending & (1u << p)) || !CG_GoreBoneFrame(&ref, body->dents[p].boneIndex, m, NULL)) {
            continue;
        }

        for (k = 0; k < 3; k++) {
            centre[k] = (m[3][k] + body->dents[p].offset[0] * m[0][k] + body->dents[p].offset[1] * m[1][k]
                         + body->dents[p].offset[2] * m[2][k] + ref.tiki->load_origin[k])
                      * scale;
            out[k] = body->dentOut[p][0] * m[0][k] + body->dentOut[p][1] * m[1][k] + body->dentOut[p][2] * m[2][k];
        }
        {
            vec3_t w, wout;

            VectorCopy(ref.origin, w);
            for (k = 0; k < 3; k++) {
                VectorMA(w, centre[k], ref.axis[k], w);
            }
            CG_GoreModelToWorldDir(&ref, out, wout);
            VectorNormalize(wout);
            CG_GoreLayNear(body, drawn, GK_BRAIN, w, wout, body->dents[p].radius * scale * 1.15f);
        }
    }
    body->dentPending = 0;
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

        // close enough to take him apart, if it kills him or he is dead
        if (dist < range * 0.5f) {
            body            = CG_GoreBody(d);
            body->dead      = d->dead;
            body->blastTime = cg.time;
            body->blastKind = blast->kind;
            body->blastNear = 1.0f - dist / (range * 0.5f);
            VectorCopy(blast->pos, body->blastPos);
            if (d->dead && !body->gib) {
                CG_GoreBlastBody(body);
                body->blastTime = cg.time; // for which way the parts fly
            }
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

    CG_GoreBlastGibs(pos, kind);
}

//=============================================================
// Blood: drops, splashes and pools
//=============================================================

static void CG_GoreSpawnDrop(const vec3_t pos, const vec3_t vel, float size, const vec3_t light)
{
    goreDrop_t *drop = &gore_drops[gore_nextDrop];

    gore_nextDrop = (gore_nextDrop + 1) % GORE_MAX_DROPS;

    drop->active = qtrue;
    drop->chunk  = qfalse;
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
        pool->radius = (body->gib ? 5.0f + Q_min(wounds, 4) : 12.0f + 3.0f * Q_min(wounds, 8)) * cg_gore_scale->value;
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
                float size = drop->size * (drop->chunk ? 2.5f : 1.0f);

                CG_ImpactMark(
                    gore_splatShader, tr.endpos, tr.plane.normal, GoreRand(0, 360), size * GoreRand(1.5f, 2.6f),
                    size * GoreRand(1.5f, 2.6f), GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, GORE_LIGHT_SCALE, 1, qfalse, qfalse, qtrue, qfalse, 0.5f, 0.5f
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
        len = drop->size * (1.0f + speed * (drop->chunk ? 0.002f : 0.012f));

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

        cgi.R_AddPolyToScene(drop->chunk ? gore_chunkShader : gore_dropShader, 4, verts, 0);
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

        // On a part that is gone, folded to a point: nothing to draw, and no
        // blood comes out of it.
        if (!decal.frags.empty()) {
            vec3_t mins, maxs;

            ClearBounds(mins, maxs);
            for (t = 0; t < (int)decal.frags.size(); t++) {
                for (k = 0; k < decal.frags[t].numPts; k++) {
                    AddPointToBounds(&decal.xyz[(t * GORE_FRAG_PTS + k) * 3], mins, maxs);
                }
            }
            if (Distance(mins, maxs) < 0.3f) {
                decal.hidden = qtrue;
                continue;
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

    // the parts that came off, among the bodies a round can find
    CG_GoreAddGibs();
    CG_GoreAddBits();

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

    CG_GoreStreaks();

    // Nearest first, so the budget runs out on the far ones.
    for (i = 0; i < gore_numDrawn; i++) {
        goreDrawn_t *drawn = &gore_drawn[i];
        goreBody_t  *body  = CG_GoreFindBody(drawn->entityNum);
        vec3_t       toBody;
        float        d;

        if (!body || (body->decals.empty() && !(body->stumpPending | body->dentPending)) || drawn->noDraw) {
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

        if (body->stumpPending | body->dentPending) {
            CG_GoreLayPending(body, drawn);
            body->dirty = qtrue;
            CG_GorePose(body, drawn);
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
            for (i = 0; i < GORE_MAX_GIBS; i++) {
                const goreGib_t *g = &gore_gibs[i];

                if (g->active) {
                    Com_Printf(
                        "gore:   %s: %d points, from %.0f %.0f %.0f to %.0f %.0f %.0f%s\n", gore_partNames[g->part], g->numPts,
                        g->pt[0][0], g->pt[0][1], g->pt[0][2], g->pt[Q_max(g->numPts - 1, 0)][0], g->pt[Q_max(g->numPts - 1, 0)][1],
                        g->pt[Q_max(g->numPts - 1, 0)][2], g->resting ? ", resting" : ""
                    );
                }
            }
        }
    }

    gore_numDrawn = 0;
}
