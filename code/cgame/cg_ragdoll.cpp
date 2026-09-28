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
#include "cg_parsemsg.h"
#include "cg_props.h"
#include "cg_physics.h"
#include "cg_physics_ragdoll.h"
#include "../qcommon/qfiles.h"

#include <chrono>

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
cvar_t *cg_ragdoll_bodybounce;
cvar_t *cg_ragdoll_limbdamp;
cvar_t *cg_ragdoll_sleepvel;
cvar_t *cg_ragdoll_sleeptime;
cvar_t *cg_ragdoll_debug;
cvar_t *cg_ragdoll_dump;
cvar_t *cg_ragdoll_limptime;
cvar_t *cg_ragdoll_solvegain;
cvar_t *cg_ragdoll_blastimpulse;
cvar_t *cg_ragdoll_limbpush;
cvar_t *cg_ragdoll_armfree;
cvar_t *cg_ragdoll_legfree;
cvar_t *cg_ragdoll_dumplabel;
cvar_t *cg_ragdoll_log;
cvar_t *cg_ragdoll_meshhits;
cvar_t *cg_ragdoll_meshfit;
cvar_t *cg_ragdoll_props;
cvar_t *cg_ragdoll_grab;
cvar_t *cg_ragdoll_grabsaved;
cvar_t *cg_ragdoll_grabrange;
cvar_t *cg_ragdoll_grabspring;
cvar_t *cg_ragdoll_puntspeed;
cvar_t *cg_ragdoll_chestroll;
cvar_t *cg_ragdoll_spinetwist;
cvar_t *cg_ragdoll_solver;
cvar_t *cg_ragdoll_pinsleep;
cvar_t *cg_ragdoll_jointsize;
cvar_t *cg_ragdoll_shoulderslack;
cvar_t *cg_ragdoll_bodypush;
cvar_t *cg_ragdoll_stuckhold;
cvar_t *cg_ragdoll_shove;
cvar_t *cg_ragdoll_stiffness;

//=============================================================
// The rig
//=============================================================

// The joints, shared with the Jolt ragdoll (cg_physics_ragdoll.cpp).
#include "cg_ragdoll_rig.h"

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

// How fast a thigh or an upper arm may roll about itself, in degrees a second.
// A turn of half a circle takes a third of a second. See CG_RagdollBoneFrame.
#define RD_LIMB_ROLL_RATE 540.0f

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

// The three back bones above the pelvis, which share the chest roll correction
// between them. The pelvis is left out on purpose: measured over three hundred
// corpses the drawn hip line sits a median of 6 degrees from the simulated one
// and is past 20 in eighteen of them, so it is a sound anchor, while the drawn
// shoulder line is a median of 24 degrees out and past 20 in a hundred and
// seventy four. The error is the chest's, so the pelvis should not pay for it.
#define RD_FIRST_ROLL_BONE 2
#define RD_LAST_ROLL_BONE  4
#define RD_NUM_ROLL_BONES  (RD_LAST_ROLL_BONE - RD_FIRST_ROLL_BONE + 1)

// Beyond this the correction is not a correction. A solver state wild enough to
// put the shoulders half a turn from the drawn chest is one where rolling the
// back to follow them would wring the corpse rather than settle it.
//
// 60 was too mean. With the loop closed the typical corpse needs far less than
// that, but a minority need more, and those were the ones left visibly wrong:
// of the corpses photographed at 60, the drawn arm was still as much as 19
// units from the particle it collides on behalf of. Raising it to 90 improves
// the disagreement both at rest and at worst in every configuration measured.
// 120 is worse again at rest, so this is a ceiling rather than a direction.
#define RD_MAX_CHEST_ROLL 90.0f

// How much of the measured error is taken out per frame. A closed loop, so it
// need not be one: driven whole it overshoots and the chest hunts back and
// forth, which reads as a shiver in a body that has stopped moving.
#define RD_CHEST_ROLL_GAIN 0.35f

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
    // The same for the maximum length: the joint can straighten no closer to
    // straight than this many degrees.
    float minBendDeg;
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
    // 0.5 is 120 degrees, not the 0.259 that 150 asks for. 150 is the AAOS
    // figure and it is right for a living arm, but it is a voluntary maximum,
    // reached by pulling with the biceps against nothing, and a dead arm has
    // nothing to pull with. Left at 150 a quarter of all corpses in game came
    // to rest with an elbow within five degrees of the stop, folded flat, which
    // reads exactly as the complaint it drew: bent in a little too far.
    //
    // The suite cannot see this. Its arms average 78 degrees of fold, well
    // clear of either limit, so the change costs nothing there and is worth
    // nothing there either: every other measure is unmoved at 150, 130, 120 and
    // 110 alike. The evidence is a hundred bodies in the game.
    {RD_LUARM,  RD_LHAND,    RD_LFARM,  0.500f, 0.985f, 0.12f, 0.0f },
    {RD_RUARM,  RD_RHAND,    RD_RFARM,  0.500f, 0.985f, 0.12f, 0.0f },
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

    // The ankle, as angles off the line of the shin, because this model's foot
    // bone does not run level: from the ankle it slopes down to the toe, and a
    // man standing has it about 62 degrees off the shin. As fractions of the
    // straight length these were 0.86 to 1.02, which allowed only about eight
    // degrees of lifting the toes and no limit at all on pointing them: the
    // foot could fold out straight in line with the leg, and a body lying on
    // the ground did exactly that. 25 is about 37 degrees of pointing, a relaxed
    // corpse's worth rather than the 50 a dancer can hold; 80 about 18 of
    // lifting, near the 20 a living ankle has. Soft rather than hard, see
    // CG_RagdollMeasureLimits.
    {RD_LCALF,  RD_LTOE,     RD_LFOOT,  0.0f,  9.0f,   0.10f, 80.0f, 25.0f},
    {RD_RCALF,  RD_RTOE,     RD_RFOOT,  0.0f,  9.0f,   0.10f, 80.0f, 25.0f},

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

// How far along a limb bone the world is sampled. The two ends are joints and
// are traced already; these are the places in between that nothing has ever
// looked at.
#define RD_NUM_LIMB_SAMPLES 2

static const float rd_limbSamples[RD_NUM_LIMB_SAMPLES] = {0.35f, 0.65f};

// The arms occupy the first four rows above, the legs the last four.
#define RD_FIRST_LEG_SEGMENT 4

// How long the legs take to claim the extra room they are given.
#define RD_LEG_OPEN_MS 250.0f

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

// How many corpses may exist at once, which is not the same as how many may be
// simulated: cg_ragdoll_maxcount caps that, and a body that has settled costs
// nothing to leave lying here.
//
// Sixteen was far too few for a busy round. Traced from a real one, two hundred
// men died in eighteen seconds, a death every eighteen milliseconds at the
// median and several inside the same millisecond; holding each corpse for even
// one second needs twenty five slots, and for five seconds seventy. At sixteen
// the pool turned over roughly every second and a half however the allocator
// chose, so bodies were still being taken away shortly after they settled.
//
// And sixty four was too few for single player, where corpses are kept for
// the whole level (g_keepcorpses) and every one of them is meant to stay the
// way it fell. A long level leaves a few hundred. A slot is ten and a half
// kilobytes, so this pool is a little over five megabytes; the walks over it
// are either once a frame or bounded by a sphere test per slot.
#define MAX_RAGDOLLS 512

// How close a fresh corpse entity must be to a just-orphaned ragdoll for it to
// adopt it. Body::Body copies the player's origin verbatim, so the match is
// effectively exact and this only guards against float noise.
#define RD_ADOPT_DIST 4.0f

// A stand-in for a corpse (see CG_RagdollAdopt) has to turn up within this
// long of the corpse going away. The swap is made on one server frame, so
// both are seen in the same snapshot and the gap is a single client frame.
#define RD_STANDIN_WINDOW 250

// And this close to where the corpse was. Tight is not needed: by then it has
// already been matched on model, on tiki and on timing. The copy is dropped to
// the floor as it is shown, and the mod can also place it where the corpse
// was when it started dying rather than where it ended up.
#define RD_STANDIN_DIST 64.0f

// How fast the grabber can carry the joint it holds, in units per second.
// Fast enough to swing a body round and throw it, not so fast that one frame's
// flick of the mouse teleports it.
#define RD_GRAB_SPEED 1200.0f

// The beam held back when pulling a body against the world: how far short of
// the aim the held part has to be, with at least this many joints touching
// something, and then what share of the spring and what speed it gets.
#define RD_GRAB_BLOCKED_GAP      24.0f
#define RD_GRAB_BLOCKED_CONTACTS 3
#define RD_GRAB_BLOCKED_PULL     0.3f
#define RD_GRAB_BLOCKED_SPEED    300.0f
#define RD_GRAB_BLOCKED_EASE     0.1f

// The spring the grabber holds a body on (its stiffness is
// cg_ragdoll_grabspring): a little under critically damped, so a body that is
// swung and stopped runs on and settles back once rather than stopping dead,
// and the share of gravity it is left to sag by. At the default stiffness a
// body hangs about four units under the aim and trails a swing by about a
// seventh of a second.
#define RD_GRAB_DAMPING 0.6f

// A body the grabber holds is also calmed, as the physics gun calms what it
// holds: its velocity is damped by this much more each step, and only this
// share of what the solver moves it by is handed back as speed. Hung from a
// point, a body is pulled against its own joint limits by gravity every step,
// and at full gain that tug of war was fed back in as motion: it spun and
// shivered for as long as it was held. The part being held keeps its own
// spring and its weight; it is the rest of the body that settles.
#define RD_GRAB_BODY_DAMPING 0.94f
#define RD_GRAB_SOLVEGAIN    0.4f
#define RD_GRAB_SAG     0.25f

// A ragdoll whose entity has been gone longer than this can be picked up again
// only by that same entity, coming back into view. Anything else arriving there
// later is another man's corpse and must not inherit this one's pose.
#define RD_ORPHAN_ADOPT_WINDOW 250

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

// How far, in particle radii, a particle may move from where it last struck a
// plane before the world is asked again whether the plane is still under it.
// Age alone let a plane outlive the surface by a quarter of a second: a forearm
// that came up under a lip in the game, and then slid out from beneath it, was
// held down by the lip's underside for a third of a second after it had left
// it, while the grabber carried the rest of the body up past it, and then came
// up a hundred and fifty units in one step.
#define RD_CONTACT_REACH 2.0f

// How far, in particle radii, a buried particle may be moved to get it out.
// Beyond this the exit found is not the surface it is behind.
// How long a joint may go on failing to get out of the world before it is left
// where it is. At sixty steps a second this is half a second of trying.
#define RD_STUCK_STEPS 30

// How much else has to be holding the body up before a stuck joint is pinned.
#define RD_STUCK_MIN_SUPPORT 6

#define RD_UNBURY_REACH 4.0f

// How far a joint must be drawn from the one it hangs from, as a share of the
// bone and in units, before a surface it strikes is taken to be hooking it.
#define RD_HOOK_STRETCH 1.5f
#define RD_HOOK_SLACK   4.0f

// Collision displacement below this is treated as a resting contact rather
// than an impact, and does not trigger the reconciling solve.
#define RD_IMPACT_EPSILON 0.5f

// Small gap kept between a resting particle and the surface it landed on, so
// that floating point noise cannot push it back inside.
#define RD_SURFACE_GAP 0.25f

// A hinge this close to its boundary is left alone, so that a joint resting
// right on the limit is not nudged back and forth forever.
// Share of cg_ragdoll_damping a body with nothing under it is given. A
// little rather than none: measured, none spun held bodies faster; a fifth
// left limbs hanging less and the torso less twisted than full damping did.
#define RD_AIR_DAMPING 0.2f

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

// Above this, in units a step, a grounded particle is sliding and is left to
// friction instead (120 units a second at sixty steps).
#define RD_SLIDE_SPEED    2.0f

// How far below the lowest joint it rests on, in units, a body's centre of mass
// may sit and still count as lying on it rather than hanging from it.
#define RD_BALANCE_SLACK 6.0f

// The share of the friction a body keeps while it hangs from what it rests on
// rather than lying on it. See CG_RagdollFriction.
#define RD_DRAPED_FRICTION 0.25f

// How many times the sleeping speed one joint may move before it alone keeps
// the body awake.
#define RD_SLEEP_SPIKE 4.0f

// Below this, in units per second along the surface, a resting particle is
// held rather than allowed to creep. Was written as half a unit of Verlet
// displacement per step, which is the same thing at sixty steps a second.
#define RD_STATIC_FRICTION 30.0f

// How many joints must be resting on a surface before the body counts as
// supported, and so before it is allowed to damp down and fall asleep.
#define RD_MIN_SUPPORT 3

// Or this many touching anything at all, whichever way it faces.
//
// The support test only counts faces within about forty five degrees of level,
// because what it is really asking is whether the body is still falling. A
// corpse wedged in a corner or lying against a steep slope answers no: traced
// from the game, bodies held by fourteen and fifteen contact planes with only
// one of them level went on being solved for twelve seconds and more, because
// they could never qualify to sleep. A body touching this many things is not
// in free fall whichever way those things face.
#define RD_MIN_SUPPORT_ANY 8

// How far past the lifetime cap a corpse with nothing under it is allowed to
// keep falling before it is frozen anyway. Only reached by a body that has left
// the map or wedged somewhere it can never rest.
#define RD_SLEEP_BACKSTOP 4

// The most a body's joints may be moving, on average, in units a step, for the
// lifetime cap to freeze it.
#define RD_CAP_MAX_MEAN 1.0f

// How long, in milliseconds, a body must have been lying on level ground before
// the lifetime cap may freeze it. See the cap in CG_RagdollUpdateEntity.
#define RD_CAP_SETTLE 1000

// Fraction of a limb-versus-body overlap resolved per iteration.
#define RD_SEGMENT_RATE 0.35f

// How much of the excess twist is taken out per iteration.
#define RD_SPINE_TWIST_RATE 0.25f

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

    // Where the particle was when that happened. A plane is only known to be
    // there near the place it was struck; see RD_CONTACT_REACH.
    vec3_t contactAt;
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

// How many brush entities a sleeping corpse keeps an eye on, and how far past
// its own bounds they may be. The margin catches the lift the body is lying on.
#define RD_MAX_MOVERS    8
#define RD_MOVER_MARGIN  8.0f

typedef struct {
    rdState_t state;

    int      entityNum;  // ENTITYNUM_NONE while orphaned
    dtiki_t *tiki;       // the tiki the bone indices were resolved against
    int      modelIndex; // for corpse adoption
    int      orphanTime; // cg.time the owning entity went away
    int      lastEntityNum; // the entity it last belonged to, kept while orphaned
    qboolean standIn;    // carried on by a live entity swapped in for the corpse
    qboolean gone;       // its entity has left the snapshot (for the log only)

    // The entity as it was last handed to the renderer, pose and all, so a
    // bullet arriving later in the frame is traced against what was drawn.
    refEntity_t drawn;
    qboolean    haveDrawn;

    // The model as its entity last handed it over, before any of this was done
    // to it, so the corpse can go on being drawn if the server stops sending
    // the entity; see CG_RagdollAddUnsent.
    refEntity_t unsent;
    qboolean    haveUnsent;

    // Set as the body falls asleep; the drawn mesh is checked against the world
    // once the sleeping pose is installed. See CG_RagdollMeasureClipping.
    qboolean measureClip;

    // Which edge of its twist range an elbow that has gone out of it
    // is being brought back to: -1 or 1, and 0 while it is inside. Kept so a
    // joint turned almost exactly the wrong way, which is equally far from
    // both edges, goes back by one of them rather than being thrown to and
    // fro between the two. See CG_RagdollTwistEdge.
    signed char elbowEdge[2];

    // Set once a pose has been built from the simulation, so the last frame's
    // roll of each bone means something. Until then there is nothing to keep
    // or to limit the turn from.
    qboolean rollReady;

    // The grabber's pull held back against the world (see the grab spring in
    // CG_RagdollStep): whether it is, and how far it has eased into it.
    qboolean grabBlocked;
    float    grabEase;

    // How fast the hardest-hitting joint struck the world this step, along
    // the surface's normal (see CG_RagdollStep's impact splat).
    float    impactSpeed;

    // Whether the body's weight is over what it is lying on (see
    // CG_RagdollBalanced), as the last step found it.
    qboolean balanced;
    // Resting on something but hanging below it (see CG_RagdollFriction).
    qboolean draped;
    // When it last came to lie on level ground, balanced; 0 while it is not.
    int restingSince;

    // Props the body was already inside when it died, which it passes
    // through; see CG_RagdollNoteEnclosingProps.
    short ignoredProp[8];
    int   numIgnoredProps;

    // The roll of each upper arm and thigh (see the limb twist section): the
    // way its joint bends, square to the bone, in the world; the bone's
    // direction when that was last carried along with it; and which edge of
    // its range it is being held to. Live from the moment the body leaves the
    // blend.
    vec3_t      limbY[4];
    vec3_t      limbDir[4];
    signed char limbEdge[4];
    qboolean    limbTwistLive;

    // How far CG_RagdollJointRanges has moved each joint during the current
    // solve. Taken back out of what the solver hands to the body as speed:
    // see CG_RagdollSolveTracked.
    vec3_t rangeShift[RD_NUM_JOINTS];

    // The joint the grabber is holding, or -1, and where it is being carried
    // to. See the grabber section.
    int    grabJoint;
    vec3_t grabTarget;

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

    // Each joint's distance from the joint it hangs from, as it died; see
    // CG_RagdollHooked.
    float boneRest[RD_NUM_JOINTS];

    // Deliberately not scaled to the model, unlike the other distances here.
    // It reads as an oversight and scaling it was tried; it measures worse,
    // because the value was arrived at as an absolute and scaling it drives a
    // small model far too tight. Self intersection at the 90th percentile went
    // from 40 to 55.
    float hingeLateralSlop;
    float limbRadius[RD_NUM_LIMB_SEGMENTS];

    // A contact plane for each sample along each limb, kept and projected
    // against in every solver iteration the way a joint's is; see
    // CG_RagdollBoneSweep.
    struct {
        qboolean has;
        vec3_t   normal;
        float    dist;
        int      time;
        vec3_t   at;
    } boneContact[RD_NUM_LIMB_SEGMENTS][RD_NUM_LIMB_SAMPLES];
    float collisionSlop;
    // How much of the ideal clearance each limb/trunk pair is actually asked
    // for: 1 when the two start clear of each other, less when they begin
    // touching, 0 when the pair is not checked at all.
    float segTrunkScale[RD_NUM_LIMB_SEGMENTS][RD_NUM_TRUNK_SEGMENTS];

    // The same, for one limb against another. A shin can pass clean through the
    // other shin without this: the joint-to-joint pairs cannot see it, because
    // two bones can cross with all four of their ends well apart.
    float segLimbScale[RD_NUM_LIMB_SEGMENTS][RD_NUM_LIMB_SEGMENTS];

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

    boneOverride_t overrides[RD_NUM_BONES + 2];
    int            numOverrides;

    // The same, as handed to the renderer: moved to the drawn model's own
    // origin (CG_RagdollRecenter), which changes every frame.
    boneOverride_t drawOverrides[RD_NUM_BONES + 2];

    // The clavicles (see CG_RagdollPlaceClavicles): each one's bone, where it
    // sat on the chest and which way its axes lay, both in the seed frame of
    // Bip01 Spine2. Then where it was last drawn.
    int    clavIndex[2];
    vec3_t clavOffset[2];
    vec3_t clavLocal[2][3];
    vec3_t clavPos[2];
    vec3_t clavAxis[2][3];

    // Velocity, in units per second, that the killing shot should hand to the
    // body. Held until the first integration step rather than written into the
    // particles at seed time, because Verlet carries velocity as the gap
    // between p and pPrev and that gap is only meaningful once dt is known.
    qboolean hasImpulse;
    vec3_t   impulseVel;
    int      impulseJoint;

    // With cg_ragdoll_solver 1, the Jolt ragdoll that carries the body once the
    // blend out of the death animation is over (cg_physics_ragdoll.cpp), or 0.
    int jolt;

    // An explosion near enough to have thrown this body, kept as the place it
    // went off rather than as a direction, so each particle can be pushed away
    // from it by its own distance.
    qboolean hasBlast;
    vec3_t   blastPos;
    float    blastRadius;
    float    blastSpeed;

    // Which particles are being held up by another corpse rather than by the
    // world. Without this a body that comes to rest on a heap never sleeps: the
    // sleep test asks for contact, contact comes from tracing against the
    // world, and there is no world under a man lying on another man.
    int      onBodyMask;

    // Which joints have given up trying to get out of the world, and how many
    // steps each has been inside it. See CG_RagdollPushOut.
    int      stuckMask;
    short    buriedFor[RD_NUM_JOINTS];

    // Which particles were buried in solid geometry on the last step. A trace
    // that starts inside a brush cannot say which way is out, so such a
    // particle is dropped back where it was, and if it stays buried it is
    // dropped back again every step. Only the trace can show that happening.
    int      buriedMask;

    // Which limb bones were found inside the world between their two joints on
    // the last step. Nothing else records this, and it is invisible in the
    // drawn pose: the bone is put back on the surface, so a trace read
    // afterwards shows a limb that was never in trouble.
    int      limbBuriedMask;

    // How far the drawn chest has to be rolled to face the way the simulated
    // shoulders do, in degrees. Spread along the back rather than applied where
    // it is measured; see CG_RagdollMeasureChestRoll.
    float    spineRollFix;

    // What the last measurement asked for, before the gain and the clamp. The
    // correction reaching its limit and the correction not converging look the
    // same from outside, and a trace that carries both can tell them apart.
    float    chestRollErr;

    // The average joint's movement on the last step, beside the fastest one's.
    // Sleep is judged on the average: see the comment where it is used.
    float    lastMean;

    // Where the entity was standing when this corpse went to sleep, so the
    // drawn body can be held where it settled while still following the entity
    // down as it sinks.
    // A sphere round the whole body, refreshed once a frame, so one corpse can
    // be rejected against another without walking either one's particles.
    vec3_t   boundCentre;
    float    boundRadius;

    vec3_t   sleepOrigin;
    qboolean sleepPinned;

    // How far the entity had sunk when the corpse was last drawn asleep, so a
    // body woken by a shot or a blast carries on from where it was being drawn
    // rather than jumping back to where its particles were left.
    float    sleepDrop;

    // A corpse shoved after it had settled is given this long before the
    // lifetime cap is allowed to put it back to sleep. Without it a body past
    // its five seconds goes straight back down on the very frame it is hit.
    int      wakeUntil;

    // When it was last woken, by a shot, a blast or the grabber. The lifetime
    // cap counts from here rather than from the death: counted from the death,
    // a body punted minutes later was frozen mid-slide the moment its wake
    // window ran out, and one knocked off a ledge was frozen mid-fall.
    int      lastWake;

    // How many times something has hit this corpse since it died. Without it a
    // trace cannot tell a body that is moving because it was shot from one that
    // is moving because it is stuck, and they look identical in the totals.
    int      wakeCount;

    // What the sleep test last saw, kept only so the trace can report it. A
    // corpse that will not sleep is holding one of these above its threshold,
    // and which one it is cannot be worked out from the pose.
    float    lastDisp;
    int      lastSteps;

    // Open while this corpse is being traced to a file. Zero is no file, which
    // is what the memset in CG_RagdollFree leaves behind.
    fileHandle_t dumpFile;
    char         dumpName[64];

    // Which ragdoll_dump_N.txt this body is being written to, so the number can
    // be drawn over it. Kept after the file closes: the corpse outlives the
    // trace, and a screenshot taken late is exactly the one worth matching.
    int          dumpSeq;

    // When the body this trace follows went to sleep, so the file can be let go
    // shortly after rather than held for the corpse's whole life.
    int dumpSleepAt;

    // The doors, lifts and other brush entities touching the body when it
    // fell asleep, and where they were then. See CG_RagdollMoversChanged.
    int    numMovers;
    int    moverNum[RD_MAX_MOVERS];
    vec3_t moverOrigin[RD_MAX_MOVERS];
    vec3_t moverAngles[RD_MAX_MOVERS];
} cg_ragdoll_t;

static cg_ragdoll_t cg_ragdolls[MAX_RAGDOLLS];

// When each body last had a ragdoll taken away from it.
//
// A corpse whose slot is recycled is still lying there dead, so on the very
// next frame it asks for a ragdoll again, takes a slot from another settled
// corpse, and that one asks again in its turn. Traced from a busy round, two
// hundred captures came from fifty two bodies, one of them re-seeded twelve
// times over, each new ragdoll starting from the pose its death animation
// ended on: that churn is what is seen as corpses flickering and resetting.
//
// So a body that has lost its ragdoll does not get another. It keeps the pose
// it had, which is the same thing that happens to a corpse that never got a
// slot in the first place.
static int rd_evictedAt[MAX_GENTITIES];

// When each entity last came into the snapshot, or jumped, which is when a
// stand-in for a corpse appears. Zero is never.
static int rd_appearedAt[MAX_GENTITIES];

static const char *rd_stateNames[] = {"free", "blending", "active", "sleeping"};

// See cg_ragdoll_log.
static void CG_RagdollLog(const cg_ragdoll_t *rd, const char *fmt, ...) Q_PRINTF_FUNC(2, 3);

static void CG_RagdollLog(const cg_ragdoll_t *rd, const char *fmt, ...)
{
    char    text[256];
    va_list ap;

    if (!cg_ragdoll_log || !cg_ragdoll_log->integer) {
        return;
    }

    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    cgi.Printf(
        "ragdoll %d [slot %d, entity %d, %s%s]: %s\n",
        cg.time,
        (int)(rd - cg_ragdolls),
        rd->entityNum == ENTITYNUM_NONE ? -1 : rd->entityNum,
        (unsigned)rd->state < ARRAY_LEN(rd_stateNames) ? rd_stateNames[rd->state] : "?",
        rd->standIn ? ", stand-in" : "",
        text
    );
}


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
// Which limb, if any, this bone is the top of: 0 and 1 the upper arms, 2 and 3
// the thighs, -1 anything else. These are the bones whose roll is taken from
// the joint below them (see CG_RagdollBoneFrame).
static int CG_RagdollLimbRoot(int boneNum)
{
    switch (rd_bones[boneNum].joint) {
    case RD_LUARM:
        return 0;
    case RD_RUARM:
        return 1;
    case RD_LTHIGH:
        return 2;
    case RD_RTHIGH:
        return 3;
    default:
        return -1;
    }
}

static int CG_RagdollTwistSlot(int boneNum);

