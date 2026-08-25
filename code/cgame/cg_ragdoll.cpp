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
// cg_ragdoll.cpp: Client-side ragdoll simulation for dead characters.
//
// The corpse is simulated as a set of particles at the skeleton's joints,
// integrated with Verlet and relaxed against distance constraints, the way
// cg_tempmodels.cpp already simulates debris. The resulting joint positions
// are turned back into model-space bone matrices and handed to the renderer
// through refEntity_t::bone_override.
//
// Nothing here reaches the server. The server keeps its own bounding-box
// corpse, so hit detection, corpse sinking and corpse removal are unchanged,
// and no additional data travels over the network.

#include "cg_local.h"
#include "cg_ragdoll.h"

cvar_t *cg_ragdoll;
cvar_t *cg_ragdoll_maxcount;
cvar_t *cg_ragdoll_blendtime;
cvar_t *cg_ragdoll_impulse;
cvar_t *cg_ragdoll_duration;
cvar_t *cg_ragdoll_physicsrate;
cvar_t *cg_ragdoll_iterations;
cvar_t *cg_ragdoll_damping;
cvar_t *cg_ragdoll_friction;
cvar_t *cg_ragdoll_bounce;
cvar_t *cg_ragdoll_sleepvel;
cvar_t *cg_ragdoll_sleeptime;
cvar_t *cg_ragdoll_debug;
cvar_t *cg_ragdoll_dump;
cvar_t *cg_ragdoll_limptime;
cvar_t *cg_ragdoll_solvegain;
cvar_t *cg_ragdoll_blastimpulse;
cvar_t *cg_ragdoll_stiffness;

//=============================================================
// The rig
//=============================================================

enum {
    RD_PELVIS,
    RD_SPINE,
    RD_SPINE1,
    RD_SPINE2,
    RD_NECK,
    RD_HEAD,
    RD_HEADTIP,
    RD_LUARM,
    RD_LFARM,
    RD_LHAND,
    RD_LHANDTIP,
    RD_RUARM,
    RD_RFARM,
    RD_RHAND,
    RD_RHANDTIP,
    RD_LTHIGH,
    RD_LCALF,
    RD_LFOOT,
    RD_LTOE,
    RD_RTHIGH,
    RD_RCALF,
    RD_RFOOT,
    RD_RTOE,

    RD_NUM_JOINTS
};

typedef struct {
    const char *boneName; // primary bone this joint sits on
    const char *tipName;  // alternative name tried for tip joints, may be NULL
    short       parent;   // parent joint, -1 for the root
    qboolean    required; // abort the ragdoll when the bone is missing
    float       invMass;
    float       radius; // body thickness here, for keeping limbs out of each other

    // Only used when the model has no bone of its own at this tip. How far past
    // the parent joint to put it, as a fraction of the parent bone's length.
    // A blanket fraction cannot serve: the real player skeleton has 6.2 units
    // from neck to head but 26.0 from forearm to hand, so the same fraction
    // puts the skull tip inside the head and the finger tip half a forearm
    // past the wrist. These are per joint for that reason.
    float tipRatio;
} rdJointDef_t;

// Standard 3ds Max Biped naming, which every MOHAA character model uses. Tip
// joints exist only to give the leaf bones (head, hands, feet) something to
// aim at; when the model has no such bone they are extrapolated at seed time.
// Radii are given for a roughly 72 unit tall character and are scaled to the
// actual model at seed time. They only need to be good enough to stop a limb
// visibly passing through the body.
//
// The trunk values are deliberately smaller than half a torso's width. A torso
// is much deeper than it is thick, so one sphere per spine joint sized to the
// wide axis would demand clearances a curled up body cannot give, and the
// solver would be left fighting itself. Several modest spheres along the spine
// approximate the real shape far better.
//
// The trunk is also given the greater mass. When a limb is driven against the
// body, it is then the limb that gives way rather than the torso being shoved
// aside, which is both what really happens and what reads correctly.
static const rdJointDef_t rd_joints[RD_NUM_JOINTS] = {
    {"Bip01 Pelvis",     NULL,              -1,        qtrue,  0.65f, 5.5f, 0.0f},
    {"Bip01 Spine",      NULL,              RD_PELVIS, qtrue,  0.65f, 5.0f, 0.0f},
    {"Bip01 Spine1",     NULL,              RD_SPINE,  qtrue,  0.65f, 5.0f, 0.0f},
    {"Bip01 Spine2",     NULL,              RD_SPINE1, qtrue,  0.70f, 4.8f, 0.0f},
    {"Bip01 Neck",       NULL,              RD_SPINE2, qtrue,  0.90f, 3.0f, 0.0f},
    {"Bip01 Head",       NULL,              RD_NECK,   qtrue,  0.85f, 4.5f, 0.0f},
    {NULL,               "Bip01 HeadNub",   RD_HEAD,   qfalse, 0.8f,  4.0f, 1.45f},
    {"Bip01 L UpperArm", NULL,              RD_SPINE2, qtrue,  1.0f,  4.0f, 0.0f},
    {"Bip01 L Forearm",  NULL,              RD_LUARM,  qtrue,  1.2f,  3.2f, 0.0f},
    {"Bip01 L Hand",     NULL,              RD_LFARM,  qtrue,  1.4f,  2.8f, 0.0f},
    {NULL,               "Bip01 L Finger1", RD_LHAND,  qfalse, 1.4f,  2.2f, 0.29f},
    {"Bip01 R UpperArm", NULL,              RD_SPINE2, qtrue,  1.0f,  4.0f, 0.0f},
    {"Bip01 R Forearm",  NULL,              RD_RUARM,  qtrue,  1.2f,  3.2f, 0.0f},
    {"Bip01 R Hand",     NULL,              RD_RFARM,  qtrue,  1.4f,  2.8f, 0.0f},
    {NULL,               "Bip01 R Finger1", RD_RHAND,  qfalse, 1.4f,  2.2f, 0.29f},
    {"Bip01 L Thigh",    NULL,              RD_PELVIS, qtrue,  1.0f,  5.5f, 0.0f},
    {"Bip01 L Calf",     NULL,              RD_LTHIGH, qtrue,  1.0f,  4.0f, 0.0f},
    {"Bip01 L Foot",     NULL,              RD_LCALF,  qtrue,  1.1f,  3.5f, 0.0f},
    {NULL,               "Bip01 L Toe0",    RD_LFOOT,  qfalse, 1.1f,  3.0f, 0.36f},
    {"Bip01 R Thigh",    NULL,              RD_PELVIS, qtrue,  1.0f,  5.5f, 0.0f},
    {"Bip01 R Calf",     NULL,              RD_RTHIGH, qtrue,  1.0f,  4.0f, 0.0f},
    {"Bip01 R Foot",     NULL,              RD_RCALF,  qtrue,  1.1f,  3.5f, 0.0f},
    {NULL,               "Bip01 R Toe0",    RD_RFOOT,  qfalse, 1.1f,  3.0f, 0.36f}
};

// Bones that receive a model-space override. Each one takes its origin from
// "joint" and aims its X axis at "aim". When helperA/helperB are set the frame
// is completed from that reference direction, otherwise the previous frame's Y
// axis is transported forward, which keeps the bone from spinning about its
// own axis.
// helperA may be a joint index, or one of these.
#define RD_TWIST_FREE   -1 // carry the previous frame's up vector forward
#define RD_TWIST_PARENT -2 // take it from the parent bone, so twist is inherited

// How much of a bone's spurious twist is taken out each frame.
#define RD_UNTWIST_RATE 1.0f


// How closely a roll reference may run to the bone it is orienting before it is
// given up on. At one it is parallel and there is nothing left of it across the
// bone to build a frame from.
//
// Given up on over a band rather than at a single value. A thigh takes its roll
// from the shin below it, and a corpse's legs straighten, so the two run into
// line constantly; switching outright the moment they do steps the drawn bone
// by however far the two references happen to disagree, which at the hip and
// the knee is where the mesh tears. Between the two the references are mixed,
// so the handover is continuous however close to the boundary a leg sits.
#define RD_TWIST_PARALLEL_LO 0.88f
#define RD_TWIST_PARALLEL_HI 0.97f

typedef struct {
    const char *boneName;
    short       joint;
    short       aim;
    short       helperA, helperB;
    short       parent; // parent within this table, -1 for the root
} rdBoneDef_t;

// Ordered parent before child, which is what lets the pose be rebuilt in a
// single forward pass.
//
// The spine takes its roll from the pelvis and passes it up, one bone to the
// next. Anchoring the lower spine to the hips and the upper spine to the
// shoulders instead, as would seem natural, puts the whole difference between
// the two into whichever single joint sits between them: the torso then appears
// to twist sharply at one point rather than gradually along its length, and a
// mesh skinned across that joint pinches into an hourglass.
//
// The upper arms and thighs take their reference direction from the limb
// segment below them rather than from the torso. That rolls each of those bones
// to line up with the plane its own joint is bending in, so an elbow or knee
// always reads as bending the way it should, whichever way the limb as a whole
// has been thrown.
static const rdBoneDef_t rd_bones[] = {
    {"Bip01",            RD_PELVIS, RD_SPINE,    RD_LTHIGH,       RD_RTHIGH, -1},
    {"Bip01 Pelvis",     RD_PELVIS, RD_SPINE,    RD_LTHIGH,       RD_RTHIGH, 0 },
    {"Bip01 Spine",      RD_SPINE,  RD_SPINE1,   RD_TWIST_PARENT, -1,        1 },
    {"Bip01 Spine1",     RD_SPINE1, RD_SPINE2,   RD_TWIST_PARENT, -1,        2 },
    // Spine2 inherits its roll from the back below it rather than squaring
    // against the line of the shoulders. Squaring against the shoulders is what
    // the original design called for, and it does reduce arm-into-chest
    // clipping, but measured against the real death animations it more than
    // doubles the twist in the drawn torso (15 deg to 35 deg): the shoulders
    // are themselves being moved by the solver, so referencing them feeds the
    // chest's own error back into its roll. Twist reads worse than the
    // clipping does, so the roll is inherited.
    {"Bip01 Spine2",     RD_SPINE2, RD_NECK,     RD_TWIST_PARENT, -1,        3 },
    {"Bip01 Neck",       RD_NECK,   RD_HEAD,     RD_TWIST_PARENT, -1,        4 },
    {"Bip01 Head",       RD_HEAD,   RD_HEADTIP,  RD_TWIST_PARENT, -1,        5 },
    {"Bip01 L UpperArm", RD_LUARM,  RD_LFARM,    RD_LFARM,        RD_LHAND,  4 },
    {"Bip01 L Forearm",  RD_LFARM,  RD_LHAND,    RD_TWIST_PARENT, -1,        7 },
    {"Bip01 L Hand",     RD_LHAND,  RD_LHANDTIP, RD_TWIST_PARENT, -1,        8 },
    {"Bip01 R UpperArm", RD_RUARM,  RD_RFARM,    RD_RFARM,        RD_RHAND,  4 },
    {"Bip01 R Forearm",  RD_RFARM,  RD_RHAND,    RD_TWIST_PARENT, -1,        10},
    {"Bip01 R Hand",     RD_RHAND,  RD_RHANDTIP, RD_TWIST_PARENT, -1,        11},
    {"Bip01 L Thigh",    RD_LTHIGH, RD_LCALF,    RD_LCALF,        RD_LFOOT,  1 },
    {"Bip01 L Calf",     RD_LCALF,  RD_LFOOT,    RD_TWIST_PARENT, -1,        13},
    {"Bip01 L Foot",     RD_LFOOT,  RD_LTOE,     RD_TWIST_PARENT, -1,        14},
    {"Bip01 R Thigh",    RD_RTHIGH, RD_RCALF,    RD_RCALF,        RD_RFOOT,  1 },
    {"Bip01 R Calf",     RD_RCALF,  RD_RFOOT,    RD_TWIST_PARENT, -1,        16},
    {"Bip01 R Foot",     RD_RFOOT,  RD_RTOE,     RD_TWIST_PARENT, -1,        17}
};

#define RD_NUM_BONES ((int)(sizeof(rd_bones) / sizeof(rd_bones[0])))

// "Bip01" and "Bip01 Pelvis" are two drawn bones sharing one physics particle,
// so a loop that walks the bone table and writes through rd_bones[i].joint
// would touch the pelvis twice while every other joint is touched once. The
// particle is seeded from "Bip01 Pelvis" (see rd_joints), which is the later of
// the two entries, so the last bone mapping to a joint is the one that owns it.
static qboolean CG_RagdollBoneOwnsJoint(int bone)
{
    int i;

    for (i = bone + 1; i < RD_NUM_BONES; i++) {
        if (rd_bones[i].joint == rd_bones[bone].joint) {
            return qfalse;
        }
    }

    return qtrue;
}

// A constraint keeps |a - b| within [minScale, maxScale] of a reference
// length. The reference is the seed distance between a and b, unless "via" is
// set, in which case it is the length of the two-segment chain a-via-b. The
// chain form is what lets an elbow or knee straighten out even when the corpse
// was seeded with the limb already bent.
//
// Inside those bounds a constraint may also pull weakly back toward the seed
// distance. That soft term is what gives the corpse some memory of its own
// shape: without it gravity folds every limb flat against its minimum, and the
// result reads as a heap rather than a body.
typedef struct {
    short a, b, via;
    float minScale, maxScale;
    float stiffness;
    // When set, the minimum length is not a fraction of anything but is worked
    // out at seed time from the two segment lengths and this angle, so that the
    // joint at "via" can bend no further than this many degrees away from
    // straight.
    //
    // Guessing a fraction instead does not work: the distance across a joint
    // barely changes for the first several degrees of bend, so a brace that
    // looks tight at 0.92 in fact permits about 46 degrees, and even 0.97
    // permits 28. Since the mesh nips in by an amount that follows the bend
    // angle, the angle is the thing that has to be specified.
    float maxBendDeg;
} rdConstraintDef_t;

#define RD_STICK(a, b) {a, b, -1, 1.0f, 1.0f, 0.0f, 0.0f}

static const rdConstraintDef_t rd_constraints[] = {
    // Bone sticks
    RD_STICK(RD_PELVIS, RD_SPINE),
    RD_STICK(RD_SPINE, RD_SPINE1),
    RD_STICK(RD_SPINE1, RD_SPINE2),
    RD_STICK(RD_SPINE2, RD_NECK),
    RD_STICK(RD_NECK, RD_HEAD),
    RD_STICK(RD_HEAD, RD_HEADTIP),
    RD_STICK(RD_SPINE2, RD_LUARM),
    RD_STICK(RD_LUARM, RD_LFARM),
    RD_STICK(RD_LFARM, RD_LHAND),
    RD_STICK(RD_LHAND, RD_LHANDTIP),
    RD_STICK(RD_SPINE2, RD_RUARM),
    RD_STICK(RD_RUARM, RD_RFARM),
    RD_STICK(RD_RFARM, RD_RHAND),
    RD_STICK(RD_RHAND, RD_RHANDTIP),
    RD_STICK(RD_PELVIS, RD_LTHIGH),
    RD_STICK(RD_LTHIGH, RD_LCALF),
    RD_STICK(RD_LCALF, RD_LFOOT),
    RD_STICK(RD_LFOOT, RD_LTOE),
    RD_STICK(RD_PELVIS, RD_RTHIGH),
    RD_STICK(RD_RTHIGH, RD_RCALF),
    RD_STICK(RD_RCALF, RD_RFOOT),
    RD_STICK(RD_RFOOT, RD_RTOE),

    // Rigidity cross-braces. A plain chain of sticks has no torsional
    // stiffness at all and collapses into a rope, so these are not optional.
    RD_STICK(RD_LUARM, RD_RUARM),
    RD_STICK(RD_LTHIGH, RD_RTHIGH),
    {RD_PELVIS, RD_SPINE1,   RD_SPINE,  0.0f,  1.08f,  0.10f, 20.0f},
    {RD_SPINE,  RD_SPINE2,   RD_SPINE1, 0.0f,  1.08f,  0.10f, 20.0f},
    {RD_SPINE1, RD_NECK,     RD_SPINE2, 0.0f,  1.08f,  0.10f, 22.0f},

    {RD_PELVIS, RD_LUARM,    -1,        0.88f, 1.12f,  0.0f,  0.0f },
    {RD_PELVIS, RD_RUARM,    -1,        0.88f, 1.12f,  0.0f,  0.0f },
    {RD_SPINE2, RD_HEAD,     RD_NECK,   0.0f,  1.0f,   0.20f, 30.0f},

    // Joint limits: elbows and knees may neither hyperextend nor fold
    // completely through themselves. The soft term resists the fold so the
    // limb keeps some of the shape it died in.
    {RD_LUARM,  RD_LHAND,    RD_LFARM,  0.259f, 0.985f, 0.12f, 0.0f },
    {RD_RUARM,  RD_RHAND,    RD_RFARM,  0.259f, 0.985f, 0.12f, 0.0f },
    // The knees and elbows, as a minimum distance across the joint, which is
    // how far each may fold. The geometry is unforgiving, so these are worked
    // back from the normative ranges rather than guessed: a chord of
    // cos(bend/2) of the limb's length, against the AAOS figures of 135 degrees
    // of knee flexion and 150 of elbow flexion. That gives 0.383 and 0.259.
    //
    // 0.72 was here for the knee, which is 88 degrees, and it was far too tight
    // rather than too loose: a real knee folds half as far again, and holding
    // one short of where it wants to go leaves the solver pushing against its
    // own limit for the whole life of the corpse.
    {RD_LTHIGH, RD_LFOOT,    RD_LCALF,  0.383f, 0.985f, 0.12f, 0.0f },
    {RD_RTHIGH, RD_RFOOT,    RD_RCALF,  0.383f, 0.985f, 0.12f, 0.0f },

    // The spine may curl, but not into a ball.
    {RD_PELVIS, RD_NECK,     RD_SPINE1, 0.86f, 0.99f,  0.20f, 0.0f },

    // Torso twist. Distances alone cannot see twist at all: rotating the
    // shoulders about the spine changes no length between them and the hips, so
    // without these the upper body is free to wind round like a rag. The two
    // diagonals across the torso do see it, because a twist shortens one and
    // lengthens the other, and holding both near their rest length bounds it.
    {RD_LUARM,  RD_RTHIGH,   -1,        0.93f, 1.07f,  0.25f, 0.0f },
    {RD_RUARM,  RD_LTHIGH,   -1,        0.93f, 1.07f,  0.25f, 0.0f },

    // Hip and waist extension. Nothing else stops the chest folding down onto
    // the thighs, so without these the body simply jackknifes: it settles bent
    // almost double, which looks nothing like a body and is also why parts as
    // far apart as a hand and a foot end up inside one another. A corpse's own
    // weight does not fold it up like this, so these are firm limits carrying
    // real stiffness rather than gentle suggestions.
    {RD_SPINE2, RD_LTHIGH,   -1,        0.90f, 9.0f,   0.25f, 0.0f },
    {RD_SPINE2, RD_RTHIGH,   -1,        0.90f, 9.0f,   0.25f, 0.0f },
    {RD_NECK,   RD_LTHIGH,   -1,        0.90f, 9.0f,   0.20f, 0.0f },
    {RD_NECK,   RD_RTHIGH,   -1,        0.90f, 9.0f,   0.20f, 0.0f },
    {RD_SPINE1, RD_LCALF,    -1,        0.80f, 9.0f,   0.15f, 0.0f },
    {RD_SPINE1, RD_RCALF,    -1,        0.80f, 9.0f,   0.15f, 0.0f },
    {RD_HEAD,   RD_LCALF,    -1,        0.80f, 9.0f,   0.15f, 0.0f },
    {RD_HEAD,   RD_RCALF,    -1,        0.80f, 9.0f,   0.15f, 0.0f },
    {RD_HEAD,   RD_LFOOT,    -1,        0.75f, 9.0f,   0.10f, 0.0f },
    {RD_HEAD,   RD_RFOOT,    -1,        0.75f, 9.0f,   0.10f, 0.0f },

    // Wrists, ankles and the neck. For a two segment chain the distance across
    // it is a direct measure of the angle at the middle joint, so these are
    // angle limits written as distances. Without them these joints are free to
    // fold right back on themselves: nothing else constrains them at all, and a
    // hand or a head aims at a tip joint that is only held by a single bone, so
    // it swings wherever the simulation throws it.
    //
    // The stiffness matters as much as the limit, but it has to stay gentle. A
    // limp hand hangs roughly in line with its forearm rather than sitting at
    // whatever angle it was last pushed to, yet it does still hang: hold these
    // joints firmly and the body stops reading as a body and starts reading as
    // a shop mannequin.
    {RD_LFARM,  RD_LHANDTIP, RD_LHAND,  0.93f, 1.0f,   0.10f, 0.0f },
    {RD_RFARM,  RD_RHANDTIP, RD_RHAND,  0.93f, 1.0f,   0.10f, 0.0f },
    {RD_LUARM,  RD_LHANDTIP, RD_LHAND,  0.35f, 9.0f,   0.0f,  0.0f },
    {RD_RUARM,  RD_RHANDTIP, RD_RHAND,  0.35f, 9.0f,   0.0f,  0.0f },

    {RD_LCALF,  RD_LTOE,     RD_LFOOT,  0.86f, 1.02f,  0.10f, 0.0f },
    {RD_RCALF,  RD_RTOE,     RD_RFOOT,  0.86f, 1.02f,  0.10f, 0.0f },

    {RD_SPINE2, RD_HEADTIP,  RD_HEAD,   0.88f, 1.0f,   0.12f, 0.0f },
    {RD_NECK,   RD_HEADTIP,  RD_HEAD,   0.90f, 1.0f,   0.12f, 0.0f },
    {RD_SPINE1, RD_HEAD,     RD_NECK,   0.86f, 1.0f,   0.10f, 0.0f },

    // The same for the arms: a limp arm does not fold up against its own
    // shoulder, and a hand should not come to rest down beside a foot.
    {RD_LHAND,  RD_LFOOT,    -1,        0.55f, 9.0f,   0.08f, 0.0f },
    {RD_RHAND,  RD_RFOOT,    -1,        0.55f, 9.0f,   0.08f, 0.0f },

    // Keep the limbs out of the torso and out of each other.
    {RD_SPINE1, RD_LHAND,    RD_LFARM,  0.45f, 9.0f,   0.0f,  0.0f },
    {RD_SPINE1, RD_RHAND,    RD_RFARM,  0.45f, 9.0f,   0.0f,  0.0f },
    {RD_LFOOT,  RD_RFOOT,    -1,        0.60f, 9.0f,   0.0f,  0.0f },
    {RD_LTHIGH, RD_RCALF,    -1,        0.70f, 9.0f,   0.0f,  0.0f },
    {RD_RTHIGH, RD_LCALF,    -1,        0.70f, 9.0f,   0.0f,  0.0f },

    // Shape memory. Nothing above resists a limb swinging sideways about its
    // hip or shoulder, so without these the corpse sprawls flat into a
    // starfish the moment it lands. They are deliberately weak: they bias the
    // body back toward the shape it died in without making it rigid.
    {RD_PELVIS, RD_LFOOT,    RD_LCALF,  0.30f, 1.05f,  0.07f, 0.0f },
    {RD_PELVIS, RD_RFOOT,    RD_RCALF,  0.30f, 1.05f,  0.07f, 0.0f },
    {RD_SPINE2, RD_LHAND,    RD_LFARM,  0.25f, 1.05f,  0.07f, 0.0f },
    {RD_SPINE2, RD_RHAND,    RD_RFARM,  0.25f, 1.05f,  0.07f, 0.0f },
    {RD_LFOOT,  RD_RFOOT,    -1,        0.60f, 6.0f,   0.08f, 0.0f },

    // Hip abduction. Measured through the pelvis, the distance between the two
    // knees over the length of the two thighs is the sine of half the angle the
    // legs make with one another, so a ceiling on it is a ceiling on that
    // angle. 0.42 is fifty degrees, a little past what a hip really gives.
    //
    // Nothing else limited this. The cone at each hip is measured from the
    // direction that leg happened to be pointing at the moment of death, so it
    // constrains how far a leg may move from where it died rather than how far
    // it may open, and a man shot in mid stride keeps his stride: the running
    // deaths settle with their legs sixty degrees apart, death_prone1 at
    // seventy, and neither ever closes. Capped, those come back to forty and
    // fifty, and the deaths that were never splayed are left alone.
    //
    // It also settles them. Legs held open that far go on working against the
    // constraints that want them shut, and residual movement across the real
    // animations falls to a third of what it was.
    {RD_LCALF,  RD_RCALF,    RD_PELVIS, 0.0f,  0.42f,  0.12f, 0.0f },
    {RD_LHAND,  RD_RHAND,    -1,        0.30f, 6.0f,   0.06f, 0.0f },
    {RD_LCALF,  RD_RCALF,    -1,        0.50f, 6.0f,   0.08f, 0.0f }
};

