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
// cg_ragdoll_live.cpp: living bodies, in single player.
//
// A man who is shot is made, for a second or so, a Jolt ragdoll of the same
// rig the corpses use (cg_physics_ragdoll.cpp), but alive: its pelvis is
// carried along where the animation has it, and every joint's motors pull the
// rest of it towards the animation's pose. The shot kicks the joint it hit, and
// the motors are weakened for a moment, so that part gives way, the back sags a
// little, and the animation takes him back. If he dies meanwhile, the body is
// handed to the corpse code as it is (CG_LiveRagdollTakeBody) and falls from
// there.
//
// Drawn, only the back, the head and the arms follow the physics, each bone
// turned by as much as its part has been turned off the animation: the legs
// keep the animation's, so the feet stay where they stand. The bodies are not
// collided with their own man's box (people and ragdolls pass through each
// other), but are with the world, props and the dead.

#include "cg_ragdoll_live.h"
#include "cg_ragdoll.h"
#include "cg_physics_ragdoll.h"

#include <cmath>

static cvar_t *cg_ragdoll_live;
static cvar_t *cg_ragdoll_live_max;
static cvar_t *cg_ragdoll_live_kick;
static cvar_t *cg_ragdoll_live_soft;
static cvar_t *cg_ragdoll_live_recover;
static cvar_t *cg_ragdoll_live_hold;
static cvar_t *cg_ragdoll_live_fade;

#define LR_MAX_BODIES 16
#define LR_MAX_HITS   16

// A hit is matched to a man drawn within this long of it, and only within
// this far of one of his joints.
#define LR_HIT_WINDOW 150
#define LR_HIT_RADIUS 20.0f

// How fast a hit sets the joint it struck moving, units a second, before the
// cvar scale: a rifle round, and a heavier one (iLarge counts calibre).
#define LR_KICK_SPEED       130.0f
#define LR_KICK_SPEED_LARGE 60.0f

// Each joint's parent in the rig (as rd_joints in cg_ragdoll.cpp has it). A
// hit kicks the joint struck and everything beyond it, so a man shot in the
// chest is turned at the hips as a piece rather than one short part of his back
// being flicked against the rest.
static const short lr_jointParent[RD_NUM_JOINTS] = {
    -1,        // RD_PELVIS
    RD_PELVIS, // RD_SPINE
    RD_SPINE,  // RD_SPINE1
    RD_SPINE1, // RD_SPINE2
    RD_SPINE2, // RD_NECK
    RD_NECK,   // RD_HEAD
    RD_HEAD,   // RD_HEADTIP
    RD_SPINE2, // RD_LUARM
    RD_LUARM,  // RD_LFARM
    RD_LFARM,  // RD_LHAND
    RD_LHAND,  // RD_LHANDTIP
    RD_SPINE2, // RD_RUARM
    RD_RUARM,  // RD_RFARM
    RD_RFARM,  // RD_RHAND
    RD_RHAND,  // RD_RHANDTIP
    RD_PELVIS, // RD_LTHIGH
    RD_LTHIGH, // RD_LCALF
    RD_LCALF,  // RD_LFOOT
    RD_LFOOT,  // RD_LTOE
    RD_PELVIS, // RD_RTHIGH
    RD_RTHIGH, // RD_RCALF
    RD_RCALF,  // RD_RFOOT
    RD_RFOOT,  // RD_RTOE
};

// Whether joint j is joint root or beyond it.
static qboolean CG_LiveRagdollBeyond(int j, int root)
{
    for (; j >= 0; j = lr_jointParent[j]) {
        if (j == root) {
            return qtrue;
        }
    }
    return qfalse;
}

// How much of a hit on the trunk the arms and the head take with it.
#define LR_KICK_ARMS 0.3f
#define LR_KICK_HEAD 0.4f

// Further than this off the animation, a body is not reacting but lost.
#define LR_MAX_OFF 64.0f

// A body not drawn for this long has gone (out of sight, or removed).
#define LR_STALE 500