static qboolean CG_RagdollBoneFrame(cg_ragdoll_t *rd, int boneNum, vec3_t out[3])
{
    const rdBoneDef_t *def = &rd_bones[boneNum];
    vec3_t             x, helper, carried;
    qboolean           haveCarried = qfalse;

    VectorSubtract(rd->part[def->aim].p, rd->part[def->joint].p, x);

    if (VectorNormalize(x) < 0.001f) {
        return qfalse;
    }

    // A limb bone whose twist is simulated is drawn with it, and nothing
    // below is guessed for it (see the limb twist section).
    if (rd->limbTwistLive && rd->state == RD_ACTIVE) {
        const int slot = CG_RagdollTwistSlot(boneNum);

        if (slot >= 0) {
            CG_RagdollOrthonormalize(x, rd->limbY[slot], out);
            VectorCopy(out[1], rd->transportY[boneNum]);
            VectorCopy(out[0], rd->transportX[boneNum]);
            return qtrue;
        }
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

            // A straight limb says nothing about its roll, so it keeps the one
            // it had. It used to take the roll carried over from the chest,
            // which has nothing to do with where the palm was facing: every
            // time an arm straightened its roll went to that one angle, and
            // when it bent again it came back from there, up to half a turn
            // round, which put the palms behind the body.
            if (CG_RagdollLimbRoot(boneNum) >= 0 && rd->state == RD_ACTIVE && rd->rollReady) {
                VectorCopy(rd->transportY[boneNum], fallback);
            } else if (haveCarried) {
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

    // However the thigh and the upper arm come by their roll, it may only turn
    // so fast. Both take it from the next bone down, which sets it by the way
    // the knee or elbow is bent; a limb hanging straight is bent hardly at all,
    // and as it sways that little bend passes from one side to the other and
    // the roll it gives turns over with it, in a single frame. Carried down to
    // the calf, the foot, the forearm and the hand, that was a whole leg or arm
    // flipping over and back while the body hung from the grabber, seen as the
    // corpse spasming. A real roll takes a moment to happen and still does;
    // noise flicking the reference to and fro now barely moves the limb.
    //
    // Only once the body is its own: through the blend the pose is being
    // taken from the animation, and at the seed there is no last frame.
    if (def->helperA >= 0 && rd->state == RD_ACTIVE && rd->rollReady) {
        vec3_t prevY, newY, side;

        VectorMA(rd->transportY[boneNum], -DotProduct(rd->transportY[boneNum], x), x, prevY);
        VectorMA(helper, -DotProduct(helper, x), x, newY);

        if (VectorNormalize(prevY) > 0.001f && VectorNormalize(newY) > 0.001f) {
            const float maxTurn = RD_LIMB_ROLL_RATE * Q_clamp_float((float)cg.frametime, 1.0f, 100.0f) * 0.001f;
            float       turn;

            CrossProduct(x, prevY, side);
            turn = RAD2DEG(atan2(DotProduct(newY, side), DotProduct(newY, prevY)));

            if (fabs(turn) > maxTurn) {
                RotatePointAroundVector(helper, x, prevY, turn > 0.0f ? maxTurn : -maxTurn);
            }
        }
    }

    // The back's share of the chest roll correction. Added to this bone's roll
    // reference, and inherited by the bone above through the transport, so the
    // three shares accumulate to the whole by the time the chest is reached.
    if (rd->spineRollFix != 0.0f && boneNum >= RD_FIRST_ROLL_BONE && boneNum <= RD_LAST_ROLL_BONE) {
        vec3_t rolled;

        RotatePointAroundVector(rolled, x, helper, rd->spineRollFix / (float)RD_NUM_ROLL_BONES);
        VectorCopy(rolled, helper);
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

// Moves the drawn model's origin to where the body is, and the pose with it.
//
// The corpse's entity stays where the man died, and the renderer judges the
// model by that point: how much detail it keeps (the level of detail comes from
// how large the model looks from its origin), and where it takes its light
// from. A body carried off with the grabber, or punted down the street, was
// drawn at the detail due to something far away, which on these models drops
// most of the vertices and collapses the arms, and lit as though it still lay
// where it fell. The matrices are relative to the origin, so moving the one and
// taking the same distance off the other leaves every bone exactly where it was.
static void CG_RagdollRecenter(cg_ragdoll_t *rd, refEntity_t *model)
{
    const float scale = CG_RagdollModelScale(model);
    vec3_t      target, shift;
    int         i, k;

    memcpy(rd->drawOverrides, rd->overrides, rd->numOverrides * sizeof(rd->overrides[0]));

    if (scale <= 0.0f) {
        return;
    }

    // Where the root is drawn this frame, from its own matrix: that is right
    // however the pose was written, sleeping, pinned or riding the entity.
    for (i = 0; i < rd->numOverrides; i++) {
        if (rd->overrides[i].boneIndex == rd->boneIndex[0]) {
            break;
        }
    }
    if (i == rd->numOverrides) {
        return;
    }

    VectorCopy(model->origin, target);
    for (k = 0; k < 3; k++) {
        const float local = (rd->overrides[i].matrix[3][k] + (model->tiki ? model->tiki->load_origin[k] : 0.0f)) * scale;

        VectorMA(target, local, model->axis[k], target);
    }

    VectorSubtract(target, model->origin, shift);

    for (i = 0; i < rd->numOverrides; i++) {
        for (k = 0; k < 3; k++) {
            rd->drawOverrides[i].matrix[3][k] -= DotProduct(shift, model->axis[k]) / scale;
        }
    }

    VectorAdd(model->origin, shift, model->origin);
    VectorAdd(model->oldorigin, shift, model->oldorigin);
    VectorAdd(model->lightingOrigin, shift, model->lightingOrigin);
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

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rd->boneRest[i] = rd_joints[i].parent >= 0 ? Distance(rd->part[i].p, rd->part[rd_joints[i].parent].p) : 0.0f;
    }

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

        if (def->minBendDeg > 0.0f && def->via >= 0) {
            const float rad = (180.0f - def->minBendDeg) * (float)M_PI / 180.0f;
            vec3_t      d1, d2;
            float       l1, l2;

            VectorSubtract(rd->part[def->via].p, rd->part[def->a].p, d1);
            VectorSubtract(rd->part[def->b].p, rd->part[def->via].p, d2);
            l1 = VectorLength(d1);
            l2 = VectorLength(d2);

            out->maxLen = sqrt(l1 * l1 + l2 * l2 - 2.0f * l1 * l2 * cos(rad));
        }
        out->stiffness = def->stiffness;
        // A joint with both ends given as angles, the ankle, is left with the
        // soft constraints. Held hard, a foot lying face down, which wants to
        // lie flat, was pressed against its limit by the floor for as long as
        // it lay there and never quite settled.
        out->isHard    = ((def->maxBendDeg > 0.0f && def->minBendDeg <= 0.0f) || (def->minScale == 1.0f && def->maxScale == 1.0f)) ? qtrue : qfalse;

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
    // And how much each limb is asked to keep from every other limb, on the
    // same principle: a pair that starts touching is not asked to come apart.
    for (i = 0; i < RD_NUM_LIMB_SEGMENTS; i++) {
        for (j = 0; j < RD_NUM_LIMB_SEGMENTS; j++) {
            float  want, rest, ta, tb;
            vec3_t dir;

            rd->segLimbScale[i][j] = 0.0f;

            // Only each unordered pair once, and never two bones that meet at a
            // joint: an upper arm and its own forearm touch by construction.
            if (j <= i) {
                continue;
            }

            if (rd_limbSegments[i].a == rd_limbSegments[j].a || rd_limbSegments[i].a == rd_limbSegments[j].b
                || rd_limbSegments[i].b == rd_limbSegments[j].a || rd_limbSegments[i].b == rd_limbSegments[j].b) {
                continue;
            }

            rest = CG_RagdollSegmentToSegment(
                rd->part[rd_limbSegments[i].a].p,
                rd->part[rd_limbSegments[i].b].p,
                rd->part[rd_limbSegments[j].a].p,
                rd->part[rd_limbSegments[j].b].p,
                &ta,
                &tb,
                dir
            );

            want = rd->limbRadius[i] + rd->limbRadius[j];

            if (want < 0.0001f) {
                continue;
            }

            if (rest >= want) {
                rd->segLimbScale[i][j] = 1.0f;
                continue;
            }

            rd->segLimbScale[i][j] = rest * 0.85f / want;

            if (rd->segLimbScale[i][j] < RD_SEGMENT_MIN_FRACTION) {
                rd->segLimbScale[i][j] = RD_SEGMENT_MIN_FRACTION;
            }

            if (rd->segLimbScale[i][j] * want > rest) {
                rd->segLimbScale[i][j] = rest / want;
            }
        }
    }

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

// How much of an explosion's push is turned upward, as a fraction of it.
//
// A grenade goes off on the floor and a corpse is lying on the floor, so the
// line between them is nearly flat and the body is shoved sideways into the
// ground it is already resting on. It slides, and friction has it a few feet
// later. What makes a body actually leave the ground is the part of the blast
// that is not horizontal, and on level ground there is almost none of it.
#define RD_BLAST_LIFT 0.85f

// The fastest an explosion may launch any part of a corpse, in units a second.
//
// Without a cap the response is wildly non-linear: while the body is sliding on
// the floor friction eats most of the push, and the moment it clears the ground
// nothing does. Measured, doubling the strength took a corpse from twenty six
// units to thirteen hundred, which is a man leaving the map. A running soldier
// does about two hundred and fifty, so this is a body moving several times
// faster than anyone can run and no faster than that.
#define RD_BLAST_MAX_SPEED 1250.0f

#define RD_NUM_BLAST_KINDS ((int)(sizeof(rd_blastKinds) / sizeof(rd_blastKinds[0])))

// Called by the message parser for every explosion, whatever it hit. kind
// indexes rd_blastKinds and is clamped, so an unknown one reads as a grenade.
static void CG_RagdollWake(cg_ragdoll_t *rd);

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

    // The bodies already lying about when it went off.
    //
    // Until now this only wrote the blast down for whoever was about to die in
    // it: a man killed by the grenade was thrown, and a man who had died a
    // second earlier lay through it untouched. Corpses are thrown by explosions
    // now whether or not the explosion is what killed them.
    //
    // Scaled by the same cg_ragdoll_blastimpulse that throws the man the
    // grenade kills, and not by the shove that a bullet uses. An explosion
    // should move a body the same whether it has been dead a second or an
    // instant, and the two want very different strengths from one another: a
    // round jolts a corpse, a grenade throws it across the room.
    {
        int n, i;

        for (n = 0; n < MAX_RAGDOLLS; n++) {
            cg_ragdoll_t *rd = &cg_ragdolls[n];
            qboolean      touched = qfalse;

            if (rd->state == RD_FREE || !rd->boundRadius) {
                continue;
            }

            for (i = 0; i < RD_NUM_JOINTS; i++) {
                vec3_t away;
                float  dist, push;

                VectorSubtract(rd->part[i].p, pos, away);
                dist = VectorNormalize(away);

                if (dist >= blast->radius) {
                    continue;
                }

                // Right on top of it there is no direction to be thrown in.
                if (dist < 1.0f) {
                    VectorSet(away, 0.0f, 0.0f, 1.0f);
                }

                away[2] += RD_BLAST_LIFT;
                VectorNormalize(away);

                push = blast->speed * cg_ragdoll_blastimpulse->value * (1.0f - dist / blast->radius);

                if (push > RD_BLAST_MAX_SPEED) {
                    push = RD_BLAST_MAX_SPEED;
                }

                VectorMA(rd->part[i].v, push, away, rd->part[i].v);

                touched = qtrue;
            }

            if (touched) {
                CG_RagdollWake(rd);
            }
        }
    }

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

static void CG_RagdollFitToMesh(cg_ragdoll_t *rd, refEntity_t *model);

static void CG_RagdollSeedClavicles(cg_ragdoll_t *rd, refEntity_t *model);

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

    CG_RagdollSeedClavicles(rd, model);

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

    // Everything measured below works from these sizes, so the mesh has its say
    // first.
    CG_RagdollFitToMesh(rd, model);

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

    // And the middles of the limbs, against the planes CG_RagdollBoneSweep
    // found for them, moving the two ends of the bone.
    for (i = 0; i < RD_NUM_LIMB_SEGMENTS; i++) {
        rdParticle_t *pa = &rd->part[rd_limbSegments[i].a];
        rdParticle_t *pb = &rd->part[rd_limbSegments[i].b];
        int           q, k;

        if (rd_limbSegments[i].a == rd->grabJoint || rd_limbSegments[i].b == rd->grabJoint) {
            continue;
        }

        for (q = 0; q < RD_NUM_LIMB_SAMPLES; q++) {
            const float u = rd_limbSamples[q];
            vec3_t      mid;
            float       d;

            if (!rd->boneContact[i][q].has) {
                continue;
            }

            for (k = 0; k < 3; k++) {
                mid[k] = pa->p[k] + (pb->p[k] - pa->p[k]) * u;
            }

            d = DotProduct(mid, rd->boneContact[i][q].normal) - rd->boneContact[i][q].dist;

            if (d < 0.0f) {
                const float push = -d / ((1.0f - u) * (1.0f - u) + u * u);

                VectorMA(pa->p, push * (1.0f - u), rd->boneContact[i][q].normal, pa->p);
                VectorMA(pb->p, push * u, rd->boneContact[i][q].normal, pb->p);
            }
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
//
// It is a memory of the death pose, not a joint, so it opens up as the body
// goes limp (cg_ragdoll_limptime), until nothing is left of it. Held for ever
// it was what kept a man who died mid stride in that stride however he was
// hung: picked up by the head, his thigh could come no nearer than thirty
// degrees to the way it pointed when he was shot, and his shin hung from it,
// so he dangled sitting down. What the hip and the shoulder can really do is
// kept by CG_RagdollJointRanges, which never lets go.
static float CG_RagdollLimpness(const cg_ragdoll_t *rd);

static void CG_RagdollCones(cg_ragdoll_t *rd)
{
    const float limp = CG_RagdollLimpness(rd);
    vec3_t      torso[3];
    int         i, k;

    if (limp <= 0.0f || !CG_RagdollTorsoFrame(rd, torso)) {
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

        limit = cos((rd_cones[i].maxDeg + (180.0f - rd_cones[i].maxDeg) * (1.0f - limp)) * (float)M_PI / 180.0f);
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

// What the hip and the shoulder can actually do, which the death-pose cones
// above only stood in for. Each limb's direction is taken apart into how far
// it has swung forward or back (flexion) and how far out to the side or in
// across the body (abduction), measured against the pelvis for a leg and the
// line of the shoulders for an arm, and each is held within the range a man's
// joint has. Straight down along the body is zero on both.
//
// Unlike the cones this does not fade: a limp leg still cannot fold up behind
// the back. The ranges are wide, so a body hangs, sprawls and folds freely
// inside them.
typedef struct {
    short root, tip;  // the joint the limb swings about, and the one it swings
    short left;       // which side of the body, so outward can be told apart
    short leg;        // measured against the pelvis rather than the shoulders
    float flexMin, flexMax; // degrees, forward positive
    float abdMin, abdMax;   // degrees, outward positive
} rdJointRange_t;

static const rdJointRange_t rd_jointRanges[] = {
    {RD_LTHIGH, RD_LCALF, 1, 1, -45.0f, 130.0f, -25.0f, 70.0f },
    {RD_RTHIGH, RD_RCALF, 0, 1, -45.0f, 130.0f, -25.0f, 70.0f },
    {RD_LUARM,  RD_LFARM, 1, 0, -60.0f, 180.0f, -45.0f, 180.0f},
    {RD_RUARM,  RD_RFARM, 0, 0, -60.0f, 180.0f, -45.0f, 180.0f},
};

#define RD_NUM_JOINT_RANGES ((int)(sizeof(rd_jointRanges) / sizeof(rd_jointRanges[0])))

// Share of the joint ranges' strength kept while the grabber holds the body.
#define RD_RANGE_HELD 0.1f

static void CG_RagdollJointRanges(cg_ragdoll_t *rd)
{
    // Taking over from the cones as they let go, rather than alongside them
    // from the moment of death: measured against a pelvis pitched forward by
    // a running stride, the trailing leg of a man shot mid step is well past
    // any range a standing man has, and yanked into it at once the legs
    // scissored through each other and wrung the body round.
    float       strength = 1.0f - CG_RagdollLimpness(rd);
    vec3_t      up;
    int         i, k;

    // Held by the grabber, the ranges only lean on the limbs. The beam moves
    // the body faster than anything else does, and a range applied at full
    // strength to a body being swung about took each limb back by the whole
    // excess on every pass and threw it out again on the next: held by a knee
    // or a thigh and swung, the pelvis flapped up and down on alternate steps.
    // Measured, a tenth of the strength takes the joints reversing like that
    // down four to eight times, and the jitter and spin of a body simply held
    // up down with them; a fifth was worse than either.
    if (rd->grabJoint >= 0) {
        strength *= RD_RANGE_HELD;
    }

    if (strength <= 0.0f) {
        return;
    }

    VectorSubtract(rd->part[RD_SPINE1].p, rd->part[RD_PELVIS].p, up);
    if (VectorNormalize(up) < 0.001f) {
        return;
    }

    for (i = 0; i < RD_NUM_JOINT_RANGES; i++) {
        const rdJointRange_t *jr   = &rd_jointRanges[i];
        rdParticle_t         *root = &rd->part[jr->root];
        rdParticle_t         *tip  = &rd->part[jr->tip];
        vec3_t                right, fwd, out, down, dir, sag, want, target, delta;
        float                 len, flex, abd, flexTo, abdTo, total;

        // The body's own frame at this joint: up the spine, across the hips or
        // the shoulders, and forward.
        if (jr->leg) {
            VectorSubtract(rd->part[RD_RTHIGH].p, rd->part[RD_LTHIGH].p, right);
        } else {
            VectorSubtract(rd->part[RD_RUARM].p, rd->part[RD_LUARM].p, right);
        }
        VectorMA(right, -DotProduct(right, up), up, right);
        if (VectorNormalize(right) < 0.001f) {
            continue;
        }
        CrossProduct(up, right, fwd);

        VectorScale(right, jr->left ? -1.0f : 1.0f, out);
        VectorNegate(up, down);

        VectorSubtract(tip->p, root->p, dir);
        len = VectorNormalize(dir);
        if (len < 0.001f) {
            continue;
        }

        flex = RAD2DEG(atan2(DotProduct(dir, fwd), DotProduct(dir, down)));
        abd  = RAD2DEG(asin(Q_clamp_float(DotProduct(dir, out), -1.0f, 1.0f)));

        // Flexion goes round a circle, and its range has to be read that way.
        // An arm raised overhead sits at a hundred and eighty; the least
        // movement behind the head takes it to minus a hundred and seventy
        // nine, which clamped as a plain number is more than a hundred degrees
        // behind the back and was yanked there in a single step. Hung from a
        // leg, a body has its arms overhead the whole time, and they whipped
        // about as fast as the solver could move them. Outside the range, the
        // nearer edge is the one it goes back to, measured round the circle.
        flexTo = flex;
        if (flex < jr->flexMin || flex > jr->flexMax) {
            const float toMin = fabs(AngleNormalize180(flex - jr->flexMin));
            const float toMax = fabs(AngleNormalize180(flex - jr->flexMax));

            flexTo = toMin < toMax ? jr->flexMin : jr->flexMax;
        }

        // Swung right out to the side, a limb has no forward or back to speak
        // of, and the angle that says which it is swings about wildly for the
        // least movement; so flexion lets go there.
        flexTo = flex + AngleNormalize180(flexTo - flex) * Q_clamp_float((85.0f - fabs(abd)) / 20.0f, 0.0f, 1.0f);

        abdTo = Q_clamp_float(abd, jr->abdMin, jr->abdMax);

        if (fabs(AngleNormalize180(flexTo - flex)) < 0.01f && abdTo == abd) {
            continue;
        }

        // Put back together from the two angles, the way they were taken
        // apart: swung forward within the plane of the body, then out of it.
        for (k = 0; k < 3; k++) {
            sag[k] = cos(DEG2RAD(flexTo)) * down[k] + sin(DEG2RAD(flexTo)) * fwd[k];
        }
        for (k = 0; k < 3; k++) {
            want[k] = cos(DEG2RAD(abdTo)) * sag[k] + sin(DEG2RAD(abdTo)) * out[k];
        }
        VectorMA(root->p, len, want, target);

        // Shared by mass and only partly applied, as the cones are.
        VectorSubtract(target, tip->p, delta);
        total = root->invMass + tip->invMass;
        if (total < 0.0001f) {
            continue;
        }

        VectorMA(tip->p, strength * RD_CONE_RATE * (tip->invMass / total), delta, tip->p);
        VectorMA(root->p, -strength * RD_CONE_RATE * (root->invMass / total), delta, root->p);

        VectorMA(rd->rangeShift[jr->tip], strength * RD_CONE_RATE * (tip->invMass / total), delta, rd->rangeShift[jr->tip]);
        VectorMA(rd->rangeShift[jr->root], -strength * RD_CONE_RATE * (root->invMass / total), delta, rd->rangeShift[jr->root]);
    }
}

// Where the front of a limb faces when it has swung from hanging straight down
// to where it now points without turning about its own length: the reference
// that its twist is measured against. front0 is the front with the limb
// hanging down. swingDeg is how far it has swung, 180 being straight up along
// the body, where no such reference exists.
static void CG_RagdollSwingFront(
    const vec3_t down, const vec3_t front0, const vec3_t limbDir, vec3_t out, float *swingDeg
)
{
    vec3_t axis;
    float  c = Q_clamp_float(DotProduct(down, limbDir), -1.0f, 1.0f);

    *swingDeg = RAD2DEG(acos(c));
    CrossProduct(down, limbDir, axis);

    if (VectorNormalize(axis) < 0.0001f) {
        VectorCopy(front0, out);
        return;
    }

    RotatePointAroundVector(out, axis, front0, *swingDeg);
}

// How much a twist range may act for a limb swung this far. Straight up along
// the body the reference above turns on a pin and means nothing, so the range
// lets go over the last thirty degrees before it.
static float CG_RagdollSwingTrust(float swingDeg)
{
    return Q_clamp_float((170.0f - swingDeg) / 30.0f, 0.0f, 1.0f);
}

// The signed angle from a to b about axis, both taken square to it.
static float CG_RagdollTwistAbout(const vec3_t axis, const vec3_t a, const vec3_t b)
{
    vec3_t pa, pb, c;

    VectorMA(a, -DotProduct(a, axis), axis, pa);
    VectorMA(b, -DotProduct(b, axis), axis, pb);
    CrossProduct(pa, pb, c);

    return RAD2DEG(atan2(DotProduct(c, axis), DotProduct(pa, pb)));
}

// Where a twist that is out of its range [lo, hi] should be brought back to.
// Turned nearly all the way round, a joint is as close to one edge as the
// other, and the nearer one flips from frame to frame; so the first edge it
// was sent to is kept in *edge until it is back inside.
static float CG_RagdollTwistEdge(float turn, float lo, float hi, signed char *edge)
{
    if (turn >= lo && turn <= hi) {
        *edge = 0;
        return turn;
    }

    if (!*edge) {
        // The nearer edge, going the short way round the circle.
        const float toLo = fabs(AngleNormalize180(turn - lo));
        const float toHi = fabs(AngleNormalize180(turn - hi));

        *edge = toLo < toHi ? -1 : 1;
    }

    return *edge < 0 ? lo : hi;
}

// How far the elbow may point from straight back, either way, before the arm
// reads as bending the wrong way.
//
// This is what a shoulder's twist amounts to: the upper arm turned about its
// own length carries the elbow round with it. 45 degrees is the usual figure
// for a shoulder's twist either way. It was 100, taken back a tenth a pass,
// which let arms read as bent backwards and could not even hold the 100.
#define RD_ELBOW_TURN 55.0f

// Share of an elbow's excess turn taken back each pass, and the most it may
// be turned in one pass, in degrees. The whole of the excess: a limit held
// softly is one the body can lean on, and the arm settles past it.
#define RD_ELBOW_RATE     1.0f
#define RD_ELBOW_MAX_STEP 30.0f

// Swing, from hanging straight down, over which the range fades out. With the
// arm pointing up, which way is straight back has no answer: the reference
// turns right round for a tiny movement of the arm, and a range held against
// it whips the elbow round with it. Held on toward overhead, as it was up to
// 140 degrees, a body hung by a hip, a hand or the pelvis, whose arms hang
// toward the head, was spun measurably faster.
#define RD_ELBOW_FADE_FROM 80.0f
#define RD_ELBOW_FADE_TO   110.0f

// How far the elbow has to stand off the line from shoulder to hand, in units,
// before its range starts to act, and how much further before it acts fully.
#define RD_ELBOW_BENT_MIN  1.0f
#define RD_ELBOW_BENT_FULL 4.0f

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


// What the elbow can do, which nothing held before: the knees have their
// hinge above, but an elbow's bend turns with the upper arm, and the shoulder
// rotates freely enough that no plane fixed to the body will do. So instead of
// a plane, a range. The arm's swing is followed to find which way straight back
// is for an arm pointing where this one points, and the elbow may point
// anywhere within RD_ELBOW_TURN of that. Past it the arm reads as bent the
// wrong way, and the elbow is turned back about the line from shoulder to hand,
// which leaves both bones their lengths.
//
// Only a bent elbow has a direction, so a nearly straight arm is left alone.
// Faded in as the body goes limp, like the other ranges.
static void CG_RagdollElbows(cg_ragdoll_t *rd)
{
    static const short arms[2][3] = {
        {RD_LUARM, RD_LFARM, RD_LHAND},
        {RD_RUARM, RD_RFARM, RD_RHAND},
    };
    const float strength = 1.0f - CG_RagdollLimpness(rd);
    vec3_t      torso[3], down, back;
    int         i, k;

    
    if (strength <= 0.0f || !CG_RagdollTorsoFrame(rd, torso)) {
        return;
    }

    VectorNegate(torso[0], down);
    VectorNegate(torso[2], back);

    for (i = 0; i < 2; i++) {
        rdParticle_t *shoulder = &rd->part[arms[i][0]];
        rdParticle_t *elbow    = &rd->part[arms[i][1]];
        rdParticle_t *hand     = &rd->part[arms[i][2]];
        vec3_t        upper, chord, rel, offset, ref, dir, target;
        float         along, bentBy, swing, trust, turn, to;

        VectorSubtract(elbow->p, shoulder->p, upper);
        VectorSubtract(hand->p, shoulder->p, chord);

        if (VectorNormalize(upper) < 0.001f || VectorNormalize(chord) < 0.001f) {
            continue;
        }

        VectorSubtract(elbow->p, shoulder->p, rel);
        along = DotProduct(rel, chord);
        VectorMA(rel, -along, chord, offset);
        bentBy = VectorNormalize(offset);

        // Nearly straight, it has hardly any direction to hold, and what it
        // has swings wildly for a tiny movement of the hand; so the range
        // comes in only as the elbow bends.
        CG_RagdollSwingFront(down, back, upper, ref, &swing);
        trust = Q_clamp_float((RD_ELBOW_FADE_TO - swing) / (RD_ELBOW_FADE_TO - RD_ELBOW_FADE_FROM), 0.0f, 1.0f)
              * strength
              * Q_clamp_float((bentBy - RD_ELBOW_BENT_MIN) / RD_ELBOW_BENT_FULL, 0.0f, 1.0f);
        if (trust <= 0.0f) {
            rd->elbowEdge[i] = 0;
            continue;
        }

        turn = CG_RagdollTwistAbout(chord, ref, offset);
        to   = CG_RagdollTwistEdge(turn, -RD_ELBOW_TURN, RD_ELBOW_TURN, &rd->elbowEdge[i]);
        if (to == turn) {
            continue;
        }

        {
            // Never snapped: an elbow a long way out of range, turned right
            // round, is brought back over a few passes rather than thrown
            // there in one, which read as the arm whipping about.
            const float step = AngleNormalize180(to - turn) * trust * RD_ELBOW_RATE;

            to = turn + Q_clamp_float(step, -RD_ELBOW_MAX_STEP, RD_ELBOW_MAX_STEP);
        }

        VectorMA(ref, -DotProduct(ref, chord), chord, ref);
        if (VectorNormalize(ref) < 0.001f) {
            continue;
        }
        RotatePointAroundVector(dir, chord, ref, to);

        for (k = 0; k < 3; k++) {
            target[k] = shoulder->p[k] + chord[k] * along + dir[k] * bentBy;
        }

        if (elbow->invMass > 0.0f) {
            vec3_t moved;

            // A limit, like the joint ranges: it moves the elbow and hands
            // none of that back as speed (CG_RagdollSolveTracked).
            VectorSubtract(target, elbow->p, moved);

            // Never into what the elbow is lying on. Pushed into the floor,
            // the floor pushes back, and a settled arm is turned a little
            // back and forth for as long as it lies there. Along the floor
            // it still goes.
            if (elbow->hasContact) {
                const float into = DotProduct(moved, elbow->contactNormal);

                if (into < 0.0f) {
                    VectorMA(moved, -into, elbow->contactNormal, moved);
                }
            }

            VectorAdd(rd->rangeShift[arms[i][1]], moved, rd->rangeShift[arms[i][1]]);
            VectorAdd(elbow->p, moved, elbow->p);
        }
    }
}

//=============================================================
// Limb twist
//=============================================================

// The particles are points, so nothing in them says how an upper arm or a
// thigh is turned about its own length. While the elbow or knee is bent, the
// plane it bends in says it, and the bone is drawn rolled to that plane. A
// straight limb says nothing, and it used to keep whatever roll it had last:
// an arm that straightened while turned the wrong way stayed turned the wrong
// way, palms behind the body, and a straight leg could lie twisted half round
// with nothing to bring it back.
//
// So the roll of each upper arm and thigh is kept here, as a direction square
// to the bone: the way its joint bends (an elbow forward, a knee back). It is
// carried along with the bone as it swings, which is what a limb with nothing
// turning it does; held within how far a shoulder or a hip turns the bone
// about its length, measured from the limb's swing (CG_RagdollSwingFront); and
// the more the joint is bent, the more it follows the plane the joint actually
// bends in, so a bent elbow or knee is drawn on its hinge. Which way the joint
// may bend stays with the particles, in CG_RagdollHinges and CG_RagdollElbows:
// turning the forearm or shin round here as well was tried, and it set a held
// body spinning.
typedef struct {
    short a, mid, end; // shoulder or hip, elbow or knee, hand or foot
    short leg;
    short left;
} rdTwistLimb_t;

static const rdTwistLimb_t rd_twistLimbs[4] = {
    {RD_LUARM,  RD_LFARM, RD_LHAND, 0, 1},
    {RD_RUARM,  RD_RFARM, RD_RHAND, 0, 0},
    {RD_LTHIGH, RD_LCALF, RD_LFOOT, 1, 1},
    {RD_RTHIGH, RD_RCALF, RD_RFOOT, 1, 0},
};

// How far the shoulder turns the upper arm about its length either way, and
// the hip the thigh, in toward the other leg and out, from the joint bending
// straight forward (elbow) or back (knee).
#define RD_ARM_TWIST     85.0f
#define RD_HIP_TWIST_IN  35.0f
#define RD_HIP_TWIST_OUT 60.0f

// The most the roll may be turned back into range in one pass, and the most
// it may turn in one pass to follow the joint, in degrees. The second is what
// keeps a joint passing through straight to its other side from snapping the
// limb half round in a frame.
#define RD_TWIST_STEP  2.0f
#define RD_FOLLOW_STEP 8.0f

// Below this much bend an elbow or knee has no plane to follow, and it is
// followed fully from RD_HINGE_BEND_FULL degrees more.
#define RD_HINGE_BEND_MIN  5.0f
#define RD_HINGE_BEND_FULL 15.0f

// Which limb of rd_twistLimbs a bone of rd_bones is the upper bone of, or -1.
static int CG_RagdollTwistSlot(int boneNum)
{
    int t;

    for (t = 0; t < 4; t++) {
        if (rd_bones[boneNum].joint == rd_twistLimbs[t].a && rd_bones[boneNum].aim == rd_twistLimbs[t].mid) {
            return t;
        }
    }

    return -1;
}

// v turned by the shortest turn that takes direction from onto direction to.
static void CG_RagdollCarry(const vec3_t from, const vec3_t to, const vec3_t v, vec3_t out)
{
    vec3_t axis;
    float  sinA, cosA;

    CrossProduct(from, to, axis);
    sinA = VectorNormalize(axis);
    cosA = DotProduct(from, to);

    if (sinA < 0.0001f) {
        VectorCopy(v, out);
        return;
    }

    RotatePointAroundVector(out, axis, v, RAD2DEG(atan2(sinA, cosA)));
}

// Squares v to dir and normalises it. False if nothing is left of it.
static qboolean CG_RagdollSquareTo(vec3_t v, const vec3_t dir)
{
    VectorMA(v, -DotProduct(v, dir), dir, v);
    return VectorNormalize(v) > 0.001f ? qtrue : qfalse;
}

// Takes over the roll of each upper arm and thigh from the pose last drawn,
// as the body leaves the blend, so nothing moves when it does.
static void CG_RagdollStartTwist(cg_ragdoll_t *rd)
{
    int t, b;

    for (t = 0; t < 4; t++) {
        vec3_t dir;

        VectorSubtract(rd->part[rd_twistLimbs[t].mid].p, rd->part[rd_twistLimbs[t].a].p, dir);
        if (VectorNormalize(dir) < 0.001f) {
            VectorSet(dir, 0, 0, -1);
        }

        VectorSet(rd->limbY[t], 1, 0, 0);
        for (b = 0; b < RD_NUM_BONES; b++) {
            if (CG_RagdollTwistSlot(b) == t) {
                VectorCopy(rd->transportY[b], rd->limbY[t]);
                break;
            }
        }

        if (!CG_RagdollSquareTo(rd->limbY[t], dir)) {
            VectorSet(rd->limbY[t], fabs(dir[2]) > 0.9f ? 1 : 0, 0, fabs(dir[2]) > 0.9f ? 0 : 1);
            CG_RagdollSquareTo(rd->limbY[t], dir);
        }

        VectorCopy(dir, rd->limbDir[t]);
        rd->limbEdge[t] = 0;
    }

    rd->limbTwistLive = qtrue;
}

// Brings a roll back toward [lo, hi] about axis, from ref, by no more than
// RD_TWIST_STEP and scaled by trust. sign flips the sense, so a hip's outward
// turn is measured the same way on both sides.
static void CG_RagdollHoldTwist(
    vec3_t y, const vec3_t axis, const vec3_t ref, float lo, float hi, float sign, float trust, signed char *edge
)
{
    const float turn = CG_RagdollTwistAbout(axis, ref, y) * sign;
    float       to   = CG_RagdollTwistEdge(turn, lo, hi, edge);
    vec3_t      turned;

    if (to == turn || trust <= 0.0f) {
        return;
    }

    to = Q_clamp_float(AngleNormalize180(to - turn) * trust, -RD_TWIST_STEP, RD_TWIST_STEP);
    RotatePointAroundVector(turned, axis, y, to * sign);
    VectorCopy(turned, y);
}

// Once a frame, after the solver.
static void CG_RagdollLimbTwist(cg_ragdoll_t *rd)
{
    vec3_t chest[3], hipUp, hipRight, hipFwd;
    int    t;

    if (!rd->limbTwistLive || rd->state != RD_ACTIVE || !CG_RagdollTorsoFrame(rd, chest)) {
        return;
    }

    VectorSubtract(rd->part[RD_SPINE1].p, rd->part[RD_PELVIS].p, hipUp);
    VectorSubtract(rd->part[RD_RTHIGH].p, rd->part[RD_LTHIGH].p, hipRight);
    if (VectorNormalize(hipUp) < 0.001f || !CG_RagdollSquareTo(hipRight, hipUp)) {
        return;
    }
    CrossProduct(hipUp, hipRight, hipFwd);

    for (t = 0; t < 4; t++) {
        const rdTwistLimb_t *L   = &rd_twistLimbs[t];
        const float         *up  = L->leg ? hipUp : chest[0];
        const float         *fwd = L->leg ? hipFwd : chest[2];
        vec3_t               dir, lower, y, down, neutral, ref;
        float                bend, follow, swing;

        VectorSubtract(rd->part[L->mid].p, rd->part[L->a].p, dir);
        VectorSubtract(rd->part[L->end].p, rd->part[L->mid].p, lower);
        if (VectorNormalize(dir) < 0.001f || VectorNormalize(lower) < 0.001f) {
            continue;
        }

        // Carried along with the bone as it has swung since the last frame.
        CG_RagdollCarry(rd->limbDir[t], dir, rd->limbY[t], y);
        if (CG_RagdollSquareTo(y, dir)) {
            VectorCopy(y, rd->limbY[t]);
        }
        VectorCopy(dir, rd->limbDir[t]);

        bend   = RAD2DEG(acos(Q_clamp_float(DotProduct(dir, lower), -1.0f, 1.0f)));
        follow = Q_clamp_float((bend - RD_HINGE_BEND_MIN) / RD_HINGE_BEND_FULL, 0.0f, 1.0f);

        // Held within the shoulder's or hip's range, as far as the joint below
        // is not already saying where the roll is.
        VectorNegate(up, down);
        VectorScale(fwd, L->leg ? -1.0f : 1.0f, neutral);
        CG_RagdollSwingFront(down, neutral, dir, ref, &swing);

        if (follow < 1.0f && CG_RagdollSquareTo(ref, dir)) {
            const float trust = CG_RagdollSwingTrust(swing) * (1.0f - follow);

            if (L->leg) {
                vec3_t out;

                // Y is the back of the knee, so turning it outward is the
                // thigh turning in.
                VectorScale(hipRight, L->left ? -1.0f : 1.0f, out);
                CG_RagdollHoldTwist(
                    rd->limbY[t], dir, ref, -RD_HIP_TWIST_OUT, RD_HIP_TWIST_IN,
                    CG_RagdollTwistAbout(dir, ref, out) < 0.0f ? -1.0f : 1.0f, trust, &rd->limbEdge[t]
                );
            } else {
                CG_RagdollHoldTwist(rd->limbY[t], dir, ref, -RD_ARM_TWIST, RD_ARM_TWIST, 1.0f, trust, &rd->limbEdge[t]);
            }
        }

        // Turned toward the plane the joint bends in, as far as it is bent.
        if (follow > 0.0f) {
            const float roll =
                Q_clamp_float(CG_RagdollTwistAbout(dir, rd->limbY[t], lower) * follow, -RD_FOLLOW_STEP, RD_FOLLOW_STEP);

            RotatePointAroundVector(y, dir, rd->limbY[t], roll);
            VectorCopy(y, rd->limbY[t]);
        }
    }
}

// Keeps whole limb bones out of the head and the trunk. Testing only the joints
// at each end of a bone is not enough: a forearm can lie straight through the
// head with the elbow out one side and the wrist out the other, and every joint
// to joint distance still perfectly satisfied. That is exactly what an arm
// clipping through the head looks like.
// One limb against another.
//
// The joint-to-joint pairs in CG_RagdollSelfCollide cannot see this: two bones
// can cross at their middles with all four of their ends comfortably apart, so
// a shin passes through the other shin and nothing objects.
static float CG_RagdollLimpness(const cg_ragdoll_t *rd);

static void CG_RagdollLimbCollide(cg_ragdoll_t *rd)
{
    const float rate = cg_ragdoll_limbpush->value;
    int         n, m, k;

    if (rate <= 0.0f) {
        return;
    }

    for (n = 0; n < RD_NUM_LIMB_SEGMENTS; n++) {
        rdParticle_t *pa = &rd->part[rd_limbSegments[n].a];
        rdParticle_t *pb = &rd->part[rd_limbSegments[n].b];

        for (m = n + 1; m < RD_NUM_LIMB_SEGMENTS; m++) {
            rdParticle_t *pc = &rd->part[rd_limbSegments[m].a];
            rdParticle_t *pd = &rd->part[rd_limbSegments[m].b];
            vec3_t        dir, push;
            float         ta, tb, dist, scale, want, keep;

            if (rd->segLimbScale[n][m] <= 0.0f) {
                continue;
            }

            dist = CG_RagdollSegmentToSegment(pa->p, pb->p, pc->p, pd->p, &ta, &tb, dir);

            if (dist < 0.0001f) {
                continue;
            }

            keep = rd->segLimbScale[n][m];

            // The legs are what a player actually catches crossing. Of the
            // corpses with a limb well inside another limb, sixteen in
            // eighteen were a calf or a foot against the other leg, and the
            // worst of them had the two shins very nearly coincident. Raising
            // the push rate does not reach it: it flattens off above 0.2 with
            // the crossing still at thirty per cent, which says the room the
            // two are asked to keep is too small rather than that they are
            // pushed apart too gently. Trouser and boot are wider than the
            // bone.
            //
            // Ramped, but over a quarter of a second rather than over the
            // limpness clock the hands use. Applied at once it stretches the
            // body past its own standing height and turns the ankles over,
            // since a leg is heavy and shoving one sideways drags the pelvis
            // with it. Applied as slowly as the hands it does almost nothing:
            // the corpse has come to rest by the time the room is asked for,
            // and friction holds the legs where they crossed. The separating
            // has to happen while the body is still moving.
            if (cg_ragdoll_legfree->value > 0.0f && n >= RD_FIRST_LEG_SEGMENT && m >= RD_FIRST_LEG_SEGMENT) {
                const float room = 1.0f + cg_ragdoll_legfree->value;
                float       open = (float)(cg.time - rd->startTime) / (float)RD_LEG_OPEN_MS;

                if (open > 1.0f) {
                    open = 1.0f;
                }

                if (open > 0.0f && room > keep) {
                    keep += (room - keep) * open;
                }
            }

            want = (rd->limbRadius[n] + rd->limbRadius[m]) * keep - rd->collisionSlop;

            if (want <= 0.0f || dist >= want) {
                continue;
            }

            VectorScale(dir, 1.0f / dist, push);
            scale = (want - dist) * rate;

            // Shared evenly between the two, and along each bone in proportion
            // to where the contact falls, so a touch near one end moves that end.
            for (k = 0; k < 3; k++) {
                pa->p[k] += push[k] * scale * 0.5f * (1.0f - ta);
                pb->p[k] += push[k] * scale * 0.5f * ta;
                pc->p[k] -= push[k] * scale * 0.5f * (1.0f - tb);
                pd->p[k] -= push[k] * scale * 0.5f * tb;
            }
        }
    }
}

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

            float keep = rd->segTrunkScale[n][m];

            if (keep <= 0.0f) {
                continue;
            }

            // The deepest limb inside the body, over a thousand corpses, was a
            // hand in the upper chest, in one in four of them. It is not shape
            // memory: these two segments are seeded at their full clearance
            // already, so letting them recover it changes nothing at all. The
            // clearance itself is too small. A hand's collision radius is the
            // bone's, and the hand a player sees is a fist in a sleeve, wider
            // than that and attached to an arm that can lie flat along the
            // chest, so the solver holds the capsules apart correctly and the
            // mesh still overlaps.
            //
            // Doubled once the labelled screenshots came in. Eight of eighteen
            // bodies photographed as wrong were an arm inside the torso, and
            // one of them settles it: an officer in a leather greatcoat, whose
            // arm leaves his body at the waist rather than at a shoulder. The
            // bone is outside the capsule and the sleeve is inside the coat.
            // The capsule is not the body a player sees, so clearing it is not
            // enough and holding the arm a little beyond it is right rather
            // than excessive.
            //
            // So ask these two segments to keep more room than they need, and
            // ramp it in on the limpness clock rather than applying it at once,
            // which would lift the hands off the chest on the first frame in
            // front of the player. Only the two forearm-to-hand segments: a
            // thigh given extra clearance has nowhere to put it.
            if (cg_ragdoll_armfree->value > 0.0f && (n == 1 || n == 3)) {
                const float want = 1.0f + cg_ragdoll_armfree->value;

                if (want > keep) {
                    keep += (want - keep) * (1.0f - CG_RagdollLimpness(rd));
                }
            }

            dist = CG_RagdollSegmentToSegment(pa->p, pb->p, pc->p, pd->p, &ta, &tb, dir);

            if (dist < 0.0001f) {
                continue;
            }

            want = (CG_RagdollTrunkClearance(rd, m, torso, dir, dist) + rd->limbRadius[n]) * keep
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
// How far the shoulders may wind round from the hips, about the line of the
// spine. A living trunk turns some way and then stops, and nothing here was
// stopping it.
//
// Every brace holding the shoulders on -- shoulder to shoulder, hip to hip, and
// both of the pelvis to shoulder braces -- is symmetric about the spine axis,
// and turning the shoulders about that axis changes the length of not one of
// them. A distance constraint anchored on an axis cannot resist rotation about
// it, whatever its stiffness, so the trunk had no torsional limit of any kind
// and the chest was free to wind round until something else stopped it.
// Measured in game it wound past twenty degrees in more than half of all
// corpses, which is the single thing that most reads as wrong about them.
//
// Adding a diagonal brace, hip to opposite shoulder, is the ordinary way to
// stiffen a frame against racking and was tried first. It moved the measurement
// by three tenths of a degree and cost two scenarios: a diagonal resists a
// little at every angle rather than nothing until the limit and then firmly,
// and what is wanted here is a limit.
//
// So this measures the angle directly and turns the shoulders back about the
// spine when it is exceeded.
//
// It used to turn only the shoulders, leaving the hips as the anchor. But a
// correction is also a push, since the solver hands what it moves back to the
// body as speed (cg_ragdoll_solvegain), and a push on one end of the trunk
// alone sets the whole body turning. On the floor that is lost in friction;
// hung from the grabber, with the legs below winding the trunk past its limit
// every step, it was a corpse that spun and never stopped. Shoulders and hips
// now turn back half each, the opposite ways, which untwists the trunk the
// same and turns the body not at all.
static void CG_RagdollSpineTwist(cg_ragdoll_t *rd)
{
    const float lim = cg_ragdoll_spinetwist->value;
    vec3_t      axis, hip, sho, side;
    float       over, ang;
    int         k;

    if (lim <= 0.0f) {
        return;
    }

    VectorSubtract(rd->part[RD_SPINE2].p, rd->part[RD_PELVIS].p, axis);

    if (VectorNormalize(axis) < 0.001f) {
        return;
    }

    VectorSubtract(rd->part[RD_RTHIGH].p, rd->part[RD_LTHIGH].p, hip);
    VectorSubtract(rd->part[RD_RUARM].p, rd->part[RD_LUARM].p, sho);

    // Squared to the spine, so what is left is the turn about it and not the
    // trunk leaning or bending.
    VectorMA(hip, -DotProduct(hip, axis), axis, hip);
    VectorMA(sho, -DotProduct(sho, axis), axis, sho);

    if (VectorNormalize(hip) < 0.001f || VectorNormalize(sho) < 0.001f) {
        return;
    }

    CrossProduct(axis, hip, side);

    ang = (float)(atan2(DotProduct(sho, side), DotProduct(sho, hip)) * 180.0 / M_PI);

    // Measured the whole way round. Both lines run from left to right, so
    // they are arrows, and a turn past a quarter is a turn past a quarter.
    // This used to fold them as lines, which read a body wrung 120 degrees as
    // one wrung 60 the other way: the limit then turned it further round, and a
    // corpse flung hard enough finished facing one way above the waist and the
    // other below, and stayed like that.

    if (ang > lim) {
        over = ang - lim;
    } else if (ang < -lim) {
        over = ang + lim;
    } else {
        return;
    }

    // Partial, like every other correction here. Taken out whole it feeds the
    // corpse energy faster than the damping removes it and the trunk rings.
    over *= RD_SPINE_TWIST_RATE;

    for (k = 0; k < 2; k++) {
        rdParticle_t *part = &rd->part[k ? RD_RUARM : RD_LUARM];
        vec3_t        rel, turned;

        VectorSubtract(part->p, rd->part[RD_SPINE2].p, rel);
        RotatePointAroundVector(turned, axis, rel, -over * 0.5f);
        VectorAdd(rd->part[RD_SPINE2].p, turned, part->p);
    }

    // The hips' half, the other way about the pelvis.
    for (k = 0; k < 2; k++) {
        rdParticle_t *part = &rd->part[k ? RD_RTHIGH : RD_LTHIGH];
        vec3_t        rel, turned;

        VectorSubtract(part->p, rd->part[RD_PELVIS].p, rel);
        RotatePointAroundVector(turned, axis, rel, over * 0.5f);
        VectorAdd(rd->part[RD_PELVIS].p, turned, part->p);
    }
}

//=============================================================
// One body against another
//=============================================================

// The trunk and the limbs as one list, so a body can be tested against another
// body without caring which is which. Twelve capsules a man.
#define RD_NUM_BODY_SEGMENTS (RD_NUM_LIMB_SEGMENTS + RD_NUM_TRUNK_SEGMENTS)

static void CG_RagdollBodySegment(const cg_ragdoll_t *rd, int n, int *a, int *b, float *radius)
{
    if (n < RD_NUM_LIMB_SEGMENTS) {
        *a      = rd_limbSegments[n].a;
        *b      = rd_limbSegments[n].b;
        *radius = rd->limbRadius[n];
        return;
    }

    n -= RD_NUM_LIMB_SEGMENTS;

    *a      = rd_trunkSegments[n].a;
    *b      = rd_trunkSegments[n].b;

    // The narrow axis of the trunk. A torso is much deeper than it is thick and
    // this pass has no idea which way round two bodies have met, so asking for
    // the wide figure would hold them apart by a torso's width whichever way
    // they are lying and leave a visible gap between two men on a heap.
    *radius = rd->trunkDeep[n];
}

// A sphere round the whole body, used to throw out most pairs before any
// segment is looked at.
static void CG_RagdollUpdateBounds(cg_ragdoll_t *rd)
{
    float worst = 0.0f;
    int   i;

    VectorCopy(rd->part[RD_SPINE1].p, rd->boundCentre);

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        vec3_t d;
        float  len;

        VectorSubtract(rd->part[i].p, rd->boundCentre, d);
        len = VectorLength(d);

        if (len > worst) {
            worst = len;
        }
    }

    rd->boundRadius = worst + rd->radius;
}

// Keeps one corpse out of another, so bodies pile up instead of lying through
// each other.
//
// Every collision pass before this one is a body against itself or against the
// world; two men who fell in the same doorway simply occupied the same space.
//
// A corpse that has gone to sleep is treated as immovable and takes none of the
// correction. That is most of what makes a heap: the body already on the floor
// is what the next one lands on, and waking it so the two can settle together
// costs a great deal and buys a pile that squirms. The one still moving gives
// way instead, which is also the cheaper half.
//
// Both awake, each gives half, and the other body's own pass gives the rest, so
// the pair converges without either being special.
// How fast, in units a second, a moving body has to be driving into a sleeping
// one to wake it. A body settling onto a pile is slower than this and leaves
// the one underneath asleep; one thrown into it is not.
#define RD_BODY_WAKE_SPEED 80.0f

// The most one body is pushed out of another in a single pass, in units.
#define RD_BODY_PUSH_STEP 0.5f

static void CG_RagdollKeepAwake(cg_ragdoll_t *rd);

static void CG_RagdollBodyCollide(cg_ragdoll_t *rd)
{
    const float rate = cg_ragdoll_bodypush->value;
    int         other;

    if (rate <= 0.0f) {
        return;
    }

    for (other = 0; other < MAX_RAGDOLLS; other++) {
        cg_ragdoll_t *od = &cg_ragdolls[other];
        float         apart, reach, share, deepest;
        vec3_t        between, deepestNormal;
        int           n, m, k;

        if (od == rd || od->state == RD_FREE || !od->boundRadius) {
            continue;
        }

        VectorSubtract(od->boundCentre, rd->boundCentre, between);
        apart = VectorLength(between);
        reach = rd->boundRadius + od->boundRadius;

        if (apart > reach) {
            continue;
        }

        // A sleeping body is the floor as far as this is concerned.
        share   = (od->state == RD_SLEEPING) ? 1.0f : 0.5f;
        deepest = 0.0f;

        for (n = 0; n < RD_NUM_BODY_SEGMENTS; n++) {
            int   na, nb;
            float nr;

            CG_RagdollBodySegment(rd, n, &na, &nb, &nr);

            for (m = 0; m < RD_NUM_BODY_SEGMENTS; m++) {
                int    ma, mb;
                float  mr, ta, tb, dist, want, scale;
                vec3_t dir, push;

                CG_RagdollBodySegment(od, m, &ma, &mb, &mr);

                dist = CG_RagdollSegmentToSegment(
                    rd->part[na].p, rd->part[nb].p, od->part[ma].p, od->part[mb].p, &ta, &tb, dir
                );

                if (dist < 0.0001f) {
                    continue;
                }

                want = nr + mr - rd->collisionSlop;

                if (want <= 0.0f || dist >= want) {
                    continue;
                }

                // dir runs from the other body toward this one, so it is the
                // way this one has to move.
                VectorScale(dir, 1.0f / dist, push);

                // Thrown into a sleeping body, this one used to bounce off it as
                // off a wall. Hard enough, the sleeper wakes and takes its half.
                if (od->state == RD_SLEEPING && rd->state == RD_ACTIVE
                    && -DotProduct(rd->part[ta < 0.5f ? na : nb].v, push) > RD_BODY_WAKE_SPEED) {
                    CG_RagdollKeepAwake(od);
                    CG_RagdollLog(od, "woken by another body");
                    share = 0.5f;

                    for (k = 0; k < RD_NUM_JOINTS; k++) {
                        VectorClear(od->part[k].v);
                    }
                }

                // Deepest contact between the two bodies this pass, for the
                // momentum trade below.
                if (want - dist > deepest) {
                    deepest = want - dist;
                    VectorCopy(push, deepestNormal);
                }

                scale = (want - dist) * rate * share;

                // Only so far a pass, and not as speed. A limb found deep
                // inside another body, or squeezed between two of them, was
                // pushed out by the whole overlap on every pass, and ten passes
                // a step threw a knee thirty and forty units in a frame, over
                // and over: the corpse spasming in the pile. The speed of a
                // collision is handed over by the momentum trade below, so the
                // push only has to separate them, which it does over a few
                // steps as well as in one (CG_RagdollSolveTracked).
                if (scale > RD_BODY_PUSH_STEP) {
                    scale = RD_BODY_PUSH_STEP;
                }

                for (k = 0; k < 3; k++) {
                    rd->part[na].p[k] += push[k] * scale * (1.0f - ta);
                    rd->part[nb].p[k] += push[k] * scale * ta;
                    rd->rangeShift[na][k] += push[k] * scale * (1.0f - ta);
                    rd->rangeShift[nb][k] += push[k] * scale * ta;
                }

                // Held up by the other body rather than shouldered aside by it.
                // Only upward pushes count, so two corpses standing against a
                // wall do not each decide the other is the floor.
                if (push[2] > 0.7f) {
                    rd->onBodyMask |= (1 << na) | (1 << nb);
                }
            }
        }

        // And it is knocked along. The push apart above only moves the two
        // bodies out of each other, each by half, which hands over almost none
        // of the speed: a body thrown into another stopped dead against it and
        // the one it hit barely stirred. Traded between the two contact points
        // alone it still barely stirred, because two joints of twenty three
        // take it and the rest of the body, lying on the ground, holds them.
        // So the two bodies trade as whole bodies: how fast they are closing
        // along the deepest contact, taken from each body's average speed, is
        // split between them evenly, with a bounce of its own
        // (cg_ragdoll_bodybounce): bodies bounce off each other far less than
        // off the ground, and sharing the ground's figure made a thrown body
        // spring back off the one it hit. Once
        // done they are no longer closing, so the other body's own pass does
        // not do it again.
        if (deepest > 0.0f && rd->state == RD_ACTIVE && od->state == RD_ACTIVE) {
            vec3_t va, vb;
            float  closing, dv;

            VectorClear(va);
            VectorClear(vb);
            for (k = 0; k < RD_NUM_JOINTS; k++) {
                VectorAdd(va, rd->part[k].v, va);
                VectorAdd(vb, od->part[k].v, vb);
            }

            closing = (DotProduct(va, deepestNormal) - DotProduct(vb, deepestNormal)) / RD_NUM_JOINTS;

            if (closing < 0.0f) {
                dv = -(1.0f + cg_ragdoll_bodybounce->value) * closing * 0.5f;

                for (k = 0; k < RD_NUM_JOINTS; k++) {
                    VectorMA(rd->part[k].v, dv, deepestNormal, rd->part[k].v);
                    VectorMA(od->part[k].v, -dv, deepestNormal, od->part[k].v);
                }
            }
        }
    }
}

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

// Puts the joint the grabber holds back where the step left it.
static void CG_RagdollPinHeld(cg_ragdoll_t *rd, const vec3_t held)
{
    if (rd->grabJoint >= 0) {
        VectorCopy(held, rd->part[rd->grabJoint].p);
    }
}

static void CG_RagdollSolveConstraints(cg_ragdoll_t *rd, int iterations)
{
    const float limp = CG_RagdollLimpness(rd);
    vec3_t      held;
    int it, i, k;

    if (rd->grabJoint >= 0) {
        VectorCopy(rd->part[rd->grabJoint].p, held);
    } else {
        VectorClear(held);
    }

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
        // The joint the grabber holds is weightless to the bone sticks, but
        // several of these move joints directly, whatever their weight. Each
        // is followed by putting the held one back, so they move the rest of
        // the body instead of fighting the beam for it. Left to move it, the
        // beam's spring pulled it straight back on the next step and the two
        // fought for as long as the body was held: measured, a body held by
        // the knee shook three times as hard as one held by the head, and
        // hung wedged sideways instead of below the knee.
        CG_RagdollHinges(rd);
        CG_RagdollPinHeld(rd, held);
        CG_RagdollElbows(rd);
        CG_RagdollPinHeld(rd, held);
        CG_RagdollSpineTwist(rd);
        CG_RagdollPinHeld(rd, held);
        CG_RagdollCones(rd);
        CG_RagdollJointRanges(rd);
        CG_RagdollSelfCollide(rd);
        CG_RagdollPinHeld(rd, held);
        CG_RagdollBodyCollide(rd);
        CG_RagdollSegmentCollide(rd);
        CG_RagdollLimbCollide(rd);
        CG_RagdollPinHeld(rd, held);
        CG_RagdollProjectContacts(rd);
        CG_RagdollPinHeld(rd, held);
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
        CG_RagdollLimbCollide(rd);
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
        VectorClear(rd->rangeShift[i]);
    }

    CG_RagdollSolveConstraints(rd, iterations);

    // A joint range is a limit, not a spring: it says where a limb may not
    // go and has no business pushing. What it moves is left out of the speed
    // the solver hands back, so it puts a limb back in range without flinging
    // it. Counted in, a body hung by its hip, where gravity pulls the leg past
    // the range on every step and the range pulls it back, was given that
    // tug of war as motion each time and spun faster and faster.
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        vec3_t d;

        VectorSubtract(rd->part[i].p, before[i], d);
        VectorSubtract(d, rd->rangeShift[i], d);
        VectorAdd(rd->part[i].solved, d, rd->part[i].solved);
    }
}

static void CG_RagdollTrace(
    trace_t     *result,
    const vec3_t start,
    const vec3_t mins,
    const vec3_t maxs,
    const vec3_t end,
    int          skipNumber,
    int          mask,
    qboolean     cylinder,
    qboolean     cliptoentities,
    const char  *description
);

// The body whose step is being traced, so the props it ignores can be skipped.
static const cg_ragdoll_t *rd_traceBody;
static void                CG_RagdollReleaseEnclosingProps(cg_ragdoll_t *rd);

// How big a box to trace for one joint against the world.
//
// Every trace here used a single radius for all twenty three joints, three
// units scaled to the model, while the table the model was built against gives
// each its own: a head is 4.5 and a pelvis 5.5, a wrist 2.8. A head resting on
// the floor was therefore held three units clear when it needs four and a half,
// so it sat a unit and a half inside the ground, which is exactly the complaint
// it drew. A wrist was being held further off than it should be.
//
// These figures are already measured and already scaled to this model. They
// were simply not used for anything except keeping limbs out of each other.
static void CG_RagdollJointBox(const cg_ragdoll_t *rd, int joint, vec3_t mins, vec3_t maxs)
{
    float r = rd->radius;
    int   k;

    // The head, and at 2 the forearms and hands as well; never the whole
    // skeleton.
    //
    // Given to every joint this is a clear loss: the trunk figures are half a
    // torso's width, which is the right thing to keep another limb out of and
    // much too much to hold the body off the floor with, and a body resting on
    // a pelvis inflated from three units to five and a half perches on ledges
    // it should roll off. Seven scenarios broke and two mended.
    //
    // The forearms and hands were added after a corpse was photographed with an
    // arm through the ground while every measurement of it came back clean: it
    // had never touched geometry, it had settled, and it was drawn within two
    // units of its own particles. The bone was where it belonged and the sleeve
    // was in the floor. Gridded, that change improves a limb in the trunk in
    // four configurations of four, twist in three, crossing in three, and gains
    // two scenarios while losing none.
    //
    // The head is the case where the single figure is plainly wrong and nothing
    // else depends on it. It is the largest thing on the body after the trunk,
    // it was being held three units clear when the model says four and a half,
    // so it sat a unit and a half inside the ground, and it was photographed
    // doing exactly that twice in one round.
    if (cg_ragdoll_jointsize->integer && (joint == RD_HEAD || joint == RD_HEADTIP
                                          || (cg_ragdoll_jointsize->integer > 1
                                              && (joint == RD_LFARM || joint == RD_LHAND || joint == RD_LHANDTIP
                                                  || joint == RD_RFARM || joint == RD_RHAND || joint == RD_RHANDTIP)))) {
        r = rd->jointRadius[joint];

        if (r < 0.5f) {
            r = 0.5f;
        }
    }

    // At 3, the trunk and the upper and lower limbs too: the body meets the
    // world as thick as it is drawn, as a Half-Life 2 ragdoll's hulls do.
    //
    // The loss measured against this above belongs to a solver long since
    // gone. With the limbs swept through the world as the joints are (see
    // CG_RagdollBoneSweep), the full figures keep every scenario the suite
    // passed, cut the whips by a tenth, and take the trunk from a mean of one
    // and a half units sunk into the floor, three and a half at worst, to a
    // fifth of a unit; the limbs from six tenths to a fifth. Dropped on a beam,
    // a body keeps a quarter less of the sideways speed the landing gives it.
    // The feet and toes keep the single figure; they were not tried.
    if (cg_ragdoll_jointsize->integer > 2
        && (joint <= RD_NECK || joint == RD_LUARM || joint == RD_RUARM || joint == RD_LFARM || joint == RD_RFARM
            || joint == RD_LTHIGH || joint == RD_RTHIGH || joint == RD_LCALF || joint == RD_RCALF)) {
        r = Q_max(r, rd->jointRadius[joint]);
    }

    for (k = 0; k < 3; k++) {
        mins[k] = -r;
        maxs[k] = r;
    }
}

static float CG_RagdollFriction(const cg_ragdoll_t *rd);

// Whether a joint that has just struck a surface is caught on it: the body it
// belongs to has been carried off past the far side, and the surface is the
// only thing between the joint and the joint it hangs from.
//
// Collision always has the last word over the bones, so nothing else here can
// ever let such a joint go. Traced in the game: a corpse lying in a hollow was
// lifted by the head, a forearm and a foot met the underside of the lip above
// them and stayed there, and the body was drawn out to three times its length
// -- pelvis to head from twenty six units to seventy five -- until the arm slid
// out from under the lip a second later and came up a hundred and fifty units
// in one step. A limb hooked under something that far is going to come free;
// letting it through the lip at once is much less wrong to look at than either.
static qboolean CG_RagdollHooked(const cg_ragdoll_t *rd, int joint, const vec3_t normal)
{
    const int parent = rd_joints[joint].parent;
    vec3_t    up;
    float     len;

    if (parent < 0 || rd->boneRest[joint] < 0.5f) {
        return qfalse;
    }

    VectorSubtract(rd->part[parent].p, rd->part[joint].p, up);
    len = VectorLength(up);

    if (len < rd->boneRest[joint] * RD_HOOK_STRETCH || len < rd->boneRest[joint] + RD_HOOK_SLACK) {
        return qfalse;
    }

    // The surface faces away from the parent, so it lies between the two.
    return DotProduct(up, normal) < 0.0f ? qtrue : qfalse;
}

static int CG_RagdollCollide(cg_ragdoll_t *rd, int skipEntity)
{
    // Sized from the model, so a scaled-down character does not end up
    // colliding as though its joints were as fat as a full-size one, and from
    // the joint, so a head is not traced as though it were a wrist.
    vec3_t rd_mins, rd_maxs;

    const int   mask     = RD_CLIPMASK;
    const float friction = CG_RagdollFriction(rd);

    const float bounce   = cg_ragdoll_bounce->value;
    int         moved    = 0;

    rd->impactSpeed = 0.0f;
    int         i, k;

    rd->buriedMask     = 0;
    rd->limbBuriedMask = 0;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];

        // The part the grabber holds goes where the beam takes it, but not
        // through the world. Left out of collision altogether, the beam
        // dragged it through a ledge or a bracket while the rest of the body
        // caught on it, and the limbs were torn round the edges. It is only
        // stopped at a surface it would cross, though: swept from inside a
        // floor it was lying a hair into, the trace would put it back where it
        // started every step, and a body could not be lifted by it at all.
        if (i == rd->grabJoint) {
            trace_t held;

            CG_RagdollJointBox(rd, i, rd_mins, rd_maxs);
            CG_RagdollTrace(&held, part->pPrev, rd_mins, rd_maxs, part->p, skipEntity, mask, qfalse, qtrue, "CG_RagdollCollide");

            if (!held.startsolid && !held.allsolid && held.fraction < 1.0f) {
                const float into = DotProduct(part->v, held.plane.normal);

                VectorCopy(held.endpos, part->p);
                VectorMA(part->p, RD_SURFACE_GAP, held.plane.normal, part->p);
                if (into < 0.0f) {
                    VectorMA(part->v, -into, held.plane.normal, part->v);
                }
                moved++;
            }
            continue;
        }

        CG_RagdollJointBox(rd, i, rd_mins, rd_maxs);
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
            // A plane can hook a joint as well as a surface struck on the way
            // can (see CG_RagdollHooked). A head that had come to rest under a
            // lip never struck it again as the body was lifted: its plane held
            // it where it was, and was confirmed each time the world was asked,
            // since the lip was still there. The neck was drawn out from five
            // units to twenty two and the head then came up fifty seven units
            // in one step.
            if (CG_RagdollHooked(rd, i, part->contactNormal)) {
                part->hasContact = qfalse;
            } else if (DotProduct(part->p, part->contactNormal) - part->contactDist > RD_CONTACT_FORGET) {
                part->hasContact = qfalse;
            } else if (cg.time - part->contactTime > RD_CONTACT_STALE
                       || DistanceSquared(part->p, part->contactAt) > Square(rd->radius * RD_CONTACT_REACH)) {
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
                CG_RagdollTrace(
                    &probe, part->p, rd_mins, rd_maxs, behind, skipEntity, mask, qfalse, qtrue, "CG_RagdollProbe"
                );

                if (probe.fraction >= 1.0f && !probe.startsolid && !probe.allsolid) {
                    part->hasContact = qfalse;
                } else {
                    part->contactTime = cg.time;
                    VectorCopy(part->p, part->contactAt);
                }
            }
        }

        CG_RagdollTrace(&trace, part->pPrev, rd_mins, rd_maxs, part->p, skipEntity, mask, qfalse, qtrue, "CG_RagdollCollide");

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
            CG_RagdollTrace(
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

        // Let through, and the plane it was held under forgotten, or the
        // contact projection would put it straight back.
        if (CG_RagdollHooked(rd, i, trace.plane.normal)) {
            part->hasContact = qfalse;
            continue;
        }

        VectorCopy(trace.endpos, part->p);
        VectorMA(part->p, RD_SURFACE_GAP, trace.plane.normal, part->p);

        part->hasContact  = qtrue;
        part->contactTime = cg.time;
        VectorCopy(part->p, part->contactAt);
        VectorCopy(trace.plane.normal, part->contactNormal);
        part->contactDist = DotProduct(part->p, part->contactNormal);

        // Split the velocity across the surface and reflect it there. This
        // used to be done by moving pPrev, which said the same thing in a way
        // that could not be told apart from the constraint solver's corrections.
        VectorCopy(part->v, v);
        dot = DotProduct(v, trace.plane.normal);

        if (-dot > rd->impactSpeed) {
            rd->impactSpeed = -dot;
        }
        VectorScale(trace.plane.normal, dot, vn);
        VectorSubtract(v, vn, vt);

        if (trace.plane.normal[2] > 0.7f) {
            part->onGround = qtrue;

            // Static friction, so settled corpses stop creeping downhill.
            if (rd->balanced && VectorLength(vt) < RD_STATIC_FRICTION) {
                VectorClear(vt);
            }
        }

        // Friction as friction: the slide slows by an amount set by how hard
        // the surface is being pressed, the impact itself and the weight on
        // it, not by a share of its speed. It used to keep 1 - friction of
        // the sliding speed on every contact, which at sixty contacts a second
        // stops a body dead whatever the cvar says. The rest of the slide is
        // taken off in CG_RagdollStep, on every step a joint rests on
        // something, not only on the steps it strikes it.
        {
            const float load  = (dot < 0.0f ? -dot : 0.0f)
                              + CG_RagdollGravity() * Q_max(trace.plane.normal[2], 0.0f) / cg_ragdoll_physicsrate->value;
            const float slide = VectorLength(vt);
            const float keep  = slide > 0.001f ? Q_max(0.0f, slide - friction * load) / slide : 0.0f;

            for (k = 0; k < 3; k++) {
                part->v[k] = vt[k] * keep - vn[k] * bounce;
            }
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
// Whether the body has stopped falling, which is what the sleep test needs to
// know. Level faces are the usual answer; being pressed against enough of
// anything is the other one.
static qboolean CG_RagdollSupported(const cg_ragdoll_t *rd)
{
    int level = 0;
    int any   = 0;
    int i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        // Lying on another corpse is as good as lying on the ground, and a
        // joint wedged in the world is going nowhere at all, which is better
        // than either. The contact test knows about neither.
        if ((rd->onBodyMask | rd->stuckMask) & (1 << i)) {
            any++;
            level++;
            continue;
        }

        if (!rd->part[i].hasContact) {
            continue;
        }

        any++;

        if (rd->part[i].contactNormal[2] > 0.7f) {
            level++;
        }
    }

    return (level >= RD_MIN_SUPPORT || any >= RD_MIN_SUPPORT_ANY) ? qtrue : qfalse;
}

// Joints on level ground or on another corpse: enough of them, and the body is
// lying on something rather than leaning on it or hanging from it.
static qboolean CG_RagdollRestingOnLevel(const cg_ragdoll_t *rd)
{
    int level = 0;
    int i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        if ((rd->onBodyMask & (1 << i)) || (rd->part[i].hasContact && rd->part[i].contactNormal[2] > 0.7f)) {
            level++;
        }
    }

    return level >= RD_MIN_SUPPORT ? qtrue : qfalse;
}

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

// The shortest way out of the world for a point found inside it, or qfalse if
// there is none within reach. Tried from each of the given points (the bone
// ends, which are the way a limb went in) and from just outside the point along
// the four level directions and straight up, and whichever reaches a surface
// nearest the point wins. Nothing further than the unbury reach counts: a way
// out that far off is some other surface rather than the face the point came
// in through, and moving there is throwing the limb, not freeing it.
//
// The middle of a limb used to try the bone, then up. For a leg dragged across
// the edge of a ledge the way out along the bone is the whole buried part of
// the bone, while the way out past the face is a unit or two. Moving along the
// bone slid the limb along its own length, the constraints pulled it back the
// next step, and the feet and shins flicked five to fifteen units a step for as
// long as they were over the edge.
//
// Joints keep their own order, the bone and then up. Given this, bodies lying
// against a wall and one on open floor settled with a hand or foot inside the
// other leg, and it did nothing for the legs over an edge.
static qboolean CG_RagdollNearestExit(
    cg_ragdoll_t *rd,
    const vec3_t  point,
    const vec3_t  mins,
    const vec3_t  maxs,
    int           skipEntity,
    const float  *from[],
    int           numFrom,
    vec3_t        shift,
    vec3_t        normal
)
{
    static const vec3_t dirs[] = {
        {1, 0, 0},
        {-1, 0, 0},
        {0, 1, 0},
        {0, -1, 0},
        {0, 0, 1},
    };
    const float reach = rd->radius * RD_UNBURY_REACH;
    float       best  = reach;
    qboolean    found = qfalse;
    int         n;

    for (n = 0; n < numFrom + (int)ARRAY_LEN(dirs); n++) {
        vec3_t  start, d;
        trace_t out;
        float   len;

        if (n < numFrom) {
            VectorCopy(from[n], start);
        } else {
            VectorMA(point, reach, dirs[n - numFrom], start);
        }

        CG_RagdollTrace(&out, start, mins, maxs, point, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollNearestExit");

        if (out.startsolid || out.allsolid || out.fraction >= 1.0f) {
            continue;
        }

        VectorSubtract(out.endpos, point, d);
        len = VectorLength(d);

        if (len > best) {
            continue;
        }

        best  = len;
        found = qtrue;
        VectorCopy(d, shift);
        VectorCopy(out.plane.normal, normal);
    }

    return found;
}

// The one thing a step must not end with: a particle inside the world.

// Sweeps the middle of every limb through the world, as the joints are swept.
//
// The world only ever collided with the joints, and a bone between two of them
// was looked at only after it had ended up inside something (the push out
// below). A beam narrower than a thigh passes between the hip and the knee
// without either of them touching it, and the push out then shoves the bone
// back out through whichever face is nearest -- often the far one. Traced in
// the game, a body dropped across a beam had both calves inside it; the legs
// were shoved about by up to thirty units a step, a thigh was squeezed to less
// than half its length, and the body was thrown off the beam at a hundred and
// fifty units a second.
//
// Swept, a bone meets the beam where a real one would, at its first touch, and
// is held on the outside of it. The two ends are moved along the surface's
// normal in proportion to how near the sample each one is, just far enough to
// put the sample on the surface, and each loses the speed it had into the
// surface in the same proportion. Nothing is added: the move is a position
// correction, the way a contact holds a joint.
static int CG_RagdollBoneSweep(cg_ragdoll_t *rd, int skipEntity)
{
    vec3_t rd_mins, rd_maxs;
    int    n, q, k;
    int    moved = 0;

    for (n = 0; n < RD_NUM_LIMB_SEGMENTS; n++) {
        rdParticle_t *pa = &rd->part[rd_limbSegments[n].a];
        rdParticle_t *pb = &rd->part[rd_limbSegments[n].b];

        // A limb the grabber holds by one end; see CG_RagdollCollide.
        if (rd_limbSegments[n].a == rd->grabJoint || rd_limbSegments[n].b == rd->grabJoint) {
            continue;
        }

        CG_RagdollJointBox(
            rd,
            rd->jointRadius[rd_limbSegments[n].a] < rd->jointRadius[rd_limbSegments[n].b]
                ? rd_limbSegments[n].a
                : rd_limbSegments[n].b,
            rd_mins,
            rd_maxs
        );

        for (q = 0; q < RD_NUM_LIMB_SAMPLES; q++) {
            const float u     = rd_limbSamples[q];
            const float share = (1.0f - u) * (1.0f - u) + u * u;
            trace_t     tr;
            vec3_t      from, to;
            float       push, into;

            for (k = 0; k < 3; k++) {
                from[k] = pa->pPrev[k] + (pb->pPrev[k] - pa->pPrev[k]) * u;
                to[k]   = pa->p[k] + (pb->p[k] - pa->p[k]) * u;
            }

            // A remembered plane is questioned as a joint's is: dropped once
            // the sample is well clear of it, and asked again of the world when
            // it is old or the sample has moved along it.
            if (rd->boneContact[n][q].has) {
                const float above = DotProduct(to, rd->boneContact[n][q].normal) - rd->boneContact[n][q].dist;

                if (above > RD_CONTACT_FORGET) {
                    rd->boneContact[n][q].has = qfalse;
                } else if (cg.time - rd->boneContact[n][q].time > RD_CONTACT_STALE
                           || DistanceSquared(to, rd->boneContact[n][q].at) > Square(rd->radius * RD_CONTACT_REACH)) {
                    trace_t probe;
                    vec3_t  behind;

                    VectorMA(to, -(Q_max(above, 0.0f) + rd->radius + RD_CONTACT_PROBE), rd->boneContact[n][q].normal, behind);
                    CG_RagdollTrace(&probe, to, rd_mins, rd_maxs, behind, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollBoneProbe");

                    if (probe.fraction >= 1.0f && !probe.startsolid && !probe.allsolid) {
                        rd->boneContact[n][q].has = qfalse;
                    } else {
                        rd->boneContact[n][q].time = cg.time;
                        VectorCopy(to, rd->boneContact[n][q].at);
                    }
                }
            }

            CG_RagdollTrace(&tr, from, rd_mins, rd_maxs, to, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollBoneSweep");

            // Already inside is the push out's business, and nothing struck is
            // nothing to do.
            if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) {
                continue;
            }

            push = DotProduct(tr.endpos, tr.plane.normal) - DotProduct(to, tr.plane.normal) + RD_SURFACE_GAP;

            if (push <= 0.0f) {
                continue;
            }

            // So that the sample, which moves by (1-u) of the one end's move
            // and u of the other's, moves by exactly push.
            push /= share;

            for (k = 0; k < 3; k++) {
                pa->p[k] += tr.plane.normal[k] * push * (1.0f - u);
                pb->p[k] += tr.plane.normal[k] * push * u;
            }

            rd->boneContact[n][q].has  = qtrue;
            rd->boneContact[n][q].time = cg.time;
            VectorCopy(tr.plane.normal, rd->boneContact[n][q].normal);
            rd->boneContact[n][q].dist = DotProduct(tr.endpos, tr.plane.normal) + RD_SURFACE_GAP;
            VectorCopy(tr.endpos, rd->boneContact[n][q].at);

            into = DotProduct(pa->v, tr.plane.normal);
            if (into < 0.0f) {
                VectorMA(pa->v, -into * Q_min(1.0f, (1.0f - u) / share), tr.plane.normal, pa->v);
            }

            into = DotProduct(pb->v, tr.plane.normal);
            if (into < 0.0f) {
                VectorMA(pb->v, -into * Q_min(1.0f, u / share), tr.plane.normal, pb->v);
            }

            moved++;
        }
    }

    return moved;
}

// Keeps the middle of a limb out of the world.
//
// Everything else here collides particles: a box at each joint, swept from
// where it was to where it wants to be. Nothing has ever tested the bone
// between two joints, so a forearm lies inside a beam with the elbow out one
// side and the hand out the other and no trace objects -- the same blind spot
// self collision had, where two bones crossed at their middles with all four
// ends comfortably apart.
//
// It is what the screenshots keep showing. Of the corpses photographed as
// wrong, the commonest complaint by far was an arm sunk into the ground or
// into a timber, and every skeleton measure scored those bodies clean.
//
// Deliberately in the push out pass rather than in the solve. A sample is not
// a particle: it has no velocity, no mass and no contact plane, and giving it
// those would let the middle of a bone bounce and rub against the world on its
// own account, which is a much larger change than this. Here it only puts back
// what has already ended up inside something.
static void CG_RagdollLimbPushOut(cg_ragdoll_t *rd, int skipEntity)
{
    vec3_t rd_mins, rd_maxs;
    int    n, q, k;

    for (n = 0; n < RD_NUM_LIMB_SEGMENTS; n++) {
        rdParticle_t *pa = &rd->part[rd_limbSegments[n].a];
        rdParticle_t *pb = &rd->part[rd_limbSegments[n].b];

        // A limb the grabber holds by one end; see CG_RagdollCollide.
        if (rd_limbSegments[n].a == rd->grabJoint || rd_limbSegments[n].b == rd->grabJoint) {
            continue;
        }

        // The thinner of the bone's two ends, since the middle of a forearm is
        // nearer the wrist's thickness than the elbow's.
        CG_RagdollJointBox(
            rd,
            rd->jointRadius[rd_limbSegments[n].a] < rd->jointRadius[rd_limbSegments[n].b]
                ? rd_limbSegments[n].a
                : rd_limbSegments[n].b,
            rd_mins,
            rd_maxs
        );

        for (q = 0; q < RD_NUM_LIMB_SAMPLES; q++) {
            const float u = rd_limbSamples[q];
            trace_t     probe;
            vec3_t      mid, shift, normal;

            for (k = 0; k < 3; k++) {
                mid[k] = pa->p[k] + (pb->p[k] - pa->p[k]) * u;
            }

            CG_RagdollTrace(&probe, mid, rd_mins, rd_maxs, mid, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollLimbPushOut");

            if (!probe.startsolid && !probe.allsolid) {
                continue;
            }

            rd->limbBuriedMask |= 1 << n;

            {
                const float *ends[] = {pa->p, pb->p};

                if (!CG_RagdollNearestExit(rd, mid, rd_mins, rd_maxs, skipEntity, ends, 2, shift, normal)) {
                    continue;
                }
            }

            VectorMA(shift, RD_SURFACE_GAP, normal, shift);

            // Shared between the two ends by where along the bone the sample
            // sits, so a bone caught near its wrist lifts the wrist. Carried on
            // the previous positions as well, so this reads as the limb having
            // been there all along rather than as a shove, and the corpse does
            // not gain the speed on the next step.
            for (k = 0; k < 3; k++) {
                pa->p[k] += shift[k] * (1.0f - u);
                pa->pPrev[k] += shift[k] * (1.0f - u);
                pb->p[k] += shift[k] * u;
                pb->pPrev[k] += shift[k] * u;
            }
        }
    }
}

// The full solve knows only about planes already remembered, so a limb meeting
// a wall for the first time is pushed straight into it and the step ends there.
// Running the whole collision pass again to fix that costs the corpse its
// stillness, because it is a fresh impact as far as friction and bounce are
// concerned. This is only the part that has to happen: anything now inside
// geometry is put back on the surface, by the same route out that
// CG_RagdollCollide uses, and nothing else is touched.
static void CG_RagdollPushOut(cg_ragdoll_t *rd, int skipEntity)
{
    vec3_t rd_mins, rd_maxs;
    int    i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];

        // Carried by the grabber; see CG_RagdollCollide.
        if (i == rd->grabJoint) {
            continue;
        }

        CG_RagdollJointBox(rd, i, rd_mins, rd_maxs);
        const int     toward = rd_joints[i].parent >= 0 ? rd_joints[i].parent : RD_SPINE;
        trace_t       probe, out;
        vec3_t        shift;

        CG_RagdollTrace(&probe, part->p, rd_mins, rd_maxs, part->p, skipEntity, RD_CLIPMASK, qfalse, qtrue, "CG_RagdollPushOut");

        if (!probe.startsolid && !probe.allsolid) {
            rd->buriedFor[i] = 0;
            rd->stuckMask &= ~(1 << i);
            part->invMass = rd_joints[i].invMass;
            continue;
        }

        rd->buriedMask |= 1 << i;

        // A joint that has spent half a second inside the world is not going to
        // get out of it, and going on trying is what the player sees as a
        // spasm: the push frees it, the constraints and gravity put it back,
        // and the pair of them trade the limb to and fro for as long as the
        // corpse lives. Traced in the game, the corpses reported as spazzing
        // are exactly these -- those still moving after two seconds have a bone
        // inside the world for fifty frames against none for the ones that
        // settle, and the worst spent three hundred and fourteen.
        //
        // So it is left where it is, and held there, and the rest of the body
        // is allowed to settle around it. A limb resting inside a step looks
        // wrong; a limb shivering inside a step looks broken, and it also stops
        // the whole corpse ever falling asleep.
        if (rd->buriedFor[i] < RD_STUCK_STEPS) {
            rd->buriedFor[i]++;
        } else if (CG_RagdollSupportCount(rd) < RD_STUCK_MIN_SUPPORT) {
            // Held only when the body has somewhere else to rest. A joint that
            // is the only thing touching anything is not steadying the corpse,
            // it is carrying it, and pinning that is how a man ends up hanging
            // off a lamp he ought to slide from. Measured, corpses resting on
            // fewer than eight contacts went from seven in a thousand to
            // seventeen when the hold came in. So a body with nothing else
            // under it goes on struggling, which is the only way it will ever
            // come free.
            rd->buriedFor[i] = RD_STUCK_STEPS;
        } else {
            rd->stuckMask |= 1 << i;

            // Made heavy, optionally. Held completely still it stops moving
            // and so does the twitching, but the body can then hang off it:
            // a hand stuck in a wall leaves a corpse dangling, which the suite
            // reports as a limb held up by nothing by fourteen units. Off, the
            // joint is merely left alone and the body sags past it.
            if (cg_ragdoll_stuckhold->value > 0.0f) {
                part->invMass = cg_ragdoll_stuckhold->value * rd_joints[i].invMass;
            }

            VectorClear(part->v);
            VectorCopy(part->p, part->pPrev);
            continue;
        }

        CG_RagdollTrace(
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

            CG_RagdollTrace(
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

// Impact speed, into a surface, from which a landing starts to be soaked up,
// how much more speed it takes to reach the most, and the most: the share of
// the body's spin and of its limbs' flailing taken out in that step.
#define RD_SPLAT_SPEED 300.0f
#define RD_SPLAT_RANGE 600.0f
#define RD_SPLAT_MAX   0.6f

// Share of a held body's turn about the vertical, through the joint held,
// that is turned back each step.
#define RD_GRAB_HEADING_HOLD 0.7f

// Keeps a body the grabber holds from spinning about the vertical through the
// held joint: the physics gun does the same to what it carries.
//
// Hanging from one point nothing else stops it turning, and the joint limits
// set it turning. Gravity pulls a dangling limb past its range on every step
// and the range moves it back, without turning the rest of the body the other
// way as a real joint would, so each correction leaves a little turn behind.
// Held by a thigh in the game a body was spun up to two hundred degrees a
// second, one way, for as long as it was held. Taking turn out of the
// velocities did nothing, since the solver puts it straight back; so it is
// taken out of the positions the step ends with, and out of the velocities
// with them. Swinging the body on the beam is a movement of the whole body,
// or a turn about a level axis, and is left alone.
static void CG_RagdollHoldHeading(cg_ragdoll_t *rd)
{
    const float *pivot = rd->part[rd->grabJoint].p;
    float        num = 0.0f, den = 0.0f, turn, c, s;
    int          i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const float ax = rd->part[i].pPrev[0] - pivot[0], ay = rd->part[i].pPrev[1] - pivot[1];
        const float bx = rd->part[i].p[0] - pivot[0], by = rd->part[i].p[1] - pivot[1];

        num += ax * by - ay * bx;
        den += ax * bx + ay * by;
    }

    if (fabs(num) < 0.0001f && den <= 0.0f) {
        return;
    }

    turn = -atan2(num, den) * RD_GRAB_HEADING_HOLD;
    c    = cos(turn);
    s    = sin(turn);

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];
        const float   x    = part->p[0] - pivot[0];
        const float   y    = part->p[1] - pivot[1];
        const float   vx   = part->v[0];
        const float   vy   = part->v[1];

        part->p[0] = pivot[0] + x * c - y * s;
        part->p[1] = pivot[1] + x * s + y * c;
        part->v[0] = vx * c - vy * s;
        part->v[1] = vx * s + vy * c;
    }
}

// Whether the body is lying on what holds it up rather than hanging from it:
// its centre of mass no lower than the joints resting on something level, or on
// another body, with a little to spare.
//
// Resting is judged by the joints alone, and a body draped over the end of a
// beam, a sill or a lamp bracket has four or five of them lying on its top
// face while the rest of it hangs down the side. Counted as resting, it had the
// slow start of its slide taken away on every step, by static friction and by
// the damping for bodies at rest, and was then let fall asleep hanging there.
// A body lying on a floor, or slumped against a wall with its seat on the
// ground, has its weight above what it rests on; one hanging from a ledge has
// it below, and is left to slide off.
static qboolean CG_RagdollBalanced(const cg_ragdoll_t *rd, qboolean *draped)
{
    float centre = 0.0f, mass = 0.0f, lowest = 0.0f;
    int   i, level = 0;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const rdParticle_t *part = &rd->part[i];
        const float         m    = rd_joints[i].invMass > 0.0f ? 1.0f / rd_joints[i].invMass : 1.0f;

        centre += m * part->p[2];
        mass   += m;

        if ((rd->onBodyMask & (1 << i)) || (part->hasContact && part->contactNormal[2] > 0.7f)) {
            if (!level || part->p[2] < lowest) {
                lowest = part->p[2];
            }
            level++;
        }
    }

    *draped = qfalse;

    if (!level || mass <= 0.0f) {
        return qfalse;
    }

    if (centre / mass >= lowest - RD_BALANCE_SLACK) {
        return qtrue;
    }

    *draped = qtrue;
    return qfalse;
}

// Friction for this body as it lies now.
//
// A body draped across a beam, torso on top and arms and legs hanging down
// either side, is held there by nothing but the rub of its chest on the top,
// and with the weight nearly even on both sides that was enough to hold it
// indefinitely: one in the game hung in the air for ten seconds, perfectly
// still, until the player took hold of it again. The same body lying on the
// beam would rightly stay. So a body that is hanging rather than lying slides
// on a quarter of the grip, and comes off unless it is truly balanced. Only a
// body resting on something counts: one in the air has nothing to hang from,
// and taking the grip off it cut the friction of every landing, so bodies
// dropped on open floor skidded and twisted where they fell.
static float CG_RagdollFriction(const cg_ragdoll_t *rd)
{
    return cg_ragdoll_friction->value * (rd->draped ? RD_DRAPED_FRICTION : 1.0f);
}

// Damps the body's limbs moving relative to one another, and nothing else.
//
// Each joint's velocity is taken apart into what the body as a whole is doing,
// moving and turning as though it were rigid, and the rest, which is its limbs
// flailing about it; a share of the rest is taken away (Mueller et al.,
// Position Based Dynamics, 2007, section 3.5). A body thrown or falling keeps
// all of its speed and all of its tumble, since those are the rigid part, and
// the momentum of both is left exactly as it was.
static void CG_RagdollInternalDamping(cg_ragdoll_t *rd, float share, float spinShare)
{
    vec3_t centre, vel, spin, omega;
    float  inertia[3][3], inv[3][3], det, mass = 0.0f;
    int    i, a, b;

    VectorClear(centre);
    VectorClear(vel);
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const float m = rd_joints[i].invMass > 0.0f ? 1.0f / rd_joints[i].invMass : 1.0f;

        VectorMA(centre, m, rd->part[i].p, centre);
        VectorMA(vel, m, rd->part[i].v, vel);
        mass += m;
    }
    if (mass <= 0.0f) {
        return;
    }
    VectorScale(centre, 1.0f / mass, centre);
    VectorScale(vel, 1.0f / mass, vel);

    VectorClear(spin);
    memset(inertia, 0, sizeof(inertia));
    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const float m = rd_joints[i].invMass > 0.0f ? 1.0f / rd_joints[i].invMass : 1.0f;
        vec3_t      r, l;

        VectorSubtract(rd->part[i].p, centre, r);
        CrossProduct(r, rd->part[i].v, l);
        VectorMA(spin, m, l, spin);

        for (a = 0; a < 3; a++) {
            for (b = 0; b < 3; b++) {
                inertia[a][b] += m * ((a == b ? DotProduct(r, r) : 0.0f) - r[a] * r[b]);
            }
        }
    }

    det = inertia[0][0] * (inertia[1][1] * inertia[2][2] - inertia[1][2] * inertia[2][1])
        - inertia[0][1] * (inertia[1][0] * inertia[2][2] - inertia[1][2] * inertia[2][0])
        + inertia[0][2] * (inertia[1][0] * inertia[2][1] - inertia[1][1] * inertia[2][0]);
    if (fabs(det) < 0.0001f) {
        return;
    }
    inv[0][0] = (inertia[1][1] * inertia[2][2] - inertia[1][2] * inertia[2][1]) / det;
    inv[0][1] = (inertia[0][2] * inertia[2][1] - inertia[0][1] * inertia[2][2]) / det;
    inv[0][2] = (inertia[0][1] * inertia[1][2] - inertia[0][2] * inertia[1][1]) / det;
    inv[1][0] = (inertia[1][2] * inertia[2][0] - inertia[1][0] * inertia[2][2]) / det;
    inv[1][1] = (inertia[0][0] * inertia[2][2] - inertia[0][2] * inertia[2][0]) / det;
    inv[1][2] = (inertia[0][2] * inertia[1][0] - inertia[0][0] * inertia[1][2]) / det;
    inv[2][0] = (inertia[1][0] * inertia[2][1] - inertia[1][1] * inertia[2][0]) / det;
    inv[2][1] = (inertia[0][1] * inertia[2][0] - inertia[0][0] * inertia[2][1]) / det;
    inv[2][2] = (inertia[0][0] * inertia[1][1] - inertia[0][1] * inertia[1][0]) / det;
    for (a = 0; a < 3; a++) {
        omega[a] = inv[a][0] * spin[0] + inv[a][1] * spin[1] + inv[a][2] * spin[2];
    }

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        vec3_t r, rigid, off;

        if (i == rd->grabJoint) {
            continue;
        }

        VectorSubtract(rd->part[i].p, centre, r);
        CrossProduct(omega, r, rigid);
        VectorAdd(rigid, vel, rigid);
        VectorSubtract(rigid, rd->part[i].v, off);
        VectorMA(rd->part[i].v, share, off, rd->part[i].v);

        // And, when asked, some of the body's turning as a whole as well.
        if (spinShare > 0.0f) {
            vec3_t turning;

            CrossProduct(omega, r, turning);
            VectorMA(rd->part[i].v, -spinShare, turning, rd->part[i].v);
        }
    }
}

// Joints being pulled into what they are touching: their contact surface faces
// back against the way from the held part to where the beam wants it. Lifting
// a body off the floor pulls away from every surface it lies on, and counts
// none; dragging it into a wall or over a ledge counts every joint pressed
// against it.
static int CG_RagdollContactsAgainst(const cg_ragdoll_t *rd, const vec3_t target, const vec3_t held)
{
    vec3_t dir;
    int    i, n = 0;

    VectorSubtract(target, held, dir);
    if (VectorNormalize(dir) < 0.001f) {
        return 0;
    }

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        if (i != rd->grabJoint && rd->part[i].hasContact && DotProduct(dir, rd->part[i].contactNormal) < -0.3f) {
            n++;
        }
    }

    return n;
}

static float CG_RagdollStepInner(cg_ragdoll_t *rd, int skipEntity, float dt)
{
    // Cleared here rather than in the world collision, which runs after the
    // solve that fills it in: put there, every mark this step made was wiped
    // before anything read it, so no corpse ever reported being held up by
    // another and none of them could fall asleep on a heap.
    rd->onBodyMask = 0;

    const float damping = 1.0f - cg_ragdoll_damping->value;
    const float gravity = CG_RagdollGravity();
    // The damping is there to soak up what the solver's corrections feed a
    // body that is lying on something. Applied in the air too, as it was, it
    // is drag: at 60 steps a second 2 per cent a step holds a falling body
    // under 660 units a second, and a second into a fall it is moving at
    // little more than half the speed it should. Bodies read as floating down.
    const qboolean resting = CG_RagdollSupported(rd);

    rd->balanced = CG_RagdollBalanced(rd, &rd->draped);
    float       maxDisp = 0.0f;
    float       meanDisp = 0.0f;
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

        // Carried by the grabber, on a spring toward the end of the beam, and
        // made immovable to everything else, so the constraints hang the rest
        // of the body from it rather than pulling it back.
        //
        // A spring rather than being moved straight there is what gives a body
        // weight: it lags behind a swing, runs on a little past where the swing
        // stops, and hangs a few units below the aim, and the beam bends to
        // follow it. Its velocity is kept, so letting go mid swing throws it.
        if (i == rd->grabJoint) {
            // Pulling against the world, the beam gives: when the part held
            // is well short of where the beam wants it and the body is up
            // against something, the pull is weakened and slowed, as the
            // physics gun's is. At full strength it dragged a body into a
            // ledge or a bracket at up to RD_GRAB_SPEED, the limbs caught on
            // the edges and were snapped free a joint at a time.
            //
            // Eased in and out over a few steps, and let go of only once the
            // gap has mostly closed: switched outright, the pull jumped between
            // full and weak from one step to the next as the gap or the count
            // of contacts crossed the line, and the held part shook with it.
            const float gap      = Distance(rd->grabTarget, part->p);
            const int   contacts = CG_RagdollContactsAgainst(rd, rd->grabTarget, part->p);
            float       k, c, maxSpeed, ease;
            vec3_t      pull;
            float       speed;

            if (gap > RD_GRAB_BLOCKED_GAP && contacts >= RD_GRAB_BLOCKED_CONTACTS) {
                rd->grabBlocked = qtrue;
            } else if (gap < RD_GRAB_BLOCKED_GAP * 0.5f || contacts < RD_GRAB_BLOCKED_CONTACTS - 1) {
                rd->grabBlocked = qfalse;
            }

            rd->grabEase += ((rd->grabBlocked ? 1.0f : 0.0f) - rd->grabEase) * RD_GRAB_BLOCKED_EASE;
            ease     = rd->grabEase;
            k        = cg_ragdoll_grabspring->value * (1.0f + (RD_GRAB_BLOCKED_PULL - 1.0f) * ease);
            c        = 2.0f * RD_GRAB_DAMPING * sqrtf(k);
            maxSpeed = RD_GRAB_SPEED + (RD_GRAB_BLOCKED_SPEED - RD_GRAB_SPEED) * ease;

            VectorSubtract(rd->grabTarget, part->p, pull);
            VectorScale(pull, k, pull);
            VectorMA(pull, -c, part->v, pull);
            pull[2] -= gravity * RD_GRAB_SAG;

            VectorMA(part->v, dt, pull, part->v);

            speed = VectorLength(part->v);
            if (speed > maxSpeed) {
                VectorScale(part->v, maxSpeed / speed, part->v);
            }

            VectorCopy(part->p, part->pPrev);
            VectorMA(part->p, dt, part->v, part->p);
            VectorClear(part->solved);
            part->invMass = 0.0f;
            continue;
        }

        // A joint that has given up getting out of the world is held where it
        // is. Pinning it in the push out alone was not enough: that runs once,
        // at the end of the step, and gravity and the bone sticks move it again
        // on the next one, so the trading went on exactly as before. Measured
        // over two hundred corpses afterwards, one was still inside the world
        // on seven hundred and twenty one of its seven hundred and twenty two
        // frames.
        if ((rd->stuckMask & (1 << i)) && cg_ragdoll_stuckhold->value > 0.0f) {
            VectorClear(part->v);
            VectorCopy(part->p, part->pPrev);
            VectorClear(part->solved);
            continue;
        }

        {
            const float d = resting ? damping : 1.0f - (1.0f - damping) * RD_AIR_DAMPING;

            VectorScale(part->v, rd->grabJoint >= 0 ? d * RD_GRAB_BODY_DAMPING : d, part->v);
        }
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

    if (rd->grabJoint >= 0) {
        CG_RagdollHoldHeading(rd);
    }

    // Collision resolution moves particles with no regard for what they are
    // attached to, so if it had to move any, relax once more against the
    // planes it just found. Without this the emitted pose keeps whatever
    // violation the impact introduced, which reads as limbs stretching on
    // impact and as a corpse that creeps instead of settling.
    if (CG_RagdollCollide(rd, skipEntity) | CG_RagdollBoneSweep(rd, skipEntity)) {
        CG_RagdollSolveTracked(rd, cg_ragdoll_iterations->integer);
        CG_RagdollPushOut(rd, skipEntity);
        CG_RagdollLimbPushOut(rd, skipEntity);
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
            VectorMA(part->v, (rd->grabJoint >= 0 ? solveGain * RD_GRAB_SOLVEGAIN : solveGain) / dt, part->solved, part->v);
        }

        // Sliding friction, on every joint resting on a surface: the slide
        // loses friction times the weight pressing it into the surface. A
        // joint sliding along the floor is held just off it and its trace
        // strikes nothing, so friction applied only where a trace hits
        // (CG_RagdollCollide) acted only at the moment of landing.
        if (part->hasContact && part->contactNormal[2] > 0.0f) {
            const float along = DotProduct(part->v, part->contactNormal);
            vec3_t      vt;
            float       slide, keep;

            VectorMA(part->v, -along, part->contactNormal, vt);
            slide = VectorLength(vt);
            if (slide > 0.001f) {
                keep = Q_max(0.0f, slide - CG_RagdollFriction(rd) * CG_RagdollGravity() * part->contactNormal[2] * dt) / slide;
                VectorMA(part->v, keep - 1.0f, vt, part->v);
            }
        }

        VectorSubtract(part->p, part->pPrev, d);
        len = VectorLength(d);

        if (part->hasContact && supported && rd->balanced) {
            if (len < RD_REST_SPEED) {
                VectorClear(part->v);
                len = 0.0f;
            } else if (len > RD_SLIDE_SPEED) {
                // Sliding, which friction has in hand. The damping below is
                // for the slow shuffle of a body that has nearly stopped; left
                // on a body at speed it took two thirds of its speed a step,
                // and nothing slid at all, whatever the friction was.
            } else {
                VectorScale(part->v, RD_GROUND_DAMPING, part->v);
                len *= RD_GROUND_DAMPING;
            }
        }

        if (len > maxDisp) {
            maxDisp = len;
        }

        meanDisp += len;
    }

    // A hard landing is soaked up rather than carried on. A body striking the
    // world at speed kept all of its tumble, and with friction under it that
    // turned into rolling: bounced off a wall, one landed at seven hundred
    // units a second and was thrown three hundred and fifty sideways and up.
    // Flesh does not do that; it gives, and the body splats. So the harder a
    // step's worst impact, the more of the body's spin and of its limbs'
    // flailing is taken out with it. Its sliding speed is left alone, which
    // is what the friction is for.
    {
        const float splat = Q_clamp_float((rd->impactSpeed - RD_SPLAT_SPEED) / RD_SPLAT_RANGE, 0.0f, 1.0f) * RD_SPLAT_MAX;
        const float limb  = Q_max(cg_ragdoll_limbdamp->value, splat);

        if (limb > 0.0f || splat > 0.0f) {
            CG_RagdollInternalDamping(rd, limb, splat);
        }
    }

    rd->lastMean = meanDisp / (float)RD_NUM_JOINTS;

    return maxDisp;
}