#define RD_NUM_CONSTRAINTS ((int)(sizeof(rd_constraints) / sizeof(rd_constraints[0])))

// Hinges whose bend direction must not invert. Expressed relative to the torso
// frame so the reference rotates with the body.
//
// A limb that is straight at the moment of death has no measurable bend to
// record, and that is the common case, so each hinge also carries the
// anatomical direction its middle joint bulges toward. In torso coordinates
// axis 0 runs up the spine, axis 1 runs across the shoulders and axis 2 runs
// forward: an elbow falls behind the shoulder-to-hand line, a knee sits in
// front of the hip-to-foot line.
typedef struct {
    short  a, mid, b;
    vec3_t defaultBend;
} rdHingeDef_t;

// Only the knees. An elbow cannot be constrained this way: the shoulder has
// three rotational degrees of freedom, so the plane an elbow bends in turns
// with the upper arm, and in a particle model "the elbow is bent backwards" and
// "the whole arm is rotated at the shoulder" are the same set of positions.
// Trying to pin the elbow to a torso relative plane simply fights the solver.
// The hip is far more restricted, so for a knee the plane really is meaningful.
// Elbows are dealt with in the reconstruction instead, by choosing the roll of
// the upper arm so that the bend reads correctly.
static const rdHingeDef_t rd_hinges[] = {
    {RD_LTHIGH, RD_LCALF, RD_LFOOT, {0.0f, 0.0f, 1.0f}},
    {RD_RTHIGH, RD_RCALF, RD_RFOOT, {0.0f, 0.0f, 1.0f}}
};

#define RD_NUM_HINGES ((int)(sizeof(rd_hinges) / sizeof(rd_hinges[0])))

// How far a limb may swing from the direction it died in, at the hip and at
// the shoulder. A shoulder is much the freer of the two.
#define RD_CONE_HIP      30.0f
#define RD_CONE_SHOULDER 45.0f

// How much of a cone violation is taken out per iteration.
#define RD_CONE_RATE 0.5f

// Cone limits at the four joints where a limb meets the body.
//
// These do the job the long reach springs were doing badly. What actually
// stops a corpse spreading itself out flat is the hips and shoulders refusing
// to let the limbs swing right away from the trunk, and that is an angle at one
// joint. Expressed instead as a distance between two joints half a body apart,
// as a head to foot spring is, it cannot tell "this leg is splayed out" from
// "this leg is straight", because both read as head and foot being far apart.
// That is why every attempt to stop the sprawl also stopped a drawn up knee
// ever straightening, and why the foot stayed in the air.
//
// An angle at the hip says nothing whatever about the knee, so the two come
// apart and can be set independently.
typedef struct {
    short root;   // the joint the limb hangs from
    short tip;    // the next joint down the limb, giving its direction
    float maxDeg; // how far from the direction it died in the limb may swing
} rdConeDef_t;

static const rdConeDef_t rd_cones[] = {
    {RD_LTHIGH, RD_LCALF, RD_CONE_HIP     },
    {RD_RTHIGH, RD_RCALF, RD_CONE_HIP     },
    {RD_LUARM,  RD_LFARM, RD_CONE_SHOULDER},
    {RD_RUARM,  RD_RFARM, RD_CONE_SHOULDER}
};

#define RD_NUM_CONES ((int)(sizeof(rd_cones) / sizeof(rd_cones[0])))

// The limb bones, treated as segments rather than as their two end points, and
// the thick parts of the body they must not pass through.
typedef struct {
    short a, b;
} rdLimbSegment_t;

static const rdLimbSegment_t rd_limbSegments[] = {
    {RD_LUARM,  RD_LFARM},
    {RD_LFARM,  RD_LHAND},
    {RD_RUARM,  RD_RFARM},
    {RD_RFARM,  RD_RHAND},
    {RD_LTHIGH, RD_LCALF},
    {RD_LCALF,  RD_LFOOT},
    {RD_RTHIGH, RD_RCALF},
    {RD_RCALF,  RD_RFOOT}
};

#define RD_NUM_LIMB_SEGMENTS ((int)(sizeof(rd_limbSegments) / sizeof(rd_limbSegments[0])))

// The trunk, as the segments between consecutive spine joints rather than as a
// sphere at each one. The real player skeleton is very unevenly divided: of the
// 60.5 model units from pelvis to head, a single span from "Bip01 Spine2" to
// "Bip01 Neck" accounts for 21.7 of them, better than a third of the whole
// torso with no joint anywhere inside it. Spheres at the two ends of that span
// cannot cover it, and the arms fold through the gap in the middle.
static const rdLimbSegment_t rd_trunkSegments[] = {
    {RD_PELVIS, RD_SPINE },
    {RD_SPINE,  RD_SPINE1},
    {RD_SPINE1, RD_SPINE2},
    {RD_SPINE2, RD_NECK  },
    {RD_NECK,   RD_HEAD  }
};

// Indices into rd_bones of the bones running up the back, in order. Only the
// ones with a bone above and below take part in the smoothing.
static const short rd_spineChain[] = {0, 1, 2, 3, 4, 5, 6};

#define RD_NUM_SPINE_CHAIN ((int)(sizeof(rd_spineChain) / sizeof(rd_spineChain[0])))

#define RD_NUM_TRUNK_SEGMENTS ((int)(sizeof(rd_trunkSegments) / sizeof(rd_trunkSegments[0])))

//=============================================================
// State
//=============================================================

#define MAX_RAGDOLLS 16

// How close a fresh corpse entity must be to a just-orphaned ragdoll for it to
// adopt it. Body::Body copies the player's origin verbatim, so the match is
// effectively exact and this only guards against float noise.
#define RD_ADOPT_DIST 4.0f

// A ragdoll whose entity has been gone this long is recycled outright. It is
// only a backstop against stale state piling up over a long session: the usual
// way a slot comes back is the allocator evicting a settled corpse.
#define RD_ORPHAN_MAX 30000

// MASK_DEADSOLID without CONTENTS_CORPSE: corpses must not shove each other
// around, which is both expensive and visibly jittery.
#define RD_CLIPMASK (CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_FENCE)

// How far a particle has to drift off a remembered contact plane before that
// contact is forgotten.
#define RD_CONTACT_FORGET 16.0f

// How long a remembered contact plane may go without a trace confirming it.
//
// Drifting off the plane is not enough on its own to notice that a surface has
// gone. A particle hanging underneath a remembered plane never rises above it,
// so the distance test never fires, and the plane goes on holding the particle
// up for the life of the corpse: a body was found dangling in open air from two
// planes remembered at its toes, its own entity long since on the floor below.
//
// A settled particle is not moving, so the sweep it makes each step is a point
// and strikes nothing: going without confirmation is normal and is not on its
// own evidence of anything. So the plane is not dropped on age. Age only says
// when to go and ask the world whether the surface is still there.
#define RD_CONTACT_STALE 250

// How far past its own radius that question is asked.
#define RD_CONTACT_PROBE 2.0f

// How far, in particle radii, a buried particle may be moved to get it out.
// Beyond this the exit found is not the surface it is behind.
#define RD_UNBURY_REACH 4.0f

// Collision displacement below this is treated as a resting contact rather
// than an impact, and does not trigger the reconciling solve.
#define RD_IMPACT_EPSILON 0.5f

// Small gap kept between a resting particle and the surface it landed on, so
// that floating point noise cannot push it back inside.
#define RD_SURFACE_GAP 0.25f

// A hinge this close to its boundary is left alone, so that a joint resting
// right on the limit is not nudged back and forth forever.
#define RD_HINGE_SLOP 0.08f

// How far a hinge joint may stray out of its bend plane before being pulled
// back, in units. A little play keeps it from being pushed about while at rest.
#define RD_HINGE_LATERAL_SLOP 0.04f

// Fraction of the way a wrongly-bent hinge is pushed back per iteration.
// Sideways travel is corrected far more gently than bend direction is. The
// correction moves the middle joint alone while the bone lengths either side
// hold it, so at anything like the rate below it and they fight each other
// every iteration and the argument feeds the limb energy: a straight leg gets
// levered up out of its own fall and lands propped on the knee, with the foot
// as much as eleven units off the ground. Applied gently it still holds a knee
// in its plane, and rather better, since the worst out of plane case improves
// as well. Sharing the correction with the two ends instead, so the group's
// centre is preserved, is worse than either: it drags the hip and the foot
// around and the whole body spreads out.

// How far the plane a knee swings in may follow the leg round, which is the
// rotation the hip itself has: a hip turns about the length of the thigh, and
// that is what lets a bent leg roll flat instead of standing its shin upright.
#define RD_HINGE_ROLL_LIMIT 20.0f

// Slowly, so the plane still holds the joint steady from frame to frame and
// only gives way to a leg that stays turned.
#define RD_HINGE_ROLL_RATE 0.01f
#define RD_HINGE_LATERAL_RATE 0.06f

#define RD_HINGE_RATE 0.85f

// How much of its velocity a grounded particle keeps each step, and the speed
// below which it is simply pinned in place.
#define RD_GROUND_DAMPING 0.35f
#define RD_REST_SPEED     0.40f

// Below this, in units per second along the surface, a resting particle is
// held rather than allowed to creep. Was written as half a unit of Verlet
// displacement per step, which is the same thing at sixty steps a second.
#define RD_STATIC_FRICTION 30.0f

// How many joints must be resting on a surface before the body counts as
// supported, and so before it is allowed to damp down and fall asleep.
#define RD_MIN_SUPPORT 3

// How far past the lifetime cap a corpse with nothing under it is allowed to
// keep falling before it is frozen anyway. Only reached by a body that has left
// the map or wedged somewhere it can never rest.
#define RD_SLEEP_BACKSTOP 4

// Fraction of a limb-versus-body overlap resolved per iteration.
#define RD_SEGMENT_RATE 0.35f

// The least clearance a limb bone must keep from the trunk, as a fraction of
// the full body thickness, however close the two were when the body fell.
#define RD_SEGMENT_MIN_FRACTION 0.70f

// Overlap that body parts are simply allowed to keep. Chasing the last fraction
// of a unit is what makes a settled corpse fidget: the correction is never
// quite finished, so every frame it nudges again, and with sleeping turned off
// there is nothing to hide it. A band this small cannot be seen, and inside it
// nothing pushes at all.
#define RD_COLLISION_SLOP 0.30f

// How deep the trunk is compared with how wide it is. A torso is far broader
// than it is thick, so treating it as a run of spheres demands the same
// clearance in front as at the sides, and an arm coming to rest across the
// chest ends up hovering above it with the elbow cocked in the air. Front and
// back are scaled by this so the arm can settle onto the chest while the sides
// keep their full width.
// A torso is a flattened thing: roughly as wide as the shoulders and about
// two thirds of that front to back. Held as one radius the two demands
// conflict, and the conflict is not a small one. Sized to the width, an arm
// can never come to rest on the chest and the solver pushes at it forever;
// sized to the depth, an arm lying beside a body on the ground sinks into the
// ribs. Every earlier attempt here used a single radius and had to pick which
// of the two to get wrong. These are the two axes of an elliptical cross
// section instead, measured in the torso's own frame, so each direction is
// asked for what it actually needs.
//
// The ratios are close to one because the joint radii they scale are already
// the body's, not an overestimate of it. Set well below one, as they were, the
// volume the solver defends ends up markedly thinner than the torso that is
// drawn, and an arm can satisfy every constraint while visibly lying inside the
// chest. Raising them until the defended volume matches the drawn one takes the
// deepest limb from 97 per cent of its own thickness inside the trunk to 65,
// without spreading the body out any further: measured over the real death
// animations the sprawl does not move at all. Higher again is worse, not
// better, since a torso that asks for more room than it occupies pushes limbs
// away from a body they are resting against and the deepest case returns.
#define RD_TRUNK_WIDE_RATIO 1.00f
#define RD_TRUNK_DEEP_RATIO 0.90f

// How far each spine bone is turned towards the average of its neighbours, and
// how many times that is repeated. Enough to share a bend between joints, not
// so much that the back goes limp and loses its shape.
#define RD_SPINE_SMOOTH_RATE   0.35f
#define RD_SPINE_SMOOTH_PASSES 2

typedef enum {
    RD_FREE,
    RD_BLENDING,
    RD_ACTIVE,
    RD_SLEEPING
} rdState_t;

typedef struct {
    vec3_t p;

    // Where it was when this step began. Only ever that: it is what collision
    // sweeps from, and nothing writes to it to mean something else.
    vec3_t pPrev;

    // How fast it is going, in units per second, kept rather than inferred.
    //
    // Verlet reads velocity as the gap between the two positions above, which
    // makes every correction the constraint solver applies indistinguishable
    // from motion: satisfying a bone length hands the body speed it never had,
    // and the damping needed to take that back out is the same damping that
    // stops a corpse sliding. Held separately, a correction can be applied to
    // where the particle is without lying about how fast it is going.
    vec3_t v;

    // How far the constraint solve alone moved it this step, so that its
    // contribution to the velocity above can be chosen rather than assumed.
    vec3_t solved;
    float    invMass;
    qboolean onGround;

    // Contact plane found by the last trace, kept so the constraint solver can
    // re-project against it without tracing again on every iteration.
    qboolean hasContact;
    vec3_t   contactNormal;
    float    contactDist;

    // When that plane was last confirmed by a trace actually striking
    // something. A remembered plane is a one sided constraint the particle can
    // never fall through, so one that outlives the surface it came from is an
    // invisible shelf.
    int contactTime;
} rdParticle_t;

typedef struct {
    short a, b;
    float minLen, maxLen, restLen, stiffness;
    // Solved along with the bone sticks rather than with the soft shaping
    // constraints, so it is still being enforced on the closing iterations.
    // The joint angle limits need this: left in the soft group they are given
    // up just as the solver finishes, and end up several degrees over.
    qboolean isHard;
} rdConstraint_t;

typedef struct {
    rdState_t state;

    int      entityNum;  // ENTITYNUM_NONE while orphaned
    dtiki_t *tiki;       // the tiki the bone indices were resolved against
    int      modelIndex; // for corpse adoption
    int      orphanTime; // cg.time the owning entity went away

    int   startTime;
    int   lastTime;
    int   quietSince;
    float accum;

    vec3_t entOrigin; // last known entity origin, for corpse adoption
    vec3_t entAngles;
    float  radius; // particle collision radius, scaled with the model

    rdParticle_t   part[RD_NUM_JOINTS];
    rdConstraint_t constraint[RD_NUM_CONSTRAINTS];
    int            numConstraints;

    // Reconstruction state, one entry per overridden bone.
    int    boneIndex[RD_NUM_BONES];
    float  correction[RD_NUM_BONES][3][3]; // seed frame -> seed bone basis
    vec3_t transportY[RD_NUM_BONES];

    // The direction each bone pointed when it was last built, so a child can
    // carry its parent's roll across to its own direction.
    vec3_t transportX[RD_NUM_BONES];

    // How each bone of the back sat relative to the average of its neighbours
    // at the moment of death: the curve the spine had while the body was alive.
    float    spineRest[RD_NUM_SPINE_CHAIN][3][3];
    qboolean spineRestValid;

    // The twist each bone has about its own length relative to its parent, as
    // its anatomy had it at the moment of death. See CG_RagdollUntwist.
    float    twistRest[RD_NUM_BONES];
    qboolean twistRestValid;

    // Each bone's orientation before its spurious twist is taken out. The
    // skeleton is still hung together with these: see the second pass.
    vec3_t rolledAxis[RD_NUM_BONES][3];

    // Offset of each bone from its parent, expressed in the parent's own seed
    // frame. The pose is rebuilt from these rather than from the particle
    // positions directly, which is what makes the emitted bone lengths exact
    // no matter what the solver leaves behind.
    vec3_t localOffset[RD_NUM_BONES];

    // Per-frame scratch for the hierarchical rebuild.
    vec3_t boneAxis[RD_NUM_BONES][3];
    vec3_t bonePos[RD_NUM_BONES];

    // Joint thickness, and the pairs that are allowed to push each other apart.
    float jointRadius[RD_NUM_JOINTS];

    // Deliberately not scaled to the model, unlike the other distances here.
    // It reads as an oversight and scaling it was tried; it measures worse,
    // because the value was arrived at as an absolute and scaling it drives a
    // small model far too tight. Self intersection at the 90th percentile went
    // from 40 to 55.
    float hingeLateralSlop;
    float limbRadius[RD_NUM_LIMB_SEGMENTS];
    float collisionSlop;
    // How much of the ideal clearance each limb/trunk pair is actually asked
    // for: 1 when the two start clear of each other, less when they begin
    // touching, 0 when the pair is not checked at all.
    float segTrunkScale[RD_NUM_LIMB_SEGMENTS][RD_NUM_TRUNK_SEGMENTS];

    // Cone axes, held in the torso's own frame so they turn with the body
    // rather than staying pinned to world directions.
    vec3_t coneAxis[RD_NUM_CONES];
    float trunkWide[RD_NUM_TRUNK_SEGMENTS];
    float trunkDeep[RD_NUM_TRUNK_SEGMENTS];

    struct {
        short a, b;
        float minLen;
    } selfPair[RD_NUM_JOINTS * RD_NUM_JOINTS / 2];

    int numSelfPairs;

    // Hinge bend directions, in torso-frame coordinates.
    vec3_t hingeBend[RD_NUM_HINGES];

    // The plane each hinge was seeded with, so the roll the hip is allowed can
    // be measured against where the joint started rather than drifting freely.
    vec3_t hingeBendRest[RD_NUM_HINGES];

    boneOverride_t overrides[RD_NUM_BONES];
    int            numOverrides;

    // Velocity, in units per second, that the killing shot should hand to the
    // body. Held until the first integration step rather than written into the
    // particles at seed time, because Verlet carries velocity as the gap
    // between p and pPrev and that gap is only meaningful once dt is known.
    qboolean hasImpulse;
    vec3_t   impulseVel;
    int      impulseJoint;

    // An explosion near enough to have thrown this body, kept as the place it
    // went off rather than as a direction, so each particle can be pushed away
    // from it by its own distance.
    qboolean hasBlast;
    vec3_t   blastPos;
    float    blastRadius;
    float    blastSpeed;

    // Which particles were buried in solid geometry on the last step. A trace
    // that starts inside a brush cannot say which way is out, so such a
    // particle is dropped back where it was, and if it stays buried it is
    // dropped back again every step. Only the trace can show that happening.
    int      buriedMask;

    // What the sleep test last saw, kept only so the trace can report it. A
    // corpse that will not sleep is holding one of these above its threshold,
    // and which one it is cannot be worked out from the pose.
    float    lastDisp;
    int      lastSteps;

    // Open while this corpse is being traced to a file. Zero is no file, which
    // is what the memset in CG_RagdollFree leaves behind.
    fileHandle_t dumpFile;
    char         dumpName[64];
} cg_ragdoll_t;