// The bones drawn from the physics, parent first. Each is turned by as much as
// the part between its two joints has been turned off the animation.
typedef struct {
    const char *name;
    short       a, b;
    short       parent; // in this table, -1: the pelvis, which is the animation's
} lrBoneDef_t;

static const lrBoneDef_t lr_bones[] = {
    {"Bip01 Spine",      RD_SPINE,  RD_SPINE1,   -1},
    {"Bip01 Spine1",     RD_SPINE1, RD_SPINE2,   0 },
    {"Bip01 Spine2",     RD_SPINE2, RD_NECK,     1 },
    {"Bip01 Neck",       RD_NECK,   RD_HEAD,     2 },
    {"Bip01 Head",       RD_HEAD,   RD_HEADTIP,  3 },
    {"Bip01 L UpperArm", RD_LUARM,  RD_LFARM,    2 },
    {"Bip01 L Forearm",  RD_LFARM,  RD_LHAND,    5 },
    {"Bip01 L Hand",     RD_LHAND,  RD_LHANDTIP, 6 },
    {"Bip01 R UpperArm", RD_RUARM,  RD_RFARM,    2 },
    {"Bip01 R Forearm",  RD_RFARM,  RD_RHAND,    8 },
    {"Bip01 R Hand",     RD_RHAND,  RD_RHANDTIP, 9 },
};

#define LR_NUM_BONES ((int)(sizeof(lr_bones) / sizeof(lr_bones[0])))

typedef struct {
    int      entityNum; // -1: free
    int      jolt;
    dtiki_t *tiki;
    int      bone[LR_NUM_BONES];
    int      lastHit;
    int      lastSeen;
    int      lastPrint; // cg_ragdoll_debug

    boneOverride_t overrides[LR_NUM_BONES];
} lrBody_t;

typedef struct {
    vec3_t   pos, dir;
    int      large;
    int      time;
    qboolean used;
} lrHit_t;

static lrBody_t lr_bodies[LR_MAX_BODIES];
static lrHit_t  lr_hits[LR_MAX_HITS];
static int      lr_hitHead;

static void CG_LiveRagdollFree(lrBody_t *body, qboolean destroy)
{
    if (destroy && body->jolt) {
        CG_JoltRagdollDestroy(body->jolt);
    }
    body->jolt      = 0;
    body->entityNum = -1;
    body->tiki      = NULL;
}

void CG_LiveRagdollInit(void)
{
    int i;

    // Whether a living man shot reacts with his body (single player, with the
    // Jolt ragdolls, cg_ragdoll_solver 1).
    cg_ragdoll_live = cgi.Cvar_Get("cg_ragdoll_live", "1", CVAR_ARCHIVE);
    // How many may be reacting at once.
    cg_ragdoll_live_max = cgi.Cvar_Get("cg_ragdoll_live_max", "6", CVAR_ARCHIVE);
    // Scale on how hard a hit kicks the part it struck.
    cg_ragdoll_live_kick = cgi.Cvar_Get("cg_ragdoll_live_kick", "1", CVAR_ARCHIVE);
    // How much of their strength the motors lose at a hit (0..1), and how long
    // they take to get it back, milliseconds.
    cg_ragdoll_live_soft    = cgi.Cvar_Get("cg_ragdoll_live_soft", "0.6", CVAR_ARCHIVE);
    cg_ragdoll_live_recover = cgi.Cvar_Get("cg_ragdoll_live_recover", "400", CVAR_ARCHIVE);
    // How long after the last hit the body is drawn from the physics, and how
    // long it then takes to hand back to the animation, milliseconds.
    cg_ragdoll_live_hold = cgi.Cvar_Get("cg_ragdoll_live_hold", "700", CVAR_ARCHIVE);
    cg_ragdoll_live_fade = cgi.Cvar_Get("cg_ragdoll_live_fade", "400", CVAR_ARCHIVE);

    for (i = 0; i < LR_MAX_BODIES; i++) {
        lr_bodies[i].entityNum = -1;
        lr_bodies[i].jolt      = 0;
    }
    memset(lr_hits, 0, sizeof(lr_hits));
    lr_hitHead = 0;
}