static float CG_RagdollStep(cg_ragdoll_t *rd, int skipEntity, float dt)
{
    float maxDisp;

    if (rd->numIgnoredProps) {
        CG_RagdollReleaseEnclosingProps(rd);
    }

    rd_traceBody = rd;
    maxDisp      = CG_RagdollStepInner(rd, skipEntity, dt);
    rd_traceBody = NULL;

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

// How far the drawn chest is from the chest the solver actually has.
//
// The back takes its roll from the pelvis and hands it up one bone at a time,
// so the drawn chest is a function of the hips alone and never looks at where
// the shoulder particles have got to. The solver is free to turn the shoulders
// about the spine and the drawn chest does not follow. Measured over three
// hundred corpses the two shoulder lines sit a median of 24 degrees apart, are
// more than 20 apart in 174 of them, and in the worst case very nearly face
// opposite ways.
//
// Everything hanging off the chest is then drawn where that leaves it while
// collision goes on protecting particles somewhere else, which is why an arm
// lies through the ground or through the body while every measurement of the
// skeleton says it is clear: the arm is the right length, points the right way,
// and is simply in the wrong place, by as much as 21 units.
//
// Anchoring the chest to the shoulders outright was tried before and reverted:
// it puts the whole of this difference into one joint, and a mesh skinned across
// that joint pinches. So it is measured here and spread along the back instead.
// Measuring costs a throwaway pass over the five spine bones, which is the only
// way to know what the uncorrected chest would have been.
static void CG_RagdollMeasureChestRoll(cg_ragdoll_t *rd)
{
    const int lua = 7, rua = 10;   // the two upper arms in rd_bones
    vec3_t    drawn, want, side, ref;
    float     err;
    int       k;

    if (!rd->twistRestValid || rd->boneIndex[RD_LAST_ROLL_BONE] < 0 || rd->boneIndex[lua] < 0
        || rd->boneIndex[rua] < 0) {
        return;
    }

    // Where the shoulders were actually put last frame. Read straight out of
    // the positions that went to the renderer, and not rebuilt here from the
    // offsets and the chest's axes.
    //
    // Rebuilding them is what this did first, and it converged beautifully on
    // the wrong thing: traced in the game the error it reported was a tenth of
    // a degree while the shoulders it had emitted were eighty nine degrees from
    // the ones the solver had. A loop that measures its own reconstruction will
    // drive that reconstruction to zero and leave the pose exactly where it
    // was. Only the emitted positions can say what was drawn.
    VectorSubtract(rd->bonePos[rua], rd->bonePos[lua], drawn);

    VectorSubtract(rd->part[RD_RUARM].p, rd->part[RD_LUARM].p, want);

    // Compared square to the chest's own axis, so only the roll about it is
    // being measured and not the chest nodding or leaning.
    VectorCopy(rd->rolledAxis[RD_LAST_ROLL_BONE][0], ref);
    VectorMA(drawn, -DotProduct(drawn, ref), ref, drawn);
    VectorMA(want, -DotProduct(want, ref), ref, want);

    if (VectorNormalize(drawn) < 0.001f || VectorNormalize(want) < 0.001f) {
        return;
    }

    CrossProduct(ref, drawn, side);

    err = (float)(atan2(DotProduct(want, side), DotProduct(want, drawn)) * 180.0 / M_PI);

    // The shoulder line is a line, not an arrow: half a turn puts it back on
    // itself, and without this a chest a little past square reads as needing
    // most of a turn the other way.
    if (err > 90.0f) {
        err -= 180.0f;
    } else if (err < -90.0f) {
        err += 180.0f;
    }

    rd->chestRollErr = err;
    rd->spineRollFix += err * RD_CHEST_ROLL_GAIN * cg_ragdoll_chestroll->value;

    // Beyond this the correction is not a correction. A solver state wild
    // enough to put the shoulders half a turn from the drawn chest is one where
    // rolling the back to follow them would wring the corpse rather than settle
    // it.
    if (rd->spineRollFix > RD_MAX_CHEST_ROLL) {
        rd->spineRollFix = RD_MAX_CHEST_ROLL;
    } else if (rd->spineRollFix < -RD_MAX_CHEST_ROLL) {
        rd->spineRollFix = -RD_MAX_CHEST_ROLL;
    }
}

//=============================================================
// Clavicles
//=============================================================

// The ragdoll draws no clavicle: each upper arm hangs off the chest directly,
// and is allowed to slide a few units toward where the solver has its shoulder
// (cg_ragdoll_shoulderslack). The clavicle was left to the animation, hanging
// off the chest in the death pose, so its end and the top of the upper arm came
// apart by as much as the shoulder had moved. Nothing of the mesh hangs on the
// clavicle itself, but the helper bones that shape the shoulder do (helper
// Lshoulder and Rshoulder, placed along the clavicle), and on the right arm the
// elbow helper hangs off those in turn. Every vertex on them was drawn where the
// clavicle said the arm was, which on screen is an arm broken at the shoulder
// or at the elbow.
//
// So the clavicle is drawn too: its root where it sat on the chest, turned to
// point at the upper arm wherever that is drawn.
static const char *const rd_clavicleNames[2] = {"Bip01 L Clavicle", "Bip01 R Clavicle"};
static const int         rd_clavicleArm[2]   = {7, 10}; // rd_bones: L and R UpperArm
#define RD_CLAVICLE_PARENT 4                           // rd_bones: Bip01 Spine2
#define RD_CLAVICLE_MAX_TURN 35.0f
#define RD_CLAVICLE_COS_TURN 0.8191520f                  // cos(35 degrees)

// At seed, after the bones: where each clavicle is, against its chest.
static void CG_RagdollSeedClavicles(cg_ragdoll_t *rd, refEntity_t *model)
{
    const int par = RD_CLAVICLE_PARENT;
    int       s, j, k;

    for (s = 0; s < 2; s++) {
        vec3_t pos, axis[3], d;

        rd->clavIndex[s] = -1;

        if (rd->boneIndex[par] < 0 || rd->boneIndex[rd_clavicleArm[s]] < 0) {
            continue;
        }

        rd->clavIndex[s] = cgi.Tag_NumForName(model->tiki, rd_clavicleNames[s]);
        if (rd->clavIndex[s] < 0 || !CG_RagdollReadBoneWorld(model, rd->clavIndex[s], pos, axis)) {
            rd->clavIndex[s] = -1;
            continue;
        }

        VectorSubtract(pos, rd->bonePos[par], d);
        for (k = 0; k < 3; k++) {
            rd->clavOffset[s][k] = DotProduct(d, rd->boneAxis[par][k]);
            for (j = 0; j < 3; j++) {
                rd->clavLocal[s][j][k] = DotProduct(axis[j], rd->boneAxis[par][k]);
            }
        }

        VectorCopy(pos, rd->clavPos[s]);
        AxisCopy(axis, rd->clavAxis[s]);
    }
}

// Once the bones are placed: each clavicle carried on the chest as it sat
// there, then turned by as much as the shoulder slack has moved its upper arm
// off the place the chest alone would put it, and slid so that it still meets
// the arm. With no slack that is the clavicle exactly as the animation had it.
static void CG_RagdollPlaceClavicles(cg_ragdoll_t *rd)
{
    const int par = RD_CLAVICLE_PARENT;
    int       s, j, k;

    for (s = 0; s < 2; s++) {
        const int arm = rd_clavicleArm[s];
        vec3_t    root, rigid, from, to, axis[3];
        float     len;

        if (rd->clavIndex[s] < 0) {
            continue;
        }

        VectorCopy(rd->bonePos[par], root);
        VectorCopy(rd->bonePos[par], rigid);
        for (k = 0; k < 3; k++) {
            VectorMA(root, rd->clavOffset[s][k], rd->rolledAxis[par][k], root);
            VectorMA(rigid, rd->localOffset[arm][k], rd->rolledAxis[par][k], rigid);
        }
        for (j = 0; j < 3; j++) {
            VectorClear(axis[j]);
            for (k = 0; k < 3; k++) {
                VectorMA(axis[j], rd->clavLocal[s][j][k], rd->rolledAxis[par][k], axis[j]);
            }
        }

        VectorSubtract(rigid, root, from);
        VectorSubtract(rd->bonePos[arm], root, to);
        len = VectorNormalize(from);
        if (len < 0.001f || VectorNormalize(to) < 0.001f) {
            continue;
        }

        // Turned no further than this. The slack can move the arm by more
        // than the clavicle is long, right across its root, and the turn that
        // follows it there has no settled direction: it flipped the clavicle,
        // and the shoulder with it, half round in a frame. Past the cap the
        // clavicle only slides.
        {
            const float cosTurn = DotProduct(from, to);

            if (cosTurn < RD_CLAVICLE_COS_TURN) {
                vec3_t axisTurn;

                CrossProduct(from, to, axisTurn);
                if (VectorNormalize(axisTurn) > 0.001f) {
                    RotatePointAroundVector(to, axisTurn, from, RD_CLAVICLE_MAX_TURN);
                } else {
                    VectorCopy(from, to);
                }
            }
        }

        for (j = 0; j < 3; j++) {
            CG_RagdollCarry(from, to, axis[j], rd->clavAxis[s][j]);
        }

        // The slack moves the arm by as much as the clavicle is long, so
        // turning alone leaves its end well short of the arm or past it. The
        // clavicle keeps its length and its root slides along it instead: into
        // or out of the base of the neck, which hides it, rather than the
        // shoulder being drawn torn open.
        VectorMA(rd->bonePos[arm], -len, to, rd->clavPos[s]);
    }
}

// Appends the clavicles to the overrides, offset by drop.
static void CG_RagdollWriteClavicles(cg_ragdoll_t *rd, refEntity_t *model, const vec3_t drop)
{
    int s;

    for (s = 0; s < 2; s++) {
        boneOverride_t *out = &rd->overrides[rd->numOverrides];
        vec3_t          pos;

        if (rd->clavIndex[s] < 0) {
            continue;
        }

        VectorAdd(rd->clavPos[s], drop, pos);
        out->boneIndex = rd->clavIndex[s];
        CG_RagdollWriteBoneModel(model, pos, rd->clavAxis[s], out->matrix);
        rd->numOverrides++;
    }
}

// Where the two shoulders are drawn: each hung off the chest as it was at the
// moment of death, then let slide up to cg_ragdoll_shoulderslack toward where
// the simulation has it. Never closer together than they were at death,
// though. The simulated shoulders are held apart by a rigid bar, so they never
// are; but when the drawn chest is turned against them, after a hard fling or
// a twist, each one slid toward its own and the two met in the middle: the
// shoulders drawn collapsed into the chest, and the clavicles, which follow
// the arms, dragged in after them. Measured, a body wrung half round was drawn
// with its shoulders at 59 per cent of their width.
static void CG_RagdollSlideShoulders(cg_ragdoll_t *rd, vec3_t out[2])
{
    static const int bones[2] = {7, 10}; // rd_bones: L and R UpperArm
    const float      slack    = cg_ragdoll_shoulderslack->value;
    vec3_t           rigid[2], apart, mid;
    float            restWidth, width;
    int              s, k;

    for (s = 0; s < 2; s++) {
        const int b = bones[s];
        vec3_t    off;
        float     len;

        VectorCopy(rd->bonePos[rd_bones[b].parent], rigid[s]);
        for (k = 0; k < 3; k++) {
            VectorMA(rigid[s], rd->localOffset[b][k], rd->rolledAxis[rd_bones[b].parent][k], rigid[s]);
        }

        VectorSubtract(rd->part[rd_bones[b].joint].p, rigid[s], off);
        len = VectorLength(off);
        if (len > slack) {
            VectorScale(off, slack / len, off);
        }
        VectorAdd(rigid[s], off, out[s]);
    }

    VectorSubtract(rigid[1], rigid[0], apart);
    restWidth = VectorLength(apart);

    VectorSubtract(out[1], out[0], apart);
    width = VectorNormalize(apart);

    if (width >= restWidth) {
        return;
    }

    if (width < 0.001f) {
        VectorSubtract(rigid[1], rigid[0], apart);
        VectorNormalize(apart);
    }

    VectorAdd(out[0], out[1], mid);
    VectorScale(mid, 0.5f, mid);
    VectorMA(mid, -0.5f * restWidth, apart, out[0]);
    VectorMA(mid, 0.5f * restWidth, apart, out[1]);
}

static void CG_RagdollBuildPose(cg_ragdoll_t *rd, refEntity_t *model, float weight)
{
    vec3_t   shoulderAt[2];
    qboolean haveShoulders = qfalse;
    int      i, j, k;

    rd->numOverrides = 0;

    if (cg_ragdoll_chestroll->value > 0.0f) {
        CG_RagdollMeasureChestRoll(rd);
    } else {
        rd->spineRollFix = 0.0f;
    }

    CG_RagdollLimbTwist(rd);

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

            // The shoulders are allowed to leave that offset a little.
            //
            // Everything else hangs rigidly off its parent, which is what makes
            // every bone length exactly right. For a shoulder it is what makes
            // the arm wrong. The offset is captured at the moment of death, so
            // the drawn shoulders are welded to the chest in the pose the man
            // was shot in, while the simulated ones go on moving relative to
            // it. Measured over a hundred corpses, the part of the shoulder
            // error that rolling the spine can remove is down to a degree at
            // the median -- the correction does its job -- and what is left is
            // a tilt out of that plane, nine degrees at the median and forty
            // two at the ninetieth, which no amount of rolling can reach. The
            // whole arm is then drawn ten units from the particles that collide
            // on its behalf, rigidly: the arm adds nothing to the error, it
            // just inherits all of it from the shoulder.
            //
            // A real shoulder girdle slides over the ribs, so letting this one
            // move a few units towards where the solver has it is anatomy
            // rather than a fudge. Clamped, because letting it go all the way
            // is how limbs used to stretch.
            if (cg_ragdoll_shoulderslack->value > 0.0f && (i == 7 || i == 10)) {
                // Both shoulders are worked out together, when the first is
                // reached: each is held apart from the other.
                if (!haveShoulders) {
                    CG_RagdollSlideShoulders(rd, shoulderAt);
                    haveShoulders = qtrue;
                }
                VectorCopy(shoulderAt[i == 7 ? 0 : 1], worldPos);
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

    {
        const vec3_t none = {0, 0, 0};

        CG_RagdollPlaceClavicles(rd);
        CG_RagdollWriteClavicles(rd, model, none);
    }

    // Every bone now has a roll from a frame of its own to be measured
    // against. See CG_RagdollBoneFrame.
    rd->rollReady = qtrue;
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

// A digit as strokes on a 4x7 grid, one bit per segment of the outline below.
// There is no way to draw text in the world from here -- R_DrawString is flat
// against the screen and would have to be deferred to the 2D pass -- and a
// number floating over the body is the whole point, so the digits are drawn as
// debug lines like everything else.
//
//   --0--        Numbered clockwise from the top bar, with 6 across the middle,
//  |     |       which is the ordinary seven segment layout.
//  5     1
//  |     |
//   --6--
//  |     |
//  4     2
//  |     |
//   --3--
static const unsigned char rd_digitSegments[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

// Endpoints of each segment on a unit cell, x right and y up.
static const float rd_digitStroke[7][4] = {
    {0.0f, 1.0f, 1.0f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.5f},
    {1.0f, 0.5f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f},
    {0.0f, 0.5f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f, 0.5f},
    {0.0f, 0.5f, 1.0f, 0.5f}
};

// Writes the trace number in the air above the corpse, facing whoever is
// looking, so a screenshot carries the name of the file that explains it.
static void CG_RagdollDrawNumber(const cg_ragdoll_t *rd)
{
    vec3_t right, up, at;
    char   text[16];
    float  size, span;
    int    len, i, k;

    if (rd->dumpSeq <= 0) {
        return;
    }

    Com_sprintf(text, sizeof(text), "%d", rd->dumpSeq);
    len = (int)strlen(text);

    // Billboarded. viewaxis[1] runs to the left, so the sign is flipped to get
    // digits that read the right way round rather than mirrored.
    VectorScale(cg.refdef.viewaxis[1], -1.0f, right);
    VectorCopy(cg.refdef.viewaxis[2], up);

    size = 6.0f;
    span = (float)len * size * 0.8f;

    // Over the head if there is one, and clear of it.
    VectorCopy(rd->part[RD_HEAD].p, at);
    VectorMA(at, 12.0f, up, at);
    VectorMA(at, span * -0.5f, right, at);

    for (i = 0; i < len; i++) {
        const unsigned char segs = rd_digitSegments[text[i] - '0'];

        for (k = 0; k < 7; k++) {
            vec3_t a, b;

            if (!(segs & (1 << k))) {
                continue;
            }

            VectorCopy(at, a);
            VectorMA(a, rd_digitStroke[k][0] * size * 0.6f, right, a);
            VectorMA(a, rd_digitStroke[k][1] * size, up, a);

            VectorCopy(at, b);
            VectorMA(b, rd_digitStroke[k][2] * size * 0.6f, right, b);
            VectorMA(b, rd_digitStroke[k][3] * size, up, b);

            cgi.R_DebugLine(a, b, 0.2f, 0.9f, 1.0f, 1.0f);
        }

        VectorMA(at, size * 0.8f, right, at);
    }
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
// Being shot at, and blown up, after the fact
//=============================================================

// How long a corpse goes on simulating after something hits it.
#define RD_WAKE_TIME 2500

// How near a bullet has to pass a joint to move it, and how hard.
#define RD_BULLET_REACH 12.0f
#define RD_BULLET_SPEED 220.0f


// What a bullet is tested against: the game's own hit model, the one a live
// soldier is shot with (cm_trace_lbd.cpp, CL_TraceDeep and SV_TraceDeep). It is
// a sphere or two on each of nineteen bones, placed in that bone's own frame,
// so evaluated against the bones as the ragdoll draws them it covers the same
// body a live man's does. The collision capsules are no use for this: they are
// cut thin on purpose and stop at the base of the skull, the wrists and the
// ankles.
//
// Copied rather than called. CL_TraceDeep is in the engine, out of cgame's
// reach, and returns the first bone in table order rather than the nearest.
typedef struct {
    const char *boneName;
    float       radius;
    vec3_t      offset;
    float       radius2; // a second sphere on the same bone, if non-zero
    vec3_t      offset2;
} rdHitSphere_t;

static const rdHitSphere_t rd_hitSpheres[] = {
    {"Bip01 Head",       5.0f, {2.5f, 3.5f, 0.0f},  0.0f, {0.0f, 0.0f, 0.0f}  }, // head
    {"Bip01 Head",       5.5f, {8.5f, 1.0f, 0.0f},  5.5f, {7.0f, -2.5f, 0.0f} }, // helmet
    {"Bip01 Neck",       4.0f, {3.5f, -1.0f, 0.0f}, 4.0f, {2.0f, 2.0f, 0.0f}  },
    {"Bip01 Spine2",     9.0f, {5.0f, 1.0f, 3.0f},  9.0f, {5.0f, 1.0f, -3.0f} },
    {"Bip01 Spine1",     8.0f, {2.0f, 1.0f, 3.0f},  8.0f, {2.0f, 1.0f, -3.0f} },
    {"Bip01 Spine",      9.0f, {1.0f, 1.0f, 3.0f},  9.0f, {1.0f, 1.0f, -3.0f} },
    {"Bip01 Pelvis",     9.0f, {-3.0f, 1.0f, 4.0f}, 9.0f, {-3.0f, 1.0f, -4.0f}},
    {"Bip01 R UpperArm", 7.0f, {4.0f, 0.0f, 0.0f},  6.0f, {11.5f, 0.0f, 0.0f} },
    {"Bip01 L UpperArm", 7.0f, {4.0f, 0.0f, 0.0f},  6.0f, {11.5f, 0.0f, 0.0f} },
    {"Bip01 R Thigh",    8.0f, {12.0f, 0.0f, 0.0f}, 8.0f, {22.0f, 0.0f, 0.0f} },
    {"Bip01 L Thigh",    8.0f, {12.0f, 0.0f, 0.0f}, 8.0f, {22.0f, 0.0f, 0.0f} },
    {"Bip01 R Forearm",  5.5f, {5.0f, 1.0f, 0.0f},  5.0f, {11.5f, 0.0f, 0.0f} },
    {"Bip01 L Forearm",  5.5f, {5.0f, 1.0f, 0.0f},  5.0f, {11.5f, 0.0f, 0.0f} },
    {"Bip01 R Calf",     7.5f, {8.0f, 0.0f, 0.0f},  6.5f, {19.0f, 0.0f, 0.0f} },
    {"Bip01 L Calf",     7.5f, {8.0f, 0.0f, 0.0f},  6.5f, {19.0f, 0.0f, 0.0f} },
    {"Bip01 R Hand",     6.0f, {5.0f, 1.0f, 0.0f},  0.0f, {0.0f, 0.0f, 0.0f}  },
    {"Bip01 L Hand",     6.0f, {5.0f, 1.0f, 0.0f},  0.0f, {0.0f, 0.0f, 0.0f}  },
    {"Bip01 R Foot",     6.0f, {3.0f, 2.0f, 0.0f},  5.0f, {4.0f, 9.0f, 0.0f}  },
    {"Bip01 L Foot",     6.0f, {3.0f, 2.0f, 0.0f},  5.0f, {4.0f, 9.0f, 0.0f}  }
};

#define RD_NUM_HIT_SPHERES ((int)(sizeof(rd_hitSpheres) / sizeof(rd_hitSpheres[0])))

// How far the hit spheres can reach past the joints the bounding sphere is
// built from: the helmet, eight and a half units up the head bone and five and
// a half across, is the farthest.
#define RD_HIT_BOUND_MARGIN 12.0f

// Which of rd_bones each hit sphere sits on, found once by name.
static int rd_hitSphereBone[RD_NUM_HIT_SPHERES];
static qboolean rd_hitSphereBonesFound;

static void CG_RagdollFindHitSphereBones(void)
{
    int i, b;

    for (i = 0; i < RD_NUM_HIT_SPHERES; i++) {
        rd_hitSphereBone[i] = -1;

        for (b = 0; b < RD_NUM_BONES; b++) {
            if (!Q_stricmp(rd_bones[b].boneName, rd_hitSpheres[i].boneName)) {
                rd_hitSphereBone[i] = b;
                break;
            }
        }
    }

    rd_hitSphereBonesFound = qtrue;
}

// How far along a bullet it enters one sphere, if it does.
static qboolean CG_RagdollBulletSphere(
    const vec3_t start, const vec3_t dir, float length, const vec3_t centre, float radius, float *entry
)
{
    vec3_t rel;
    float  along, missSq, depth, in;

    VectorSubtract(centre, start, rel);
    along  = DotProduct(rel, dir);
    missSq = DotProduct(rel, rel) - along * along;

    if (missSq > radius * radius) {
        return qfalse;
    }

    depth = sqrtf(radius * radius - missSq);
    in    = along - depth;

    // Behind the start or past the end of this stretch of the bullet.
    if (along + depth < 0.0f || in > length) {
        return qfalse;
    }

    *entry = in < 0.0f ? 0.0f : in;
    return qtrue;
}

// The drawn mesh, skinned by the renderer on request (GetSkinnedMesh). One
// scratch buffer serves every use: bullet hits, fitting the collision to the
// model, and the clipping measurement.
#define RD_SKIN_MAX_VERTS 8192
#define RD_SKIN_MAX_TRIS  12288

static skinnedVert_t rd_skinVerts[RD_SKIN_MAX_VERTS];
static int           rd_skinTris[RD_SKIN_MAX_TRIS * 3];
static int           rd_skinNumVerts;
static int           rd_skinNumTris;

// What is in the buffer: several rounds into one body in one frame skin it once.
static const cg_ragdoll_t *rd_skinOwner;
static int                 rd_skinTime;

// Whether this engine and renderer can hand the mesh over at all.
static qboolean CG_RagdollCanSkin(void)
{
    return cgi.apiversion >= 4 && cgi.R_GetSkinnedMesh ? qtrue : qfalse;
}

// Skins model into the scratch buffer. owner, if given, lets a second call for
// the same corpse in the same frame reuse the result.
static qboolean CG_RagdollSkin(refEntity_t *model, const cg_ragdoll_t *owner)
{
    if (!CG_RagdollCanSkin()) {
        return qfalse;
    }

    if (owner && owner == rd_skinOwner && cg.time == rd_skinTime && rd_skinNumVerts) {
        return qtrue;
    }

    rd_skinNumVerts = cgi.R_GetSkinnedMesh(
        model, rd_skinVerts, RD_SKIN_MAX_VERTS, rd_skinTris, RD_SKIN_MAX_TRIS, &rd_skinNumTris
    );
    rd_skinOwner = owner;
    rd_skinTime  = cg.time;

    return rd_skinNumVerts > 0 && rd_skinNumTris > 0 ? qtrue : qfalse;
}

//=============================================================
// Props (cg_props.cpp): ragdoll traces collide with them
//=============================================================

// CG_Trace, and then the props.
static void CG_RagdollTrace(
    trace_t     *result,
    const vec3_t start,
    const vec3_t mins,
    const vec3_t maxs,
    const vec3_t end,
    int          skipNumber,
    int          mask,
    qboolean     cylinder,
    qboolean     cliptoentities,
    const char  *description
)
{
    vec3_t smin, smax;
    int    i, k;

    CG_Trace(result, start, mins, maxs, end, skipNumber, mask, cylinder, cliptoentities, description);

    if (!cg_ragdoll_props || !cg_ragdoll_props->integer || !(mask & CONTENTS_SOLID)) {
        return;
    }

    CG_PropsLoad();

    if (!cg_numProps || result->allsolid) {
        return;
    }

    for (k = 0; k < 3; k++) {
        smin[k] = Q_min(start[k], end[k]) + mins[k];
        smax[k] = Q_max(start[k], end[k]) + maxs[k];
    }

    for (i = 0; i < cg_numProps; i++) {
        const cgProp_t *p = &cg_props[i];
        trace_t         tr;
        clipHandle_t    box;

        if (smin[0] > p->absmax[0] || smax[0] < p->absmin[0] || smin[1] > p->absmax[1] || smax[1] < p->absmin[1]
            || smin[2] > p->absmax[2] || smax[2] < p->absmin[2]) {
            continue;
        }

        if (rd_traceBody) {
            int n;

            for (n = 0; n < rd_traceBody->numIgnoredProps && rd_traceBody->ignoredProp[n] != i; n++) {
            }

            if (n < rd_traceBody->numIgnoredProps) {
                continue;
            }
        }

        box = cgi.CM_TempBoxModel(p->mins, p->maxs, CONTENTS_SOLID);
        cgi.CM_TransformedBoxTrace(&tr, start, end, mins, maxs, box, mask, p->origin, p->angles, cylinder);

        if (tr.allsolid || tr.fraction < result->fraction) {
            tr.entityNum = ENTITYNUM_WORLD;
            *result      = tr;
        } else if (tr.startsolid) {
            result->startsolid = qtrue;
        }
    }
}

// Props the body is already inside as it dies, which it is then left to pass
// through. A soldier sitting on a bench, lying in a bunk or standing close in at
// a table is inside that prop's box, since the box is solid right through where
// the real thing is not; pushed out of it, the whole body was thrown clear of
// it at once, a toe travelling forty eight units in one step.
// Whether any of the body's joints is inside a prop's box.
static qboolean CG_RagdollInsideProp(const cg_ragdoll_t *rd, const cgProp_t *p)
{
    int j, k;

    for (j = 0; j < RD_NUM_JOINTS; j++) {
        const float *x = rd->part[j].p;
        vec3_t       d;
        qboolean     inside = qtrue;

        if (x[0] < p->absmin[0] - rd->radius || x[0] > p->absmax[0] + rd->radius || x[1] < p->absmin[1] - rd->radius
            || x[1] > p->absmax[1] + rd->radius || x[2] < p->absmin[2] - rd->radius
            || x[2] > p->absmax[2] + rd->radius) {
            continue;
        }

        VectorSubtract(x, p->origin, d);
        for (k = 0; k < 3; k++) {
            const float local = DotProduct(d, p->axis[k]);

            if (local < p->mins[k] - rd->radius || local > p->maxs[k] + rd->radius) {
                inside = qfalse;
                break;
            }
        }

        if (inside) {
            return qtrue;
        }
    }

    return qfalse;
}

static void CG_RagdollNoteEnclosingProps(cg_ragdoll_t *rd)
{
    int i;

    rd->numIgnoredProps = 0;

    if (!cg_ragdoll_props->integer) {
        return;
    }

    CG_PropsLoad();

    for (i = 0; i < cg_numProps && rd->numIgnoredProps < (int)ARRAY_LEN(rd->ignoredProp); i++) {
        if (CG_RagdollInsideProp(rd, &cg_props[i])) {
            rd->ignoredProp[rd->numIgnoredProps++] = (short)i;
            CG_RagdollLog(rd, "inside prop %d as it died; passing through it", i);
        }
    }
}

// Once the body is clear of a prop it died inside, the prop is solid to it
// again: a corpse thrown back onto the bunk it was lying in lands on it.
static void CG_RagdollReleaseEnclosingProps(cg_ragdoll_t *rd)
{
    int n = 0, i;

    for (i = 0; i < rd->numIgnoredProps; i++) {
        const int index = rd->ignoredProp[i];

        if (index < cg_numProps && CG_RagdollInsideProp(rd, &cg_props[index])) {
            rd->ignoredProp[n++] = (short)index;
        }
    }

    rd->numIgnoredProps = n;
}

// For cg_ragdoll_debug 3: the last triangle a round went into, and where.
static vec3_t rd_lastHitTri[3];
static vec3_t rd_lastHitPoint;
static int    rd_lastHitTime;

// For cg_ragdoll_log: what the mesh traces are costing.
static int    rd_meshTraces;
static double rd_meshTraceUs;

// Where a bullet first enters the skinned mesh in the scratch buffer, by
// Moller-Trumbore against every triangle. Either face counts: a bullet can go
// in through a sleeve from inside a crooked arm as easily as from outside.
static qboolean CG_RagdollMeshEntry(const vec3_t start, const vec3_t dir, float length, float *entry)
{
    qboolean hit = qfalse;
    int      best = -1;
    int      i;

    for (i = 0; i < rd_skinNumTris; i++) {
        const float *v0 = rd_skinVerts[rd_skinTris[i * 3]].xyz;
        const float *v1 = rd_skinVerts[rd_skinTris[i * 3 + 1]].xyz;
        const float *v2 = rd_skinVerts[rd_skinTris[i * 3 + 2]].xyz;
        vec3_t       e1, e2, p, t, q;
        float        det, inv, u, v, along;

        VectorSubtract(v1, v0, e1);
        VectorSubtract(v2, v0, e2);
        CrossProduct(dir, e2, p);
        det = DotProduct(e1, p);

        if (det > -1e-6f && det < 1e-6f) {
            continue;
        }

        inv = 1.0f / det;
        VectorSubtract(start, v0, t);
        u = DotProduct(t, p) * inv;

        if (u < 0.0f || u > 1.0f) {
            continue;
        }

        CrossProduct(t, e1, q);
        v = DotProduct(dir, q) * inv;

        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }

        along = DotProduct(e2, q) * inv;

        if (along < 0.0f || along > length) {
            continue;
        }

        if (!hit || along < *entry) {
            *entry = along;
            best   = i;
            hit    = qtrue;
        }
    }

    if (hit) {
        for (i = 0; i < 3; i++) {
            VectorCopy(rd_skinVerts[rd_skinTris[best * 3 + i]].xyz, rd_lastHitTri[i]);
        }
        VectorMA(start, *entry, dir, rd_lastHitPoint);
        rd_lastHitTime = cg.time;
    }

    return hit;
}

// The model's own thickness, measured from its mesh once per model and kept.
// Sizes are stored per unit of rd->radius, which is three units at full size,
// so one measurement serves the same model at any scale.
typedef struct {
    dtiki_t *tiki;
    qboolean valid;
    float    limb[RD_NUM_LIMB_SEGMENTS];   // 0: too few vertices, keep the default
    float    trunkWide[RD_NUM_TRUNK_SEGMENTS];
    float    trunkDeep[RD_NUM_TRUNK_SEGMENTS];
    float    joint[RD_NUM_JOINTS];
} rdMeshFit_t;

#define RD_MAX_MESH_FITS 64

// Fewer vertices than this on a bone and the figure is left alone.
#define RD_FIT_MIN_VERTS 12

// Of a bone's vertices, how far out the one this far through them sits. The
// very outermost are pouches, straps and the rim of a helmet, which the body
// should not be held off the floor by.
#define RD_FIT_PERCENTILE 0.85f

static rdMeshFit_t rd_meshFits[RD_MAX_MESH_FITS];
static int         rd_numMeshFits;
static float       rd_fitScratch[RD_SKIN_MAX_VERTS];
static float       rd_fitScratch2[RD_SKIN_MAX_VERTS];
static float       rd_fitScratch3[RD_SKIN_MAX_VERTS];

static int CG_RagdollCompareFloat(const void *a, const void *b)
{
    const float fa = *(const float *)a;
    const float fb = *(const float *)b;

    return fa < fb ? -1 : (fa > fb ? 1 : 0);
}

static float CG_RagdollPercentile(float *values, int count)
{
    qsort(values, count, sizeof(float), CG_RagdollCompareFloat);
    return values[(int)((count - 1) * RD_FIT_PERCENTILE)];
}

// The ends of the head, hands and feet, measured around the bone into their
// tip joint and given to both joints.
static const short rd_fitExtremities[][2] = {
    {RD_HEAD,  RD_HEADTIP },
    {RD_LHAND, RD_LHANDTIP},
    {RD_RHAND, RD_RHANDTIP},
    {RD_LFOOT, RD_LTOE    },
    {RD_RFOOT, RD_RTOE    },
};

#define RD_NUM_FIT_EXTREMITIES ((int)(sizeof(rd_fitExtremities) / sizeof(rd_fitExtremities[0])))

// Every stretch of body a vertex can be counted against: the limbs, then the
// trunk, then the extremities above.
#define RD_NUM_FIT_SEGMENTS (RD_NUM_LIMB_SEGMENTS + RD_NUM_TRUNK_SEGMENTS + RD_NUM_FIT_EXTREMITIES)

static void CG_RagdollFitSegment(int n, int *a, int *b)
{
    if (n < RD_NUM_LIMB_SEGMENTS) {
        *a = rd_limbSegments[n].a;
        *b = rd_limbSegments[n].b;
    } else if (n < RD_NUM_LIMB_SEGMENTS + RD_NUM_TRUNK_SEGMENTS) {
        *a = rd_trunkSegments[n - RD_NUM_LIMB_SEGMENTS].a;
        *b = rd_trunkSegments[n - RD_NUM_LIMB_SEGMENTS].b;
    } else {
        *a = rd_fitExtremities[n - RD_NUM_LIMB_SEGMENTS - RD_NUM_TRUNK_SEGMENTS][0];
        *b = rd_fitExtremities[n - RD_NUM_LIMB_SEGMENTS - RD_NUM_TRUNK_SEGMENTS][1];
    }
}

// Which segment each skinned vertex was counted against, -1 for none.
static short rd_fitOwner[RD_SKIN_MAX_VERTS];

// The high percentile of the values on one side of zero, or 0 if there are
// too few of them to go by.
static float CG_RagdollOneSided(const float *values, int count, qboolean positive)
{
    int n = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (positive ? values[i] > 0.0f : values[i] < 0.0f) {
            rd_fitScratch2[n++] = fabsf(values[i]);
        }
    }

    if (n < RD_FIT_MIN_VERTS / 2) {
        return 0.0f;
    }

    return CG_RagdollPercentile(rd_fitScratch2, n);
}

// The smaller of the two sides, where both could be measured. Gear is carried
// on one side of a man, a pack on his back or a pouch at his hip, and taking
// the other side is what keeps it out: the collision stays the size of the
// body, and the pack sinks into the floor a little rather than propping the
// whole corpse up off it.
static float CG_RagdollBodySide(const float *values, int count)
{
    const float pos = CG_RagdollOneSided(values, count, qtrue);
    const float neg = CG_RagdollOneSided(values, count, qfalse);

    if (pos > 0.0f && neg > 0.0f) {
        return pos < neg ? pos : neg;
    }

    return pos > 0.0f ? pos : neg;
}

// Measures a model from its mesh in the pose the corpse is being seeded in.
//
// Each vertex is counted against whichever stretch of the body it lies
// nearest, not against the bone that moves it. The weights are no guide: much
// of a soldier's mesh follows helper bones ("helper Rhip" and the like) that
// the ragdoll knows nothing about, and counted by weight the upper arms of
// every model measured came out empty.
static void CG_RagdollMeasureMesh(cg_ragdoll_t *rd, refEntity_t *model, rdMeshFit_t *fit)
{
    vec3_t shift;
    vec3_t torso[3];
    float  unit = rd->radius;
    int    i, n, k;

    // The drawn mesh leaves out the model's load origin and the joints do not,
    // so the mesh is moved onto the joints before anything is compared.
    VectorClear(shift);
    for (k = 0; k < 3; k++) {
        VectorMA(shift, model->tiki->load_origin[k] * model->tiki->load_scale * model->scale, model->axis[k], shift);
    }

    if (!CG_RagdollTorsoFrame(rd, torso)) {
        return;
    }

    for (i = 0; i < rd_skinNumVerts; i++) {
        vec3_t p;
        float  best = 0.0f;

        VectorAdd(rd_skinVerts[i].xyz, shift, p);
        rd_fitOwner[i] = -1;

        for (n = 0; n < RD_NUM_FIT_SEGMENTS; n++) {
            vec3_t d, rel, closest;
            float  lengthSq, t, dist;
            int    a, b;

            CG_RagdollFitSegment(n, &a, &b);
            VectorSubtract(rd->part[b].p, rd->part[a].p, d);
            VectorSubtract(p, rd->part[a].p, rel);
            lengthSq = DotProduct(d, d);

            if (lengthSq < 0.0001f) {
                continue;
            }

            t = Q_clamp_float(DotProduct(rel, d) / lengthSq, 0.0f, 1.0f);
            VectorMA(rd->part[a].p, t, d, closest);
            dist = Distance(p, closest);

            if (rd_fitOwner[i] < 0 || dist < best) {
                rd_fitOwner[i] = n;
                best           = dist;
            }
        }
    }

    for (n = 0; n < RD_NUM_FIT_SEGMENTS; n++) {
        vec3_t axis, side, front;
        float  length;
        int    count = 0;
        int    a, b;

        CG_RagdollFitSegment(n, &a, &b);
        VectorSubtract(rd->part[b].p, rd->part[a].p, axis);
        length = VectorNormalize(axis);

        if (length < 0.001f) {
            continue;
        }

        VectorMA(torso[1], -DotProduct(torso[1], axis), axis, side);
        if (VectorNormalize(side) < 0.001f) {
            continue;
        }
        CrossProduct(axis, side, front);

        for (i = 0; i < rd_skinNumVerts; i++) {
            vec3_t rel, off;
            float  along;

            if (rd_fitOwner[i] != n) {
                continue;
            }

            VectorAdd(rd_skinVerts[i].xyz, shift, rel);
            VectorSubtract(rel, rd->part[a].p, rel);
            along = DotProduct(rel, axis);

            // Only what surrounds the stretch, not what caps either end of it.
            if (along < 0.0f || along > length) {
                continue;
            }

            VectorMA(rel, -along, axis, off);

            if (n >= RD_NUM_LIMB_SEGMENTS && n < RD_NUM_LIMB_SEGMENTS + RD_NUM_TRUNK_SEGMENTS) {
                // signed, so each side of the trunk can be told apart
                rd_fitScratch[count]  = DotProduct(off, side);
                rd_fitScratch3[count] = DotProduct(off, front);
            } else {
                rd_fitScratch[count] = VectorLength(off);
            }
            count++;
        }

        if (count < RD_FIT_MIN_VERTS) {
            continue;
        }

        if (n < RD_NUM_LIMB_SEGMENTS) {
            fit->limb[n] = CG_RagdollPercentile(rd_fitScratch, count) / unit;
        } else if (n < RD_NUM_LIMB_SEGMENTS + RD_NUM_TRUNK_SEGMENTS) {
            const float wide = CG_RagdollBodySide(rd_fitScratch, count);
            const float deep = CG_RagdollBodySide(rd_fitScratch3, count);

            if (wide > 0.0f && deep > 0.0f) {
                fit->trunkWide[n - RD_NUM_LIMB_SEGMENTS] = wide / unit;
                fit->trunkDeep[n - RD_NUM_LIMB_SEGMENTS] = deep / unit;
            }
        } else {
            const float r = CG_RagdollPercentile(rd_fitScratch, count) / unit;

            fit->joint[a] = r;
            fit->joint[b] = r;
        }
    }
}

// Sizes the corpse's collision from its model's mesh (cg_ragdoll_meshfit).
// The hand-set figures in rd_joints are for one average man; this is the man
// actually lying there, greatcoat, pack and all.
static void CG_RagdollFitToMesh(cg_ragdoll_t *rd, refEntity_t *model)
{
    const float  scale = cg_ragdoll_meshfit->value;
    rdMeshFit_t *fit   = NULL;
    int          i;

    if (scale <= 0.0f || !model->tiki || !CG_RagdollCanSkin()) {
        return;
    }

    for (i = 0; i < rd_numMeshFits; i++) {
        if (rd_meshFits[i].tiki == model->tiki) {
            fit = &rd_meshFits[i];
            break;
        }
    }

    if (!fit) {
        // A full table is only a missed saving: measure into the last slot.
        fit = &rd_meshFits[rd_numMeshFits < RD_MAX_MESH_FITS ? rd_numMeshFits++ : RD_MAX_MESH_FITS - 1];
        memset(fit, 0, sizeof(*fit));
        fit->tiki = model->tiki;

        if (CG_RagdollSkin(model, NULL)) {
            CG_RagdollMeasureMesh(rd, model, fit);
            fit->valid = qtrue;

            if (cg_ragdoll_log->integer) {
                cgi.Printf(
                    "ragdoll %d: measured %s from %d vertices: upper arm %.1f (was %.1f), thigh %.1f (was %.1f), "
                    "chest %.1f x %.1f (was %.1f x %.1f), head %.1f (was %.1f)\n",
                    cg.time,
                    cgi.TIKI_Name(model->tiki),
                    rd_skinNumVerts,
                    fit->limb[0] * rd->radius,
                    rd->limbRadius[0],
                    fit->limb[RD_FIRST_LEG_SEGMENT] * rd->radius,
                    rd->limbRadius[RD_FIRST_LEG_SEGMENT],
                    fit->trunkWide[2] * rd->radius,
                    fit->trunkDeep[2] * rd->radius,
                    rd->trunkWide[2],
                    rd->trunkDeep[2],
                    fit->joint[RD_HEAD] * rd->radius,
                    rd->jointRadius[RD_HEAD]
                );
            }
        }
    }

    if (!fit->valid) {
        return;
    }

    for (i = 0; i < RD_NUM_LIMB_SEGMENTS; i++) {
        if (fit->limb[i] > 0.0f) {
            rd->limbRadius[i] = fit->limb[i] * rd->radius * scale;
        }
    }

    for (i = 0; i < RD_NUM_TRUNK_SEGMENTS; i++) {
        if (fit->trunkWide[i] > 0.0f) {
            rd->trunkWide[i] = fit->trunkWide[i] * rd->radius * scale;
            rd->trunkDeep[i] = fit->trunkDeep[i] * rd->radius * scale;
        }
    }

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        if (fit->joint[i] > 0.0f) {
            rd->jointRadius[i] = fit->joint[i] * rd->radius * scale;
        }
    }
}

// Where a bullet running from start along dir for length first enters the
// body as drawn. Returns qfalse if it misses it altogether.
static qboolean CG_RagdollBulletEntry(cg_ragdoll_t *rd, const vec3_t start, const vec3_t dir, float length, float *entry)
{
    vec3_t   drop = {0.0f, 0.0f, 0.0f};
    qboolean hit  = qfalse;
    int      i;

    if (!rd_hitSphereBonesFound) {
        CG_RagdollFindHitSphereBones();
    }

    // A sleeping corpse is drawn this far below where it settled, riding its
    // entity down as it sinks.
    if (rd->state == RD_SLEEPING && rd->sleepPinned) {
        drop[2] = rd->sleepDrop;
    }

    for (i = 0; i < RD_NUM_HIT_SPHERES; i++) {
        const rdHitSphere_t *hs = &rd_hitSpheres[i];
        const int            b  = rd_hitSphereBone[i];
        int                  k;

        if (b < 0 || rd->boneIndex[b] < 0) {
            continue;
        }

        for (k = 0; k < 2; k++) {
            const float  radius = k ? hs->radius2 : hs->radius;
            const float *offset = k ? hs->offset2 : hs->offset;
            vec3_t       centre;
            float        in;

            if (radius <= 0.0f) {
                continue;
            }

            VectorAdd(rd->bonePos[b], drop, centre);
            VectorMA(centre, offset[0], rd->boneAxis[b][0], centre);
            VectorMA(centre, offset[1], rd->boneAxis[b][1], centre);
            VectorMA(centre, offset[2], rd->boneAxis[b][2], centre);

            if (CG_RagdollBulletSphere(start, dir, length, centre, radius, &in) && (!hit || in < *entry)) {
                *entry = in;
                hit    = qtrue;
            }
        }
    }

    // The spheres only say the round came close enough to be worth checking.
    // Where the mesh can be had, it decides: a round through the gap between an
    // arm and the chest misses, and one that clips a boot hits the boot.
    if (hit && cg_ragdoll_meshhits->integer && rd->haveDrawn) {
        auto     began = std::chrono::steady_clock::now();
        qboolean onMesh;

        if (CG_RagdollSkin(&rd->drawn, rd)) {
            onMesh = CG_RagdollMeshEntry(start, dir, length, entry);

            rd_meshTraces++;
            rd_meshTraceUs +=
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();

            return onMesh;
        }
    }

    return hit;
}

// Puts a settled corpse back to work.
static void CG_RagdollWake(cg_ragdoll_t *rd)
{
    int i;

    if (rd->state != RD_SLEEPING && rd->state != RD_ACTIVE) {
        return;
    }

    if (rd->state == RD_SLEEPING) {
        CG_RagdollLog(rd, "woken");

        // While it slept the drawn body followed the entity down as it sank,
        // and the particles did not. Carrying that across means the corpse
        // starts moving from where it was last seen rather than jumping back up
        // to where it stopped simulating.
        if (rd->sleepPinned && rd->sleepDrop != 0.0f) {
            for (i = 0; i < RD_NUM_JOINTS; i++) {
                rd->part[i].p[2] += rd->sleepDrop;
                rd->part[i].pPrev[2] += rd->sleepDrop;
            }
        }

        rd->state = RD_ACTIVE;

        if (rd->jolt) {
            CG_JoltRagdollWake(rd->jolt);
        }

        // The clock starts again from now. It is not advanced while a body
        // sleeps, so waking it left the whole sleep owed as simulation time:
        // five steps were run on the next frame, the most a frame may take,
        // and a body punted or shot as it woke travelled five frames' worth
        // at once, the whole corpse jumping thirty and forty units.
        rd->lastTime = cg.time;
        rd->accum    = 0.0f;
    }

    rd->sleepPinned = qfalse;
    rd->sleepDrop   = 0.0f;
    rd->quietSince  = cg.time;
    rd->wakeUntil   = cg.time + RD_WAKE_TIME;
    rd->lastWake    = cg.time;
    rd->wakeCount++;

    // A joint that had given up getting out of the world is given another go:
    // whatever just hit the body may well have freed it.
    // The hold is released so the body can actually move, but how long each
    // joint has been buried is not forgotten.
    //
    // Clearing that as well made every hit restart the half second of
    // struggling that the hold exists to end. A corpse lying in geometry and
    // shot at more than once never reached the hold at all: the worst one
    // traced had a bone inside the world for nine hundred and ninety three
    // frames of thirteen hundred and was woken five times, and it shivered for
    // all of them. Left alone, a joint that is still buried is pinned again by
    // the very next probe, and one the hit actually freed is cleared by that
    // same probe, which is what it is for.
    rd->stuckMask = 0;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rd->part[i].invMass = rd_joints[i].invMass;
    }
}

// A bullet passing through a body that is already dead.
//
// This is handed the line the bullet actually took, after the fact, and moves
// whatever it happened to pass through. The server never traces against the
// corpse and nothing here changes what the bullet hits or damages: corpses are
// built on the client and the server knows nothing about them, so a corpse that
// stopped bullets for real would stop them on one machine and not another, and a
// man could be sheltered by a body his killer cannot see.
//
// What it does change is what is seen and heard. A round that goes into a
// corpse draws blood there and makes no impact on whatever lay behind it.

// Whether a round can be stopped by this corpse, drawing blood where it goes
// in. Only a body that is being drawn, and only once the server has stopped
// colliding with it: while it is still solid the server reports the hit itself,
// and the two would play on top of each other.
static qboolean CG_RagdollCanStopBullet(const cg_ragdoll_t *rd)
{
    return rd->state != RD_FREE && rd->boundRadius && rd->entityNum != ENTITYNUM_NONE
            && cg_entities[rd->entityNum].currentValid && !cg_entities[rd->entityNum].currentState.solid
             ? qtrue
             : qfalse;
}

qboolean CG_RagdollNoteBullet(const vec3_t start, const vec3_t end, int large, vec3_t stopAt)
{
    const float   strength = cg_ragdoll_shove->value * (large ? 1.6f : 1.0f);
    cg_ragdoll_t *stopper  = NULL;
    float         stopDist = 0.0f;
    vec3_t        dir;
    float         length;
    int           n, i;

    VectorSubtract(end, start, dir);
    length = VectorNormalize(dir);

    if (length < 1.0f) {
        return qfalse;
    }

    // The first corpse the round goes into is where it ends. Nothing beyond
    // is shoved or bloodied, and the caller drops the impact on whatever was
    // behind the body: a shot into a corpse lying on the ground must not also
    // kick up the dirt under it.
    for (n = 0; n < MAX_RAGDOLLS; n++) {
        cg_ragdoll_t *rd = &cg_ragdolls[n];
        vec3_t        toCentre, nearest;
        float         centreAlong, entry;

        if (!CG_RagdollCanStopBullet(rd)) {
            continue;
        }

        VectorSubtract(rd->boundCentre, start, toCentre);
        centreAlong = DotProduct(toCentre, dir);

        if (centreAlong < -rd->boundRadius - RD_HIT_BOUND_MARGIN
            || centreAlong > length + rd->boundRadius + RD_HIT_BOUND_MARGIN) {
            continue;
        }

        VectorMA(start, Q_clamp_float(centreAlong, 0.0f, length), dir, nearest);
        if (Distance(rd->boundCentre, nearest) > rd->boundRadius + RD_HIT_BOUND_MARGIN) {
            continue;
        }

        if (CG_RagdollBulletEntry(rd, start, dir, length, &entry) && (!stopper || entry < stopDist)) {
            stopper  = rd;
            stopDist = entry;
        }
    }

    if (stopper) {
        vec3_t norm;

        VectorMA(start, stopDist, dir, stopAt);
        VectorNegate(dir, norm);
        CG_AddCorpseFleshImpact(stopAt, norm, large);

        // Far enough on to push the whole of the body it went into.
        length = Q_min(length, stopDist + RD_BULLET_REACH * 2.0f);
    }

    if (strength <= 0.0f) {
        return stopper ? qtrue : qfalse;
    }

    for (n = 0; n < MAX_RAGDOLLS; n++) {
        cg_ragdoll_t *rd = &cg_ragdolls[n];
        qboolean      touched = qfalse;

        vec3_t toCentre, nearest;
        float  centreAlong;

        if (rd->state == RD_FREE || !rd->boundRadius) {
            continue;
        }

        // Thrown out whole before any joint is looked at. Automatic fire calls
        // this many times a second and most rounds go nowhere near a body.
        VectorSubtract(rd->boundCentre, start, toCentre);
        centreAlong = DotProduct(toCentre, dir);

        if (centreAlong < -rd->boundRadius || centreAlong > length + rd->boundRadius) {
            continue;
        }

        VectorMA(start, centreAlong < 0.0f ? 0.0f : (centreAlong > length ? length : centreAlong), dir, nearest);
        VectorSubtract(rd->boundCentre, nearest, toCentre);

        if (VectorLength(toCentre) > rd->boundRadius + RD_BULLET_REACH) {
            continue;
        }

        for (i = 0; i < RD_NUM_JOINTS; i++) {
            vec3_t rel, closest;
            float  along, away;

            VectorSubtract(rd->part[i].p, start, rel);
            along = DotProduct(rel, dir);

            if (along < 0.0f || along > length) {
                continue;
            }

            VectorMA(start, along, dir, closest);
            VectorSubtract(rd->part[i].p, closest, rel);
            away = VectorLength(rel);

            if (away > RD_BULLET_REACH) {
                continue;
            }

            // Hardest along the line and tailing off to nothing at the edge, so
            // a round that grazes a corpse twitches it and one through the
            // middle throws it.
            VectorMA(
                rd->part[i].v,
                RD_BULLET_SPEED * strength * (1.0f - away / RD_BULLET_REACH),
                dir,
                rd->part[i].v
            );

            touched = qtrue;
        }

        if (touched) {
            CG_RagdollWake(rd);
        }
    }

    return stopper ? qtrue : qfalse;
}

//=============================================================
// The grabber
//=============================================================

// A tractor beam for handling corpses, after the physics gun in the Half-Life 2
// beta, for testing the ragdolls by hand: pick a body up by whatever part is
// under the crosshair, carry it, swing it, drop it on things and throw it.
// Entirely client side, like the ragdolls themselves, so it works in any game
// and nobody else sees it. Bound to keys rather than being a weapon, and off
// until cg_ragdoll_grab is set, which binds them (see rd_grabBinds):
//
//   mouse2       +rdgrab          hold to carry the body under the crosshair
//   mouse3       rdpunt           throw what is held, or knock what is aimed at
//   mwheeldown   rdgrab_nearer
//   mwheelup     rdgrab_farther
static struct {
    qboolean held;
    int      slot;
    int      startTime; // tells the corpse apart from a later one in the same slot
    int      joint;
    float    dist;
} rd_grab;

// How much nearer or farther one turn of the wheel carries a body.
#define RD_GRAB_WHEEL 16.0f

// How near the eye a held body may be brought.
#define RD_GRAB_MIN_DIST 24.0f

// The beam drawn while holding: how wide, in how many pieces its curve is
// drawn, how far along the aim its curve is pulled, and how many units of beam
// one repeat of its texture covers.
#define RD_GRAB_BEAM_WIDTH    1.2f
#define RD_GRAB_BEAM_SEGMENTS 20
#define RD_GRAB_BEAM_PULL     0.66f
#define RD_GRAB_BEAM_REPEAT   96.0f

// scripts/opm_ragdoll.shader in zzzzzzzzz-opm-ragdoll.pk3. The game has no
// beam texture of its own: the retail beam shaders name images it never
// shipped, and draw as the missing texture.
#define RD_GRAB_BEAM_SHADER "opm/grabbeam"

static void CG_RagdollViewRay(vec3_t start, vec3_t dir)
{
    VectorCopy(cg.refdef.vieworg, start);
    VectorCopy(cg.refdef.viewaxis[0], dir);
}

// The drawn corpse the crosshair is on, nearest first and not behind a wall,
// with how far along the view it is hit.
static cg_ragdoll_t *CG_RagdollUnderCrosshair(float *entry)
{
    cg_ragdoll_t *best = NULL;
    vec3_t        start, dir, end;
    trace_t       tr;
    float         length;
    int           n;

    if (!cg.snap) {
        return NULL;
    }

    CG_RagdollViewRay(start, dir);
    VectorMA(start, cg_ragdoll_grabrange->value, dir, end);

    // Only as far as the first wall.
    CG_RagdollTrace(&tr, start, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, RD_CLIPMASK, qfalse, qfalse, "ragdoll grab");
    length = cg_ragdoll_grabrange->value * tr.fraction;

    for (n = 0; n < MAX_RAGDOLLS; n++) {
        cg_ragdoll_t *rd = &cg_ragdolls[n];
        float         in;

        if (rd->state == RD_FREE || !rd->haveDrawn || rd->entityNum == ENTITYNUM_NONE
            || !cg_entities[rd->entityNum].currentValid) {
            continue;
        }

        if (CG_RagdollBulletEntry(rd, start, dir, length, &in) && (!best || in < *entry)) {
            best   = rd;
            *entry = in;
        }
    }

    return best;
}

// The joint nearest a point, which is the one the beam takes hold of.
static int CG_RagdollNearestJoint(const cg_ragdoll_t *rd, const vec3_t point)
{
    float bestDist = 0.0f;
    int   best     = 0;
    int   i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const float d = DistanceSquared(rd->part[i].p, point);

        if (!i || d < bestDist) {
            best     = i;
            bestDist = d;
        }
    }

    return best;
}