static cg_ragdoll_t cg_ragdolls[MAX_RAGDOLLS];

// The animation pose of the entity being processed, in world space. Reading a
// bone means going out to the renderer and back, so it is gathered once per
// entity per frame and shared by the blend and by the drive-toward-animation
// pass, which would otherwise each re-read every bone on every substep.
typedef struct {
    qboolean valid;
    vec3_t   pos;
    vec3_t   axis[3];
} rdAnimPose_t;

static rdAnimPose_t rd_animPose[RD_NUM_BONES];

//=============================================================
// Small math helpers
//=============================================================

// Closest points between two line segments, returning the distance and, in
// ta/tb, where along each segment that closest point falls. Needed because the
// trunk is treated as a chain of capsules rather than a string of spheres: the
// real player skeleton runs 21.7 model units from "Bip01 Spine2" to
// "Bip01 Neck" with no joint in between, so spheres at the two ends leave a
// hole through the middle of the ribcage that an arm passes straight through.
static float CG_RagdollSegmentToSegment(
    const vec3_t p1, const vec3_t q1, const vec3_t p2, const vec3_t q2, float *ta, float *tb, vec3_t dir
)
{
    vec3_t d1, d2, r, c1, c2;
    float  a, e, f, b, c, denom, s, t;

    VectorSubtract(q1, p1, d1);
    VectorSubtract(q2, p2, d2);
    VectorSubtract(p1, p2, r);

    a = DotProduct(d1, d1);
    e = DotProduct(d2, d2);
    f = DotProduct(d2, r);

    if (a < 0.0001f && e < 0.0001f) {
        s = t = 0.0f;
    } else if (a < 0.0001f) {
        s = 0.0f;
        t = Q_clamp_float(f / e, 0.0f, 1.0f);
    } else {
        c = DotProduct(d1, r);

        if (e < 0.0001f) {
            t = 0.0f;
            s = Q_clamp_float(-c / a, 0.0f, 1.0f);
        } else {
            b     = DotProduct(d1, d2);
            denom = a * e - b * b;

            // Parallel segments leave s free; anchoring it at zero picks one of
            // the equally close answers, which is all this needs.
            s = denom > 0.0001f ? Q_clamp_float((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;

            if (t < 0.0f) {
                t = 0.0f;
                s = Q_clamp_float(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = Q_clamp_float((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    VectorMA(p1, s, d1, c1);
    VectorMA(p2, t, d2, c2);

    *ta = s;
    *tb = t;

    VectorSubtract(c1, c2, dir);
    return VectorLength(dir);
}

// The trunk's radius in one particular direction. The cross section is an
// ellipse in the plane across the capsule, wide from side to side and shallow
// from front to back, so a limb approaching the ribs from the side is held off
// much further than one settling onto the chest. dir runs from the trunk toward
// the limb and need not be normalised; dist is its length.
static float
CG_RagdollTrunkClearance(const cg_ragdoll_t *rd, int seg, const vec3_t torso[3], const vec3_t dir, float dist)
{
    vec3_t axis, lateral, forward, u;
    float  along, side, front, wide, deep, denom;

    wide = rd->trunkWide[seg];
    deep = rd->trunkDeep[seg];

    if (wide < 0.0001f || deep < 0.0001f || dist < 0.0001f) {
        return wide;
    }

    VectorScale(dir, 1.0f / dist, u);

    // The capsule's own axis, with the torso's side to side direction squared
    // up against it, so each segment carries its own frame rather than the
    // whole trunk sharing one.
    VectorSubtract(rd->part[rd_trunkSegments[seg].b].p, rd->part[rd_trunkSegments[seg].a].p, axis);

    if (VectorNormalize(axis) < 0.001f) {
        VectorCopy(torso[0], axis);
    }

    VectorMA(torso[1], -DotProduct(torso[1], axis), axis, lateral);

    if (VectorNormalize(lateral) < 0.001f) {
        return wide;
    }

    CrossProduct(axis, lateral, forward);

    along = DotProduct(u, axis);
    side  = DotProduct(u, lateral);
    front = DotProduct(u, forward);

    // The radius of the ellipse along u. Along the capsule's own axis the ends
    // are capped at the wide radius, which keeps the caps from pinching in.
    denom = (side * side + along * along) / (wide * wide) + (front * front) / (deep * deep);

    if (denom < 0.000001f) {
        return wide;
    }

    return 1.0f / sqrtf(denom);
}


static void CG_RagdollOrthonormalize(const vec3_t x, const vec3_t helper, vec3_t out[3])
{
    vec3_t y, z;
    float  d;

    VectorCopy(x, out[0]);

    d = DotProduct(helper, out[0]);
    VectorMA(helper, -d, out[0], y);

    if (VectorNormalize(y) < 0.001f) {
        // The helper is parallel to the bone. Any perpendicular will do, and
        // because the seed correction is applied on top the choice only has to
        // be stable, not meaningful.
        VectorSet(y, 0, 0, 1);
        d = DotProduct(y, out[0]);
        VectorMA(y, -d, out[0], y);

        if (VectorNormalize(y) < 0.001f) {
            VectorSet(y, 0, 1, 0);
            d = DotProduct(y, out[0]);
            VectorMA(y, -d, out[0], y);
            VectorNormalize(y);
        }
    }

    VectorCopy(y, out[1]);
    CrossProduct(out[0], out[1], z);
    VectorCopy(z, out[2]);
}

// Builds the construction frame of one overridden bone from the current
// particle positions. When the bone has no helper reference the previous
// frame's Y axis is carried forward, which is a discrete parallel transport:
// it introduces no twist and has no singularity, unlike deriving the rotation
// from the seed direction directly.
static qboolean CG_RagdollBoneFrame(cg_ragdoll_t *rd, int boneNum, vec3_t out[3])
{
    const rdBoneDef_t *def = &rd_bones[boneNum];
    vec3_t             x, helper, carried;
    qboolean           haveCarried = qfalse;

    VectorSubtract(rd->part[def->aim].p, rd->part[def->joint].p, x);

    if (VectorNormalize(x) < 0.001f) {
        return qfalse;
    }

    // The parent's roll carried across to this bone's direction, by the
    // shortest rotation taking the parent's own direction onto this one. The
    // parent's Y is square to the parent's direction, so after that rotation it
    // is square to this one: this reference cannot run parallel to the bone it
    // is orienting, whatever the two happen to be doing.
    if (def->parent >= 0 && rd->boneIndex[def->parent] >= 0) {
        vec3_t axis;
        float  sinA, cosA;

        CrossProduct(rd->transportX[def->parent], x, axis);
        sinA = VectorNormalize(axis);
        cosA = DotProduct(rd->transportX[def->parent], x);

        if (sinA < 0.001f) {
            VectorCopy(rd->transportY[def->parent], carried);
        } else {
            RotatePointAroundVector(
                carried, axis, rd->transportY[def->parent], (float)(atan2(sinA, cosA) * 180.0 / M_PI)
            );
        }

        haveCarried = qtrue;
    }

    if (def->helperA >= 0) {
        float len, par;

        VectorSubtract(rd->part[def->helperB].p, rd->part[def->helperA].p, helper);

        // A limb bone takes its roll from the next bone down, so its Y follows
        // the plane the knee or elbow bends in. That reference lines up with
        // the bone itself as soon as the limb straightens, which for a corpse
        // is most of the time, and there is then almost nothing of it left
        // across the bone: what little remains is noise, and the frame it gives
        // turns with that noise. The bones below inherit it, so a whole leg
        // ends up twisted about itself.
        len = VectorNormalize(helper);
        par = len < 0.001f ? 1.0f : (float)fabs(DotProduct(helper, x));

        if (par > RD_TWIST_PARALLEL_LO) {
            vec3_t fallback, hy, fy, side;
            float  t;

            if (haveCarried) {
                VectorCopy(carried, fallback);
            } else {
                VectorCopy(rd->transportY[boneNum], fallback);
            }

            t = (par - RD_TWIST_PARALLEL_LO) / (RD_TWIST_PARALLEL_HI - RD_TWIST_PARALLEL_LO);
            if (t > 1.0f) {
                t = 1.0f;
            }

            // Mixed as a turn about the bone rather than as two directions
            // added together. The two references can point opposite ways, and
            // averaging those passes through nothing at all; turning one toward
            // the other the short way round is continuous whatever they do.
            VectorMA(helper, -DotProduct(helper, x), x, hy);
            VectorMA(fallback, -DotProduct(fallback, x), x, fy);

            if (VectorNormalize(hy) < 0.001f || VectorNormalize(fy) < 0.001f) {
                VectorCopy(fallback, helper);
            } else {
                CrossProduct(x, hy, side);
                RotatePointAroundVector(
                    helper,
                    x,
                    hy,
                    (float)(atan2(DotProduct(fy, side), DotProduct(fy, hy)) * 180.0 / M_PI) * t
                );
            }
        }
    } else if (def->helperA == RD_TWIST_PARENT && haveCarried) {
        // Was the parent's Y as it stood at the moment of death, which is a
        // fixed direction in the world: as the corpse turns over, the bone's
        // own axis swings into line with it and the same collapse follows.
        VectorCopy(carried, helper);
    } else {
        VectorCopy(rd->transportY[boneNum], helper);
    }

    CG_RagdollOrthonormalize(x, helper, out);
    VectorCopy(out[1], rd->transportY[boneNum]);
    VectorCopy(out[0], rd->transportX[boneNum]);

    return qtrue;
}

// Builds a stable frame for the torso from the current particle positions.
// Used as the reference for the hinge bend directions.
static qboolean CG_RagdollTorsoFrame(cg_ragdoll_t *rd, vec3_t out[3])
{
    vec3_t x, helper;

    VectorSubtract(rd->part[RD_SPINE1].p, rd->part[RD_PELVIS].p, x);

    if (VectorNormalize(x) < 0.001f) {
        return qfalse;
    }

    VectorSubtract(rd->part[RD_RUARM].p, rd->part[RD_LUARM].p, helper);

    if (VectorNormalize(helper) < 0.001f) {
        return qfalse;
    }

    CG_RagdollOrthonormalize(x, helper, out);
    return qtrue;
}

//=============================================================
// Reading and writing the skeleton
//=============================================================

// TIKI_OrientationInternal scales the bone origin by the entity and model
// scale and adds the model's load origin. The skinning path applies neither of
// those to the matrices it consumes, so both have to be undone on the way in
// and reapplied on the way out.
// Must match what TIKI_OrientationInternal applies on the way in, or the round
// trip through model space silently resizes the corpse. Callers guarantee the
// result is non-zero by refusing to start a ragdoll on a degenerately scaled
// entity, so this never divides by zero.
static float CG_RagdollModelScale(const refEntity_t *model)
{
    float scale = model->scale;

    if (model->tiki) {
        scale *= model->tiki->load_scale;
    }

    return scale;
}

// Reads one bone's current pose and converts it from model space to world
// space. Returns qfalse when the bone could not be evaluated: a bad tag makes
// TIKI_Orientation hand back a zeroed, and therefore singular, orientation.
static qboolean CG_RagdollReadBoneWorld(refEntity_t *model, int boneIndex, vec3_t worldPos, vec3_t worldAxis[3])
{
    orientation_t orient;
    int           r, k;

    if (boneIndex < 0) {
        return qfalse;
    }

    orient = cgi.TIKI_Orientation(model, boneIndex);

    for (r = 0; r < 3; r++) {
        if (VectorLengthSquared(orient.axis[r]) < 0.5f) {
            return qfalse;
        }
    }

    // orient.origin already carries the entity and model scale, which is
    // exactly the space the rendered geometry lives in, so it maps straight to
    // world space through the entity axis.
    VectorCopy(model->origin, worldPos);
    for (k = 0; k < 3; k++) {
        VectorMA(worldPos, orient.origin[k], model->axis[k], worldPos);
    }

    for (r = 0; r < 3; r++) {
        VectorClear(worldAxis[r]);
        for (k = 0; k < 3; k++) {
            VectorMA(worldAxis[r], orient.axis[r][k], model->axis[k], worldAxis[r]);
        }
    }

    return qtrue;
}

// The inverse of the above: turns a world-space bone frame back into the
// model-space matrix layout the skeletor expects.
static void
CG_RagdollWriteBoneModel(const refEntity_t *model, const vec3_t worldPos, const vec3_t worldAxis[3], float out[4][3])
{
    vec3_t rel;
    float  scale;
    int    r, k;

    scale = CG_RagdollModelScale(model);

    VectorSubtract(worldPos, model->origin, rel);

    for (k = 0; k < 3; k++) {
        out[3][k] = DotProduct(rel, model->axis[k]) / scale;

        if (model->tiki) {
            out[3][k] -= model->tiki->load_origin[k];
        }
    }

    for (r = 0; r < 3; r++) {
        for (k = 0; k < 3; k++) {
            out[r][k] = DotProduct(worldAxis[r], model->axis[k]);
        }
    }
}

//=============================================================
// Seeding
//=============================================================

static float CG_RagdollGravity(void)
{
    if (cg.snap && cg.snap->ps.gravity) {
        return (float)cg.snap->ps.gravity;
    }

    return 800.0f;
}

// How far bone b is twisted about its own length relative to bone a, in signed
// degrees. The parent's reference is first swung onto the child's direction by
// the shortest rotation between them, so what is left is twist alone and not
// how far the joint between them happens to be bent.
static float CG_RagdollTwistBetween(const vec3_t ax, const vec3_t ay, const vec3_t bx, const vec3_t by)
{
    vec3_t rot, carried, ref, side;
    float  sinA, cosA;

    CrossProduct(ax, bx, rot);
    sinA = VectorNormalize(rot);
    cosA = DotProduct(ax, bx);

    if (sinA < 0.001f) {
        VectorCopy(ay, carried);
    } else {
        RotatePointAroundVector(carried, rot, ay, (float)(atan2(sinA, cosA) * 180.0 / M_PI));
    }

    VectorMA(carried, -DotProduct(carried, bx), bx, ref);

    if (VectorNormalize(ref) < 0.001f) {
        return 0.0f;
    }

    CrossProduct(bx, ref, side);

    return (float)(atan2(DotProduct(by, side), DotProduct(by, ref)) * 180.0 / M_PI);
}

static float CG_RagdollRefLength(const cg_ragdoll_t *rd, const rdConstraintDef_t *def)
{
    vec3_t d;
    float  len;

    if (def->via >= 0) {
        VectorSubtract(rd->part[def->via].p, rd->part[def->a].p, d);
        len = VectorLength(d);
        VectorSubtract(rd->part[def->b].p, rd->part[def->via].p, d);
        len += VectorLength(d);
        return len;
    }

    VectorSubtract(rd->part[def->b].p, rd->part[def->a].p, d);
    return VectorLength(d);
}

// Fills in the particle positions, the constraint rest lengths, the per-bone
// seed corrections and the hinge bend references from the entity's current
// animation pose. Returns qfalse when the model is not a usable biped, in
// which case the caller falls back to the plain animated corpse.
// Everything about the corpse that is measured from the pose it is in rather
// than derived from its size: the joint limits, the shape memory, how much
// clearance each limb is asked to keep from the trunk, which pairs of joints
// are checked against each other, and which way the knees bend.
//
// Kept apart from the seed because it has to happen twice. The seed runs at the
// instant of death, but for the length of the blend window the particles are
// still being dragged onto the death animation's pose, and that pose is moving.
// A body that dies bringing its arms in has them somewhere quite different by
// the time the blend ends, and every one of these references still describes
// where it was when it was shot. The solver then spends the rest of the
// corpse's life trying to restore a pose the body left long ago, which is what
// drives the arms into the chest and holds them there.
static qboolean CG_RagdollMeasureLimits(cg_ragdoll_t *rd)
{
    int i;

    rd->numConstraints = 0;
    for (i = 0; i < RD_NUM_CONSTRAINTS; i++) {
        const rdConstraintDef_t *def = &rd_constraints[i];
        rdConstraint_t          *out = &rd->constraint[rd->numConstraints];
        float                    ref = CG_RagdollRefLength(rd, def);

        if (ref < 0.01f) {
            continue;
        }

        out->a      = def->a;
        out->b      = def->b;
        out->minLen = ref * def->minScale;

        // An angle limit is turned into the distance that corresponds to it,
        // using the two segment lengths this joint actually has in this model.
        if (def->maxBendDeg > 0.0f && def->via >= 0) {
            const float rad = (180.0f - def->maxBendDeg) * (float)M_PI / 180.0f;
            vec3_t      d1, d2;
            float       l1, l2;

            VectorSubtract(rd->part[def->via].p, rd->part[def->a].p, d1);
            VectorSubtract(rd->part[def->b].p, rd->part[def->via].p, d2);
            l1 = VectorLength(d1);
            l2 = VectorLength(d2);

            out->minLen = sqrt(l1 * l1 + l2 * l2 - 2.0f * l1 * l2 * cos(rad));
        }
        out->maxLen    = ref * def->maxScale;
        out->stiffness = def->stiffness;
        out->isHard    = (def->maxBendDeg > 0.0f || (def->minScale == 1.0f && def->maxScale == 1.0f)) ? qtrue : qfalse;

        // The soft term always pulls back toward the distance the two joints
        // actually had at the moment of death, not toward the chain length.
        {
            vec3_t d;

            VectorSubtract(rd->part[def->b].p, rd->part[def->a].p, d);
            out->restLen = VectorLength(d);
        }

        rd->numConstraints++;
    }

    return qtrue;
}


// The clearances: how far each limb bone has to stay from each part of the
// trunk, and which pairs of joints are checked against one another at all.
//
// Where two parts already overlap, the clearance asked for is relaxed to
// roughly what they have between them, so the solver is not left fighting
// something it can never satisfy. That makes the pose this is measured from
// matter a great deal: measure it off a body that has already settled onto
// itself and every clearance it has lost is written down as acceptable for
// good. It is always taken from an animation pose, never from the particles.
static qboolean CG_RagdollMeasureClearances(cg_ragdoll_t *rd)
{
    vec3_t torso[3];
    int    i, j;

    // How much of that clearance each limb bone is actually asked to keep from
    // each part of the trunk. Measured against the rest pose, because parts
    // that are close together to begin with, an upper arm and the top of the
    // chest for instance, would otherwise be permanently overlapping and
    // permanently pushed apart.
    if (!CG_RagdollTorsoFrame(rd, torso)) {
        return qfalse;
    }

    for (i = 0; i < RD_NUM_LIMB_SEGMENTS; i++) {
        for (j = 0; j < RD_NUM_TRUNK_SEGMENTS; j++) {
            float  want, rest, ta, tb;
            vec3_t dir;

            rd->segTrunkScale[i][j] = 0.0f;

            // A limb hanging off one end of a trunk capsule always touches it.
            if (rd_trunkSegments[j].a == rd_limbSegments[i].a || rd_trunkSegments[j].a == rd_limbSegments[i].b
                || rd_trunkSegments[j].b == rd_limbSegments[i].a || rd_trunkSegments[j].b == rd_limbSegments[i].b) {
                continue;
            }

            rest = CG_RagdollSegmentToSegment(
                rd->part[rd_limbSegments[i].a].p,
                rd->part[rd_limbSegments[i].b].p,
                rd->part[rd_trunkSegments[j].a].p,
                rd->part[rd_trunkSegments[j].b].p,
                &ta,
                &tb,
                dir
            );

            if (rest < 0.0001f) {
                rd->segTrunkScale[i][j] = RD_SEGMENT_MIN_FRACTION;
                continue;
            }

            want = CG_RagdollTrunkClearance(rd, j, torso, dir, rest) + rd->limbRadius[i];

            if (rest >= want) {
                rd->segTrunkScale[i][j] = 1.0f;
                continue;
            }

            // Never relax below a floor. An arm and the top of the chest are
            // close together to begin with, so relaxing all the way to what
            // they had at the moment of death leaves almost nothing holding
            // them apart, and the arm swings straight through the body near the
            // shoulder, which is precisely where it is most visible.
            rd->segTrunkScale[i][j] = rest * 0.85f / want;

            if (rd->segTrunkScale[i][j] < RD_SEGMENT_MIN_FRACTION) {
                rd->segTrunkScale[i][j] = RD_SEGMENT_MIN_FRACTION;
            }

            // Never ask for more room than the two actually have.
            //
            // The floor above is there so a limb that starts against the body
            // is not simply let through it, and for an arm across a chest that
            // is right. For a thigh it is not: a hip hangs less than four units
            // off the middle of the pelvis and can never be anywhere else, yet
            // the floor was asking for better than five. The constraint was
            // therefore violated from the first frame and pushed for the whole
            // life of the corpse, and with the body on its back the only way
            // out is upward, so the knee rose off the ground and stayed there
            // with the leg propped up like a tent and the foot hanging in the
            // air. Capped at what the pair has, the floor still holds an arm
            // out of a chest and no longer asks a leg for the impossible.
            if (rd->segTrunkScale[i][j] * want > rest) {
                rd->segTrunkScale[i][j] = rest / want;
            }
        }
    }

    // Self collision pairs. Any two joints that are already further apart than
    // their combined thickness must stay that way, which is what stops an arm
    // sinking into the chest or the legs passing through each other. Pairs that
    // legitimately overlap, neighbours in the skeleton above all, are left out.
    rd->numSelfPairs = 0;
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        for (j = i + 1; j < RD_NUM_JOINTS; j++) {
            float  want = rd->jointRadius[i] + rd->jointRadius[j];
            float  seedDist;
            vec3_t d;

            VectorSubtract(rd->part[j].p, rd->part[i].p, d);
            seedDist = VectorLength(d);

            // A limb sunk into the chest is glaring, two limbs resting against
            // each other on the ground is not, and demanding full clearance
            // everywhere only sets the solver against itself.
            if (i > RD_HEADTIP && j > RD_HEADTIP) {
                want *= 0.6f;
            }

            // Parts that already sit closer than their combined thickness, a
            // shoulder against the chest above all, must not simply be dropped
            // from the set: that leaves nothing at all holding them apart, and
            // the shoulders sink into the ribcage. Ask such a pair to keep most
            // of the separation it has at rest instead.
            if (seedDist < want) {
                want = seedDist * 0.85f;
            }

            if (want < 0.01f) {
                continue;
            }

            rd->selfPair[rd->numSelfPairs].a      = i;
            rd->selfPair[rd->numSelfPairs].b      = j;
            rd->selfPair[rd->numSelfPairs].minLen = want;
            rd->numSelfPairs++;
        }
    }

    return qtrue;
}


// Which way each knee bends. Seeded from the pose and, like the clearances,
// deliberately taken only once: after the blend the knee has already been
// worked on by gravity, and if it has started to go the wrong way at all then
// re-measuring here would adopt that as the direction it is supposed to bend.
static qboolean CG_RagdollMeasureHinges(cg_ragdoll_t *rd)
{
    vec3_t torso[3];
    int    i, k;

    // Hinge bend references, expressed in the torso frame so they rotate with
    // the body rather than staying pinned to world axes.
    if (!CG_RagdollTorsoFrame(rd, torso)) {
        return qfalse;
    }

    // The direction each limb hangs in, written down in the torso's frame.
    for (i = 0; i < RD_NUM_CONES; i++) {
        vec3_t dir;

        VectorSubtract(rd->part[rd_cones[i].tip].p, rd->part[rd_cones[i].root].p, dir);

        if (VectorNormalize(dir) < 0.001f) {
            VectorCopy(torso[0], dir);
        }

        for (k = 0; k < 3; k++) {
            rd->coneAxis[i][k] = DotProduct(dir, torso[k]);
        }
    }

    for (i = 0; i < RD_NUM_HINGES; i++) {
        const rdHingeDef_t *h = &rd_hinges[i];
        vec3_t              chord, offset;
        float               len;
        int                 b;

        VectorSubtract(rd->part[h->b].p, rd->part[h->a].p, chord);
        len = VectorNormalize(chord);

        VectorSubtract(rd->part[h->mid].p, rd->part[h->a].p, offset);
        VectorMA(offset, -DotProduct(offset, chord), chord, offset);

        if (len < 0.01f || VectorNormalize(offset) < 0.01f) {
            // The limb died straight, so there is no bend to measure. Use the
            // direction this joint anatomically folds toward instead.
            VectorCopy(rd_hinges[i].defaultBend, rd->hingeBend[i]);
        } else {
            for (k = 0; k < 3; k++) {
                rd->hingeBend[i][k] = DotProduct(offset, torso[k]);
            }
        }

        // Written on both paths, and it used not to be. The straight limb case
        // returned early, so the rest kept the zero it was allocated with, and
        // the roll limit in CG_RagdollHinges is measured against it: a dot
        // product against zero is zero on every frame, so the limit never binds
        // and the bend plane is free to follow the knee wherever it wanders. A
        // leg that dies straight is the common case, which left the limit inert
        // in exactly the deaths it was written for.
        for (b = 0; b < 3; b++) {
            rd->hingeBendRest[i][b] = rd->hingeBend[i][b];
        }
    }


    return qtrue;
}

// Reads every rig joint's world position and axes straight out of the death
// animation, extrapolating the tips the model does not carry. Shared between
// the seed and the frames leading up to it, which need the same positions in
// order to work out how fast the animation is moving.
static qboolean CG_RagdollReadJoints(
    cg_ragdoll_t *rd, refEntity_t *model, vec3_t worldPos[RD_NUM_JOINTS], vec3_t worldAxis[RD_NUM_JOINTS][3]
)
{
    qboolean haveJoint[RD_NUM_JOINTS];
    int      i;

    // Make sure we are reading the pure animation pose, with no override left
    // over from a previous ragdoll on this entity number.
    model->bone_override      = NULL;
    model->num_bone_overrides = 0;
    cgi.ForceUpdatePose(model);

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const rdJointDef_t *def  = &rd_joints[i];
        int                 bone = -1;

        haveJoint[i] = qfalse;

        if (def->boneName) {
            bone = cgi.Tag_NumForName(model->tiki, def->boneName);
        } else if (def->tipName) {
            bone = cgi.Tag_NumForName(model->tiki, def->tipName);
        }

        if (bone >= 0) {
            haveJoint[i] = CG_RagdollReadBoneWorld(model, bone, worldPos[i], worldAxis[i]);
        }

        if (!haveJoint[i] && def->required) {
            return qfalse;
        }
    }

    // Tip joints that the model does not carry are extrapolated along the
    // parent bone's own X axis, which is the direction Biped bones run in.
    // MOHAA player models carry no "Bip01 HeadNub" and no "Bip01 L/R Finger1",
    // so the head and both hands always take this path, and the length it picks
    // is what decides how those bones are oriented. Too short and the tip sits
    // inside its own parent, where a hair of positional noise swings the bone
    // through a large angle; too long and the hand becomes a club that reaches
    // half way up the forearm and shoulders the rest of the body around.
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const rdJointDef_t *def = &rd_joints[i];
        float               len;
        vec3_t              d;

        if (haveJoint[i] || def->parent < 0) {
            continue;
        }

        if (rd_joints[def->parent].parent >= 0 && def->tipRatio > 0.0f) {
            VectorSubtract(worldPos[def->parent], worldPos[rd_joints[def->parent].parent], d);
            len = VectorLength(d) * def->tipRatio;
        } else {
            len = 6.0f;
        }

        if (len < 1.0f) {
            len = 1.0f;
        }

        VectorMA(worldPos[def->parent], len, worldAxis[def->parent][0], worldPos[i]);
        AxisCopy(worldAxis[def->parent], worldAxis[i]);
        haveJoint[i] = qtrue;
    }


    (void)rd;
    return qtrue;
}

// Recent flesh hits, so the ragdoll can be pushed by the shot that killed it.
//
// The impact messages carry a world position and a direction and are parsed a
// frame or more before the entity is seen carrying EF_DEAD, and the parser
// clears its own buffer every frame once it has played the sounds. So they are
// copied here and kept for a short window instead.
//
// Nothing in the message says which entity was hit, so the match is made on
// distance at seed time. That is good enough in the open and can pick the wrong
// body in a crowded doorway, which is a wrong push on a corpse rather than
// anything that affects play.
#define RD_MAX_IMPACTS   16
// A hit is only considered for a body that dies within this long of it.
#define RD_IMPACT_WINDOW 500
// and only if it landed this close to one of the body's particles.
#define RD_IMPACT_RADIUS 40.0f
// Base speed handed to the body, in units per second, before the cvar scale.
#define RD_IMPULSE_SPEED 90.0f
// Most of the push is given to every particle at once, as one velocity for the
// whole body. Handing it to a single joint instead does almost nothing: that
// only stretches the bones leaving it, and the constraint solver takes the
// stretch back out on its next iteration, so the push is absorbed rather than
// moving anything. A uniform velocity creates no stretch, so there is nothing
// for the solver to cancel. The remainder is applied at the joint that was hit,
// where it does stretch against the rest of the body, and that is what turns
// the body about its own centre instead of sliding it flat.
#define RD_IMPULSE_LOCAL 0.30f

typedef struct {
    vec3_t pos;
    vec3_t dir;
    int    large;
    int    time;
} rdImpact_t;

static rdImpact_t rd_impacts[RD_MAX_IMPACTS];
static int        rd_impactHead;

// An explosion is not a bullet and cannot be treated as one.
//
// A shot arrives with a direction, and the body is pushed that way. A blast has
// no direction of its own: it has a place, and which way it throws a body
// depends entirely on where the body was standing. So the position is what is
// kept, and the direction is worked out per particle at the moment it is
// applied.
//
// That is also what makes a body turn over. Nothing here reaches the corpse
// through a flesh hit, so before this the only thing a grenade gave a ragdoll
// was the entity's own velocity, handed to every particle alike. A uniform
// velocity moves a body without turning it, and the server's blast knockback is
// mostly upward, which is why a grenade sent corpses straight into the air like
// a lift. Pushed from a point instead, the near side of the body gets more than
// the far side, and a body that is pushed harder at one end goes over.
typedef struct {
    vec3_t pos;
    float  radius;
    float  speed;
    int    time;

    // Whether this slot holds a blast at all. Kept apart from the time rather
    // than inferred from it being non-zero, which is the same thing everywhere
    // except at the one moment it is not: a blast at time zero would read as an
    // empty slot and be ignored.
    qboolean used;
} rdBlast_t;

static rdBlast_t rd_blasts[RD_MAX_IMPACTS];
static int       rd_blastHead;

// How far each kind of blast reaches, and how hard it pushes at the centre, in
// units and units per second before the cvar scale. A tank shell throws a body
// across a courtyard; a grenade rolls one over.
//
// These speeds are what a grenade was actually judged to look right at in the
// game, rather than what was first guessed: the guesses were half this and were
// only ever seen through cg_ragdoll_impulse set to 2. Folded in here so the
// scale cvar can sit at 1 and mean it.
static const struct {
    float radius;
    float speed;
} rd_blastKinds[] = {
    {200.0f, 520.0f}, // grenade
    {260.0f, 680.0f}, // bazooka
    {340.0f, 840.0f}, // heavy shell
    {420.0f, 1000.0f} // tank
};

#define RD_NUM_BLAST_KINDS ((int)(sizeof(rd_blastKinds) / sizeof(rd_blastKinds[0])))

// Called by the message parser for every explosion, whatever it hit. kind
// indexes rd_blastKinds and is clamped, so an unknown one reads as a grenade.
void CG_RagdollNoteExplosion(const vec3_t pos, int kind)
{
    rdBlast_t *blast;

    if (cg_ragdoll_blastimpulse->value <= 0.0f) {
        return;
    }

    if (kind < 0) {
        kind = 0;
    }
    if (kind >= RD_NUM_BLAST_KINDS) {
        kind = RD_NUM_BLAST_KINDS - 1;
    }

    blast = &rd_blasts[rd_blastHead];
    rd_blastHead = (rd_blastHead + 1) % RD_MAX_IMPACTS;

    VectorCopy(pos, blast->pos);
    blast->radius = rd_blastKinds[kind].radius;
    blast->speed  = rd_blastKinds[kind].speed;
    blast->time   = cg.time;
    blast->used   = qtrue;

    if (cg_ragdoll_debug->integer) {
        cgi.Printf(
            "ragdoll: blast at %.0f %.0f %.0f kind %d radius %.0f t %d\n",
            pos[0], pos[1], pos[2], kind, blast->radius, cg.time
        );
    }
}

// Look for an explosion close enough and recent enough to have been what killed
// this body. The nearest one wins, since that is the one that did the throwing.
static void CG_RagdollFindBlast(cg_ragdoll_t *rd)
{
    float bestDist = 0.0f;
    int   i;

    rd->hasBlast = qfalse;

    for (i = 0; i < RD_MAX_IMPACTS; i++) {
        const rdBlast_t *blast = &rd_blasts[i];
        vec3_t           d;
        float            dist;

        if (!blast->used || cg.time - blast->time > RD_IMPACT_WINDOW) {
            continue;
        }

        VectorSubtract(rd->part[RD_PELVIS].p, blast->pos, d);
        dist = VectorLength(d);

        if (dist > blast->radius) {
            continue;
        }

        if (!rd->hasBlast || dist < bestDist) {
            rd->hasBlast = qtrue;
            bestDist     = dist;
            VectorCopy(blast->pos, rd->blastPos);
            rd->blastRadius = blast->radius;
            rd->blastSpeed  = blast->speed * cg_ragdoll_blastimpulse->value;
        }
    }

    if (rd->hasBlast && cg_ragdoll_debug->integer) {
        cgi.Printf(
            "ragdoll: blast %.0f units away, radius %.0f, speed %.0f\n", bestDist, rd->blastRadius, rd->blastSpeed
        );
    }
}

// Called by the message parser for every flesh hit. dir is the outward normal
// it stores, so the bullet was travelling the other way.
void CG_RagdollNoteFleshImpact(const vec3_t pos, const vec3_t dir, int large)
{
    rdImpact_t *imp;

    if (cg_ragdoll_impulse->value <= 0.0f) {
        return;
    }

    imp = &rd_impacts[rd_impactHead];
    rd_impactHead = (rd_impactHead + 1) % RD_MAX_IMPACTS;

    VectorCopy(pos, imp->pos);
    // The server sends the surface normal at the hit, which points back out
    // toward whoever fired. The parser has already flipped it once, so what
    // arrives here is pointing into the body, which is the way to push it.
    VectorCopy(dir, imp->dir);
    VectorNormalize(imp->dir);
    imp->large = large;
    imp->time  = cg.time;

    if (cg_ragdoll_debug->integer) {
        cgi.Printf(
            "ragdoll: flesh hit at %.0f %.0f %.0f dir %.2f %.2f %.2f large %d t %d\n",
            imp->pos[0], imp->pos[1], imp->pos[2], imp->dir[0], imp->dir[1], imp->dir[2], large, cg.time
        );
    }
}

// Look for a hit that could have been the one that killed this body, and if
// there is one, work out the velocity it should hand over.
static void CG_RagdollFindImpulse(cg_ragdoll_t *rd)
{
    int   i, j;
    int   best     = -1;
    int   bestJoint = RD_PELVIS;
    float bestDist = RD_IMPACT_RADIUS;
    // Nearest hit regardless of the radius, so the debug line can tell a body
    // that saw no hits at all from one that saw a hit and judged it too far.
    float nearestAny = 1.0e9f;

    rd->hasImpulse = qfalse;

    if (cg_ragdoll_impulse->value <= 0.0f) {
        return;
    }

    for (i = 0; i < RD_MAX_IMPACTS; i++) {
        const rdImpact_t *imp = &rd_impacts[i];

        if (!imp->time || cg.time - imp->time > RD_IMPACT_WINDOW) {
            continue;
        }

        for (j = 0; j < RD_NUM_JOINTS; j++) {
            vec3_t d;
            float  dist;

            VectorSubtract(imp->pos, rd->part[j].p, d);
            dist = VectorLength(d);

            if (dist < nearestAny) {
                nearestAny = dist;
            }

            if (dist < bestDist) {
                bestDist  = dist;
                best      = i;
                bestJoint = j;
            }
        }
    }

    if (cg_ragdoll_debug->integer) {
        int live = 0;

        for (i = 0; i < RD_MAX_IMPACTS; i++) {
            if (rd_impacts[i].time && cg.time - rd_impacts[i].time <= RD_IMPACT_WINDOW) {
                live++;
            }
        }

        cgi.Printf(
            "ragdoll: seed t %d, %d hit(s) in window, nearest %.1f units (need < %.0f) -> %s\n",
            cg.time, live, (nearestAny > 1.0e8f) ? -1.0f : nearestAny, RD_IMPACT_RADIUS,
            (best >= 0) ? "PUSH" : "no push"
        );
    }

    if (best < 0) {
        return;
    }

    // iLarge is two bits of calibre, which is the only measure of how hard the
    // shot was that reaches the client at all.
    VectorScale(
        rd_impacts[best].dir,
        RD_IMPULSE_SPEED * cg_ragdoll_impulse->value * (1.0f + 0.5f * (float)rd_impacts[best].large),
        rd->impulseVel
    );

    rd->impulseJoint = bestJoint;
    rd->hasImpulse   = qtrue;
}

static qboolean CG_RagdollSeed(cg_ragdoll_t *rd, centity_t *cent, refEntity_t *model)
{
    vec3_t   worldPos[RD_NUM_JOINTS];
    vec3_t   worldAxis[RD_NUM_JOINTS][3];
    vec3_t   entVel;
    float    bodyScale;
    int      i, j, k;

    if (!CG_RagdollReadJoints(rd, model, worldPos, worldAxis)) {
        return qfalse;
    }
    // The corpse inherits the entity's own motion, where the snapshots give it.
    VectorClear(entVel);
    if (cent->interpolate && cg.nextSnap && cg.nextSnap->serverTime > cg.snap->serverTime) {
        float dt = (float)(cg.nextSnap->serverTime - cg.snap->serverTime) * 0.001f;

        VectorSubtract(cent->nextState.origin, cent->currentState.origin, entVel);
        VectorScale(entVel, 1.0f / dt, entVel);
    }


    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];
        vec3_t        vel;

        VectorCopy(worldPos[i], part->p);
        part->invMass    = rd_joints[i].invMass;
        part->onGround   = qfalse;
        part->hasContact = qfalse;

        VectorCopy(entVel, vel);

        // A little asymmetry, otherwise mirrored limbs move identically and
        // the corpse reads as mechanical rather than limp.
        for (k = 0; k < 3; k++) {
            vel[k] += crandom() * 6.0f;
        }

        VectorCopy(vel, part->v);
        VectorCopy(part->p, part->pPrev);
    }

    // Constraint rest lengths, measured from the seeded pose so they adapt to
    // any model's proportions and to the entity scale.
    // Scale the hard coded body thicknesses to this particular skeleton. Going
    // by the pelvis to head distance rather than the model scale means an
    // unusually short or lanky character is handled correctly too.
    {
        vec3_t d;

        VectorSubtract(rd->part[RD_HEAD].p, rd->part[RD_PELVIS].p, d);
        bodyScale = VectorLength(d) / 28.0f;

        if (bodyScale < 0.05f) {
            bodyScale = 1.0f;
        }
    }

    rd->collisionSlop    = RD_COLLISION_SLOP * bodyScale;
    rd->hingeLateralSlop = RD_HINGE_LATERAL_SLOP;
    rd->radius        = 3.0f * bodyScale;
    if (rd->radius < 0.5f) {
        rd->radius = 0.5f;
    }


    // Per-bone seed correction. Storing frame^T * boneBasis means we never
    // need to know which local axis runs down the bone, and it makes the
    // reconstruction reproduce the seed pose exactly at t = 0.
    rd->numOverrides = 0;
    for (i = 0; i < RD_NUM_BONES; i++) {
        vec3_t frame[3];
        vec3_t ownPos, ownAxis[3];
        int    bone;

        rd->boneIndex[i] = -1;
        VectorSet(rd->transportY[i], 0, 0, 1);
        rd->spineRestValid = qfalse;
        rd->twistRestValid = qfalse;
        VectorSet(rd->transportX[i], 1, 0, 0);

        bone = cgi.Tag_NumForName(model->tiki, rd_bones[i].boneName);
        if (bone < 0) {
            continue;
        }

        // The bone's own pose, and not the pose of the joint it hangs from.
        //
        // Those are the same thing for every bone but one. "Bip01" and "Bip01
        // Pelvis" are two rows of the table sharing a single joint, and the
        // joint is read from the second of them, so a correction taken from the
        // joint hands the root the pelvis's orientation instead of its own. In
        // the rig the two stand a quarter turn apart, the root along the body's
        // facing and the pelvis up the spine: measured out of the retail pak,
        // every one of the seventeen death animations has them ninety degrees
        // apart, to the tenth of a degree.
        //
        // That quarter turn does not stay in the root. It is what the first
        // pose measures as the twist between the two, so it is what goes into
        // twistRest, and from the moment the blend ends the two are built from
        // the same frame and measure as having no twist between them at all.
        // CG_RagdollUntwist reads the difference as ninety degrees of twist
        // that has to be taken out, and puts a quarter turn into the pelvis on
        // every frame for the rest of the corpse's life. Being parent relative
        // it then carries the same turn down the spine, into both thighs and
        // out along the arms, each about its own length rather than about a
        // common axis, which is a shear at the waist rather than a turn: the
        // lower body facing one way, the chest another, and the mesh torn open
        // across the joints between them.
        if (!CG_RagdollReadBoneWorld(model, bone, ownPos, ownAxis)) {
            continue;
        }

        if (!CG_RagdollBoneFrame(rd, i, frame)) {
            continue;
        }

        // correction[j][k] is the k-th coordinate of the bone's j-th axis in
        // the construction frame. Storing it this way means we never need to
        // know which local axis runs down the bone, and it makes the
        // reconstruction reproduce the seed pose exactly.
        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                rd->correction[i][j][k] = DotProduct(ownAxis[j], frame[k]);
            }
        }

        rd->boneIndex[i] = bone;
        AxisCopy(ownAxis, rd->boneAxis[i]);

        VectorCopy(ownPos, rd->bonePos[i]);
        rd->numOverrides++;
    }

    if (!rd->numOverrides) {
        return qfalse;
    }

    // Offset of each bone from its parent, in the parent's seed frame. Because
    // this is what positions the bone from now on, the distance between a bone
    // and its parent can never drift, however hard the solver is pushed.
    for (i = 0; i < RD_NUM_BONES; i++) {
        const int par = rd_bones[i].parent;
        vec3_t    d;

        VectorClear(rd->localOffset[i]);

        if (par < 0 || rd->boneIndex[i] < 0 || rd->boneIndex[par] < 0) {
            continue;
        }

        VectorSubtract(rd->bonePos[i], rd->bonePos[par], d);

        for (k = 0; k < 3; k++) {
            rd->localOffset[i][k] = DotProduct(d, rd->boneAxis[par][k]);
        }
    }

    // How thick the body is at each joint, and so how far things have to stay
    // apart. Fixed by the size of the model, not by the pose, so this belongs
    // with the seed rather than with the measurements taken from the pose.
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rd->jointRadius[i] = rd_joints[i].radius * bodyScale;
    }

    // A bone is about as thick as the thinner of the two joints it runs
    // between, which is the wrist end of a forearm or the ankle end of a shin.
    for (i = 0; i < RD_NUM_LIMB_SEGMENTS; i++) {
        const float ra = rd->jointRadius[rd_limbSegments[i].a];
        const float rb = rd->jointRadius[rd_limbSegments[i].b];

        rd->limbRadius[i] = (ra < rb ? ra : rb) * 0.85f;
    }

    // The trunk capsules are sized to the thinner of their two ends, then
    // given a wide and a shallow axis rather than one radius.
    for (i = 0; i < RD_NUM_TRUNK_SEGMENTS; i++) {
        const float ra = rd->jointRadius[rd_trunkSegments[i].a];
        const float rb = rd->jointRadius[rd_trunkSegments[i].b];
        const float r  = ra < rb ? ra : rb;

        rd->trunkWide[i] = r * RD_TRUNK_WIDE_RATIO;
        rd->trunkDeep[i] = r * RD_TRUNK_DEEP_RATIO;
    }

    if (!CG_RagdollMeasureLimits(rd) || !CG_RagdollMeasureClearances(rd) || !CG_RagdollMeasureHinges(rd)) {
        return qfalse;
    }
    return qtrue;
}