void CG_LiveRagdollShutdown(void)
{
    int i;

    for (i = 0; i < LR_MAX_BODIES; i++) {
        if (lr_bodies[i].entityNum >= 0) {
            CG_LiveRagdollFree(&lr_bodies[i], qtrue);
        }
    }
}

static qboolean CG_LiveRagdollEnabled(void)
{
    return (qboolean)(cg_ragdoll_live && cg_ragdoll_live->integer && cg_ragdoll->integer && cgs.ragdollAllowed
                      && cg_ragdoll_solver->integer == 1 && cgs.gametype == GT_SINGLE_PLAYER);
}

void CG_LiveRagdollNoteHit(const vec3_t pos, const vec3_t dir, int large)
{
    lrHit_t *hit;

    if (!CG_LiveRagdollEnabled()) {
        return;
    }

    hit = &lr_hits[lr_hitHead];
    lr_hitHead = (lr_hitHead + 1) % LR_MAX_HITS;

    VectorCopy(pos, hit->pos);
    VectorCopy(dir, hit->dir);
    VectorNormalize(hit->dir);
    hit->large = large;
    hit->time  = cg.time;
    hit->used  = qfalse;
}

int CG_LiveRagdollTakeBody(int entityNum)
{
    int i;

    for (i = 0; i < LR_MAX_BODIES; i++) {
        lrBody_t *body = &lr_bodies[i];

        if (body->entityNum == entityNum) {
            const int jolt = body->jolt;

            CG_LiveRagdollFree(body, qfalse);
            return jolt;
        }
    }
    return 0;
}

static lrBody_t *CG_LiveRagdollFind(int entityNum)
{
    int i;

    for (i = 0; i < LR_MAX_BODIES; i++) {
        if (lr_bodies[i].entityNum == entityNum) {
            return &lr_bodies[i];
        }
    }
    return NULL;
}

// Bodies whose men have not been drawn for a while.
static void CG_LiveRagdollExpire(void)
{
    int i;

    for (i = 0; i < LR_MAX_BODIES; i++) {
        lrBody_t *body = &lr_bodies[i];

        if (body->entityNum >= 0 && (cg.time - body->lastSeen > LR_STALE || cg.time < body->lastSeen)) {
            CG_LiveRagdollFree(body, qtrue);
        }
    }
}

static lrBody_t *CG_LiveRagdollStart(centity_t *cent, refEntity_t *model, const vec3_t p[RD_NUM_JOINTS], const float radius[RD_NUM_JOINTS])
{
    vec3_t    v[RD_NUM_JOINTS];
    lrBody_t *body = NULL;
    int       count = 0, i;

    for (i = 0; i < LR_MAX_BODIES; i++) {
        if (lr_bodies[i].entityNum >= 0) {
            count++;
        } else if (!body) {
            body = &lr_bodies[i];
        }
    }
    if (!body || count >= cg_ragdoll_live_max->integer) {
        return NULL;
    }

    // Moving with the man.
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        VectorCopy(cent->currentState.pos.trDelta, v[i]);
    }

    body->jolt = CG_JoltRagdollCreate(p, v, radius, 0.0f);
    if (!body->jolt) {
        return NULL;
    }
    CG_JoltRagdollSetLive(body->jolt, qtrue, 0.0f);

    body->entityNum = cent->currentState.number;
    body->tiki      = model->tiki;
    body->lastPrint = 0;
    if (cg_ragdoll_debug->integer) {
        cgi.Printf("live ragdoll: entity %d reacting (t %d)\n", body->entityNum, cg.time);
    }
    body->lastSeen  = cg.time;
    body->lastHit   = cg.time;
    for (i = 0; i < LR_NUM_BONES; i++) {
        body->bone[i] = cgi.Tag_NumForName(model->tiki, lr_bones[i].name);
    }
    return body;
}