static cg_ragdoll_t *CG_RagdollHeld(void)
{
    cg_ragdoll_t *rd;

    if (!rd_grab.held) {
        return NULL;
    }

    rd = &cg_ragdolls[rd_grab.slot];

    // Gone, or the slot now holds somebody else.
    if (rd->state == RD_FREE || rd->startTime != rd_grab.startTime) {
        rd_grab.held = qfalse;
        return NULL;
    }

    return rd;
}

static void CG_RagdollRelease(void)
{
    cg_ragdoll_t *rd = CG_RagdollHeld();

    if (rd) {
        rd->part[rd->grabJoint].invMass = rd_joints[rd->grabJoint].invMass;
        rd->grabJoint                   = -1;
    }

    rd_grab.held = qfalse;
}

// Keeps a body awake and moving; asleep, it would ignore the beam.
static void CG_RagdollKeepAwake(cg_ragdoll_t *rd)
{
    if (rd->state == RD_SLEEPING) {
        CG_RagdollWake(rd);
    }

    rd->quietSince = cg.time;
    rd->wakeUntil  = cg.time + RD_WAKE_TIME;
    rd->lastWake   = cg.time;
}

extern "C" void CG_RagdollGrabDown_f(void)
{
    cg_ragdoll_t *rd;
    vec3_t        start, dir, point;
    float         entry;

    CG_RagdollRelease();
    CG_PhysicsGrabRelease();

    if (!cg_ragdoll_grab->integer) {
        return;
    }

    rd = CG_RagdollUnderCrosshair(&entry);
    CG_RagdollViewRay(start, dir);

    // A prop, if one is nearer along the aim than any body.
    {
        float propEntry;

        if (CG_PhysicsGrabCandidate(start, dir, cg_ragdoll_grabrange->value, &propEntry) && (!rd || propEntry < entry)) {
            CG_PhysicsGrabStart(start, dir, cg_ragdoll_grabrange->value, RD_GRAB_MIN_DIST);
            return;
        }
    }

    if (!rd) {
        return;
    }

    VectorMA(start, entry, dir, point);

    rd_grab.held      = qtrue;
    rd_grab.slot      = (int)(rd - cg_ragdolls);
    rd_grab.startTime = rd->startTime;
    rd_grab.joint     = CG_RagdollNearestJoint(rd, point);
    rd_grab.dist      = Q_max(entry, RD_GRAB_MIN_DIST);

    rd->grabJoint = rd_grab.joint;
    VectorCopy(rd->part[rd_grab.joint].p, rd->grabTarget);
    CG_RagdollKeepAwake(rd);

    CG_RagdollLog(rd, "grabbed by joint %d, %.0f units away", rd_grab.joint, rd_grab.dist);
}