// Gathers the pure animation pose for every overridden bone. The override must
// be detached from the refEntity first, otherwise this reads back the ragdoll
// pose installed on the skeletor at the end of the previous frame.
static void CG_RagdollReadAnimPose(const cg_ragdoll_t *rd, refEntity_t *model)
{
    int i;

    model->bone_override      = NULL;
    model->num_bone_overrides = 0;
    cgi.ForceUpdatePose(model);

    for (i = 0; i < RD_NUM_BONES; i++) {
        rd_animPose[i].valid = qfalse;

        if (rd->boneIndex[i] < 0) {
            continue;
        }

        rd_animPose[i].valid =
            CG_RagdollReadBoneWorld(model, rd->boneIndex[i], rd_animPose[i].pos, rd_animPose[i].axis);
    }
}

//=============================================================
// Simulation
//=============================================================

// Pushes any particle that a constraint has driven back into its contact plane
// out again. Re-tracing on every iteration would be far too expensive, but the
// plane the last trace found is a good enough stand-in, and applying it inside
// the solver is what lets the constraints and the world converge together
// instead of taking turns overriding each other.
static void CG_RagdollProjectContacts(cg_ragdoll_t *rd)
{
    int i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];
        float         d;

        if (!part->hasContact) {
            continue;
        }

        d = DotProduct(part->p, part->contactNormal) - part->contactDist;

        if (d < 0.0f) {
            VectorMA(part->p, -d, part->contactNormal, part->p);
        }
    }
}