// The rotation turning direction from to direction to, by share of the way.
static void CG_LiveRagdollSwing(const vec3_t from, const vec3_t to, float share, float m[3][3])
{
    vec3_t a, b, axis;
    float  s, c, angle, sn, cs, t;

    VectorCopy(from, a);
    VectorCopy(to, b);
    if (VectorNormalize(a) < 1e-4f || VectorNormalize(b) < 1e-4f) {
        AxisClear(m);
        return;
    }

    CrossProduct(a, b, axis);
    s = VectorNormalize(axis);
    c = DotProduct(a, b);
    if (s < 1e-6f) {
        AxisClear(m);
        return;
    }

    angle = atan2f(s, c) * share;
    sn    = sinf(angle);
    cs    = cosf(angle);
    t     = 1.0f - cs;

    m[0][0] = t * axis[0] * axis[0] + cs;
    m[0][1] = t * axis[0] * axis[1] - sn * axis[2];
    m[0][2] = t * axis[0] * axis[2] + sn * axis[1];
    m[1][0] = t * axis[0] * axis[1] + sn * axis[2];
    m[1][1] = t * axis[1] * axis[1] + cs;
    m[1][2] = t * axis[1] * axis[2] - sn * axis[0];
    m[2][0] = t * axis[0] * axis[2] - sn * axis[1];
    m[2][1] = t * axis[1] * axis[2] + sn * axis[0];
    m[2][2] = t * axis[2] * axis[2] + cs;
}

static void CG_LiveRagdollRotate(const float m[3][3], const vec3_t in, vec3_t out)
{
    out[0] = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2];
    out[1] = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2];
    out[2] = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2];
}

static qboolean CG_LiveRagdollFinite(const float *v, int count)
{
    for (int i = 0; i < count; i++) {
        if (!isfinite(v[i]) || fabsf(v[i]) > 1.0e6f) {
            return qfalse;
        }
    }
    return qtrue;
}