extern "C" void CG_RagdollGrabUp_f(void)
{
    CG_RagdollRelease();
    CG_PhysicsGrabRelease();
}

extern "C" void CG_RagdollGrabNearer_f(void)
{
    if (!cg_ragdoll_grab->integer) {
        return;
    }

    rd_grab.dist = Q_max(rd_grab.dist - RD_GRAB_WHEEL, RD_GRAB_MIN_DIST);
    CG_PhysicsGrabSetDistance(Q_max(CG_PhysicsGrabDistance() - RD_GRAB_WHEEL, RD_GRAB_MIN_DIST));
}

extern "C" void CG_RagdollGrabFarther_f(void)
{
    if (!cg_ragdoll_grab->integer) {
        return;
    }

    rd_grab.dist = Q_min(rd_grab.dist + RD_GRAB_WHEEL, cg_ragdoll_grabrange->value);
    CG_PhysicsGrabSetDistance(Q_min(CG_PhysicsGrabDistance() + RD_GRAB_WHEEL, cg_ragdoll_grabrange->value));
}

// Throws the body being carried, or knocks the one the crosshair is on: the
// whole body is sent along the view, the part hit hardest.
extern "C" void CG_RagdollPunt_f(void)
{
    cg_ragdoll_t *rd = CG_RagdollHeld();
    vec3_t        start, dir, point;
    float         entry = 0.0f;
    int           i;

    if (!cg_ragdoll_grab->integer) {
        return;
    }

    CG_RagdollViewRay(start, dir);

    // A prop held, or aimed at nearer than any body, is thrown or knocked.
    if (CG_PhysicsGrabHeld()) {
        CG_PhysicsPunt(start, dir, cg_ragdoll_grabrange->value, cg_ragdoll_puntspeed->value);
        return;
    }

    if (rd) {
        VectorCopy(rd->part[rd->grabJoint].p, point);
        CG_RagdollRelease();
    } else {
        float propEntry;

        rd = CG_RagdollUnderCrosshair(&entry);
        if (CG_PhysicsGrabCandidate(start, dir, cg_ragdoll_grabrange->value, &propEntry) && (!rd || propEntry < entry)) {
            CG_PhysicsPunt(start, dir, cg_ragdoll_grabrange->value, cg_ragdoll_puntspeed->value);
            return;
        }
        if (!rd) {
            return;
        }
        VectorMA(start, entry, dir, point);
    }

    CG_RagdollKeepAwake(rd);

    {
        vec3_t dv[RD_NUM_JOINTS];

        for (i = 0; i < RD_NUM_JOINTS; i++) {
            // Full speed at the part hit, down to half of it a body's length
            // away, so a punt to the legs turns the body over rather than
            // sliding it.
            const float falloff = 1.0f - 0.5f * Q_min(Distance(rd->part[i].p, point) / 64.0f, 1.0f);

            VectorScale(dir, cg_ragdoll_puntspeed->value * falloff, dv[i]);
            VectorAdd(rd->part[i].v, dv[i], rd->part[i].v);
        }

        // A Jolt body takes it through the physics.
        if (rd->jolt) {
            CG_JoltRagdollAddVelocity(rd->jolt, dv);
        }
    }

    CG_RagdollLog(rd, "punted");
}