// Holds each limb within a cone about the direction it died in, measured at the
// hip or the shoulder and carried in the torso's frame so it turns with the
// body. This is what keeps a corpse from spreading out flat.
//
// Being an angle at one joint, it says nothing at all about what the rest of
// that limb is doing: a leg held inside its cone at the hip is free to
// straighten at the knee, which is exactly what a distance from head to foot
// could never allow.
static void CG_RagdollCones(cg_ragdoll_t *rd)
{
    vec3_t torso[3];
    int    i, k;

    if (!CG_RagdollTorsoFrame(rd, torso)) {
        return;
    }

    for (i = 0; i < RD_NUM_CONES; i++) {
        rdParticle_t *root = &rd->part[rd_cones[i].root];
        rdParticle_t *tip  = &rd->part[rd_cones[i].tip];
        vec3_t        axis, dir, side, want, target, delta;
        float         len, along, limit, sinLimit, sideLen, total;

        // The stored axis back into world space.
        VectorClear(axis);
        for (k = 0; k < 3; k++) {
            VectorMA(axis, rd->coneAxis[i][k], torso[k], axis);
        }

        if (VectorNormalize(axis) < 0.001f) {
            continue;
        }

        VectorSubtract(tip->p, root->p, dir);
        len = VectorNormalize(dir);

        if (len < 0.001f) {
            continue;
        }

        limit = cos(rd_cones[i].maxDeg * (float)M_PI / 180.0f);
        along = DotProduct(dir, axis);

        if (along >= limit) {
            continue;
        }

        // The part of the limb's direction across the cone axis, which is what
        // has to be pulled back in.
        VectorMA(dir, -along, axis, side);
        sideLen = VectorNormalize(side);

        if (sideLen < 0.001f) {
            // Folded straight back along the axis, so any direction across it
            // will do to lift it out of the singularity.
            VectorCopy(torso[1], side);
            VectorMA(side, -DotProduct(side, axis), axis, side);

            if (VectorNormalize(side) < 0.001f) {
                continue;
            }
        }

        sinLimit = sqrt(1.0f - limit * limit);

        VectorScale(axis, limit, want);
        VectorMA(want, sinLimit, side, want);
        VectorMA(root->p, len, want, target);

        // Shared between the two ends by mass, and only partly applied, in
        // keeping with everything else the solver does.
        VectorSubtract(target, tip->p, delta);
        total = root->invMass + tip->invMass;

        if (total < 0.0001f) {
            continue;
        }

        VectorMA(tip->p, RD_CONE_RATE * (tip->invMass / total), delta, tip->p);
        VectorMA(root->p, -RD_CONE_RATE * (root->invMass / total), delta, root->p);
    }
}

// Keeps elbows and knees bending the way they were seeded. The distance limits
// alone happily let a knee fold backwards.
static void CG_RagdollHinges(cg_ragdoll_t *rd)
{
    vec3_t torso[3];
    vec3_t correction;
    vec3_t lateral;
    float  violation, scale;
    int    i, k;

    if (!CG_RagdollTorsoFrame(rd, torso)) {
        return;
    }

    for (i = 0; i < RD_NUM_HINGES; i++) {
        const rdHingeDef_t *h = &rd_hinges[i];
        vec3_t              chord, offset, want;
        float               len;

        if (VectorLengthSquared(rd->hingeBend[i]) < 0.0001f) {
            continue;
        }

        VectorClear(want);
        for (k = 0; k < 3; k++) {
            VectorMA(want, rd->hingeBend[i][k], torso[k], want);
        }

        if (VectorNormalize(want) < 0.001f) {
            continue;
        }

        VectorSubtract(rd->part[h->b].p, rd->part[h->a].p, chord);
        if (VectorNormalize(chord) < 0.001f) {
            continue;
        }

        // Strip the component along the chord from both the reference
        // and the current offset, so we compare pure bend directions.
        VectorMA(want, -DotProduct(want, chord), chord, want);
        if (VectorNormalize(want) < 0.001f) {
            continue;
        }

        VectorSubtract(rd->part[h->mid].p, rd->part[h->a].p, offset);
        VectorMA(offset, -DotProduct(offset, chord), chord, offset);
        len = VectorNormalize(offset);

        if (len < 0.01f) {
            continue;
        }

        // How far onto the wrong side the joint has gone, from 0 at the
        // boundary to 1 when it is fully inverted.
        // Hold the joint in its bend plane. A knee is a hinge with a single
        // degree of freedom, but nothing above stops the shin swinging out
        // sideways: sideways travel leaves the "which side is it bending"
        // test reading almost zero, so it passes unnoticed and the leg ends up
        // bent in a direction a leg cannot bend.
        CrossProduct(chord, want, lateral);

        if (VectorNormalize(lateral) > 0.001f) {
            const float outOfPlane = DotProduct(offset, lateral) * len;

            if (fabs(outOfPlane) > rd->hingeLateralSlop) {
                const float pull =
                    (outOfPlane > 0.0f ? -1.0f : 1.0f) * (fabs(outOfPlane) - rd->hingeLateralSlop)
                    * RD_HINGE_LATERAL_RATE;

                VectorMA(rd->part[h->mid].p, pull, lateral, rd->part[h->mid].p);
            }
        }

        // A knee is a hinge, but the thigh it hangs from can rotate at the hip,
        // so the plane the knee swings in is not fixed to the body: a leg lying
        // on the ground rolls until the bent knee points out to the side and
        // the whole leg lies flat. Held in the torso's frame the leg cannot
        // roll at all, and a body that died on its feet goes on bending its
        // knee the way it bent then, which once the body is on its back means
        // straight upward. That is what leaves a shin standing vertical with
        // the foot in the air, and it is stable, so the corpse sleeps like it.
        //
        // Letting the stored plane follow where the knee has actually gone,
        // slowly, gives back the roll the hip should have had while still
        // holding the joint steady from one frame to the next.
        {
            vec3_t local;
            int    b;

            for (b = 0; b < 3; b++) {
                local[b] = DotProduct(offset, torso[b]);
            }

            for (b = 0; b < 3; b++) {
                rd->hingeBend[i][b] += (local[b] - rd->hingeBend[i][b]) * RD_HINGE_ROLL_RATE;
            }

            VectorNormalize(rd->hingeBend[i]);

            // A hip rotates about the length of the thigh only so far, so the
            // plane is allowed to follow the leg that far and no further.
            {
                const float dot = DotProduct(rd->hingeBend[i], rd->hingeBendRest[i]);
                const float lim = (float)cos(RD_HINGE_ROLL_LIMIT * M_PI / 180.0);

                if (dot < lim) {
                    vec3_t across;
                    float  acrossLen;

                    VectorMA(rd->hingeBend[i], -dot, rd->hingeBendRest[i], across);
                    acrossLen = VectorNormalize(across);

                    if (acrossLen > 0.001f) {
                        const float s2 = (float)sqrt(1.0 - (double)lim * lim);

                        for (b = 0; b < 3; b++) {
                            rd->hingeBend[i][b] = rd->hingeBendRest[i][b] * lim + across[b] * s2;
                        }
                    }
                }
            }
        }

        violation = -DotProduct(offset, want);

        if (violation < RD_HINGE_SLOP) {
            continue;
        }

        // The correction has to fade to nothing as the joint approaches the
        // boundary. Applying a full sized nudge for a violation of any size, as
        // this used to, means a knee resting near the limit is shoved back and
        // forth every single frame, and with sleeping turned off the whole
        // corpse fidgets for as long as it lies there.
        scale = (violation - RD_HINGE_SLOP) * len * RD_HINGE_RATE;

        for (k = 0; k < 3; k++) {
            correction[k] = want[k] * scale;
        }

        VectorAdd(rd->part[h->mid].p, correction, rd->part[h->mid].p);
    }
}


// Keeps whole limb bones out of the head and the trunk. Testing only the joints
// at each end of a bone is not enough: a forearm can lie straight through the
// head with the elbow out one side and the wrist out the other, and every joint
// to joint distance still perfectly satisfied. That is exactly what an arm
// clipping through the head looks like.
static void CG_RagdollSegmentCollide(cg_ragdoll_t *rd)
{
    int    n, m, k;
    vec3_t torso[3];

    if (!CG_RagdollTorsoFrame(rd, torso)) {
        return;
    }

    for (n = 0; n < RD_NUM_LIMB_SEGMENTS; n++) {
        rdParticle_t *pa = &rd->part[rd_limbSegments[n].a];
        rdParticle_t *pb = &rd->part[rd_limbSegments[n].b];

        for (m = 0; m < RD_NUM_TRUNK_SEGMENTS; m++) {
            rdParticle_t *pc = &rd->part[rd_trunkSegments[m].a];
            rdParticle_t *pd = &rd->part[rd_trunkSegments[m].b];
            vec3_t        dir, push;
            float         ta, tb, dist, scale, want;

            if (rd->segTrunkScale[n][m] <= 0.0f) {
                continue;
            }

            dist = CG_RagdollSegmentToSegment(pa->p, pb->p, pc->p, pd->p, &ta, &tb, dir);

            if (dist < 0.0001f) {
                continue;
            }

            want = (CG_RagdollTrunkClearance(rd, m, torso, dir, dist) + rd->limbRadius[n])
                     * rd->segTrunkScale[n][m]
                 - rd->collisionSlop;

            if (want <= 0.0f || dist >= want) {
                continue;
            }

            // dir already runs from the trunk toward the limb, so it is the
            // direction the limb has to move in.
            VectorScale(dir, 1.0f / dist, push);

            // Deliberately a partial correction. Pushing the whole overlap out
            // in one go, every iteration, feeds the body energy faster than the
            // damping can take it out and the corpse tears itself apart.
            scale = (want - dist) * RD_SEGMENT_RATE;

            // Shared between the four ends in proportion to where along each
            // segment the contact falls, so a touch near one end of a bone
            // moves that end and leaves the other where it is.
            for (k = 0; k < 3; k++) {
                pa->p[k] += push[k] * scale * (1.0f - ta);
                pb->p[k] += push[k] * scale * ta;
                pc->p[k] -= push[k] * scale * 0.5f * (1.0f - tb);
                pd->p[k] -= push[k] * scale * 0.5f * tb;
            }
        }
    }
}

// Pushes apart any two body parts that have come closer than their combined
// thickness. Without this the solver is free to fold an arm straight through
// the chest, because the distance constraints alone say nothing about the
// volume the body actually occupies.
static void CG_RagdollSelfCollide(cg_ragdoll_t *rd)
{
    int n, k;

    for (n = 0; n < rd->numSelfPairs; n++) {
        rdParticle_t *pa = &rd->part[rd->selfPair[n].a];
        rdParticle_t *pb = &rd->part[rd->selfPair[n].b];
        vec3_t        d;
        float         len, diff, target, wa, wb, wsum;

        VectorSubtract(pb->p, pa->p, d);
        len = VectorLength(d);

        if (len < 0.0001f) {
            continue;
        }

        // Resolve only the overlap beyond the allowed slop, so that once the
        // two parts are merely touching nothing pushes them any further.
        target = rd->selfPair[n].minLen - rd->collisionSlop;

        if (len >= target) {
            continue;
        }

        wa   = pa->invMass;
        wb   = pb->invMass;
        wsum = wa + wb;

        if (wsum < 0.0001f) {
            continue;
        }

        diff = (target - len) / len;

        for (k = 0; k < 3; k++) {
            const float da = -d[k] * diff * (wa / wsum);
            const float db = d[k] * diff * (wb / wsum);

            pa->p[k] += da;
            pb->p[k] += db;
        }
    }
}

// How much of the shape memory is still acting, from 1 at the moment of death
// to 0 once the body has gone limp.
//
// Every soft constraint here pulls back toward the distance it had at the
// instant of death. Held for the whole life of the corpse, that does not make a
// body, it makes a mannequin welded in the posture it died in. Traced from the
// game: a man shot standing has his waist driven to 29 degrees by the impact
// and pulled back to 8, and holds his hip and knee within a few degrees of
// their dying angles for five seconds together. Stood on its head on a
// staircase a mannequin balances there quite happily. A body folds and goes
// down the steps.
//
// A corpse is briefly tense and then is not, so this fades. Only the bias
// toward the death pose goes: the hard limits are taken before it ever applies,
// so bone lengths, the joint ranges, the ceiling on how far the legs may open
// and the braces that stop the chest folding onto the thighs are all untouched.
static float CG_RagdollLimpness(const cg_ragdoll_t *rd)
{
    float t;

    if (cg_ragdoll_limptime->integer <= 0) {
        return 1.0f;
    }

    t = (float)(cg.time - rd->startTime) / (float)cg_ragdoll_limptime->integer;

    if (t <= 0.0f) {
        return 1.0f;
    }

    if (t >= 1.0f) {
        return 0.0f;
    }

    // Eased rather than straight, so the body does not visibly change its mind
    // at either end of the fade.
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

static void CG_RagdollSolveConstraints(cg_ragdoll_t *rd, int iterations)
{
    const float limp = CG_RagdollLimpness(rd);
    int it, i, k;

    if (iterations < 3) {
        iterations = 3;
    }

    for (it = 0; it < iterations; it++) {
        // The soft shaping constraints, the joint limits, the torso braces and
        // the shape memory springs, stop taking part for the last couple of
        // iterations. They decide the overall shape the body falls into, but
        // they must not get the final word: in a Gauss-Seidel solver whatever
        // runs last wins, and letting a shape spring win means it can pull a
        // hand back into the chest that self collision has just pushed out.
        const qboolean useSoft = (it < iterations - 2) ? qtrue : qfalse;
        int            pass;

        for (pass = 0; pass < 2; pass++) {
            const qboolean wantStick = pass ? qfalse : qtrue;

            if (!wantStick && !useSoft) {
                continue;
            }

            for (i = 0; i < rd->numConstraints; i++) {
                const rdConstraint_t *c = &rd->constraint[i];
                rdParticle_t         *pa;
                rdParticle_t         *pb;
                vec3_t                d;
                float                 len, target, diff, wa, wb, wsum;

                if (c->isHard != wantStick) {
                    continue;
                }

                pa = &rd->part[c->a];
                pb = &rd->part[c->b];

                VectorSubtract(pb->p, pa->p, d);
                len = VectorLength(d);

                if (len < 0.0001f) {
                    continue;
                }

                if (len < c->minLen) {
                    target = c->minLen;
                } else if (len > c->maxLen) {
                    target = c->maxLen;
                } else if (c->stiffness > 0.0f) {
                    float soft = c->stiffness * cg_ragdoll_stiffness->value * limp;


                    if (soft > 0.9f) {
                        soft = 0.9f;
                    }

                    target = len + (c->restLen - len) * soft;
                } else {
                    continue;
                }

                if (target == len) {
                    continue;
                }

                wa   = pa->invMass;
                wb   = pb->invMass;
                wsum = wa + wb;

                if (wsum < 0.0001f) {
                    continue;
                }

                diff = (target - len) / len;

                for (k = 0; k < 3; k++) {
                    pa->p[k] -= d[k] * diff * (wa / wsum);
                    pb->p[k] += d[k] * diff * (wb / wsum);
                }
            }
        }

        // Anatomy and collision run every iteration, after the bone sticks, and
        // keep running through the final iterations once the soft constraints
        // have dropped out. A knee on the wrong side of its own leg, or a hand
        // inside the ribcage, is far more noticeable than a joint sitting a
        // little outside one of the soft limits.
        CG_RagdollHinges(rd);
        CG_RagdollCones(rd);
        CG_RagdollSelfCollide(rd);
        CG_RagdollSegmentCollide(rd);
        CG_RagdollProjectContacts(rd);
    }

    // Self collision is the slowest of these to converge, because every sweep
    // of the bone sticks pulls a little of it back. A couple of extra rounds of
    // just the hard requirements at the end costs very little and takes the
    // residual overlap down to something that cannot be seen.
    for (it = 0; it < 3; it++) {
        for (i = 0; i < rd->numConstraints; i++) {
            const rdConstraint_t *c = &rd->constraint[i];
            rdParticle_t         *pa;
            rdParticle_t         *pb;
            vec3_t                d;
            float                 len, diff, wa, wb, wsum;

            if (!c->isHard) {
                continue;
            }

            pa = &rd->part[c->a];
            pb = &rd->part[c->b];

            VectorSubtract(pb->p, pa->p, d);
            len = VectorLength(d);

            if (len < 0.0001f) {
                continue;
            }

            wa   = pa->invMass;
            wb   = pb->invMass;
            wsum = wa + wb;

            if (wsum < 0.0001f) {
                continue;
            }

            diff = (c->restLen - len) / len;

            for (k = 0; k < 3; k++) {
                pa->p[k] -= d[k] * diff * (wa / wsum);
                pb->p[k] += d[k] * diff * (wb / wsum);
            }
        }

        CG_RagdollSelfCollide(rd);
        CG_RagdollSegmentCollide(rd);
        CG_RagdollProjectContacts(rd);
    }
}

// The constraint solve, with a note kept of how far it moved each particle.
//
// That displacement is the whole difficulty with a Verlet ragdoll: it is
// indistinguishable from motion, so satisfying a bone length or a joint limit
// hands the body speed out of nowhere, and the damping that takes the speed
// back out is the same damping that stops a corpse sliding. Recorded here, how
// much of it counts as motion becomes a number that can be chosen and measured
// rather than a property of the integrator.
static void CG_RagdollSolveTracked(cg_ragdoll_t *rd, int iterations)
{
    vec3_t before[RD_NUM_JOINTS];
    int    i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        VectorCopy(rd->part[i].p, before[i]);
    }

    CG_RagdollSolveConstraints(rd, iterations);

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        vec3_t d;

        VectorSubtract(rd->part[i].p, before[i], d);
        VectorAdd(rd->part[i].solved, d, rd->part[i].solved);
    }
}