static float CG_LiveRagdollSmooth(float t)
{
    t = Q_clamp_float(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Hits that landed on this man since he was last drawn: the nearest joint of
// each is kicked, and the body made if he had none. qfalse if none did.
static qboolean CG_LiveRagdollTakeHits(
    centity_t *cent, refEntity_t *model, lrBody_t **bodyp, vec3_t p[RD_NUM_JOINTS], float radius[RD_NUM_JOINTS], qboolean *haveJoints
)
{
    vec3_t   dv[RD_NUM_JOINTS];
    qboolean kicked = qfalse;
    int      i, j;

    memset(dv, 0, sizeof(dv));

    for (i = 0; i < LR_MAX_HITS; i++) {
        lrHit_t *hit = &lr_hits[i];
        vec3_t   d;
        float    best = LR_HIT_RADIUS;
        int      joint = -1;

        if (hit->used || !hit->time || cg.time - hit->time > LR_HIT_WINDOW || cg.time < hit->time) {
            continue;
        }

        // Near enough to be worth reading the pose for.
        VectorSubtract(hit->pos, model->origin, d);
        if (d[0] * d[0] + d[1] * d[1] > 48.0f * 48.0f || d[2] < -16.0f || d[2] > 110.0f) {
            continue;
        }

        if (!*haveJoints) {
            if (!CG_RagdollAnimJoints(model, p, radius)) {
                return qfalse;
            }
            *haveJoints = qtrue;
        }

        for (j = 0; j < RD_NUM_JOINTS; j++) {
            const float dist = Distance(hit->pos, p[j]);

            if (dist < best) {
                best  = dist;
                joint = j;
            }
        }
        if (joint < 0) {
            continue;
        }

        hit->used = qtrue;
        {
            float speed = (LR_KICK_SPEED + LR_KICK_SPEED_LARGE * hit->large) * cg_ragdoll_live_kick->value;

            // The hips and legs are the animation's, carried and not drawn
            // from the physics: hit there, the man lurches above them.
            if (joint == RD_PELVIS || joint == RD_SPINE || CG_LiveRagdollBeyond(joint, RD_LTHIGH)
                || CG_LiveRagdollBeyond(joint, RD_RTHIGH)) {
                joint = RD_SPINE1;
                speed *= 0.5f;
            }

            for (j = 0; j < RD_NUM_JOINTS; j++) {
                float share = 1.0f;

                if (!CG_LiveRagdollBeyond(j, joint)) {
                    continue;
                }

                // Carried along by the trunk, not struck: an arm kicked as
                // hard as the chest it hangs from was flung out and swung
                // back like a rope, rifle and all.
                if (!CG_LiveRagdollBeyond(joint, RD_LUARM) && !CG_LiveRagdollBeyond(joint, RD_RUARM)
                    && (CG_LiveRagdollBeyond(j, RD_LUARM) || CG_LiveRagdollBeyond(j, RD_RUARM))) {
                    share = LR_KICK_ARMS;
                } else if (j != joint && CG_LiveRagdollBeyond(j, RD_NECK) && !CG_LiveRagdollBeyond(joint, RD_NECK)) {
                    share = LR_KICK_HEAD;
                }
                VectorMA(dv[j], speed * share, hit->dir, dv[j]);
            }
        }
        kicked = qtrue;
    }

    if (!kicked) {
        return qfalse;
    }

    if (!*bodyp) {
        *bodyp = CG_LiveRagdollStart(cent, model, p, radius);
        if (!*bodyp) {
            return qfalse;
        }
    }

    (*bodyp)->lastHit = cg.time;
    CG_JoltRagdollAddVelocity((*bodyp)->jolt, dv);
    return qtrue;
}

void CG_LiveRagdollUpdateEntity(centity_t *cent, refEntity_t *model)
{
    vec3_t    p[RD_NUM_JOINTS], q[RD_NUM_JOINTS], v[RD_NUM_JOINTS], normal[RD_NUM_JOINTS];
    vec3_t    animPos[LR_NUM_BONES], animAxis[LR_NUM_BONES][3], pos[LR_NUM_BONES];
    float     swing[LR_NUM_BONES][3][3];
    float     radius[RD_NUM_JOINTS];
    qboolean  contact[RD_NUM_JOINTS], haveJoints = qfalse, valid[LR_NUM_BONES];
    lrBody_t *body;
    float     since, strength, weight;
    int       mask, n = 0, i, k;

    CG_LiveRagdollExpire();

    body = CG_LiveRagdollFind(cent->currentState.number);

    if (!CG_LiveRagdollEnabled() || model->bone_override || (cent->currentState.eFlags & EF_DEAD) || !model->tiki
        || !model->tiki->a || !model->tiki->a->bIsCharacter || cent->currentState.number == cg.snap->ps.clientNum) {
        // The dead are the corpse code's, which takes the body over itself.
        if (body && !(cent->currentState.eFlags & EF_DEAD)) {
            CG_LiveRagdollFree(body, qtrue);
        }
        return;
    }

    if (body && body->tiki != model->tiki) {
        CG_LiveRagdollFree(body, qtrue);
        body = NULL;
    }

    CG_LiveRagdollTakeHits(cent, model, &body, p, radius, &haveJoints);

    if (!body) {
        return;
    }
    body->lastSeen = cg.time;

    since    = (float)(cg.time - body->lastHit);
    weight   = 1.0f - CG_LiveRagdollSmooth((since - cg_ragdoll_live_hold->value) / Q_max(1.0f, cg_ragdoll_live_fade->value));
    strength = 1.0f
             - Q_clamp_float(cg_ragdoll_live_soft->value, 0.0f, 1.0f)
                   * (1.0f - CG_LiveRagdollSmooth(since / Q_max(1.0f, cg_ragdoll_live_recover->value)));

    if (weight <= 0.0f) {
        CG_LiveRagdollFree(body, qtrue);
        return;
    }

    // The animation's pose, which the body follows and the bones are drawn
    // from (read with no override, as CG_RagdollAnimJoints leaves the model).
    if (!haveJoints && !CG_RagdollAnimJoints(model, p, radius)) {
        CG_LiveRagdollFree(body, qtrue);
        return;
    }

    CG_JoltRagdollDrive(body->jolt, p, strength);

    if (!CG_JoltRagdollRead(body->jolt, q, v, contact, normal, &mask)) {
        // Gone from the world (the map changed).
        CG_LiveRagdollFree(body, qfalse);
        return;
    }

    // A body the physics has lost hold of is dropped, never drawn: a bone
    // matrix that is not a number reaches the renderer's shadows and lights,
    // and the whole world went black until the renderer was restarted.
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        if (!CG_LiveRagdollFinite(q[i], 3) || Distance(q[i], p[i]) > LR_MAX_OFF) {
            if (cg_ragdoll_debug->integer) {
                cgi.Printf("live ragdoll: entity %d dropped: joint %d at %.1f %.1f %.1f, %.1f off\n", body->entityNum, i,
                           q[i][0], q[i][1], q[i][2], Distance(q[i], p[i]));
            }
            CG_LiveRagdollFree(body, qtrue);
            return;
        }
    }

    if (cg_ragdoll_debug->integer && cg.time - body->lastPrint >= 100) {
        float most = 0.0f;
        int   at   = 0;

        for (i = 0; i < RD_NUM_JOINTS; i++) {
            const float d = Distance(p[i], q[i]);

            if (d > most) {
                most = d;
                at   = i;
            }
        }
        body->lastPrint = cg.time;
        cgi.Printf(
            "live ragdoll: entity %d +%dms weight %.2f strength %.2f off the animation %.1f units (joint %d)\n",
            body->entityNum, cg.time - body->lastHit, weight, strength, most, at
        );
        if (cg_ragdoll_debug->integer > 1) {
            char line[512] = "";

            for (i = 0; i < RD_NUM_JOINTS; i++) {
                Q_strcat(line, sizeof(line), va(" %d:%.1f", i, Distance(p[i], q[i])));
            }
            cgi.Printf("live ragdoll: per joint%s\n", line);
        }
    }

    for (i = 0; i < LR_NUM_BONES; i++) {
        const lrBoneDef_t *def = &lr_bones[i];
        vec3_t             animDir, physDir;

        valid[i] = (qboolean)(body->bone[i] >= 0 && CG_RagdollBoneWorld(model, body->bone[i], animPos[i], animAxis[i]));
        if (!valid[i]) {
            AxisClear(swing[i]);
            continue;
        }

        VectorSubtract(p[def->b], p[def->a], animDir);
        VectorSubtract(q[def->b], q[def->a], physDir);
        CG_LiveRagdollSwing(animDir, physDir, weight, swing[i]);
    }

    // Each bone turned by its swing, and hung off its parent as the parent was
    // turned, so the bones keep their lengths and the animation's own roll.
    for (i = 0; i < LR_NUM_BONES; i++) {
        const int       parent = lr_bones[i].parent;
        boneOverride_t *out    = &body->overrides[n];
        vec3_t          axis[3], rel;

        if (!valid[i]) {
            continue;
        }

        if (parent >= 0 && valid[parent]) {
            VectorSubtract(animPos[i], animPos[parent], rel);
            CG_LiveRagdollRotate(swing[parent], rel, pos[i]);
            VectorAdd(pos[parent], pos[i], pos[i]);
        } else {
            VectorCopy(animPos[i], pos[i]);
        }

        for (k = 0; k < 3; k++) {
            CG_LiveRagdollRotate(swing[i], animAxis[i][k], axis[k]);
        }

        // A bone hung off one not drawn from the physics moves with nothing.
        if (parent >= 0 && !valid[parent]) {
            AxisCopy(animAxis[i], axis);
        }

        out->boneIndex = body->bone[i];
        CG_RagdollBoneModel(model, pos[i], axis, out->matrix);
        if (!CG_LiveRagdollFinite(out->matrix[0], 12)) {
            if (cg_ragdoll_debug->integer) {
                cgi.Printf("live ragdoll: entity %d dropped: bone %s not finite\n", body->entityNum, lr_bones[i].name);
            }
            CG_LiveRagdollFree(body, qtrue);
            return;
        }
        n++;
    }

    if (!n) {
        return;
    }

    model->bone_override      = body->overrides;
    model->num_bone_overrides = n;
    // As the corpses do: what queries a bone or a tag from here on this frame
    // (the weapon in his hands, the shadow) sees this pose.
    cgi.ForceUpdatePose(model);
}