// The keys cg_ragdoll_grab takes over, what it binds them to, and what a
// stock game has on them, which is what they go back to when what they did
// before cannot be found.
static const struct {
    const char *key;
    const char *command;
    const char *stock;
} rd_grabBinds[] = {
    {"MOUSE2",     "+rdgrab",        "+attacksecondary"},
    {"MOUSE3",     "rdpunt",         ""                },
    {"MWHEELDOWN", "rdgrab_nearer",  "weapprev"        },
    {"MWHEELUP",   "rdgrab_farther", "weapnext"        },
};

#define RD_NUM_GRAB_BINDS ((int)(sizeof(rd_grabBinds) / sizeof(rd_grabBinds[0])))

// Whether a bound command is one of the grabber's own, which is never worth
// putting back.
static qboolean CG_RagdollIsGrabCommand(const char *command)
{
    return !Q_stricmpn(command, "+rdgrab", 7) || !Q_stricmpn(command, "-rdgrab", 7)
                || !Q_stricmpn(command, "rdgrab", 6) || !Q_stricmpn(command, "rdpunt", 6)
             ? qtrue
             : qfalse;
}

// What each of the grabber's keys is bound to now. cgame cannot ask the engine
// for a key's binding, but the engine rewrites the config as soon as a bind
// changes, so it is read from there. Fills saved with "KEY=command|..." and
// uses the stock binding for any key that holds one of the grabber's own
// commands already, or something that would not survive the round trip.
static void CG_RagdollReadBinds(char *saved, int size)
{
    char *buf = NULL;
    int   i;

    saved[0] = 0;
    cgi.FS_ReadFile("configs/" CONFIG_PREFIX ".cfg", (void **)&buf, qtrue);

    for (i = 0; i < RD_NUM_GRAB_BINDS; i++) {
        char        command[MAX_STRING_CHARS];
        const char *found = rd_grabBinds[i].stock;

        if (buf) {
            char        needle[64];
            const char *line;

            Com_sprintf(needle, sizeof(needle), "\nbind %s ", rd_grabBinds[i].key);
            line = Q_stristr(buf, needle);

            if (line) {
                const char *p = line + strlen(needle);
                int         n = 0;

                if (*p == '"') {
                    p++;
                }
                while (*p && *p != '"' && *p != '\r' && *p != '\n' && n < (int)sizeof(command) - 1) {
                    command[n++] = *p++;
                }
                command[n] = 0;

                if (!CG_RagdollIsGrabCommand(command) && !strchr(command, '|')) {
                    found = command;
                }
            }
        }

        Q_strcat(saved, size, va("%s%s=%s", i ? "|" : "", rd_grabBinds[i].key, found));
    }

    if (buf) {
        cgi.FS_FreeFile(buf);
    }
}