static int CG_RagdollCollide(cg_ragdoll_t *rd, int skipEntity)
{
    // Sized from the model, so a scaled-down character does not end up
    // colliding as though its joints were as fat as a full-size one.
    const vec3_t rd_mins = {-rd->radius, -rd->radius, -rd->radius};
    const vec3_t rd_maxs = {rd->radius, rd->radius, rd->radius};

    const int   mask     = RD_CLIPMASK;
    const float friction = cg_ragdoll_friction->value;

    const float bounce   = cg_ragdoll_bounce->value;
    int         moved    = 0;
    int         i, k;

    rd->buriedMask = 0;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];
        trace_t       trace;
        vec3_t        v, vn, vt;
        float         dot;

        vec3_t before;

        VectorCopy(part->p, before);
        part->onGround = qfalse;

        // Contact planes are deliberately kept from step to step. Once a
        // particle has touched something, the solver keeps honouring that
        // plane, which is what stops a constraint from quietly dragging it
        // through the surface on a later step.
        if (part->hasContact) {
            if (DotProduct(part->p, part->contactNormal) - part->contactDist > RD_CONTACT_FORGET) {
                part->hasContact = qfalse;
            } else if (cg.time - part->contactTime > RD_CONTACT_STALE) {
                // Drifting off the plane is not enough to notice that a surface
                // has gone, because a particle hanging under a remembered plane
                // never rises above it and the distance test never fires. It
                // simply hangs there, and the body hangs off it.
                //
                // So the world is asked directly: a short trace along the
                // plane's own normal either still finds the surface or does
                // not. Only for a plane nothing has confirmed for a while, so
                // a settled corpse pays for this a handful of times a second.
                trace_t probe;
                vec3_t  behind;
                float   above;

                // Traced as far as the plane itself, wherever the particle has
                // drifted to, and a little past it. Reaching a fixed distance
                // instead asks a different question from the one the forget
                // rule asks: a plane is kept until the particle is sixteen
                // units clear of it, but the probe only looked three and a half
                // down, so anything that had drifted between those two numbers
                // was reported as a surface that had gone. Traced in the game, a
                // hand resting on a step lost its plane every quarter second,
                // dropped, caught the step again and settled, over and over,
                // which kept resetting the quiet timer just short of the four
                // hundred milliseconds the body needed to fall asleep.
                above = DotProduct(part->p, part->contactNormal) - part->contactDist;

                if (above < 0.0f) {
                    above = 0.0f;
                }

                VectorMA(part->p, -(above + rd->radius + RD_CONTACT_PROBE), part->contactNormal, behind);
                CG_Trace(
                    &probe, part->p, rd_mins, rd_maxs, behind, skipEntity, mask, qfalse, qtrue, "CG_RagdollProbe"
                );

                if (probe.fraction >= 1.0f && !probe.startsolid && !probe.allsolid) {
                    part->hasContact = qfalse;
                } else {
                    part->contactTime = cg.time;
                }
            }
        }

        CG_Trace(&trace, part->pPrev, rd_mins, rd_maxs, part->p, skipEntity, mask, qfalse, qtrue, "CG_RagdollCollide");

        if (trace.startsolid || trace.allsolid) {
            // Buried in the world. Dropping it back where it was and leaving
            // it there is what this used to do, and it makes the particle an
            // anchor: gravity nudges it, the trace starts buried again, it is
            // dropped back again, and it goes on being dropped back every step
            // for the rest of the corpse's life while the body hangs off it.
            // That is the corpse left standing in mid air beside a wall, and
            // measured over the suite a joint is buried on almost every frame
            // of almost every scenario, so this is not a rare case.
            //
            // A trace that starts inside a brush cannot say which way is out.
            // So the way out is taken from the body instead: the joint this one
            // hangs from is nearly always in open space, and it is both a
            // direction that certainly exists and a short distance away. The
            // move is small and is made without velocity, so a buried limb
            // walks itself out over a few steps rather than being flung, which
            // is what the old comment here was rightly afraid of.
            const int toward = rd_joints[i].parent >= 0 ? rd_joints[i].parent : RD_SPINE;
            trace_t   out;

            VectorCopy(part->pPrev, part->p);
            VectorClear(part->v);
            rd->buriedMask |= 1 << i;

            // Traced from the joint this one hangs from, back toward it. That
            // joint is nearly always in open space, so the trace starts outside
            // the brush and stops on the face the particle is behind, which is
            // the one place it can be put that is both out of the solid and
            // still where the limb should be. Shoving it a fixed distance each
            // step instead does get it out, and costs the corpse its stillness:
            // residual movement over the real deaths went up five times.
            //
            // If the parent is buried too the trace starts solid as well and
            // nothing is done this step. The chain unburies from the body
            // outward over the next few, which is the right order anyway.
            CG_Trace(
                &out, rd->part[toward].p, rd_mins, rd_maxs, part->p, skipEntity, mask, qfalse, qtrue, "CG_RagdollUnbury"
            );

            if (!out.startsolid && !out.allsolid && out.fraction < 1.0f) {
                VectorCopy(out.endpos, part->p);
                VectorMA(part->p, RD_SURFACE_GAP, out.plane.normal, part->p);

                // Placed, not thrown.
                VectorClear(part->v);
            }

            moved++;
            continue;
        }

        if (trace.fraction >= 1.0f) {
            continue;
        }

        VectorCopy(trace.endpos, part->p);
        VectorMA(part->p, RD_SURFACE_GAP, trace.plane.normal, part->p);

        part->hasContact  = qtrue;
        part->contactTime = cg.time;
        VectorCopy(trace.plane.normal, part->contactNormal);
        part->contactDist = DotProduct(part->p, part->contactNormal);

        // Split the velocity across the surface and reflect it there. This
        // used to be done by moving pPrev, which said the same thing in a way
        // that could not be told apart from the constraint solver's corrections.
        VectorCopy(part->v, v);
        dot = DotProduct(v, trace.plane.normal);
        VectorScale(trace.plane.normal, dot, vn);
        VectorSubtract(v, vn, vt);

        if (trace.plane.normal[2] > 0.7f) {
            part->onGround = qtrue;

            // Static friction, so settled corpses stop creeping downhill.
            if (VectorLength(vt) < RD_STATIC_FRICTION) {
                VectorClear(vt);
            }
        }

        for (k = 0; k < 3; k++) {
            part->v[k] = vt[k] * (1.0f - friction) - vn[k] * bounce;
        }

        // Only a displacement worth reconciling counts. A corpse that has come
        // to rest still registers a contact every single step, and treating
        // that as an impact would double the solver cost forever.
        VectorSubtract(part->p, before, v);
        if (VectorLengthSquared(v) > RD_IMPACT_EPSILON * RD_IMPACT_EPSILON) {
            moved++;
        }
    }

    return moved;
}

// How many joints are resting on a surface that could hold the body up. A
// corpse draped over a ledge with only its middle touching is not supported,
// however slowly it happens to be moving at that instant.
static int CG_RagdollSupportCount(const cg_ragdoll_t *rd)
{
    int n = 0;
    int i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        // The remembered contact rather than onGround: onGround is only set on
        // a step where the trace actually strikes something, so a joint that
        // has already come to rest keeps dropping in and out of it, and the
        // body would flicker between supported and unsupported while lying
        // perfectly still.
        if (rd->part[i].hasContact && rd->part[i].contactNormal[2] > 0.7f) {
            n++;
        }
    }

    return n;
}

// The one thing a step must not end with: a particle inside the world.
//
// The full solve knows only about planes already remembered, so a limb meeting
// a wall for the first time is pushed straight into it and the step ends there.
// Running the whole collision pass again to fix that costs the corpse its
// stillness, because it is a fresh impact as far as friction and bounce are
// concerned. This is only the part that has to happen: anything now inside
// geometry is put back on the surface, by the same route out that
// CG_RagdollCollide uses, and nothing else is touched.
static void CG_RagdollPushOut(cg_ragdoll_t *rd, int skipEntity)
{
    const vec3_t rd_mins = {-rd->radius, -rd->radius, -rd->radius};
    const vec3_t rd_maxs = {rd->radius, rd->radius, rd->radius};
    int          i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];
        const int     toward = rd_joints[i].parent >= 0 ? rd_joints[i].parent : RD_SPINE;
        trace_t       probe, out;
        vec3_t        shift;

        CG_Trace(&probe, part->p, rd_mins, rd_maxs, part->p, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollPushOut");

        if (!probe.startsolid && !probe.allsolid) {
            continue;
        }

        rd->buriedMask |= 1 << i;

        CG_Trace(
            &out, rd->part[toward].p, rd_mins, rd_maxs, part->p, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollPushOut"
        );

        if (out.startsolid || out.allsolid || out.fraction >= 1.0f) {
            vec3_t above;

            // The joint it hangs from is buried too, so there is no way out
            // along the body: this is a corpse that has gone into the world
            // whole rather than caught a limb on something, and left alone it
            // sinks, because the solve drags it a little further down every
            // step and nothing can lift it. Try straight up instead, which is
            // where the surface is when a body has gone through a floor.
            VectorCopy(part->p, above);
            above[2] += rd->radius * RD_UNBURY_REACH;

            CG_Trace(
                &out, above, rd_mins, rd_maxs, part->p, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollPushOut"
            );

            if (out.startsolid || out.allsolid || out.fraction >= 1.0f) {
                continue;
            }
        }

        VectorSubtract(out.endpos, part->p, shift);

        // Only if the way out is near where the particle already is. Being
        // buried means being just inside a surface, so the face it came in
        // through is a step away; a trace from the parent that stops much
        // further off has found some other surface between the two, and moving
        // the particle there is not freeing it but throwing it. Traced in the
        // game: a foot behind a wall from its own calf was put nineteen units
        // away in one step, arrived inside the wall, and was then walked up
        // through it a couple of units at a time for the rest of the corpse's
        // life. Left where it is instead, the ordinary sweep frees it on a
        // later step once the leg has moved.
        if (VectorLength(shift) > rd->radius * RD_UNBURY_REACH) {
            continue;
        }

        VectorMA(shift, RD_SURFACE_GAP, out.plane.normal, shift);

        // Carried on the previous position too, so the correction changes where
        // the particle is without changing how fast it is going. This is a
        // tidying up, not an impact.
        VectorAdd(part->p, shift, part->p);
        VectorAdd(part->pPrev, shift, part->pPrev);
    }
}

static float CG_RagdollStep(cg_ragdoll_t *rd, int skipEntity, float dt)
{
    const float damping = 1.0f - cg_ragdoll_damping->value;
    const float gravity = CG_RagdollGravity();
    float       maxDisp = 0.0f;
    int         i, k;

    // The shot's push is a velocity, and is now simply handed over as one.
    if (rd->hasImpulse) {
        vec3_t whole, local;

        VectorScale(rd->impulseVel, 1.0f - RD_IMPULSE_LOCAL, whole);
        VectorScale(rd->impulseVel, RD_IMPULSE_LOCAL, local);

        for (i = 0; i < RD_NUM_JOINTS; i++) {
            VectorAdd(rd->part[i].v, whole, rd->part[i].v);
        }

        VectorAdd(rd->part[rd->impulseJoint].v, local, rd->part[rd->impulseJoint].v);

        rd->hasImpulse = qfalse;
    }

    // The blast, given to each particle separately and along its own line from
    // the explosion. Falling off with distance is what makes it turn a body
    // over: the near side is pushed harder than the far side, and the
    // difference between the two is a rotation. Handed over as one velocity for
    // the whole body, as the shot's push is, it would only slide the corpse.
    if (rd->hasBlast) {
        for (i = 0; i < RD_NUM_JOINTS; i++) {
            vec3_t away;
            float  dist;

            VectorSubtract(rd->part[i].p, rd->blastPos, away);
            dist = VectorNormalize(away);

            if (dist >= rd->blastRadius) {
                continue;
            }

            // Right on top of it there is no direction to be thrown in, so it
            // goes upward rather than nowhere or somewhere arbitrary.
            if (dist < 1.0f) {
                VectorSet(away, 0.0f, 0.0f, 1.0f);
            }

            VectorMA(rd->part[i].v, rd->blastSpeed * (1.0f - dist / rd->blastRadius), away, rd->part[i].v);
        }

        rd->hasBlast = qfalse;
    }

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];

        VectorScale(part->v, damping, part->v);
        part->v[2] -= gravity * dt;

        VectorCopy(part->p, part->pPrev);
        VectorMA(part->p, dt, part->v, part->p);
        VectorClear(part->solved);
    }

    // Relax the skeleton first, against the contact planes carried over from
    // the previous step, then trace. Tracing the constrained position rather
    // than the merely predicted one is what makes tunnelling impossible: a
    // particle the constraints dragged into a surface is swept from a position
    // that was outside it, so the trace always catches it.
    CG_RagdollSolveTracked(rd, cg_ragdoll_iterations->integer);

    // Collision resolution moves particles with no regard for what they are
    // attached to, so if it had to move any, relax once more against the
    // planes it just found. Without this the emitted pose keeps whatever
    // violation the impact introduced, which reads as limbs stretching on
    // impact and as a corpse that creeps instead of settling.
    if (CG_RagdollCollide(rd, skipEntity)) {
        CG_RagdollSolveTracked(rd, cg_ragdoll_iterations->integer);
        CG_RagdollPushOut(rd, skipEntity);
    }

    // Every constraint correction is a position change, and in a Verlet
    // integrator a position change is indistinguishable from velocity. A body
    // resting on the floor is exactly where the constraints disagree most, so
    // without this the corpse is fed a trickle of energy forever and writhes
    // instead of settling.
    //
    // It is applied only once enough of the body is actually resting on
    // something. Damping a corpse that is still going over a ledge, or still
    // coming down a staircase, robs it of the motion it should be carrying and
    // leaves it stuck partway.
    const qboolean supported  = (CG_RagdollSupportCount(rd) >= RD_MIN_SUPPORT) ? qtrue : qfalse;
    const float    solveGain = cg_ragdoll_solvegain->value;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];
        vec3_t        d;
        float         len;

        // How much of what the solver moved is allowed to count as motion. At
        // one this is an ordinary position based solver and behaves as the
        // Verlet one did; below it, a correction moves the body without also
        // pushing it.
        if (solveGain > 0.0f) {
            VectorMA(part->v, solveGain / dt, part->solved, part->v);
        }

        VectorSubtract(part->p, part->pPrev, d);
        len = VectorLength(d);

        if (part->hasContact && supported) {
            if (len < RD_REST_SPEED) {
                VectorClear(part->v);
                len = 0.0f;
            } else {
                VectorScale(part->v, RD_GROUND_DAMPING, part->v);
                len *= RD_GROUND_DAMPING;
            }
        }

        if (len > maxDisp) {
            maxDisp = len;
        }
    }

    return maxDisp;
}

//=============================================================
// Pose reconstruction and blending
//=============================================================

// Turns the simulated particle positions back into model-space bone matrices,
// cross-fading with the animation pose while the blend window is open.
// Turns each spine bone part of the way towards the orientation halfway between
// its neighbours, which shares a bend out along the back instead of leaving it
// all at one joint. Only the drawn rotation is touched; the simulation is not
// affected at all.
// Records the curve the back has at the moment of death: for each bone of the
// spine that gets smoothed, how it sits relative to the average of the two
// either side of it. Without this the smoothing has nothing to aim at but a
// straight back.
static void CG_RagdollSeedSpineCurve(cg_ragdoll_t *rd)
{
    int n, j, k;

    rd->spineRestValid = qfalse;

    for (n = 1; n + 1 < RD_NUM_SPINE_CHAIN; n++) {
        const int prev = rd_spineChain[n - 1];
        const int cur  = rd_spineChain[n];
        const int next = rd_spineChain[n + 1];
        quat_t    qPrev, qNext, qMid;
        float     m[3][3];
        vec3_t    mid[3];

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                rd->spineRest[n][j][k] = (j == k) ? 1.0f : 0.0f;
            }
        }

        if (rd->boneIndex[prev] < 0 || rd->boneIndex[cur] < 0 || rd->boneIndex[next] < 0) {
            continue;
        }

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                m[j][k] = rd->boneAxis[prev][j][k];
            }
        }
        MatToQuat(m, qPrev);

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                m[j][k] = rd->boneAxis[next][j][k];
            }
        }
        MatToQuat(m, qNext);

        SlerpQuaternion(qPrev, qNext, 0.5f, qMid);
        QuatToMat(qMid, m);

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                mid[j][k] = m[j][k];
            }
        }

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                rd->spineRest[n][j][k] = DotProduct(rd->boneAxis[cur][j], mid[k]);
            }
        }
    }

    rd->spineRestValid = qtrue;
}


// Puts every bone back to the twist about its own length that its anatomy had
// at the moment of death.
//
// Turning a bone about its own length moves no particle, so the simulation
// holds no opinion about it whatsoever, and every degree of it in the drawn
// pose is invented. It is invented in quantity, too. The frames the solver
// builds carry a parent's roll onto its child exactly, measured at flat zero on
// every frame of every animation; but what gets drawn is that frame turned by a
// fixed per bone correction, and a parent's correction and its child's are
// different rotations. Composing the two leaves a twist that grows as the joint
// between them bends, and a foot reached forty degrees of it as its ankle
// turned, on frames where the construction twist behind it was zero.
//
// So it is taken out completely rather than merely limited: the alternative to
// the twist the body died with is not some other twist the physics asked for,
// it is arithmetic.
//
// This runs as its own pass, after the spine smoothing rather than woven into
// the pass that builds the orientations. Corrected in place, each bone would be
// measured against a parent that had already been moved, and against a spine
// that had not yet been smoothed: the reference would shift under every bone in
// turn and the chain would chase itself down the body.
static void CG_RagdollUntwist(cg_ragdoll_t *rd)
{
    const qboolean seeding = !rd->twistRestValid;
    int            i;

    for (i = 0; i < RD_NUM_BONES; i++) {
        const int par = rd_bones[i].parent;
        float     off;
        vec3_t    y, z;

        if (rd->boneIndex[i] < 0) {
            continue;
        }

        AxisCopy(rd->boneAxis[i], rd->rolledAxis[i]);

        if (par < 0 || rd->boneIndex[par] < 0) {
            continue;
        }

        // A bone with a geometric roll reference is left alone. Its roll has
        // already been decided, by the plane the knee or elbow below it bends
        // in, and that is a real measurement off the particles. Taking it again
        // against the parent overrides physics with arithmetic.
        //
        // Worse, the arithmetic is undefined for precisely these bones. The
        // twist between two bones is found by carrying the parent's roll onto
        // the child along the shortest rotation between their directions, and
        // when the two run exactly opposite there is no shortest rotation:
        // every axis across the parent turns one onto the other, and each gives
        // an answer half a turn from the last. A thigh runs down the leg while
        // the pelvis runs up the spine, so a body lying with its legs out has
        // both thighs sitting exactly on that singularity. Traced there, the
        // measurement reads minus ninety degrees on one frame and plus ninety
        // on the next with nothing moving at all, and at full rate that is the
        // drawn leg turning inside out at the hip.
        //
        // What this pass was written for is the chain below, where roll is
        // carried from bone to bone and the composition of one correction with
        // the next leaves a twist nothing asked for.
        if (rd_bones[i].helperA >= 0) {
            continue;
        }

        // Bones are ordered parents first, so the parent is already final and
        // what is measured here is what will be drawn.
        off = CG_RagdollTwistBetween(
            rd->boneAxis[par][0], rd->boneAxis[par][1], rd->boneAxis[i][0], rd->boneAxis[i][1]
        );

        // Taken on the first pose the ragdoll draws, which is the death pose
        // itself, and taken here rather than back in the seed. Measured in the
        // seed it came out a quarter turn away from what this pass measures for
        // the very same bones, and every bone was then turned by that ninety
        // degrees on the first frame. Reading it where it is used cannot
        // disagree with itself.
        if (seeding) {
            rd->twistRest[i] = off;
            continue;
        }

        off -= rd->twistRest[i];

        while (off > 180.0f) {
            off -= 360.0f;
        }
        while (off < -180.0f) {
            off += 360.0f;
        }

        if (off < 0.01f && off > -0.01f) {
            continue;
        }

        // Eased in rather than applied whole. A bone turned about its own
        // length carries everything hanging below it around with it, so a
        // correction that jumps from frame to frame shows up as the whole limb
        // shivering even though no particle has moved.
        RotatePointAroundVector(y, rd->boneAxis[i][0], rd->boneAxis[i][1], -off * RD_UNTWIST_RATE);
        RotatePointAroundVector(z, rd->boneAxis[i][0], rd->boneAxis[i][2], -off * RD_UNTWIST_RATE);
        VectorCopy(y, rd->boneAxis[i][1]);
        VectorCopy(z, rd->boneAxis[i][2]);
    }

    rd->twistRestValid = qtrue;
}