// Puts back the bindings in saved, or the stock ones if there are none.
static void CG_RagdollRestoreBinds(const char *saved)
{
    int i;

    for (i = 0; i < RD_NUM_GRAB_BINDS; i++) {
        char        command[MAX_STRING_CHARS];
        const char *entry;

        Q_strncpyz(command, rd_grabBinds[i].stock, sizeof(command));

        entry = Q_stristr(saved, va("%s=", rd_grabBinds[i].key));
        if (entry) {
            const char *p = entry + strlen(rd_grabBinds[i].key) + 1;
            int         n = 0;

            while (*p && *p != '|' && n < (int)sizeof(command) - 1) {
                command[n++] = *p++;
            }
            command[n] = 0;
        }

        if (command[0]) {
            cgi.Cmd_Execute(EXEC_APPEND, va("bind %s \"%s\"\n", rd_grabBinds[i].key, command));
        } else {
            cgi.Cmd_Execute(EXEC_APPEND, va("unbind %s\n", rd_grabBinds[i].key));
        }
    }
}

// Once a frame: follows cg_ragdoll_grab. Turned on, what the keys did is kept
// and the grabber's commands are bound; turned off, it is let go of and the
// keys are given back.
static void CG_RagdollGrabBindings(void)
{
    static int applied = -1;
    const int  wanted  = cg_ragdoll_grab->integer ? 1 : 0;
    int        i;

    if (wanted == applied) {
        return;
    }

    if (wanted) {
        // Kept only the first time: on a later map, or a later launch with it
        // still on, the keys already hold the grabber's commands.
        if (!cg_ragdoll_grabsaved->string[0]) {
            char saved[MAX_STRING_CHARS];

            CG_RagdollReadBinds(saved, sizeof(saved));
            cgi.Cvar_Set("cg_ragdoll_grabsaved", saved);
        }

        for (i = 0; i < RD_NUM_GRAB_BINDS; i++) {
            cgi.Cmd_Execute(EXEC_APPEND, va("bind %s \"%s\"\n", rd_grabBinds[i].key, rd_grabBinds[i].command));
        }

        if (applied == 0) {
            cgi.Printf("Ragdoll grabber on: hold mouse2 to grab, mouse3 punts, the wheel moves it nearer and farther\n");
        }
    } else {
        CG_RagdollRelease();

        // Nothing to give back if it was never on.
        if (cg_ragdoll_grabsaved->string[0]) {
            CG_RagdollRestoreBinds(cg_ragdoll_grabsaved->string);
            cgi.Cvar_Set("cg_ragdoll_grabsaved", "");

            if (applied == 1) {
                cgi.Printf("Ragdoll grabber off: the keys are back to what they were\n");
            }
        }
    }

    applied = wanted;
}

// The point t of the way along the grabber's curve, and the direction it runs.
static void CG_RagdollGrabCurve(
    const vec3_t from, const vec3_t control, const vec3_t to, float t, vec3_t point, vec3_t tangent
)
{
    const float u = 1.0f - t;
    int         k;

    for (k = 0; k < 3; k++) {
        point[k]   = u * u * from[k] + 2.0f * u * t * control[k] + t * t * to[k];
        tangent[k] = 2.0f * u * (control[k] - from[k]) + 2.0f * t * (to[k] - control[k]);
    }
}

static void CG_RagdollDrawGrabBeam(const vec3_t muzzle, const vec3_t dir, const vec3_t target, const vec3_t held)
{
    static qhandle_t shader;
    polyVert_t       quad[4];
    vec3_t           control, point, tangent, side;
    vec3_t           prevPoint, prevSide;
    float            along = 0.0f;
    int              n, k;

    if (!shader) {
        shader = cgi.R_RegisterShader(RD_GRAB_BEAM_SHADER);
    }

    VectorMA(muzzle, RD_GRAB_BEAM_PULL * Distance(muzzle, target), dir, control);

    for (n = 0; n <= RD_GRAB_BEAM_SEGMENTS; n++) {
        vec3_t toEye;

        CG_RagdollGrabCurve(muzzle, control, held, (float)n / RD_GRAB_BEAM_SEGMENTS, point, tangent);

        // Across the beam, square to both its run and the line to the eye, so
        // the strip is seen face on from wherever the eye is.
        VectorSubtract(cg.refdef.vieworg, point, toEye);
        CrossProduct(tangent, toEye, side);
        if (VectorNormalize(side) < 0.0001f) {
            VectorCopy(cg.refdef.viewaxis[1], side);
        }
        VectorScale(side, RD_GRAB_BEAM_WIDTH, side);

        if (n) {
            const float from = along;

            along += Distance(prevPoint, point) / RD_GRAB_BEAM_REPEAT;

            VectorAdd(prevPoint, prevSide, quad[0].xyz);
            VectorSubtract(prevPoint, prevSide, quad[1].xyz);
            VectorSubtract(point, side, quad[2].xyz);
            VectorAdd(point, side, quad[3].xyz);

            quad[0].st[0] = from;
            quad[0].st[1] = 0.0f;
            quad[1].st[0] = from;
            quad[1].st[1] = 1.0f;
            quad[2].st[0] = along;
            quad[2].st[1] = 1.0f;
            quad[3].st[0] = along;
            quad[3].st[1] = 0.0f;

            for (k = 0; k < 4; k++) {
                quad[k].modulate[0] = quad[k].modulate[1] = quad[k].modulate[2] = quad[k].modulate[3] = 255;
            }

            cgi.R_AddPolyToScene(shader, 4, quad, 0);
        }

        VectorCopy(point, prevPoint);
        VectorCopy(side, prevSide);
    }
}

// Once a frame, before any body is stepped: moves the end of the beam with the
// view, and draws it.
static void CG_RagdollGrabUpdate(void)
{
    cg_ragdoll_t *rd = CG_RagdollHeld();
    vec3_t        start, dir, target, muzzle;
    trace_t       tr;

    // A prop held: the physics carries it towards the end of the beam, which
    // stops short of walls as a body's does.
    if (CG_PhysicsGrabHeld() && cg.snap) {
        vec3_t held;

        CG_RagdollViewRay(start, dir);
        VectorMA(start, CG_PhysicsGrabDistance(), dir, target);

        cgi.CM_BoxTrace(&tr, start, target, vec3_origin, vec3_origin, 0, MASK_SOLID, qfalse);
        if (tr.fraction < 1.0f) {
            VectorMA(tr.endpos, -4.0f, dir, target);
        }

        CG_PhysicsGrabSetTarget(target);
        CG_PhysicsGrabPoint(held);

        VectorMA(start, 12.0f, dir, muzzle);
        VectorMA(muzzle, -6.0f, cg.refdef.viewaxis[1], muzzle);
        VectorMA(muzzle, -6.0f, cg.refdef.viewaxis[2], muzzle);
        CG_RagdollDrawGrabBeam(muzzle, dir, target, held);
        return;
    }

    if (!rd || !cg.snap) {
        return;
    }

    CG_RagdollViewRay(start, dir);
    VectorMA(start, rd_grab.dist, dir, target);

    // Never carried into a wall: the end of the beam stops short of it.
    CG_RagdollTrace(&tr, start, vec3_origin, vec3_origin, target, cg.snap->ps.clientNum, RD_CLIPMASK, qfalse, qfalse, "ragdoll grab");
    if (tr.fraction < 1.0f) {
        VectorMA(tr.endpos, -4.0f, dir, target);
    }

    VectorCopy(target, rd->grabTarget);
    rd->grabJoint = rd_grab.joint;
    CG_RagdollKeepAwake(rd);

    // A beam from about where the gun is to the part being held.
    //
    // It bends the way the physics gun's does: it leaves the muzzle straight
    // along the aim and curves round to wherever the body actually is. A
    // quadratic curve with its middle point out along the view does that, and
    // it is straight whenever the body is where the aim is, bowing only while
    // the body lags behind a swing or hangs below the aim.
    //
    // Drawn here as strips that face the eye rather than through the game's
    // beams, which widen only along the screen's horizontal and so show a beam
    // running away from the eye almost edge on.
    VectorMA(start, 12.0f, dir, muzzle);
    VectorMA(muzzle, -6.0f, cg.refdef.viewaxis[1], muzzle);
    VectorMA(muzzle, -6.0f, cg.refdef.viewaxis[2], muzzle);

    CG_RagdollDrawGrabBeam(muzzle, dir, target, rd->part[rd_grab.joint].p);
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

// How many traces may be open at once.
//
// A trace holds an engine file handle for as long as the corpse it follows
// exists, and the engine has few of them. Capturing many bodies in a busy round
// took every one and the game went down with "FS_HandleForFile: none free",
// taking the server with it. A handful at a time is all anyone reads anyway,
// and the ones that cannot be opened are simply not taken.
#define RD_MAX_OPEN_TRACES 4

// How long a trace goes on following a corpse that has gone to sleep. Long
// enough to show the entity wandering off underneath a frozen body, short
// enough that the handle comes back.
#define RD_DUMP_SLEEP_TAIL 3000

static int rd_openTraces;

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
static qboolean CG_RagdollDumpOpen(cg_ragdoll_t *rd, refEntity_t *model)
{
    // Comfortably over the header block below, which is written in one go and
    // was silently truncated mid-word when it outgrew a smaller buffer.
    char line[1024];
    int  i;

    if (rd_openTraces >= RD_MAX_OPEN_TRACES) {
        return qfalse;
    }

    rd->dumpSeq = ++rd_dumpSeq;

    Com_sprintf(rd->dumpName, sizeof(rd->dumpName), "ragdoll_dump_%d.txt", rd->dumpSeq);

    rd->dumpFile = cgi.FS_FOpenFileWrite(rd->dumpName);

    if (!rd->dumpFile) {
        cgi.Printf("ragdoll: could not open %s for writing\n", rd->dumpName);
        return qfalse;
    }

    rd_openTraces++;
    rd->dumpSleepAt = 0;

    Com_sprintf(
        line,
        sizeof(line),
        "# openmohaa ragdoll trace v1\n"
        "# entity %d  bodyscale %.4f\n"
        // Every tuning value that changes what this body does. Read them before
        // reading anything into the trace: six corpses once looked like they
        // could not fall asleep, and the answer was cg_ragdoll_sleepvel set to
        // zero, which switches sleeping off by design.
        "# blendtime %d  impulse %.2f  blastimpulse %.2f  stiffness %.2f  limptime %d  solvegain %.2f  limbpush %.2f  armfree %.2f  legfree %.2f  chestroll %.2f  spinetwist %.0f\n"
        "# jointsize %d  shoulderslack %.1f  bodypush %.2f  pinsleep %d  stuckhold %.2f  shove %.2f\n"
        "# sleepvel %.3f  sleeptime %d  duration %d  gravity %.1f\n"
        "# F <time_ms> <blendweight> <state> <supports> <maxdisp> <steps> <quiet_ms>\n"
        "# E <x> <y> <z>   entity origin, which the drawn corpse rides once asleep\n"
        "# C <contact> <onground> <buried>   bitmasks over the joints, low bit joint 0\n"
        "# L <limbburied>   bitmask over the limb bones, low bit bone 0, middle found inside the world\n"
        "# B2 <onbody>   bitmask over the joints, those being held up by another corpse rather than the world\n"
        "# G <joint> <gap> <ease>   the joint the grabber holds (-1 for none), how far it is from where the\n"
        "#     grabber wants it, and how far the grab has eased off against something in the way (0-1)\n"
        "# W <wakes>   how many times a shot or a blast has moved this corpse since it died\n"
        "# K <blast> <radius> <speed>   the explosion that threw this body, if any\n"
        "# R <chestrollfix> <chestrollerr>   degrees the back is being rolled, and what the last\n"
        "#     measurement asked for. Saturating and failing to converge look alike without both.\n"
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
        cg_ragdoll_limbpush->value,
        cg_ragdoll_armfree->value,
        cg_ragdoll_legfree->value,
        cg_ragdoll_chestroll->value,
        cg_ragdoll_spinetwist->value,
        cg_ragdoll_jointsize->integer,
        cg_ragdoll_shoulderslack->value,
        cg_ragdoll_bodypush->value,
        cg_ragdoll_pinsleep->integer,
        cg_ragdoll_stuckhold->value,
        cg_ragdoll_shove->value,
        cg_ragdoll_sleepvel->value,
        cg_ragdoll_sleeptime->integer,
        cg_ragdoll_duration->integer,
        CG_RagdollGravity()
    );
    CG_RagdollDumpLine(rd, line);

    // Kept apart from the header above, whose buffer it would overflow.
    Com_sprintf(
        line,
        sizeof(line),
        "# meshhits %d  meshfit %.2f  skinnedmesh %s\n"
        "# M <buried> <vertices> <deepest> <bone> <gap>   as the body fell asleep: drawn mesh vertices\n"
        "#     more than a unit inside the world, the deepest (shorter of straight up and back toward\n"
        "#     its bone) and whose bone it is, and how far the underside is held above the ground\n",
        cg_ragdoll_meshhits->integer,
        cg_ragdoll_meshfit->value,
        CG_RagdollCanSkin() ? "yes" : "no (engine too old)"
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

    return qtrue;
}

static void CG_RagdollDumpClose(cg_ragdoll_t *rd);

// The entity alone, once the body has stopped being solved. Written every frame
// the corpse is asleep, which is the window the pose can no longer explain.
static void CG_RagdollDumpSleeping(cg_ragdoll_t *rd)
{
    char line[256];

    if (!rd->dumpFile) {
        return;
    }

    if (!rd->dumpSleepAt) {
        rd->dumpSleepAt = cg.time;
    }

    // Enough of the sleeping tail to show the entity moving out from under a
    // frozen body, and then the handle goes back. Holding it until the corpse
    // is finally freed is what ran the engine out of them.
    if (cg.time - rd->dumpSleepAt > RD_DUMP_SLEEP_TAIL) {
        CG_RagdollDumpClose(rd);
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

    if (rd_openTraces > 0) {
        rd_openTraces--;
    }

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

        Com_sprintf(line, sizeof(line), "L %d\n", rd->limbBuriedMask);
        CG_RagdollDumpLine(rd, line);

        Com_sprintf(line, sizeof(line), "B2 %d\n", rd->onBodyMask);
        CG_RagdollDumpLine(rd, line);

        Com_sprintf(
            line,
            sizeof(line),
            "G %d %.1f %.2f\n",
            rd->grabJoint,
            rd->grabJoint >= 0 ? Distance(rd->grabTarget, rd->part[rd->grabJoint].p) : 0.0f,
            rd->grabEase
        );
        CG_RagdollDumpLine(rd, line);

        // Whether this man was blown up. Recorded so that a claim about which
        // bodies misbehave -- that it is the ones caught in blasts -- can be
        // answered from the trace rather than from an impression.
        Com_sprintf(line, sizeof(line), "W %d\n", rd->wakeCount);
        CG_RagdollDumpLine(rd, line);

        Com_sprintf(
            line,
            sizeof(line),
            "K %d %.0f %.0f\n",
            rd->hasBlast ? 1 : 0,
            rd->hasBlast ? rd->blastRadius : 0.0f,
            rd->hasBlast ? rd->blastSpeed : 0.0f
        );
        CG_RagdollDumpLine(rd, line);

        Com_sprintf(line, sizeof(line), "R %.1f %.1f\n", rd->spineRollFix, rd->chestRollErr);
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
    if (rd->jolt) {
        CG_JoltRagdollDestroy(rd->jolt);
    }
    CG_RagdollDumpClose(rd);
    memset(rd, 0, sizeof(*rd));
    rd->state     = RD_FREE;
    rd->entityNum = ENTITYNUM_NONE;
    rd->grabJoint = -1;
}

void CG_InitRagdoll(void)
{
    rd_debugServerTime = -1;
    rd_debugFrameTime  = -1;

    int i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        CG_RagdollFree(&cg_ragdolls[i]);
    }

    memset(rd_evictedAt, 0, sizeof(rd_evictedAt));
    memset(rd_appearedAt, 0, sizeof(rd_appearedAt));
    // Models are reloaded with the map, and a tiki pointer can be reused.
    memset(rd_meshFits, 0, sizeof(rd_meshFits));
    memset(&rd_grab, 0, sizeof(rd_grab));
    rd_numMeshFits = 0;
    rd_openTraces = 0;

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
    // The same between two bodies. Flesh into flesh hardly bounces at all.
    cg_ragdoll_bodybounce  = cgi.Cvar_Get("cg_ragdoll_bodybounce", "0", CVAR_ARCHIVE);
    // Share of the limbs' motion relative to the rest of the body taken out
    // each step (CG_RagdollInternalDamping), which calms limbs whipping about
    // on a landing or a throw without slowing the body itself. 0 is off.
    // Measured, 0.05 takes a fifth off the limb whipping in falls and punts;
    // more costs the pose elsewhere, so it is left to be judged by eye.
    cg_ragdoll_limbdamp    = cgi.Cvar_Get("cg_ragdoll_limbdamp", "0.05", CVAR_ARCHIVE);
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
    // How hard one limb pushes another out of itself, per iteration. 0 is off.
    //
    // Nothing else sees a limb inside a limb. The joint to joint pairs work on
    // positions, and two bones can cross at their middles with all four of
    // their ends comfortably apart, so a shin passes through the other shin and
    // nothing objects. Measured over six combinations of stiffness and
    // limptime, 0.1 takes a limb inside another limb down in five of them and a
    // limb inside the trunk down in five, in one case from 38 per cent of its
    // own thickness to 26.
    //
    // It ships off because of what it costs. Twist rises in five of those six,
    // by a tenth of a degree at best and a degree and a half at worst, and
    // twist is the thing that reads worst. The push is what does it: an arm
    // hangs off the top of the chest, so shoving one sideways levers the torso
    // round with it. Restricting this to the legs, where the pelvis absorbs the
    // push, was measured and keeps the twist but barely moves the crossing at
    // all, because most of the crossing is arms.
    //
    // So it is here to be judged by eye rather than settled by measurement. 0.1
    // is the balanced setting; 0.2 buys more clearance for more twist.
    cg_ragdoll_limbpush  = cgi.Cvar_Get("cg_ragdoll_limbpush", "0", CVAR_ARCHIVE);
    cg_ragdoll_armfree   = cgi.Cvar_Get("cg_ragdoll_armfree", "1.0", CVAR_ARCHIVE);
    cg_ragdoll_legfree   = cgi.Cvar_Get("cg_ragdoll_legfree", "0.4", CVAR_ARCHIVE);
    cg_ragdoll_dumplabel = cgi.Cvar_Get("cg_ragdoll_dumplabel", "1", CVAR_ARCHIVE);
    // One console line per thing that happens to a corpse's ragdoll: started,
    // handed to another entity, lost, recycled, put to sleep, woken. Not
    // archived, so it cannot be left on by accident.
    cg_ragdoll_log = cgi.Cvar_Get("cg_ragdoll_log", "0", 0);
    // Bullets are tested against the corpse's drawn mesh, not just the game's
    // hit spheres. Needs an engine and renderer that can hand the mesh over;
    // without them, or at 0, the spheres decide on their own.
    cg_ragdoll_meshhits = cgi.Cvar_Get("cg_ragdoll_meshhits", "1", CVAR_ARCHIVE);
    // Collision sized from the model's own mesh, measured once per model,
    // scaled by this. 0 keeps the hand-set sizes in rd_joints. Off until the
    // clipping metric says it helps: fatter shapes have hurt before (99fad083).
    cg_ragdoll_meshfit = cgi.Cvar_Get("cg_ragdoll_meshfit", "0", CVAR_ARCHIVE);
    cg_ragdoll_props   = cgi.Cvar_Get("cg_ragdoll_props", "1", CVAR_ARCHIVE);
    CG_PropsReset();
    // The grabber (+rdgrab, rdpunt): how far it reaches, and how hard a punt
    // throws a body, in units per second.
    // The grabber is off until this is set, and setting it binds its keys.
    // What the keys did before is kept in cg_ragdoll_grabsaved and put back
    // when it is cleared. See CG_RagdollGrabBindings.
    cg_ragdoll_grab      = cgi.Cvar_Get("cg_ragdoll_grab", "0", CVAR_ARCHIVE);
    cg_ragdoll_grabsaved = cgi.Cvar_Get("cg_ragdoll_grabsaved", "", CVAR_ARCHIVE);
    cg_ragdoll_grabrange = cgi.Cvar_Get("cg_ragdoll_grabrange", "1024", CVAR_ARCHIVE);
    // How stiffly the grabber holds a body. Lower is heavier: it lags further
    // behind a swing and hangs lower under the aim.
    cg_ragdoll_grabspring = cgi.Cvar_Get("cg_ragdoll_grabspring", "50", CVAR_ARCHIVE);
    cg_ragdoll_puntspeed = cgi.Cvar_Get("cg_ragdoll_puntspeed", "700", CVAR_ARCHIVE);
    cg_ragdoll_chestroll = cgi.Cvar_Get("cg_ragdoll_chestroll", "1.0", CVAR_ARCHIVE);
    cg_ragdoll_spinetwist = cgi.Cvar_Get("cg_ragdoll_spinetwist", "45", CVAR_ARCHIVE);
    // Added in OPM: 0 the particle solver, 1 a Jolt ragdoll (cg_physics_ragdoll.cpp)
    cg_ragdoll_solver = cgi.Cvar_Get("cg_ragdoll_solver", "0", CVAR_ARCHIVE);
    cg_ragdoll_pinsleep   = cgi.Cvar_Get("cg_ragdoll_pinsleep", "1", CVAR_ARCHIVE);
    cg_ragdoll_jointsize  = cgi.Cvar_Get("cg_ragdoll_jointsize", "3", CVAR_ARCHIVE);
    cg_ragdoll_shoulderslack = cgi.Cvar_Get("cg_ragdoll_shoulderslack", "9", CVAR_ARCHIVE);
    cg_ragdoll_bodypush   = cgi.Cvar_Get("cg_ragdoll_bodypush", "0.35", CVAR_ARCHIVE);
    cg_ragdoll_stuckhold  = cgi.Cvar_Get("cg_ragdoll_stuckhold", "0.1", CVAR_ARCHIVE);
    cg_ragdoll_shove      = cgi.Cvar_Get("cg_ragdoll_shove", "1.0", CVAR_ARCHIVE);
    cgi.Cvar_CheckRange(cg_ragdoll_limbpush, 0, 1, qfalse);
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
    // It was left off, to be settled by eye in the game, and the game settled
    // it: a body picked up with the grabber hung in the pose it died in, legs
    // drawn up and arms held out, because nothing about that pose ever let go.
    // 1500 is the best value the harness measured, and with it the death-pose
    // cones open as well (CG_RagdollCones) and the joints' real ranges take
    // over (CG_RagdollJointRanges).
    cg_ragdoll_limptime  = cgi.Cvar_Get("cg_ragdoll_limptime", "1500", CVAR_ARCHIVE);
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
    cgi.Cvar_CheckRange(cg_ragdoll_bodybounce, 0, 1, qfalse);
    cgi.Cvar_CheckRange(cg_ragdoll_limbdamp, 0, 0.5f, qfalse);
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
//
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

        if (rd->lastEntityNum != cent->currentState.number && cg.time - rd->orphanTime > RD_ORPHAN_ADOPT_WINDOW) {
            continue;
        }

        VectorSubtract(rd->entOrigin, cent->lerpOrigin, d);
        if (VectorLengthSquared(d) > RD_ADOPT_DIST * RD_ADOPT_DIST) {
            continue;
        }

        CG_RagdollLog(rd, "adopted by entity %d", cent->currentState.number);
        rd->entityNum = cent->currentState.number;
        return rd;
    }

    return NULL;
}

// Single player has its own version of this, from a mod rather than the game.
// CorpseStay SP (in HRRTM, among others) keeps corpses past the engine's body
// queue by spawning a second actor for each one, hidden, playing the same
// death animation. When that animation ends it hides the real corpse and shows
// the copy on the same server frame. The copy is a live actor with its AI
// turned off, not a corpse, so it carries no EF_DEAD, and drawn as it is it
// snaps the body back to the last frame of the animation.
//
// It is let take over as a stand-in, but only under the conditions that mark
// that swap: it came into view just as the corpse left it, near where the
// corpse was, with the same model. Of several that fit, the nearest.
static cg_ragdoll_t *CG_RagdollAdoptStandIn(centity_t *cent, const refEntity_t *model)
{
    cg_ragdoll_t *best     = NULL;
    float         bestDist = 0.0f;
    int           i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        cg_ragdoll_t *rd = &cg_ragdolls[i];
        float         dist;

        if (rd->state == RD_FREE) {
            continue;
        }

        if (rd->entityNum != ENTITYNUM_NONE && cg_entities[rd->entityNum].currentValid) {
            continue;
        }

        if (rd->modelIndex != cent->currentState.modelindex) {
            continue;
        }

        dist = Distance(rd->entOrigin, cent->lerpOrigin);

        // Nothing else checks the tiki for a stand-in: if it differed, the
        // ragdoll would be thrown away and a live actor seeded in its place.
        if (rd->tiki != model->tiki || cg.time - rd->orphanTime > RD_STANDIN_WINDOW || dist > RD_STANDIN_DIST) {
            CG_RagdollLog(
                rd,
                "not taken by entity %d: tiki %s, orphaned %d ms ago, %.1f units away",
                cent->currentState.number,
                rd->tiki == model->tiki ? "same" : "different",
                cg.time - rd->orphanTime,
                dist
            );
            continue;
        }

        if (!best || dist < bestDist) {
            best     = rd;
            bestDist = dist;
        }
    }

    if (!best) {
        return NULL;
    }

    CG_RagdollLog(best, "taken over by stand-in entity %d, %.1f units away", cent->currentState.number, bestDist);

    best->entityNum = cent->currentState.number;
    best->standIn   = qtrue;
    // Pinned afresh to the stand-in, whose origin may not be the corpse's.
    best->sleepPinned = qfalse;

    return best;
}

// How far a joint may get from its entity before the body is given up on. A
// corpse ends up a few hundred units from it on a staircase and a hard throw
// can take it a thousand or two; one that has dropped out through the bottom
// of the map just keeps going, drawn where nobody can see it and costing its
// traces for as long as it lives. One in a thousand traced corpses did.
#define RD_RUNAWAY_DIST 4096.0f

// Whether the simulation has left the world or its numbers have gone bad, in
// which case nothing it produces is worth drawing.
static qboolean CG_RagdollRunaway(const cg_ragdoll_t *rd)
{
    int i;

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        const float *p = rd->part[i].p;

        if (Q_isnan(p[0]) || Q_isnan(p[1]) || Q_isnan(p[2])) {
            return qtrue;
        }

        // an infinite coordinate fails this too
        if (!(DistanceSquared(p, rd->entOrigin) <= Square(RD_RUNAWAY_DIST))) {
            return qtrue;
        }
    }

    return qfalse;
}

// The brush entities within reach of the body, in snapshot order.
static int CG_RagdollNearbyMovers(const cg_ragdoll_t *rd, centity_t **movers)
{
    vec3_t mins, maxs;
    float  reach = rd->boundRadius + RD_MOVER_MARGIN;

    VectorSet(mins, rd->boundCentre[0] - reach, rd->boundCentre[1] - reach, rd->boundCentre[2] - reach);
    VectorSet(maxs, rd->boundCentre[0] + reach, rd->boundCentre[1] + reach, rd->boundCentre[2] + reach);

    return CG_GetBrushEntitiesInBounds(RD_MAX_MOVERS, movers, mins, maxs);
}

// Remembered as the body falls asleep.
static void CG_RagdollNoteMovers(cg_ragdoll_t *rd)
{
    centity_t *movers[RD_MAX_MOVERS];
    int        i;

    rd->numMovers = CG_RagdollNearbyMovers(rd, movers);

    for (i = 0; i < rd->numMovers; i++) {
        rd->moverNum[i] = movers[i]->currentState.number;
        VectorCopy(movers[i]->lerpOrigin, rd->moverOrigin[i]);
        VectorCopy(movers[i]->lerpAngles, rd->moverAngles[i]);
    }
}

// Whether a door or a lift near a sleeping body has moved, or a new one has
// come within reach, since it fell asleep.
//
// Asleep, the body is frozen where it settled. A lift going down would leave
// it hanging where the floor used to be, and a door swinging open would pass
// straight through it, so it is woken to fall, or to be pushed aside by what
// hit it: every trace it makes already includes these entities.
static qboolean CG_RagdollMoversChanged(const cg_ragdoll_t *rd)
{
    centity_t *movers[RD_MAX_MOVERS];
    int        num, i;

    num = CG_RagdollNearbyMovers(rd, movers);

    if (num != rd->numMovers) {
        return qtrue;
    }

    for (i = 0; i < num; i++) {
        if (movers[i]->currentState.number != rd->moverNum[i]
            || DistanceSquared(movers[i]->lerpOrigin, rd->moverOrigin[i]) > Square(0.1f)
            || DistanceSquared(movers[i]->lerpAngles, rd->moverAngles[i]) > Square(0.1f)) {
            return qtrue;
        }
    }

    return qfalse;
}