static void CG_RagdollSmoothSpine(cg_ragdoll_t *rd)
{
    vec3_t smoothed[RD_NUM_SPINE_CHAIN][3];
    int    n, j, k;

    for (n = 0; n < RD_NUM_SPINE_CHAIN; n++) {
        AxisCopy(rd->boneAxis[rd_spineChain[n]], smoothed[n]);
    }

    for (n = 1; n + 1 < RD_NUM_SPINE_CHAIN; n++) {
        const int prev = rd_spineChain[n - 1];
        const int cur  = rd_spineChain[n];
        const int next = rd_spineChain[n + 1];
        quat_t    qPrev, qNext, qCur, qMid, qOut;
        float     m[3][3];

        if (rd->boneIndex[prev] < 0 || rd->boneIndex[cur] < 0 || rd->boneIndex[next] < 0) {
            continue;
        }

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                m[j][k] = rd->boneAxis[prev][j][k];
            }
        }
        MatToQuat(m, qPrev);

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                m[j][k] = rd->boneAxis[next][j][k];
            }
        }
        MatToQuat(m, qNext);

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                m[j][k] = rd->boneAxis[cur][j][k];
            }
        }
        MatToQuat(m, qCur);

        SlerpQuaternion(qPrev, qNext, 0.5f, qMid);
        QuatToMat(qMid, m);

        // The average of the two neighbours is a straight back, and pulled
        // toward it a spine loses whatever curve it had. A death animation's
        // spine is curved a great deal, so the corpse's back was being
        // straightened out the instant the ragdoll took over: better than
        // thirty degrees of it on the pelvis alone, on every corpse, as a jump
        // at the moment of death. Aiming instead at that average put back into
        // the shape the body died in, what the smoothing takes out is only what
        // has been added since.
        if (rd->spineRestValid) {
            vec3_t mid[3], want[3];

            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    mid[j][k] = m[j][k];
                }
            }

            for (j = 0; j < 3; j++) {
                VectorClear(want[j]);
                for (k = 0; k < 3; k++) {
                    VectorMA(want[j], rd->spineRest[n][j][k], mid[k], want[j]);
                }
            }

            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    m[j][k] = want[j][k];
                }
            }

            MatToQuat(m, qMid);
        }

        SlerpQuaternion(qCur, qMid, RD_SPINE_SMOOTH_RATE, qOut);
        QuatToMat(qOut, m);

        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                smoothed[n][j][k] = m[j][k];
            }
        }

        // Slerping whole orientations toward the neighbour average blends the
        // roll along with the bend, and the bones of the back do not agree
        // about roll, so the average of them turns the drawn chest away from
        // where the shoulders actually are. That is where the twist in the
        // drawn torso comes from: the particles carry none of it. Only the bend
        // is wanted here, so the bone's own roll is put back afterwards.
        {
            const float roll =
                CG_RagdollTwistBetween(rd->boneAxis[cur][0], rd->boneAxis[cur][1], smoothed[n][0], smoothed[n][1]);

            if (roll > 0.01f || roll < -0.01f) {
                vec3_t ry, rz;

                RotatePointAroundVector(ry, smoothed[n][0], smoothed[n][1], -roll);
                RotatePointAroundVector(rz, smoothed[n][0], smoothed[n][2], -roll);
                VectorCopy(ry, smoothed[n][1]);
                VectorCopy(rz, smoothed[n][2]);
            }
        }
    }

    for (n = 0; n < RD_NUM_SPINE_CHAIN; n++) {
        AxisCopy(smoothed[n], rd->boneAxis[rd_spineChain[n]]);
    }
}

static void CG_RagdollBuildPose(cg_ragdoll_t *rd, refEntity_t *model, float weight)
{
    int i, j, k;

    rd->numOverrides = 0;

    // First pass: work out every bone's orientation.
    for (i = 0; i < RD_NUM_BONES; i++) {
        vec3_t         frame[3];
        vec3_t         worldAxis[3];
        const vec3_t  *animAxis = rd_animPose[i].axis;
        const qboolean haveAnim = rd_animPose[i].valid;

        if (rd->boneIndex[i] < 0) {
            continue;
        }

        //
        // Orientation, from the simulated particles
        //
        if (CG_RagdollBoneFrame(rd, i, frame)) {
            for (j = 0; j < 3; j++) {
                VectorClear(worldAxis[j]);
                for (k = 0; k < 3; k++) {
                    VectorMA(worldAxis[j], rd->correction[i][j][k], frame[k], worldAxis[j]);
                }
            }
        } else if (haveAnim) {
            AxisCopy(animAxis, worldAxis);
        } else {
            AxisCopy(rd->boneAxis[i], worldAxis);
        }

        // Cross fade out of the death animation. Only the rotation is blended:
        // the position follows from it below, so the skeleton stays rigid all
        // the way through the blend.
        if (haveAnim && weight < 1.0f) {
            quat_t qAnim, qRag, qOut;
            float  m[3][3];

            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    m[j][k] = animAxis[j][k];
                }
            }
            MatToQuat(m, qAnim);

            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    m[j][k] = worldAxis[j][k];
                }
            }
            MatToQuat(m, qRag);

            // Slerp, never a component-wise lerp: the average of two rotation
            // matrices is not a rotation and shears the skinned mesh.
            SlerpQuaternion(qAnim, qRag, weight, qOut);
            QuatToMat(qOut, m);

            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    worldAxis[j][k] = m[j][k];
                }
            }
        }

        AxisCopy(worldAxis, rd->boneAxis[i]);
    }

    // Ease the rotation along the spine before anything is positioned.
    //
    // The back is skinned with linear blending, which shrinks the mesh across a
    // joint in proportion to how far the two bones either side of it have
    // turned apart. Spread a given curve along the spine and it looks right;
    // concentrate it at one joint and the waist nips in to an hourglass. The
    // physics cannot really be blamed for putting it there, since a body lying
    // on uneven ground genuinely bends where the ground pushes, and forcing the
    // spine straight enough to hide it only makes the corpse look rigid.
    //
    // So this is corrected where the fault actually is, in what gets drawn:
    // each spine bone is turned part way towards the average of its neighbours,
    // which keeps the overall shape of the back while sharing the bend out
    // between the joints.
    for (i = 0; i < RD_SPINE_SMOOTH_PASSES; i++) {
        // Taken on the first pose the ragdoll builds, which is the death pose
        // itself: the smoothing has not run yet, so this is the back as the
        // animation left it.
        if (!rd->spineRestValid) {
            CG_RagdollSeedSpineCurve(rd);
        }

        CG_RagdollSmoothSpine(rd);
    }

    CG_RagdollUntwist(rd);

    // Second pass: hang the bones off one another using those orientations.
    for (i = 0; i < RD_NUM_BONES; i++) {
        const rdBoneDef_t *def = &rd_bones[i];
        boneOverride_t    *out = &rd->overrides[rd->numOverrides];
        vec3_t             worldPos;
        const float       *animPos  = rd_animPose[i].pos;
        const qboolean     haveAnim = rd_animPose[i].valid;

        if (rd->boneIndex[i] < 0) {
            continue;
        }

        //
        // Position, from the parent rather than from this bone's own particle
        //
        // Taking the translation straight from the particle is what allowed
        // limbs to stretch: any residual the solver left, and any difference
        // between the animation and the simulation during the blend, showed up
        // directly as a change in bone length. Rebuilding each bone from its
        // parent using the offset captured at seed time makes every bone length
        // exactly right by construction instead.
        //
        if (def->parent >= 0 && rd->boneIndex[def->parent] >= 0) {
            VectorCopy(rd->bonePos[def->parent], worldPos);

            // Hung off the orientation the parent had before its twist was
            // taken out. Turning a bone about its own length is a correction to
            // how that bone is drawn and nothing more; run through the chain of
            // positions as well, it drags everything below it sideways, and a
            // settled corpse that has not moved a particle visibly shivers.
            for (k = 0; k < 3; k++) {
                VectorMA(worldPos, rd->localOffset[i][k], rd->rolledAxis[def->parent][k], worldPos);
            }
        } else {
            // The root is the one bone that is positioned directly, so the
            // whole body still goes where the simulation puts it.
            VectorCopy(rd->part[def->joint].p, worldPos);

            if (haveAnim && weight < 1.0f) {
                for (k = 0; k < 3; k++) {
                    worldPos[k] = animPos[k] + (worldPos[k] - animPos[k]) * weight;
                }
            }
        }

        VectorCopy(worldPos, rd->bonePos[i]);

        out->boneIndex = rd->boneIndex[i];
        rd->numOverrides++;
    }

    // The rebuilt skeleton hangs off the root, so whatever the solver failed to
    // resolve accumulates in one direction as the chain is walked, and the far
    // end of the body can finish a little way off where its particles actually
    // are. Since the particles are the things that were collided against, that
    // shows up as a hand or a foot dipped into the floor. Shifting the whole
    // skeleton by the average error spreads it either side of the particles
    // instead of letting it pile up at the extremities.
    {
        vec3_t drift;
        int    counted = 0;

        VectorClear(drift);

        for (i = 0; i < RD_NUM_BONES; i++) {
            if (rd->boneIndex[i] < 0 || !CG_RagdollBoneOwnsJoint(i)) {
                continue;
            }

            VectorAdd(drift, rd->part[rd_bones[i].joint].p, drift);
            VectorSubtract(drift, rd->bonePos[i], drift);
            counted++;
        }

        if (counted) {
            VectorScale(drift, 1.0f / (float)counted, drift);

            rd->numOverrides = 0;

            for (i = 0; i < RD_NUM_BONES; i++) {
                boneOverride_t *out = &rd->overrides[rd->numOverrides];

                if (rd->boneIndex[i] < 0) {
                    continue;
                }

                VectorAdd(rd->bonePos[i], drift, rd->bonePos[i]);

                out->boneIndex = rd->boneIndex[i];
                CG_RagdollWriteBoneModel(model, rd->bonePos[i], rd->boneAxis[i], out->matrix);
                rd->numOverrides++;
            }
        }
    }
}

// While the ragdoll blends in, drag the particles toward the animation pose so
// the simulation starts from the death animation's own motion instead of a
// guessed impulse.
static void CG_RagdollDriveFromAnim(cg_ragdoll_t *rd, float weight)
{
    int i;

    if (weight >= 1.0f) {
        return;
    }

    for (i = 0; i < RD_NUM_BONES; i++) {
        vec3_t delta;
        int    joint;
        int    k;

        if (rd->boneIndex[i] < 0 || !rd_animPose[i].valid) {
            continue;
        }

        if (!CG_RagdollBoneOwnsJoint(i)) {
            continue;
        }

        joint = rd_bones[i].joint;

        // Move the particle and its previous position together, so the
        // animation's velocity is accumulated rather than cancelled.
        for (k = 0; k < 3; k++) {
            delta[k] = (rd_animPose[i].pos[k] - rd->part[joint].p[k]) * (1.0f - weight);
        }

        VectorAdd(rd->part[joint].p, delta, rd->part[joint].p);
        VectorAdd(rd->part[joint].pPrev, delta, rd->part[joint].pPrev);
    }
}

// The debug lines go into a buffer that only the game module ever empties, once
// per server frame, and this runs once per rendered frame. At any decent frame
// rate that is several times faster than it is drained, so the buffer fills,
// and from then on every further line costs a console print: the renderer
// complains about MAX_DEBUG_LINES hundreds of times a frame and the game
// crawls. Emitting only when the server time has moved on matches the rate the
// lines are produced to the rate they are cleared, and the lines stay on screen
// in between because they are still sitting in the buffer.
static int rd_debugServerTime = -1;
static int rd_debugFrameTime  = -1;

static qboolean CG_RagdollDebugReady(void)
{
    if (!cg.snap) {
        return qfalse;
    }

    // Every corpse drawn in the same rendered frame gets to emit; it is the
    // next frame that has to wait for the buffer to be drained.
    if (cg.time == rd_debugFrameTime) {
        return qtrue;
    }

    if (cg.snap->serverTime == rd_debugServerTime) {
        return qfalse;
    }

    rd_debugServerTime = cg.snap->serverTime;
    rd_debugFrameTime  = cg.time;

    return qtrue;
}

static void CG_RagdollDebugDraw(const cg_ragdoll_t *rd)
{
    int i;

    // Level 2 adds the constraint web, which is by far the greater part of the
    // lines and is rarely what you want to look at.
    if (cg_ragdoll_debug->integer >= 2) {
        for (i = 0; i < rd->numConstraints; i++) {
            const rdConstraint_t *c = &rd->constraint[i];

            cgi.R_DebugLine(rd->part[c->a].p, rd->part[c->b].p, 0.2f, 1.0f, 0.2f, 1.0f);
        }
    }

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        vec3_t a, b;

        VectorCopy(rd->part[i].p, a);
        VectorCopy(rd->part[i].p, b);
        a[2] -= 1.5f;
        b[2] += 1.5f;

        if (rd->part[i].onGround) {
            cgi.R_DebugLine(a, b, 1.0f, 0.4f, 0.0f, 1.0f);
        } else {
            cgi.R_DebugLine(a, b, 1.0f, 1.0f, 0.0f, 1.0f);
        }
    }
}

//=============================================================
// Lifecycle
//=============================================================

// Traces are numbered rather than sharing one name. Set cg_ragdoll_dump to more
// than one and that many bodies are caught, each to its own file, because the
// body worth looking at is often not the next one to fall: a corpse left
// hanging on a wall was photographed while the trace ran on a different man who
// died correctly on the floor a few yards away.
static int rd_dumpSeq;

// Writes one line, whatever its length, without needing the caller to know how
// long it came out.
static void CG_RagdollDumpLine(cg_ragdoll_t *rd, const char *line)
{
    if (!rd->dumpFile) {
        return;
    }

    cgi.FS_Write(line, strlen(line), rd->dumpFile);
}

// Starts a trace of this corpse. The header names every joint and every drawn
// bone, so the file can be read without a copy of this table to hand.
static void CG_RagdollDumpOpen(cg_ragdoll_t *rd, refEntity_t *model)
{
    // Comfortably over the header block below, which is written in one go and
    // was silently truncated mid-word when it outgrew a smaller buffer.
    char line[1024];
    int  i;

    Com_sprintf(rd->dumpName, sizeof(rd->dumpName), "ragdoll_dump_%d.txt", ++rd_dumpSeq);

    rd->dumpFile = cgi.FS_FOpenFileWrite(rd->dumpName);

    if (!rd->dumpFile) {
        cgi.Printf("ragdoll: could not open %s for writing\n", rd->dumpName);
        return;
    }

    Com_sprintf(
        line,
        sizeof(line),
        "# openmohaa ragdoll trace v1\n"
        "# entity %d  bodyscale %.4f\n"
        // Every tuning value that changes what this body does. Read them before
        // reading anything into the trace: six corpses once looked like they
        // could not fall asleep, and the answer was cg_ragdoll_sleepvel set to
        // zero, which switches sleeping off by design.
        "# blendtime %d  impulse %.2f  blastimpulse %.2f  stiffness %.2f  limptime %d  solvegain %.2f\n"
        "# sleepvel %.3f  sleeptime %d  duration %d  gravity %.1f\n"
        "# F <time_ms> <blendweight> <state> <supports> <maxdisp> <steps> <quiet_ms>\n"
        "# E <x> <y> <z>   entity origin, which the drawn corpse rides once asleep\n"
        "# C <contact> <onground> <buried>   bitmasks over the joints, low bit joint 0\n"
        "# P <joint> <x> <y> <z>\n"
        "# A <bone> <x> <y> <z> <ax ay az bx by bz cx cy cz>   animation pose\n"
        "# B <bone> <x> <y> <z> <ax ay az bx by bz cx cy cz>   drawn pose\n",
        rd->entityNum,
        CG_RagdollModelScale(model),
        cg_ragdoll_blendtime->integer,
        cg_ragdoll_impulse->value,
        cg_ragdoll_blastimpulse->value,
        cg_ragdoll_stiffness->value,
        cg_ragdoll_limptime->integer,
        cg_ragdoll_solvegain->value,
        cg_ragdoll_sleepvel->value,
        cg_ragdoll_sleeptime->integer,
        cg_ragdoll_duration->integer,
        CG_RagdollGravity()
    );
    CG_RagdollDumpLine(rd, line);

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        Com_sprintf(line, sizeof(line), "# joint %d %s\n", i, rd_joints[i].boneName ? rd_joints[i].boneName : rd_joints[i].tipName);
        CG_RagdollDumpLine(rd, line);
    }

    for (i = 0; i < RD_NUM_BONES; i++) {
        // The parent goes in the file too, so a reader never has to keep its
        // own copy of this table in step with the one above.
        Com_sprintf(
            line,
            sizeof(line),
            "# bone %d parent %d joint %d aim %d %s%s\n",
            i,
            rd_bones[i].parent,
            rd_bones[i].joint,
            rd_bones[i].aim,
            rd_bones[i].boneName,
            rd->boneIndex[i] < 0 ? " (absent)" : ""
        );
        CG_RagdollDumpLine(rd, line);
    }
}

// The entity alone, once the body has stopped being solved. Written every frame
// the corpse is asleep, which is the window the pose can no longer explain.
static void CG_RagdollDumpSleeping(cg_ragdoll_t *rd)
{
    char line[256];

    if (!rd->dumpFile) {
        return;
    }

    Com_sprintf(
        line,
        sizeof(line),
        "F %d 1.0000 %d %d 0.0000 0 0\nE %.3f %.3f %.3f\n",
        cg.time,
        (int)rd->state,
        CG_RagdollSupportCount(rd),
        rd->entOrigin[0],
        rd->entOrigin[1],
        rd->entOrigin[2]
    );
    CG_RagdollDumpLine(rd, line);
}

static void CG_RagdollDumpClose(cg_ragdoll_t *rd)
{
    if (!rd->dumpFile) {
        return;
    }

    cgi.FS_FCloseFile(rd->dumpFile);
    rd->dumpFile = 0;
    cgi.Printf("ragdoll: trace written to %s\n", rd->dumpName);
}

// One frame of the corpse: where the simulation has its particles, what the
// animation would have drawn, and what was actually drawn. Having all three
// side by side is the point. A bone that moves while its particles do not is a
// reconstruction fault, and there is no other way to tell that apart from the
// physics having genuinely moved something.
static void CG_RagdollDumpFrame(cg_ragdoll_t *rd, float weight)
{
    char line[512];
    int  i;

    if (!rd->dumpFile) {
        return;
    }

    Com_sprintf(
        line,
        sizeof(line),
        "F %d %.4f %d %d %.4f %d %d\n",
        cg.time,
        weight,
        (int)rd->state,
        CG_RagdollSupportCount(rd),
        rd->lastDisp,
        rd->lastSteps,
        cg.time - rd->quietSince
    );
    CG_RagdollDumpLine(rd, line);

    Com_sprintf(line, sizeof(line), "E %.3f %.3f %.3f\n", rd->entOrigin[0], rd->entOrigin[1], rd->entOrigin[2]);
    CG_RagdollDumpLine(rd, line);

    {
        int contact = 0, ground = 0;

        for (i = 0; i < RD_NUM_JOINTS; i++) {
            if (rd->part[i].hasContact) {
                contact |= 1 << i;
            }
            if (rd->part[i].onGround) {
                ground |= 1 << i;
            }
        }

        Com_sprintf(line, sizeof(line), "C %d %d %d\n", contact, ground, rd->buriedMask);
        CG_RagdollDumpLine(rd, line);
    }

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        Com_sprintf(line, sizeof(line), "P %d %.3f %.3f %.3f\n", i, rd->part[i].p[0], rd->part[i].p[1], rd->part[i].p[2]);
        CG_RagdollDumpLine(rd, line);
    }

    for (i = 0; i < RD_NUM_BONES; i++) {
        if (rd->boneIndex[i] < 0) {
            continue;
        }

        if (rd_animPose[i].valid) {
            Com_sprintf(
                line,
                sizeof(line),
                "A %d %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                i,
                rd_animPose[i].pos[0], rd_animPose[i].pos[1], rd_animPose[i].pos[2],
                rd_animPose[i].axis[0][0], rd_animPose[i].axis[0][1], rd_animPose[i].axis[0][2],
                rd_animPose[i].axis[1][0], rd_animPose[i].axis[1][1], rd_animPose[i].axis[1][2],
                rd_animPose[i].axis[2][0], rd_animPose[i].axis[2][1], rd_animPose[i].axis[2][2]
            );
            CG_RagdollDumpLine(rd, line);
        }

        Com_sprintf(
            line,
            sizeof(line),
            "B %d %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
            i,
            rd->bonePos[i][0], rd->bonePos[i][1], rd->bonePos[i][2],
            rd->boneAxis[i][0][0], rd->boneAxis[i][0][1], rd->boneAxis[i][0][2],
            rd->boneAxis[i][1][0], rd->boneAxis[i][1][1], rd->boneAxis[i][1][2],
            rd->boneAxis[i][2][0], rd->boneAxis[i][2][1], rd->boneAxis[i][2][2]
        );
        CG_RagdollDumpLine(rd, line);
    }
}

static void CG_RagdollFree(cg_ragdoll_t *rd)
{
    CG_RagdollDumpClose(rd);
    memset(rd, 0, sizeof(*rd));
    rd->state     = RD_FREE;
    rd->entityNum = ENTITYNUM_NONE;
}