static void CG_RagdollNoteEvicted(const cg_ragdoll_t *rd)
{
    if (rd->entityNum >= 0 && rd->entityNum < MAX_GENTITIES) {
        // Zero means never, and cg.time can be zero on the very first frame of
        // a map, which is the one moment that would read as never. One is close
        // enough and cannot be mistaken for it.
        rd_evictedAt[rd->entityNum] = cg.time ? cg.time : 1;
    }
}

static qboolean CG_RagdollWasEvicted(int entityNum)
{
    if (entityNum < 0 || entityNum >= MAX_GENTITIES) {
        return qfalse;
    }

    return rd_evictedAt[entityNum] ? qtrue : qfalse;
}

static cg_ragdoll_t *CG_RagdollAlloc(void)
{
    cg_ragdoll_t *oldest       = NULL;
    cg_ragdoll_t *oldestOrphan = NULL;
    int           used         = 0;
    int           solving      = 0;
    int           i;

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        cg_ragdoll_t *rd = &cg_ragdolls[i];
        qboolean      recyclable;

        if (rd->state == RD_FREE) {
            continue;
        }

        used++;

        // A body whose entity has gone is not being solved either: nothing
        // calls it until something adopts it. Counted, it held the budget
        // against the living, and single player loses one on every corpse the
        // CorpseStay handover misses. Enough of those and every new death
        // evicted a settled corpse, which then snapped back to its animation.
        if (rd->entityNum == ENTITYNUM_NONE || !cg_entities[rd->entityNum].currentValid) {
            if (!oldestOrphan || rd->startTime < oldestOrphan->startTime) {
                oldestOrphan = rd;
            }
            continue;
        }

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
        // entity is already gone (taken first, above). Evicting a body that is
        // still falling is very visible; running out of slots is not.
        recyclable = rd->state == RD_SLEEPING ? qtrue : qfalse;

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

    // Nobody can see an orphan, so it goes before any corpse that is drawn.
    if (oldestOrphan) {
        CG_RagdollLog(oldestOrphan, "recycled while orphaned (%d in use, %d solving)", used, solving);
        CG_RagdollFree(oldestOrphan);
        return oldestOrphan;
    }

    if (oldest) {
        CG_RagdollLog(oldest, "EVICTED, it will keep its animation pose (%d in use, %d solving)", used, solving);
        CG_RagdollNoteEvicted(oldest);
        CG_RagdollFree(oldest);
        return oldest;
    }

    CG_RagdollLog(&cg_ragdolls[0], "no slot for a new corpse (%d in use, %d solving)", used, solving);

    return NULL;
}

void CG_RagdollEntityReset(centity_t *cent)
{
    cg_ragdoll_t *rd = CG_RagdollForEntity(cent->currentState.number);

    // Whoever comes next on this entity number is a different body and starts
    // with a clean record, whatever happened to the one before it. Cleared
    // ahead of the early return below, because a body that lost its ragdoll
    // has no ragdoll to find.
    if (cent->currentState.number >= 0 && cent->currentState.number < MAX_GENTITIES) {
        rd_evictedAt[cent->currentState.number]  = 0;
        rd_appearedAt[cent->currentState.number] = cg.time ? cg.time : 1;
    }

    if (!rd) {
        return;
    }

    // Orphan rather than free: in multiplayer this fires on the respawn that
    // also spawns the Body which is about to adopt the simulation.
    CG_RagdollLog(rd, "orphaned: its entity was reset");
    rd->entityNum  = ENTITYNUM_NONE;
    rd->orphanTime = cg.time;
}

// Once a second, what the mesh traces for bullet hits cost (cg_ragdoll_log),
// and for a few seconds after a hit, the triangle it went into and where
// (cg_ragdoll_debug 3).
static void CG_RagdollMeshReport(void)
{
    static int reportedAt;

    if (cg.time - reportedAt >= 1000 || cg.time < reportedAt) {
        if (rd_meshTraces && cg_ragdoll_log->integer) {
            cgi.Printf(
                "ragdoll %d: %d mesh traces in the last second, %.1f us each, %.1f us in all\n",
                cg.time,
                rd_meshTraces,
                rd_meshTraceUs / rd_meshTraces,
                rd_meshTraceUs
            );
        }

        rd_meshTraces  = 0;
        rd_meshTraceUs = 0.0;
        reportedAt     = cg.time;
    }

    if (cg_ragdoll_debug->integer >= 3 && rd_lastHitTime && cg.time - rd_lastHitTime < 3000
        && cg.time >= rd_lastHitTime && CG_RagdollDebugReady()) {
        vec3_t a, b;
        int    k;

        for (k = 0; k < 3; k++) {
            cgi.R_DebugLine(rd_lastHitTri[k], rd_lastHitTri[(k + 1) % 3], 1.0f, 0.2f, 0.2f, 1.0f);
        }

        for (k = 0; k < 3; k++) {
            VectorCopy(rd_lastHitPoint, a);
            VectorCopy(rd_lastHitPoint, b);
            a[k] -= 2.0f;
            b[k] += 2.0f;
            cgi.R_DebugLine(a, b, 1.0f, 1.0f, 0.0f, 1.0f);
        }
    }
}

static void CG_RagdollExpire(void)
{
    static int lastTime = -1;
    int        i;

    // Once a frame is enough, and this is called for every entity.
    if (cg.time == lastTime) {
        return;
    }
    lastTime = cg.time;

    CG_RagdollMeshReport();
    CG_RagdollGrabBindings();
    CG_RagdollGrabUpdate();

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        cg_ragdoll_t *rd = &cg_ragdolls[i];
        qboolean      gone;

        if (rd->state == RD_FREE) {
            continue;
        }

        gone = (rd->entityNum == ENTITYNUM_NONE || !cg_entities[rd->entityNum].currentValid) ? qtrue : qfalse;

        if (!gone) {
            rd->orphanTime = cg.time;
            rd->gone       = qfalse;
            continue;
        }

        if (!rd->gone) {
            rd->gone = qtrue;
            CG_RagdollLog(rd, "its entity left the snapshot");
        }

        // An orphan is not timed out. Its entity is usually just out of view,
        // and a corpse the player comes back to, however much later, should
        // still be lying the way it settled rather than snapping back to its
        // death animation. The allocator takes orphans first when it needs a
        // slot, so ones whose corpse really has gone cost nothing but memory.
    }
}

//=============================================================
// Per-entity entry point
//=============================================================

// Whether this entity came into the snapshot recently enough to be a corpse's
// stand-in.
static qboolean CG_RagdollStandInArrived(int entityNum)
{
    if (entityNum < 0 || entityNum >= MAX_GENTITIES || !rd_appearedAt[entityNum]) {
        return qfalse;
    }

    return cg.time - rd_appearedAt[entityNum] <= RD_STANDIN_WINDOW ? qtrue : qfalse;
}

static qboolean CG_RagdollEligible(centity_t *cent, refEntity_t *model, qboolean bThirdPerson, qboolean standIn)
{
    const entityState_t *s1 = &cent->currentState;

    if (!cg_ragdoll->integer || !cgs.ragdollAllowed) {
        return qfalse;
    }

    if (!(s1->eFlags & EF_DEAD) && !standIn) {
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

// How much of the body as drawn has ended up inside the world, and how far the
// body as drawn is held up off it, measured once as it falls asleep and only
// when someone is looking (a trace or the log).
//
// Every other measurement here is of the particles or the bones, and the
// defects the screenshots keep finding are neither: the bone lies where it
// should and the sleeve is in the floor, or a man lying on his pack is propped
// up off the ground by it. This checks the mesh itself, vertex by vertex.
//
// A vertex counts as buried only past RD_CLIP_TOLERANCE: the underside of a
// body lying on the floor sits a hair into it, and counting that made every
// corpse on flat ground read ten percent buried. Depth is the shorter of two
// ways out, straight up and back toward the bone the vertex hangs from; the
// bone line alone runs slantwise through a floor and reads several times too
// deep.
#define RD_CLIP_TOLERANCE 1.0f

// How far above and below a vertex the floor is looked for.
#define RD_CLIP_PROBE 64.0f

// Only the underside is checked for a gap, which is the vertices this close to
// the lowest one.
#define RD_CLIP_UNDERSIDE 8.0f

static void CG_RagdollMeasureClipping(cg_ragdoll_t *rd, refEntity_t *model, int entityNum)
{
    vec3_t boneWorld[TIKI_MAX_BONES];
    byte   boneKnown[TIKI_MAX_BONES];
    char   line[256];
    float  worst     = 0.0f;
    float  lowest    = 0.0f;
    float  gap       = RD_CLIP_PROBE;
    int    worstBone = -1;
    int    buried    = 0;
    int    i;

    if (!rd->dumpFile && !cg_ragdoll_log->integer) {
        return;
    }

    if (!CG_RagdollSkin(model, NULL)) {
        return;
    }

    memset(boneKnown, 0, sizeof(boneKnown));

    for (i = 0; i < rd_skinNumVerts; i++) {
        if (!i || rd_skinVerts[i].xyz[2] < lowest) {
            lowest = rd_skinVerts[i].xyz[2];
        }
    }

    for (i = 0; i < rd_skinNumVerts; i++) {
        const skinnedVert_t *v = &rd_skinVerts[i];
        trace_t              tr;
        vec3_t               probe;
        float                depth;

        if (!(CG_PointContents(v->xyz, entityNum) & RD_CLIPMASK)) {
            // How far the underside is held off whatever is below it.
            if (v->xyz[2] <= lowest + RD_CLIP_UNDERSIDE && gap > 0.0f) {
                VectorCopy(v->xyz, probe);
                probe[2] -= RD_CLIP_PROBE;
                CG_RagdollTrace(&tr, v->xyz, vec3_origin, vec3_origin, probe, entityNum, RD_CLIPMASK, qfalse, qtrue, "ragdoll gap");

                if (!tr.startsolid && tr.fraction < 1.0f && tr.fraction * RD_CLIP_PROBE < gap) {
                    gap = tr.fraction * RD_CLIP_PROBE;
                }
            }
            continue;
        }

        // Straight up: from above, down to the vertex.
        depth = RD_CLIP_PROBE;
        VectorCopy(v->xyz, probe);
        probe[2] += RD_CLIP_PROBE;
        CG_RagdollTrace(&tr, probe, vec3_origin, vec3_origin, v->xyz, entityNum, RD_CLIPMASK, qfalse, qtrue, "ragdoll clipping");
        if (!tr.startsolid) {
            depth = RD_CLIP_PROBE * (1.0f - tr.fraction);
        }

        // Back toward its bone, which is inside the body.
        if (v->bone >= 0 && v->bone < TIKI_MAX_BONES) {
            float length;

            if (!boneKnown[v->bone]) {
                orientation_t ori = cgi.TIKI_Orientation(model, v->bone);
                int           k;

                VectorCopy(model->origin, boneWorld[v->bone]);
                for (k = 0; k < 3; k++) {
                    VectorMA(boneWorld[v->bone], ori.origin[k], model->axis[k], boneWorld[v->bone]);
                }
                boneKnown[v->bone] = 1;
            }

            length = Distance(boneWorld[v->bone], v->xyz);
            CG_RagdollTrace(&tr, boneWorld[v->bone], vec3_origin, vec3_origin, v->xyz, entityNum, RD_CLIPMASK, qfalse, qtrue, "ragdoll clipping");

            if (!tr.startsolid && length * (1.0f - tr.fraction) < depth) {
                depth = length * (1.0f - tr.fraction);
            }
        }

        // Touching, not buried.
        if (depth <= RD_CLIP_TOLERANCE) {
            gap = 0.0f;
            continue;
        }

        gap = 0.0f;
        buried++;

        if (depth > worst) {
            worst     = depth;
            worstBone = v->bone;
        }
    }

    Com_sprintf(
        line,
        sizeof(line),
        "M %d %d %.2f %s %.2f\n",
        buried,
        rd_skinNumVerts,
        worst,
        worstBone >= 0 ? cgi.Tag_NameForNum(model->tiki, worstBone) : "-",
        gap
    );
    CG_RagdollDumpLine(rd, line);

    CG_RagdollLog(
        rd,
        "mesh: %d of %d vertices more than %.0f unit in the world (%.1f%%), deepest %.1f on %s; underside %.1f above the ground",
        buried,
        rd_skinNumVerts,
        RD_CLIP_TOLERANCE,
        100.0f * buried / rd_skinNumVerts,
        worst,
        worstBone >= 0 ? cgi.Tag_NameForNum(model->tiki, worstBone) : "-",
        gap
    );
}

// How fast a part of a sleeping Jolt body has to be going for what struck it to
// wake the corpse, units a second.
#define RD_JOLT_WAKE_SPEED 20.0f

// With cg_ragdoll_solver 1: hands the Jolt ragdoll the shots, blasts and the
// grabber waiting for it, and takes back where its joints are, how fast they
// go and what they rest on, as a step of the particle solver would leave them.
// Returns the largest distance a joint moves in a step at dt.
static float CG_RagdollJoltFollow(cg_ragdoll_t *rd, float dt)
{
    vec3_t   p[RD_NUM_JOINTS], v[RD_NUM_JOINTS], normal[RD_NUM_JOINTS], dv[RD_NUM_JOINTS];
    qboolean contact[RD_NUM_JOINTS], pushed = qfalse;
    float    maxDisp = 0.0f, meanDisp = 0.0f;
    int      mask, i;

    memset(dv, 0, sizeof(dv));

    if (rd->hasImpulse) {
        for (i = 0; i < RD_NUM_JOINTS; i++) {
            VectorScale(rd->impulseVel, 1.0f - RD_IMPULSE_LOCAL, dv[i]);
        }
        VectorMA(dv[rd->impulseJoint], RD_IMPULSE_LOCAL, rd->impulseVel, dv[rd->impulseJoint]);
        rd->hasImpulse = qfalse;
        pushed         = qtrue;
    }

    if (rd->hasBlast) {
        for (i = 0; i < RD_NUM_JOINTS; i++) {
            vec3_t away;
            float  dist;

            VectorSubtract(rd->part[i].p, rd->blastPos, away);
            dist = VectorNormalize(away);
            if (dist >= rd->blastRadius) {
                continue;
            }
            if (dist < 1.0f) {
                VectorSet(away, 0.0f, 0.0f, 1.0f);
            }
            VectorMA(dv[i], rd->blastSpeed * (1.0f - dist / rd->blastRadius), away, dv[i]);
        }
        rd->hasBlast = qfalse;
        pushed       = qtrue;
    }

    if (pushed) {
        CG_JoltRagdollAddVelocity(rd->jolt, dv);
    }

    CG_JoltRagdollHold(rd->jolt, rd->grabJoint, rd->grabTarget);

    if (!CG_JoltRagdollRead(rd->jolt, p, v, contact, normal, &mask)) {
        // Gone from the world (the map changed): the particles carry on.
        rd->jolt = 0;
        return 0.0f;
    }

    for (i = 0; i < RD_NUM_JOINTS; i++) {
        rdParticle_t *part = &rd->part[i];
        const float   disp = VectorLength(v[i]) * dt;

        VectorCopy(part->p, part->pPrev);
        VectorCopy(p[i], part->p);
        VectorCopy(v[i], part->v);
        VectorClear(part->solved);

        part->hasContact = contact[i];
        part->onGround   = (contact[i] && normal[i][2] > 0.7f) ? qtrue : qfalse;
        if (contact[i]) {
            VectorCopy(normal[i], part->contactNormal);
            part->contactDist = DotProduct(part->p, normal[i]);
            part->contactTime = cg.time;
            VectorCopy(part->p, part->contactAt);
        }

        maxDisp = Q_max(maxDisp, disp);
        meanDisp += disp;
    }

    rd->onBodyMask = mask;
    rd->stuckMask  = 0;

    // Never asleep while held: a body the beam cannot move, caught on
    // something, would otherwise sleep with the grabber still on it and stop
    // answering it.
    if (rd->grabJoint >= 0) {
        rd->quietSince = cg.time;
    }
    rd->lastMean   = meanDisp / (float)RD_NUM_JOINTS;
    rd->balanced   = CG_RagdollBalanced(rd, &rd->draped);

    // The drawn roll of the upper arms and thighs, as the particle step keeps.
    CG_RagdollLimbTwist(rd);

    return maxDisp;
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

    // A stand-in for a corpse that has just been swapped out. See
    // CG_RagdollAdopt.
    if (!rd && !(cent->currentState.eFlags & EF_DEAD) && cent->currentState.eType == ET_MODELANIM
        && CG_RagdollStandInArrived(cent->currentState.number)
        && CG_RagdollEligible(cent, model, bThirdPerson, qtrue)) {
        rd = CG_RagdollAdoptStandIn(cent, model);
    }

    if (!CG_RagdollEligible(cent, model, bThirdPerson, rd ? rd->standIn : qfalse)) {
        if (rd) {
            CG_RagdollLog(rd, "freed: its entity is no longer eligible (eFlags 0x%x)", cent->currentState.eFlags);
            CG_RagdollFree(rd);
        }
        return;
    }

    // cg_forceModel can be toggled at runtime, and a tiki swap invalidates
    // every cached bone index, so re-seed rather than emit garbage.
    if (rd && rd->tiki != model->tiki) {
        CG_RagdollLog(rd, "freed: its entity changed model");
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
        // A body that has already had a ragdoll and lost it keeps the pose it
        // is in rather than starting a new one. Seeding it again would take a
        // slot from another settled corpse, which would then ask for one back.
        if (CG_RagdollWasEvicted(cent->currentState.number)) {
            return;
        }

        if (cg_ragdoll_log->integer) {
            cgi.Printf("ragdoll %d: entity %d died, starting a ragdoll\n", cg.time, cent->currentState.number);
        }

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

        CG_RagdollNoteEnclosingProps(rd);

        CG_RagdollFindImpulse(rd);
        CG_RagdollFindBlast(rd);

        rd->state     = RD_BLENDING;
        rd->entityNum = cent->currentState.number;

        // Catch the next body to fall, and only that one. Clearing the cvar
        // here rather than making the caller do it means a trace is one command
        // and cannot be left running to fill the disk. Opened after the state
        // and the entity number are set, so the header describes the corpse
        // rather than the empty slot it came from.
        if (cg_ragdoll_dump->integer > 0 && CG_RagdollDumpOpen(rd, model)) {
            char left[16];

            Com_sprintf(left, sizeof(left), "%d", cg_ragdoll_dump->integer - 1);
            cgi.Cvar_Set("cg_ragdoll_dump", left);
        }
    }

    // Or the next body woken, by a shot or the grabber, since a corpse that is
    // already down is most of what gets looked at: set before picking a body
    // up, the trace covers what it does from then on.
    if (cg_ragdoll_dump->integer > 0 && !rd->dumpFile && rd->state == RD_ACTIVE && rd->lastWake
        && cg.time - rd->lastWake < 100 && CG_RagdollDumpOpen(rd, model)) {
        char left[16];

        Com_sprintf(left, sizeof(left), "%d", cg_ragdoll_dump->integer - 1);
        cgi.Cvar_Set("cg_ragdoll_dump", left);
        CG_RagdollLog(rd, "tracing to %s", rd->dumpName);
    }

    rd->entityNum     = cent->currentState.number;
    rd->lastEntityNum = rd->entityNum;
    if (model != &rd->unsent) {
        rd->unsent     = *model;
        rd->haveUnsent = qtrue;
    }
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
        CG_RagdollStartTwist(rd);

        // Added in OPM
        //  From here the Jolt ragdoll carries the body, if one is wanted.
        if (cg_ragdoll_solver->integer == 1 && !rd->jolt) {
            vec3_t p[RD_NUM_JOINTS], v[RD_NUM_JOINTS];
            int    j;

            for (j = 0; j < RD_NUM_JOINTS; j++) {
                VectorCopy(rd->part[j].p, p[j]);
                VectorCopy(rd->part[j].v, v[j]);
            }

            // The hold on the death pose lasts as long as the particles' would.
            rd->jolt = CG_JoltRagdollCreate(
                p, v, rd->jointRadius, Q_max(0.0f, (cg_ragdoll_limptime->integer - (cg.time - rd->startTime)) * 0.001f)
            );
            CG_RagdollLog(rd, rd->jolt ? "carried by a Jolt ragdoll" : "no Jolt ragdoll could be made; the particles carry on");
        }

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

    // Kept current for every corpse, asleep or not, because a sleeping one is
    // exactly what the next body has to land on.
    CG_RagdollUpdateBounds(rd);

    if (rd->state == RD_SLEEPING && CG_RagdollMoversChanged(rd)) {
        CG_RagdollWake(rd);
    }

    // Something in the physics world struck the Jolt body while it slept. Only
    // what sets it moving counts: brushed by something it lies against, it is
    // put back to sleep there instead, or a pile of bodies wakes itself for
    // ever, each one's settling nudging the next.
    if (rd->state == RD_SLEEPING && rd->jolt && CG_JoltRagdollAwake(rd->jolt)) {
        if (CG_JoltRagdollSpeed(rd->jolt) > RD_JOLT_WAKE_SPEED) {
            CG_RagdollLog(rd, "woken by the physics");
            CG_RagdollWake(rd);
        } else {
            CG_JoltRagdollSleep(rd->jolt);
        }
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

            if (rd->jolt) {
                // The physics world steps it (CG_PhysicsFrame); this follows.
                maxDisp   = CG_RagdollJoltFollow(rd, dt);
                rd->accum = 0.0f;
                steps     = 1;
            }

            while (!rd->jolt && rd->accum >= dt && steps < 5) {
                CG_RagdollDriveFromAnim(rd, weight);
                maxDisp = CG_RagdollStep(rd, cent->currentState.number, dt);
                rd->accum -= dt;
                steps++;
            }

            if (steps) {
                // Judged on the average joint rather than the fastest one.
                //
                // A single twitching hand used to keep a whole corpse awake for
                // as long as it lived, and an awake corpse is the only kind
                // that can shiver: once it sleeps the pose is frozen. Measured
                // in the game, of four hundred and twenty nine bodies a hundred
                // and twenty six were still simulating two and a half seconds
                // after death, and those were shivering at fifty times the rate
                // the suite ever produces.
                //
                // The fastest joint still has a say, but a loose one: it may be
                // several times the sleeping speed before it counts, which
                // stops a body being frozen while some part of it is genuinely
                // flying, without letting one noisy joint hold the rest hostage.
                if (rd->lastMean > cg_ragdoll_sleepvel->value
                    || maxDisp > cg_ragdoll_sleepvel->value * RD_SLEEP_SPIKE) {
                    rd->quietSince = cg.time;
                }
            }

            rd->lastDisp  = maxDisp;
            rd->lastSteps = steps;
        }

        // The body goes back to the pose its animation gives it, where its
        // entity is, and does not get another ragdoll.
        if (CG_RagdollRunaway(rd)) {
            cgi.DPrintf("ragdoll: gave up on entity %d, it left the world\n", rd->entityNum);
            CG_RagdollLog(rd, "EVICTED: it left the world");
            CG_RagdollNoteEvicted(rd);
            CG_RagdollFree(rd);
            return;
        }

        // Build the pose before falling asleep, so the matrices that get
        // frozen are this frame's rather than the previous frame's.
        CG_RagdollBuildPose(rd, model, weight);

        CG_RagdollDumpFrame(rd, weight);

        if (CG_RagdollRestingOnLevel(rd) && rd->balanced) {
            if (!rd->restingSince) {
                rd->restingSince = cg.time;
            }
        } else {
            rd->restingSince = 0;
        }

        if (rd->state == RD_ACTIVE) {
            // Being slow is not the same as having come to rest. A body going
            // over a ledge, or working its way down a staircase, passes through
            // slow moments all the time, and freezing it at one of those is
            // what leaves a corpse hooked on an edge and dangling in mid air
            // instead of dropping. So it has to be resting on something before
            // it is allowed to sleep at all.
            const qboolean supported = CG_RagdollSupported(rd);

            if (!supported) {
                rd->quietSince = cg.time;
            }

            if (supported && rd->balanced && cg.time - rd->quietSince > cg_ragdoll_sleeptime->integer) {
                rd->state = RD_SLEEPING;
                if (rd->jolt) {
                    CG_JoltRagdollSleep(rd->jolt);
                }
                CG_RagdollLog(rd, "asleep after %d ms", elapsed);
                CG_RagdollNoteMovers(rd);
                rd->measureClip = qtrue;
            } else if (cg_ragdoll_duration->integer > 0 && cg.time - Q_max(rd->startTime, rd->lastWake) > cg_ragdoll_duration->integer
                       && cg.time > rd->wakeUntil) {
                // The lifetime cap is a budget, not a statement about the body.
                // Applied on its own it freezes whatever pose the corpse is in
                // at that instant, and a body still on its way down a staircase
                // is left standing on its head in mid air. So a corpse that has
                // nothing under it goes on simulating, and only a hard backstop
                // far beyond any real settling time overrides that.
                // Resting on level ground or another body, that is: not merely
                // touching a wall with most of itself, or hooked on something
                // by a joint wedged into it. Counted as support, those froze a
                // body that had snagged a ledge on its way down, hanging off
                // it against the wall.
                //
                // And only a body that has nearly stopped. Resting counts
                // joints, and a body toppling off a ledge feet first has them
                // on the ground the moment it lands: frozen there, it was left
                // standing upright in mid fall. The cap is for a body that
                // will not stop fidgeting, not for one still on its way down.
                //
                // Nor for one that has only just arrived. The cap counts from
                // the last wake, and a body let go of that then hung by a knee
                // over a beam for three seconds spent the whole allowance up
                // there: it was frozen a sixth of a second after it reached the
                // floor, still settling at sixty units a second. So it must
                // have been lying there a second first, time enough for an
                // ordinary settle to put it to sleep on its own.
                if ((rd->restingSince && cg.time - rd->restingSince > RD_CAP_SETTLE && rd->lastMean < RD_CAP_MAX_MEAN)
                    || cg.time - Q_max(rd->startTime, rd->lastWake) > cg_ragdoll_duration->integer * RD_SLEEP_BACKSTOP) {
                    rd->state = RD_SLEEPING;
                    if (rd->jolt) {
                        CG_JoltRagdollSleep(rd->jolt);
                    }
                    CG_RagdollLog(rd, "asleep after %d ms, at the lifetime cap", elapsed);
                    CG_RagdollNoteMovers(rd);
                    rd->measureClip = qtrue;
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
    //
    // Riding it sideways is another matter. The server goes on moving a dead
    // man's entity, and three of the six bodies photographed in one round had
    // slid across the ground: traced, they had travelled a hundred and forty
    // units while the ragdoll's own particles reported not moving at all. The
    // corpse was not sliding, the ground was.
    //
    // So the sleeping pose is rebuilt each frame from the world positions it
    // settled on, offset by however far the entity has sunk since. Down is
    // followed, which is what the freezing was for; sideways is not.
    if (rd->state == RD_SLEEPING && cg_ragdoll_pinsleep->integer && rd->numOverrides) {
        vec3_t drop;
        int    b;

        if (!rd->sleepPinned) {
            VectorCopy(model->origin, rd->sleepOrigin);
            rd->sleepPinned = qtrue;
        }

        VectorClear(drop);
        drop[2] = model->origin[2] - rd->sleepOrigin[2];
        rd->sleepDrop = drop[2];

        rd->numOverrides = 0;

        for (b = 0; b < RD_NUM_BONES; b++) {
            boneOverride_t *out = &rd->overrides[rd->numOverrides];
            vec3_t          held;

            if (rd->boneIndex[b] < 0) {
                continue;
            }

            VectorAdd(rd->bonePos[b], drop, held);

            out->boneIndex = rd->boneIndex[b];
            CG_RagdollWriteBoneModel(model, held, rd->boneAxis[b], out->matrix);
            rd->numOverrides++;
        }

        CG_RagdollWriteClavicles(rd, model, drop);
    }

    if (!rd->numOverrides) {
        return;
    }

    CG_RagdollRecenter(rd, model);

    model->bone_override      = rd->drawOverrides;
    model->num_bone_overrides = rd->numOverrides;

    // Install the pose on the skeletor now, so that everything else this frame
    // that queries a bone or a tag, attachments and the shadow included, sees
    // the ragdoll rather than the animation.
    cgi.ForceUpdatePose(model);

    rd->drawn     = *model;
    rd->haveDrawn = qtrue;

    if (rd->measureClip) {
        rd->measureClip = qfalse;
        CG_RagdollMeasureClipping(rd, model, cent->currentState.number);
    }

    // Deliberately not behind cg_ragdoll_debug: that draws a spike at every
    // joint and, at level 2, the whole constraint web, which is the last thing
    // wanted in a screenshot of how the body looks.
    {
        const qboolean label = (qboolean)(cg_ragdoll_dumplabel->integer && rd->dumpSeq > 0);

        if ((cg_ragdoll_debug->integer || label) && CG_RagdollDebugReady()) {
            if (label) {
                CG_RagdollDrawNumber(rd);
            }

            if (cg_ragdoll_debug->integer) {
                CG_RagdollDebugDraw(rd);
            }
        }
    }
}

// Added in OPM
//  Test access to the raw particle cloud. The emitted pose is rebuilt from
//  fixed offsets, so its bone lengths are exact whatever the solver does: that
//  is what stops limbs stretching, but it also means nothing about the drawn
//  skeleton reveals the particles collapsing underneath it. This is the only
//  way to see that from outside.
// Draws the corpses whose entities the server has stopped sending.
//
// The server decides what to send by where it thinks the corpse is, and that is
// where the man died: the ragdoll is the client's alone. Carried or thrown out
// of sight of that spot, a body lying in plain view stopped being drawn, and
// came back only when the player moved so that the place of death could be seen
// again: a body blinking out of existence.
//
// So a ragdoll whose entity has left the snapshot goes on being simulated and
// drawn from the last model its entity handed over, as long as the spot its
// entity stands at is out of sight from here. If that spot can be seen and the
// entity has still gone, the server has taken the corpse away, and so does
// this.
void CG_RagdollAddUnsent(void)
{
    int viewLeaf;
    int i;

    if (!cg_ragdoll->integer || !cg.snap) {
        return;
    }

    // Read the map's props as it starts rather than on the first death, which
    // would otherwise pay for skinning every one of them in the middle of a
    // firefight.
    if (cg_ragdoll_props->integer) {
        CG_PropsLoad();
    }

    viewLeaf = cgi.CM_PointLeafnum(cg.refdef.vieworg);

    for (i = 0; i < MAX_RAGDOLLS; i++) {
        cg_ragdoll_t *rd = &cg_ragdolls[i];
        centity_t    *cent;
        refEntity_t   model;

        if (rd->state == RD_FREE || rd->entityNum == ENTITYNUM_NONE || !rd->haveUnsent) {
            continue;
        }

        cent = &cg_entities[rd->entityNum];

        if (cent->currentValid) {
            continue;
        }

        if (cgi.CM_LeafInPVS(viewLeaf, cgi.CM_PointLeafnum(cent->lerpOrigin))) {
            continue;
        }

        model = rd->unsent;
        CG_RagdollUpdateEntity(cent, &model);

        // Freed, or left unposed, on the way through.
        if (rd->state == RD_FREE || !model.bone_override) {
            continue;
        }

        cgi.R_AddRefEntityToScene(&model, ENTITYNUM_NONE);
    }
}

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

// For the harness: what the Jolt ragdoll carrying an entity's corpse has at
// its joints (CG_JoltRagdollReport).
extern "C" void CG_RagdollDebugJolt(int entityNum, void (*print)(const char *fmt, ...))
{
    cg_ragdoll_t *rd = CG_RagdollForEntity(entityNum);

    if (rd && rd->jolt) {
        CG_JoltRagdollReport(rd->jolt, print);
    }
}