void CG_InitRagdoll(void)
{
    rd_debugServerTime = -1;
    rd_debugFrameTime  = -1;

    int i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        CG_RagdollFree(&cg_ragdolls[i]);
    }

    // cgs is zeroed on init, so this has to be turned on explicitly: a server
    // that never sends sv_ragdoll must not read as having disabled it.
    cgs.ragdollAllowed = qtrue;

    cg_ragdoll = cgi.Cvar_Get("cg_ragdoll", "1", CVAR_ARCHIVE);
    // How many corpses may be *solving* at once, which is where the cost is.
    // Eight, not the sixteen the pool can hold: every one of them is solved
    // every frame, and what the eye picks up is not one body settling but the
    // several still moving behind it, so doubling this doubles how much of that
    // is on screen together.
    //
    // A corpse that has settled does not count against it. Sleeping skips the
    // step entirely and re-emits the matrices the body came to rest on, so it
    // costs nothing to leave lying there, and it used to be evicted anyway: in
    // a round busy enough for eight bodies to be down at once, every further
    // death threw away the oldest of them while half the pool stood empty, and
    // a corpse that had just come to rest snapped back to the pose its death
    // animation ended on. Settled bodies now stay until the pool is full.
    cg_ragdoll_maxcount  = cgi.Cvar_Get("cg_ragdoll_maxcount", "8", CVAR_ARCHIVE);
    cg_ragdoll_blendtime = cgi.Cvar_Get("cg_ragdoll_blendtime", "200", CVAR_ARCHIVE);
    // Scale on the push the killing shot gives the body. 0 disables it and
    // restores the purely animation-driven fall.
    //
    // 1 is roughly what a rifle round really does to a body. Higher reads more
    // dramatically for a moment and then costs the pose: measured over the real
    // death animations, at 2 the energy has to go somewhere and it goes into
    // the limbs, nearly doubling how far a limb is left hanging unsupported and
    // taking the worst case from five units to twelve. A little is better than
    // none, though, since a body given a small push finds a resting pose
    // instead of landing rigidly in the one it died in.
    cg_ragdoll_impulse   = cgi.Cvar_Get("cg_ragdoll_impulse", "0.75", CVAR_ARCHIVE);
    // Kept apart from cg_ragdoll_impulse, which scales the push a bullet gives.
    // The two want different numbers and were sharing one: raising it far
    // enough for a grenade to throw a body properly also meant every rifle
    // round did, and the energy a shot puts into a corpse goes into its limbs,
    // which is how bodies end up with a leg hanging in the air. Each now has
    // its own scale and either can be turned off on its own.
    cg_ragdoll_blastimpulse = cgi.Cvar_Get("cg_ragdoll_blastimpulse", "1.0", CVAR_ARCHIVE);
    cgi.Cvar_CheckRange(cg_ragdoll_blastimpulse, 0, 10, qfalse);
    cg_ragdoll_duration  = cgi.Cvar_Get("cg_ragdoll_duration", "5000", CVAR_ARCHIVE);
    cgi.Cvar_CheckRange(cg_ragdoll_maxcount, 0, MAX_RAGDOLLS, qtrue);
    cgi.Cvar_CheckRange(cg_ragdoll_blendtime, 0, 2000, qtrue);
    cgi.Cvar_CheckRange(cg_ragdoll_impulse, 0, 10, qfalse);
    cgi.Cvar_CheckRange(cg_ragdoll_duration, 0, 60000, qtrue);

    cg_ragdoll_physicsrate = cgi.Cvar_Get("cg_ragdoll_physicsrate", "60", CVAR_ARCHIVE);
    cg_ragdoll_iterations  = cgi.Cvar_Get("cg_ragdoll_iterations", "10", CVAR_ARCHIVE);
    cg_ragdoll_damping     = cgi.Cvar_Get("cg_ragdoll_damping", "0.02", CVAR_ARCHIVE);
    cg_ragdoll_friction    = cgi.Cvar_Get("cg_ragdoll_friction", "0.55", CVAR_ARCHIVE);
    cg_ragdoll_bounce      = cgi.Cvar_Get("cg_ragdoll_bounce", "0.15", CVAR_ARCHIVE);
    // Corpses used to have to be put to sleep fairly eagerly to hide a fidget
    // that came from the joint limits never quite settling. That is fixed at
    // the source now, so this only has to catch a body that has genuinely
    // stopped, and it saves the simulation cost from then on. Set it to 0 to
    // disable sleeping entirely and let corpses keep settling for their whole
    // lifetime.
    cg_ragdoll_sleepvel  = cgi.Cvar_Get("cg_ragdoll_sleepvel", "0.25", CVAR_ARCHIVE);
    cg_ragdoll_sleeptime = cgi.Cvar_Get("cg_ragdoll_sleeptime", "400", CVAR_ARCHIVE);
    cg_ragdoll_debug     = cgi.Cvar_Get("cg_ragdoll_debug", "0", CVAR_CHEAT);
    // Set it to 1 and the next body to fall writes its whole life to
    // ragdoll_dump.txt in the home path, then clears the cvar again, so one
    // activation gives exactly one corpse. A screenshot shows the pose a defect
    // ended in; this shows how it got there, which for anything that flips,
    // drifts or props itself up is the half that matters. The harness cannot
    // stand in for it: its world is a floor and a box, and what real geometry
    // does to a corpse is precisely what it is missing.
    cg_ragdoll_dump      = cgi.Cvar_Get("cg_ragdoll_dump", "0", CVAR_CHEAT);

    // How much of what the constraint solver moves counts as the body moving.
    //
    // At 1 this is an ordinary position based solver: everything the solve
    // corrects becomes velocity, which is what a Verlet ragdoll does whether it
    // means to or not, and it is why corpses shiver. Lower it and a correction
    // still moves the body, but stops also pushing it.
    cg_ragdoll_solvegain = cgi.Cvar_Get("cg_ragdoll_solvegain", "0.7", CVAR_ARCHIVE);
    cgi.Cvar_CheckRange(cg_ragdoll_solvegain, 0, 1, qfalse);

    // How long a body takes to go limp, in milliseconds. 0 holds the shape
    // memory for ever, which is what it used to do.
    //
    // The soft constraints bias the corpse back toward the pose it died in, and
    // that is wanted for a moment: without it a body lands and sprawls flat.
    // Held indefinitely it is what leaves a corpse rigid enough to balance on
    // its head at the top of a staircase instead of folding down it. What fades
    // is only the bias; every hard limit is taken before it and stays.
    //
    // Left off by default. How limp a corpse should look is a judgement made by
    // eye rather than a thing that can be measured, so this is here to be tried
    // in the game and settled on, not guessed at from the harness.
    cg_ragdoll_limptime  = cgi.Cvar_Get("cg_ragdoll_limptime", "0", CVAR_ARCHIVE);
    cgi.Cvar_CheckRange(cg_ragdoll_limptime, 0, 10000, qtrue);

    // Scales how firmly the soft constraints hold the body toward the shape it
    // died in. Lower is floppier, higher is stiffer. It is exposed because how
    // limp a corpse should look is a judgement made by eye, not something that
    // can be measured.
    cg_ragdoll_stiffness = cgi.Cvar_Get("cg_ragdoll_stiffness", "1.0", CVAR_ARCHIVE);
    cgi.Cvar_CheckRange(cg_ragdoll_stiffness, 0, 4, qfalse);
    cgi.Cvar_CheckRange(cg_ragdoll_physicsrate, 20, 120, qtrue);
    cgi.Cvar_CheckRange(cg_ragdoll_iterations, 1, 16, qtrue);
    cgi.Cvar_CheckRange(cg_ragdoll_damping, 0, 0.5f, qfalse);
    cgi.Cvar_CheckRange(cg_ragdoll_friction, 0, 1, qfalse);
    cgi.Cvar_CheckRange(cg_ragdoll_bounce, 0, 1, qfalse);
}

void CG_ShutdownRagdoll(void)
{
    int i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        CG_RagdollFree(&cg_ragdolls[i]);
    }
}

static cg_ragdoll_t *CG_RagdollForEntity(int entityNum)
{
    int i;

    if (entityNum == ENTITYNUM_NONE) {
        return NULL;
    }

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        if (cg_ragdolls[i].state != RD_FREE && cg_ragdolls[i].entityNum == entityNum) {
            return &cg_ragdolls[i];
        }
    }

    return NULL;
}

// In multiplayer the dying player entity is the corpse until it respawns, at
// which point Player::DeadBody spawns a Body clone at the very same origin and
// hides the player, which drops it out of the snapshot entirely. Let the new
// corpse take over the running simulation so it does not visibly pop back to
// the frozen animation pose.
static cg_ragdoll_t *CG_RagdollAdopt(centity_t *cent, int modelIndex)
{
    int i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        cg_ragdoll_t *rd = &cg_ragdolls[i];
        vec3_t        d;

        if (rd->state == RD_FREE) {
            continue;
        }

        // A ragdoll is up for adoption once its owner is gone. Note that the
        // player entity is not reset when it goes: hideModel() sets
        // SVF_NOCLIENT on the corpse, which simply drops it out of the
        // snapshot, so the loss has to be spotted here rather than relying on
        // CG_ResetEntity firing.
        if (rd->entityNum != ENTITYNUM_NONE && cg_entities[rd->entityNum].currentValid) {
            continue;
        }

        if (rd->entityNum == cent->currentState.number) {
            continue;
        }

        if (rd->modelIndex != modelIndex) {
            continue;
        }

        VectorSubtract(rd->entOrigin, cent->lerpOrigin, d);
        if (VectorLengthSquared(d) > RD_ADOPT_DIST * RD_ADOPT_DIST) {
            continue;
        }

        rd->entityNum = cent->currentState.number;
        return rd;
    }

    return NULL;
}

static cg_ragdoll_t *CG_RagdollAlloc(void)
{
    cg_ragdoll_t *oldest  = NULL;
    int           used    = 0;
    int           solving = 0;
    int           i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        cg_ragdoll_t *rd = &cg_ragdolls[i];
        qboolean      recyclable;

        if (rd->state == RD_FREE) {
            continue;
        }

        used++;

        // What cg_ragdoll_maxcount is for is the cost of solving, and a
        // sleeping corpse is not solved: the whole step is skipped for it and
        // it re-emits the matrices it settled on. Counting it against the
        // budget anyway is what made a busy round throw its corpses away. With
        // enough bots dying, eight bodies would settle, and from then on every
        // new death evicted the oldest of them even though half the pool stood
        // empty, so a corpse that had just come to rest snapped back to the
        // pose its death animation ended on. Only bodies still being solved
        // count now, and settled ones stay until the pool itself is full.
        if (rd->state != RD_SLEEPING) {
            solving++;
        }

        // Only ever recycle a corpse that has finished moving, or one whose
        // entity is already gone. Evicting a body that is still falling is
        // very visible; running out of slots is not.
        recyclable =
            (rd->state == RD_SLEEPING || rd->entityNum == ENTITYNUM_NONE || !cg_entities[rd->entityNum].currentValid)
                ? qtrue
                : qfalse;

        if (recyclable && (!oldest || rd->startTime < oldest->startTime)) {
            oldest = rd;
        }
    }

    if (used < MAX_RAGDOLLS && solving < cg_ragdoll_maxcount->integer) {
        for (i = 0; i < MAX_RAGDOLLS; i++) {
            if (cg_ragdolls[i].state == RD_FREE) {
                return &cg_ragdolls[i];
            }
        }
    }

    if (oldest) {
        CG_RagdollFree(oldest);
        return oldest;
    }

    return NULL;
}

void CG_RagdollEntityReset(centity_t *cent)
{
    cg_ragdoll_t *rd = CG_RagdollForEntity(cent->currentState.number);

    if (!rd) {
        return;
    }

    // Orphan rather than free: in multiplayer this fires on the respawn that
    // also spawns the Body which is about to adopt the simulation.
    rd->entityNum  = ENTITYNUM_NONE;
    rd->orphanTime = cg.time;
}

static void CG_RagdollExpire(void)
{
    int i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        cg_ragdoll_t *rd = &cg_ragdolls[i];
        qboolean      gone;

        if (rd->state == RD_FREE) {
            continue;
        }

        gone = (rd->entityNum == ENTITYNUM_NONE || !cg_entities[rd->entityNum].currentValid) ? qtrue : qfalse;

        if (!gone) {
            rd->orphanTime = cg.time;
            continue;
        }

        // Deliberately generous: a corpse the player walks past and comes back
        // to should still be lying the way it settled rather than snapping
        // back to the frozen death animation. The allocator reclaims slots
        // long before this fires.
        if (cg.time - rd->orphanTime > RD_ORPHAN_MAX) {
            CG_RagdollFree(rd);
        }
    }
}

//=============================================================
// Per-entity entry point
//=============================================================

static qboolean CG_RagdollEligible(centity_t *cent, refEntity_t *model, qboolean bThirdPerson)
{
    const entityState_t *s1 = &cent->currentState;

    if (!cg_ragdoll->integer || !cgs.ragdollAllowed) {
        return qfalse;
    }

    if (!(s1->eFlags & EF_DEAD)) {
        return qfalse;
    }

    if (!model->tiki || !model->tiki->a) {
        return qfalse;
    }

    // The engine's own "this is an animated character" bit, already used to
    // pick the skeletal LOD path.
    if (!model->tiki->a->bIsCharacter) {
        return qfalse;
    }

    if (s1->number == cg.snap->ps.clientNum && !bThirdPerson) {
        return qfalse;
    }

    // The pose round trip divides by this, and a zero-scaled model has no
    // visible geometry to ragdoll in the first place.
    if (CG_RagdollModelScale(model) < 0.001f) {
        return qfalse;
    }

    return qtrue;
}

void CG_RagdollUpdateEntity(centity_t *cent, refEntity_t *model)
{
    cg_ragdoll_t *rd;
    qboolean      bThirdPerson;
    float         weight;
    int           elapsed;

    CG_RagdollExpire();

    bThirdPerson = cg_3rd_person->integer ? qtrue : qfalse;
    bThirdPerson |= (cg.snap->ps.pm_flags & PMF_CAMERA_VIEW && !(cg.snap->ps.pm_flags & PMF_TURRET)) ? qtrue : qfalse;

    rd = CG_RagdollForEntity(cent->currentState.number);

    if (!CG_RagdollEligible(cent, model, bThirdPerson)) {
        if (rd) {
            CG_RagdollFree(rd);
        }
        return;
    }

    // cg_forceModel can be toggled at runtime, and a tiki swap invalidates
    // every cached bone index, so re-seed rather than emit garbage.
    if (rd && rd->tiki != model->tiki) {
        CG_RagdollFree(rd);
        rd = NULL;
    }

    if (!rd) {
        rd = CG_RagdollAdopt(cent, cent->currentState.modelindex);

        if (rd && rd->tiki != model->tiki) {
            CG_RagdollFree(rd);
            rd = NULL;
        }
    }

    if (!rd) {
        rd = CG_RagdollAlloc();

        if (!rd) {
            return;
        }

        CG_RagdollFree(rd);

        rd->tiki       = model->tiki;
        rd->modelIndex = cent->currentState.modelindex;
        rd->startTime  = cg.time;
        rd->lastTime   = cg.time;
        rd->quietSince = cg.time;

        if (!CG_RagdollSeed(rd, cent, model)) {
            CG_RagdollFree(rd);
            return;
        }

        CG_RagdollFindImpulse(rd);
        CG_RagdollFindBlast(rd);

        rd->state     = RD_BLENDING;
        rd->entityNum = cent->currentState.number;

        // Catch the next body to fall, and only that one. Clearing the cvar
        // here rather than making the caller do it means a trace is one command
        // and cannot be left running to fill the disk. Opened after the state
        // and the entity number are set, so the header describes the corpse
        // rather than the empty slot it came from.
        if (cg_ragdoll_dump->integer > 0) {
            char left[16];

            CG_RagdollDumpOpen(rd, model);
            Com_sprintf(left, sizeof(left), "%d", cg_ragdoll_dump->integer - 1);
            cgi.Cvar_Set("cg_ragdoll_dump", left);
        }
    }

    rd->entityNum = cent->currentState.number;
    VectorCopy(cent->lerpOrigin, rd->entOrigin);
    VectorCopy(cent->lerpAngles, rd->entAngles);

    elapsed = cg.time - rd->startTime;

    if (cg_ragdoll_blendtime->integer > 0) {
        weight = (float)elapsed / (float)cg_ragdoll_blendtime->integer;
    } else {
        weight = 1.0f;
    }

    weight = Q_clamp_float(weight, 0.0f, 1.0f);
    // Smoothstep, so the hand-off out of the animation has no velocity kink.
    weight = weight * weight * (3.0f - 2.0f * weight);

    // Through the blend the references are taken again every frame, from the
    // pose the animation currently has.
    //
    // This is what the arms through the chest came down to. The blend does two
    // jobs at once: it drags the particles onto the death animation, and it
    // holds the body in a sane shape while the solver settles, which is worth a
    // great deal. But every reference the solver worked from, the joint limits,
    // the shape memory, how much clearance each limb keeps, was measured once
    // at the instant of death and never again. While the animation stands still
    // the two agree and the body is steadied. While it moves they disagree, and
    // a body that dies bringing its arms in spends the blend with the animation
    // pulling the arms one way and the constraints hauling them back to where
    // they were when it was shot. Free simulation then began from whatever that
    // struggle left behind.
    //
    // Following the animation removes the disagreement without giving up the
    // steadying, which is why this is done here rather than by shortening the
    // blend or dropping it: measured on the scenarios, taking the blend away
    // costs as much elsewhere as it wins here.
    if (rd->state == RD_BLENDING && weight >= 1.0f) {
        rd->state = RD_ACTIVE;

        // Nothing is measured again here, and it is worth saying why, because
        // it looks like an obvious thing to do and was tried at some length.
        //
        // Every reference the solver works from is taken at the instant of
        // death, while for the length of the blend the particles go on being
        // dragged onto a death animation that is still moving. A body that dies
        // bringing its arms in reaches the end of that window with them
        // somewhere else entirely, and the constraints spend a while hauling
        // them back toward where they were when it was shot.
        //
        // Taking the references again at the end does fix that, and costs more
        // than it fixes. The same pass hands every anti sprawl spring a target
        // measured after the animation has thrown the limbs outward, and a
        // corpse that should have kept its shape settles spread-eagled: limb
        // spread against the living body went from 2.4 to 3.9 times. Holding
        // the sprawl targets back while re-measuring the rest keeps the shape,
        // but then loses the arms again, because widening those targets was the
        // whole of what fixed them.
        //
        // Re-measuring every frame rather than once is worse still, and for a
        // reason worth recording: a reference that is continuously re-measured
        // stops being a reference. The shape memory ends up equal to the
        // current distance on every frame, so it never pulls at all.
    }

    if (rd->state != RD_SLEEPING) {
        // Everything below reads the animation pose, so gather it once here
        // rather than per bone per substep.
        CG_RagdollReadAnimPose(rd, model);

        // Guard against a paused client, a demo seek or a server restart, any
        // of which can hand us a negative or enormous delta.
        if (cg.time < rd->lastTime) {
            rd->lastTime = cg.time;
            rd->accum    = 0.0f;
        }

        rd->accum += (float)(cg.time - rd->lastTime) * 0.001f;
        rd->lastTime = cg.time;

        {
            const float dt      = 1.0f / (float)cg_ragdoll_physicsrate->integer;
            float       maxDisp = 0.0f;
            int         steps   = 0;

            if (rd->accum > dt * 5.0f) {
                rd->accum = dt * 5.0f;
            }

            while (rd->accum >= dt && steps < 5) {
                CG_RagdollDriveFromAnim(rd, weight);
                maxDisp = CG_RagdollStep(rd, cent->currentState.number, dt);
                rd->accum -= dt;
                steps++;
            }

            if (steps) {
                if (maxDisp > cg_ragdoll_sleepvel->value) {
                    rd->quietSince = cg.time;
                }
            }

            rd->lastDisp  = maxDisp;
            rd->lastSteps = steps;
        }

        // Build the pose before falling asleep, so the matrices that get
        // frozen are this frame's rather than the previous frame's.
        CG_RagdollBuildPose(rd, model, weight);

        CG_RagdollDumpFrame(rd, weight);

        if (rd->state == RD_ACTIVE) {
            // Being slow is not the same as having come to rest. A body going
            // over a ledge, or working its way down a staircase, passes through
            // slow moments all the time, and freezing it at one of those is
            // what leaves a corpse hooked on an edge and dangling in mid air
            // instead of dropping. So it has to be resting on something before
            // it is allowed to sleep at all.
            const qboolean supported = (CG_RagdollSupportCount(rd) >= RD_MIN_SUPPORT) ? qtrue : qfalse;

            if (!supported) {
                rd->quietSince = cg.time;
            }

            if (supported && cg.time - rd->quietSince > cg_ragdoll_sleeptime->integer) {
                rd->state = RD_SLEEPING;
            } else if (cg_ragdoll_duration->integer > 0 && elapsed > cg_ragdoll_duration->integer) {
                // The lifetime cap is a budget, not a statement about the body.
                // Applied on its own it freezes whatever pose the corpse is in
                // at that instant, and a body still on its way down a staircase
                // is left standing on its head in mid air. So a corpse that has
                // nothing under it goes on simulating, and only a hard backstop
                // far beyond any real settling time overrides that.
                if (supported || elapsed > cg_ragdoll_duration->integer * RD_SLEEP_BACKSTOP) {
                    rd->state = RD_SLEEPING;
                }
            }
        }
    }

    // A sleeping corpse is still worth following, in one respect. Its matrices
    // are frozen in model space, so from here on the drawn body rides the
    // entity rather than the simulation, and a corpse that settled correctly on
    // the floor can still end up drawn somewhere else entirely if the entity
    // goes there. Nothing is being solved, so only the entity is recorded.
    if (rd->state == RD_SLEEPING) {
        CG_RagdollDumpSleeping(rd);
    }

    // A sleeping ragdoll keeps emitting the matrices it settled on. That is
    // not only cheaper: the matrices are model-space, so freezing them is what
    // makes the corpse ride the entity as it sinks and is removed, instead of
    // staying pinned to fixed world coordinates.

    if (!rd->numOverrides) {
        return;
    }

    model->bone_override      = rd->overrides;
    model->num_bone_overrides = rd->numOverrides;

    // Install the pose on the skeletor now, so that everything else this frame
    // that queries a bone or a tag, attachments and the shadow included, sees
    // the ragdoll rather than the animation.
    cgi.ForceUpdatePose(model);

    if (cg_ragdoll_debug->integer && CG_RagdollDebugReady()) {
        CG_RagdollDebugDraw(rd);
    }
}

// Added in OPM
//  Test access to the raw particle cloud. The emitted pose is rebuilt from
//  fixed offsets, so its bone lengths are exact whatever the solver does: that
//  is what stops limbs stretching, but it also means nothing about the drawn
//  skeleton reveals the particles collapsing underneath it. This is the only
//  way to see that from outside.
extern "C" int CG_RagdollDebugParticles(int entityNum, float *out, int maxJoints)
{
    int i, n;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        const cg_ragdoll_t *rd = &cg_ragdolls[i];

        if (rd->state == RD_FREE || rd->entityNum != entityNum) {
            continue;
        }

        n = maxJoints < RD_NUM_JOINTS ? maxJoints : RD_NUM_JOINTS;

        for (int j = 0; j < n; j++) {
            out[j * 3 + 0] = rd->part[j].p[0];
            out[j * 3 + 1] = rd->part[j].p[1];
            out[j * 3 + 2] = rd->part[j].p[2];
        }

        return n;
    }

    return 0;
}
