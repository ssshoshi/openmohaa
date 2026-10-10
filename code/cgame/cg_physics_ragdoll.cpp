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
// A corpse as a Jolt ragdoll (cg_ragdoll_solver 1): rigid capsules for the
// bones, held together by joints with a man's range of movement, in the same
// world as the map and its props, so it lands on them and shoves them, and
// they land on it.
//
// The particle solver stays in charge of everything else. It makes this body
// from its joints as the blend out of the death animation ends, reads the
// joints back every frame to draw the body, and passes on the shots, blasts
// and the grabber. What the body rests on is gathered from the contacts, for
// the solver's sleep and settling rules.
//
// The parts, parent first:
//
//   pelvis (with a capsule across the hips), spine, spine1, chest (with a
//   capsule across the shoulders), neck, head; for each side upper arm,
//   forearm, hand; thigh, calf, foot.
//
// Each part runs from one joint of the rig to the next. Knees and elbows are
// hinges, bending one way only; everything else is a swing and a twist. The
// hips and shoulders swing within the ranges the particle solver gives a man
// (rd_jointRanges in cg_ragdoll.cpp); the spine, neck, wrists and ankles
// swing a little either way of where they started.

#include "cg_physics_local.h"
#include "cg_physics_ragdoll.h"

#include <vector>

#define PR_MAX_RAGDOLLS 512

enum {
    PR_PELVIS,
    PR_SPINE,
    PR_SPINE1,
    PR_CHEST,
    PR_NECK,
    PR_HEAD,
    PR_LUARM,
    PR_LFARM,
    PR_LHAND,
    PR_RUARM,
    PR_RFARM,
    PR_RHAND,
    PR_LTHIGH,
    PR_LCALF,
    PR_LFOOT,
    PR_RTHIGH,
    PR_RCALF,
    PR_RFOOT,

    PR_NUM_PARTS
};

typedef enum {
    PR_ROOT,     // the pelvis
    PR_BACK,     // a spine joint
    PR_NECKJ,    // the neck and the head on it
    PR_SHOULDER, // an arm on the chest
    PR_ELBOW,
    PR_WRIST,
    PR_HIP,      // a leg on the pelvis
    PR_KNEE,
    PR_ANKLE,
} prJointKind_t;

typedef struct {
    const char   *name;
    short         parent;
    short         a, b; // the joints it runs between
    float         mass; // kilograms
    prJointKind_t kind;
    short         left; // an arm or leg on the left
} prPartDef_t;

// Masses after a man's segments, about 80 kg in all.
static const prPartDef_t pr_parts[PR_NUM_PARTS] = {
    {"pelvis", -1,        RD_PELVIS, RD_SPINE,    11.0f, PR_ROOT,     0},
    {"spine",  PR_PELVIS, RD_SPINE,  RD_SPINE1,   6.0f,  PR_BACK,     0},
    {"spine1", PR_SPINE,  RD_SPINE1, RD_SPINE2,   6.0f,  PR_BACK,     0},
    {"chest",  PR_SPINE1, RD_SPINE2, RD_NECK,     12.0f, PR_BACK,     0},
    {"neck",   PR_CHEST,  RD_NECK,   RD_HEAD,     1.5f,  PR_NECKJ,    0},
    {"head",   PR_NECK,   RD_HEAD,   RD_HEADTIP,  5.0f,  PR_NECKJ,    0},
    {"luarm",  PR_CHEST,  RD_LUARM,  RD_LFARM,    2.5f,  PR_SHOULDER, 1},
    {"lfarm",  PR_LUARM,  RD_LFARM,  RD_LHAND,    1.5f,  PR_ELBOW,    1},
    {"lhand",  PR_LFARM,  RD_LHAND,  RD_LHANDTIP, 0.6f,  PR_WRIST,    1},
    {"ruarm",  PR_CHEST,  RD_RUARM,  RD_RFARM,    2.5f,  PR_SHOULDER, 0},
    {"rfarm",  PR_RUARM,  RD_RFARM,  RD_RHAND,    1.5f,  PR_ELBOW,    0},
    {"rhand",  PR_RFARM,  RD_RHAND,  RD_RHANDTIP, 0.6f,  PR_WRIST,    0},
    {"lthigh", PR_PELVIS, RD_LTHIGH, RD_LCALF,    9.0f,  PR_HIP,      1},
    {"lcalf",  PR_LTHIGH, RD_LCALF,  RD_LFOOT,    4.0f,  PR_KNEE,     1},
    {"lfoot",  PR_LCALF,  RD_LFOOT,  RD_LTOE,     1.2f,  PR_ANKLE,    1},
    {"rthigh", PR_PELVIS, RD_RTHIGH, RD_RCALF,    9.0f,  PR_HIP,      0},
    {"rcalf",  PR_RTHIGH, RD_RCALF,  RD_RFOOT,    4.0f,  PR_KNEE,     0},
    {"rfoot",  PR_RCALF,  RD_RFOOT,  RD_RTOE,     1.2f,  PR_ANKLE,    0},
};

// The hip and shoulder ranges, degrees: flexion (forward positive) and
// abduction (outward positive), straight down along the body being zero on
// both. As rd_jointRanges in cg_ragdoll.cpp.
#define PR_HIP_FLEX_MIN      -45.0f
#define PR_HIP_FLEX_MAX      130.0f
#define PR_HIP_ABD_MIN       -25.0f
#define PR_HIP_ABD_MAX       70.0f
#define PR_SHOULDER_FLEX_MIN -60.0f
#define PR_SHOULDER_FLEX_MAX 180.0f
#define PR_SHOULDER_ABD_MIN  -45.0f
#define PR_SHOULDER_ABD_MAX  180.0f

// How far a knee or an elbow bends, degrees from straight: the normative
// ranges (AAOS: knee 135, elbow 150, less a little for an elbow at rest), and
// never quite straight, as the particle solver's braces hold them (at least 20
// degrees there). The least bend is a soft limit, so a leg that died straight
// eases into it rather than being snapped.
#define PR_KNEE_MAX           135.0f
#define PR_ELBOW_MAX          145.0f
#define PR_HINGE_MIN          10.0f
#define PR_HINGE_MIN_STIFFNESS 10.0f // hertz

// Friction in the joints, newton metres, which is most of what stops a limp
// body flailing.
#define PR_FRICTION_BACK  3.0f
#define PR_FRICTION_NECK  1.0f
#define PR_FRICTION_LIMB  2.0f
#define PR_FRICTION_HINGE 1.0f
#define PR_FRICTION_END   0.3f

// The hold on the death pose while the body is still going limp: how stiff the
// joints' motors are (hertz, critically damped) and, per kind of joint, the
// most torque they have at the start (newton metres), fading to none.
#define PR_TONE_FREQUENCY 4.0f

static const float pr_tone[] = {
    0.0f,   // PR_ROOT
    120.0f, // PR_BACK
    25.0f,  // PR_NECKJ
    30.0f,  // PR_SHOULDER
    15.0f,  // PR_ELBOW
    5.0f,   // PR_WRIST
    120.0f, // PR_HIP
    80.0f,  // PR_KNEE
    10.0f,  // PR_ANKLE
};

// The grabber. The joint held is tied to an anchor carried to the end of the
// beam, by motors on a six degree of freedom constraint rather than a force of
// our own: Jolt then solves the pull against the whole body hanging from the
// joint, where a force sized for the whole body and put on one small part
// overshot it and set it shaking. The motors' stiffness (hertz, critically
// damped), the most they pull (the body's weight and this much acceleration
// besides, metres a second squared), the fastest the anchor follows the beam
// (units a second), and the friction that stops the part held spinning about
// the grip (newton metres).
#define PR_HOLD_FREQUENCY     10.0f
#define PR_HOLD_EXTRA_ACCEL   25.0f
#define PR_HOLD_MAX_SPEED     1500.0f
#define PR_HOLD_SPIN_FRICTION 3.0f

typedef struct {
    qboolean                      used;
    int                           generation;
    JPH::Ref<JPH::RagdollSettings> settings;
    JPH::Ref<JPH::Ragdoll>         ragdoll;
    JPH::Vec3                     localA[PR_NUM_PARTS], localB[PR_NUM_PARTS]; // the part's two joints, metres, body space
    float                         totalMass;

    int    holdJoint;
    vec3_t holdTarget;

    JPH::BodyID                     anchor; // what the joint held is tied to
    JPH::Ref<JPH::SixDOFConstraint> hold;

    // The hold on the death pose: how long it lasts, and how far into it.
    float toneTime, toneElapsed;

    // Gathered from the contacts of the latest step.
    qboolean contact[RD_NUM_JOINTS];
    vec3_t   contactNormal[RD_NUM_JOINTS];
    int      onBodyMask;
} prRagdoll_t;

static prRagdoll_t pr_ragdolls[PR_MAX_RAGDOLLS];
static JPH::CollisionGroup::GroupID pr_nextGroup = 1000;

// Which part a joint belongs to, and whether it is that part's far end (the
// tips: head, hands, toes).
static short    pr_jointPart[RD_NUM_JOINTS];
static qboolean pr_jointEnd[RD_NUM_JOINTS];

static void CG_JoltRagdollMapJoints(void)
{
    static qboolean done;
    int             i, j;

    if (done) {
        return;
    }

    for (j = 0; j < RD_NUM_JOINTS; j++) {
        pr_jointPart[j] = -1;
    }
    for (i = 0; i < PR_NUM_PARTS; i++) {
        pr_jointPart[pr_parts[i].a] = i;
        pr_jointEnd[pr_parts[i].a]  = qfalse;
    }
    for (i = 0; i < PR_NUM_PARTS; i++) {
        if (pr_jointPart[pr_parts[i].b] < 0) {
            pr_jointPart[pr_parts[i].b] = i;
            pr_jointEnd[pr_parts[i].b]  = qtrue;
        }
    }
    done = qtrue;
}

static prRagdoll_t *CG_JoltRagdollGet(int handle)
{
    int slot, gen;

    if (handle <= 0 || !phys_system) {
        return NULL;
    }

    slot = (handle - 1) % PR_MAX_RAGDOLLS;
    gen  = (handle - 1) / PR_MAX_RAGDOLLS;
    if (!pr_ragdolls[slot].used || pr_ragdolls[slot].generation != gen) {
        return NULL;
    }

    return &pr_ragdolls[slot];
}

//=============================================================
// Making one
//=============================================================

static JPH::Vec3 PR_Normalized(JPH::Vec3Arg v, JPH::Vec3Arg fallback)
{
    const float len = v.Length();

    return len > 1e-6f ? v / len : fallback;
}

// v with its part along axis taken out, normalized.
static JPH::Vec3 PR_Across(JPH::Vec3Arg v, JPH::Vec3Arg axis, JPH::Vec3Arg fallback)
{
    return PR_Normalized(v - axis * v.Dot(axis), fallback);
}

// Some direction square to v.
static JPH::Vec3 PR_AnySquare(JPH::Vec3Arg v)
{
    return PR_Normalized(fabsf(v.GetZ()) < 0.9f ? v.Cross(JPH::Vec3::sAxisZ()) : v.Cross(JPH::Vec3::sAxisX()), JPH::Vec3::sAxisX());
}

// A capsule between two points of a part's own space, radiusA thick at a and
// radiusB at b; a straight one if the taper cannot be made.
static JPH::ShapeRefC PR_Capsule(JPH::Vec3Arg a, JPH::Vec3Arg b, float radiusA, float radiusB)
{
    const JPH::Vec3 d    = b - a;
    const float     len  = d.Length();
    const float     half = Q_max(len * 0.5f, 0.001f);
    JPH::Quat       rot  = JPH::Quat::sIdentity();

    if (len > 1e-5f) {
        rot = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), d / len);
    }

    // Its top, +Y, is at b.
    if (fabsf(radiusA - radiusB) > 0.001f) {
        JPH::RotatedTranslatedShapeSettings tapered((a + b) * 0.5f, rot, new JPH::TaperedCapsuleShapeSettings(half, radiusB, radiusA));
        JPH::ShapeSettings::ShapeResult     result = tapered.Create();

        if (!result.HasError()) {
            return result.Get();
        }
    }

    JPH::RotatedTranslatedShapeSettings shape((a + b) * 0.5f, rot, new JPH::CapsuleShapeSettings(half, (radiusA + radiusB) * 0.5f));
    JPH::ShapeSettings::ShapeResult     result = shape.Create();

    return result.HasError() ? JPH::ShapeRefC() : result.Get();
}

// The swing and twist at a joint. The swing is measured from neutral (in the
// world, at the pose it is made in), within half cones about the plane axis
// (planeRef made square to neutral) and about the axis square to both; a
// starting direction outside them widens them to take it in. The twist is
// zero as the body is now.
static JPH::Ref<JPH::SwingTwistConstraintSettings> PR_SwingTwist(
    JPH::RVec3Arg pivot, JPH::Vec3Arg neutralIn, JPH::Vec3Arg planeRef, JPH::Vec3Arg child, float planeHalfDeg, float normalHalfDeg,
    float twistDeg, float friction
)
{
    JPH::Ref<JPH::SwingTwistConstraintSettings> s       = new JPH::SwingTwistConstraintSettings;
    const JPH::Vec3                             neutral = PR_Normalized(neutralIn, child);
    const JPH::Vec3                             plane1  = PR_Across(planeRef, neutral, PR_AnySquare(neutral));
    const JPH::Vec3                             normal1 = neutral.Cross(plane1);
    float                                       planeHalf  = DEG2RAD(planeHalfDeg);
    float                                       normalHalf = DEG2RAD(normalHalfDeg);

    {
        const float x      = child.Dot(neutral);
        const float aboutY = atan2f(-child.Dot(normal1), x);
        const float aboutZ = atan2f(child.Dot(plane1), x);
        const float e      = Square(aboutY / planeHalf) + Square(aboutZ / normalHalf);

        if (e > 0.81f) {
            const float grow = sqrtf(e) / 0.9f;

            planeHalf  = Q_min(planeHalf * grow, DEG2RAD(175.0f));
            normalHalf = Q_min(normalHalf * grow, DEG2RAD(175.0f));
        }
    }

    s->mSpace               = JPH::EConstraintSpace::WorldSpace;
    s->mPosition1           = pivot;
    s->mPosition2           = pivot;
    s->mTwistAxis1          = neutral;
    s->mPlaneAxis1          = plane1;
    s->mTwistAxis2          = child;
    s->mPlaneAxis2          = JPH::Quat::sFromTo(neutral, child) * plane1;
    s->mPlaneHalfConeAngle  = planeHalf;
    s->mNormalHalfConeAngle = normalHalf;
    s->mTwistMinAngle       = -DEG2RAD(twistDeg);
    s->mTwistMaxAngle       = DEG2RAD(twistDeg);
    s->mMaxFrictionTorque   = friction;
    return s;
}

// A hinge about axis, the angle from the parent's direction to the child's,
// limited to [PR_HINGE_MIN, maxDeg] degrees, the top widened to take in how it
// starts; the bottom is soft.
static JPH::Ref<JPH::HingeConstraintSettings> PR_Hinge(
    JPH::RVec3Arg pivot, JPH::Vec3Arg axis, JPH::Vec3Arg parent, JPH::Vec3Arg child, float maxDeg, float friction
)
{
    JPH::Ref<JPH::HingeConstraintSettings> s  = new JPH::HingeConstraintSettings;
    const JPH::Vec3                        n1 = PR_Across(parent, axis, PR_AnySquare(axis));
    const JPH::Vec3                        n2 = PR_Across(child, axis, n1);
    const float                            angle = atan2f(axis.Dot(n1.Cross(n2)), n1.Dot(n2));

    s->mSpace             = JPH::EConstraintSpace::WorldSpace;
    s->mPoint1            = pivot;
    s->mPoint2            = pivot;
    s->mHingeAxis1        = axis;
    s->mHingeAxis2        = axis;
    s->mNormalAxis1       = n1;
    s->mNormalAxis2       = n2;
    // Jolt wants the bottom at or below zero and the top at or above it, so
    // the least bend is kept by a soft limit at zero and a motor-free spring:
    // the bottom sits at PR_HINGE_MIN by rotating what zero means, the
    // normals being set that much apart.
    s->mNormalAxis2       = JPH::Quat::sRotation(axis, -DEG2RAD(PR_HINGE_MIN)) * n2;
    s->mLimitsMin         = 0.0f;
    s->mLimitsMax         = Q_clamp_float(Q_max(DEG2RAD(maxDeg - PR_HINGE_MIN), angle - DEG2RAD(PR_HINGE_MIN) + DEG2RAD(5.0f)), 0.0f, JPH::JPH_PI);
    s->mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, PR_HINGE_MIN_STIFFNESS, 1.0f);
    s->mMaxFrictionTorque = friction;
    return s;
}

// Straight down the body, turned forward by flex and outward by abd (degrees).
static JPH::Vec3 PR_RangeNeutral(
    JPH::Vec3Arg up, JPH::Vec3Arg fwd, JPH::Vec3Arg right, qboolean left, float flexMin, float flexMax, float abdMin, float abdMax
)
{
    const float     flex = DEG2RAD((flexMin + flexMax) * 0.5f);
    const float     abd  = DEG2RAD((abdMin + abdMax) * 0.5f);
    const JPH::Vec3 out  = left ? -right : right;
    const JPH::Vec3 d    = -up * cosf(flex) + fwd * sinf(flex);

    return PR_Normalized(d * cosf(abd) + out * sinf(abd), -up);
}

// Each joint's motors at a share of the hold on the death pose; off at none.
static void CG_JoltRagdollTone(prRagdoll_t *pr, float share)
{
    for (int i = 0; i < PR_NUM_PARTS; i++) {
        const int index = pr->settings->GetConstraintIndexForBodyIndex(i);

        if (index < 0) {
            continue;
        }

        JPH::TwoBodyConstraint *c      = pr->ragdoll->GetConstraint(index);
        const float             torque = pr_tone[pr_parts[i].kind] * share;
        const JPH::EMotorState  state  = share > 0.0f ? JPH::EMotorState::Position : JPH::EMotorState::Off;

        if (c->GetSubType() == JPH::EConstraintSubType::SwingTwist) {
            JPH::SwingTwistConstraint *st = static_cast<JPH::SwingTwistConstraint *>(c);

            st->GetSwingMotorSettings().SetTorqueLimit(torque);
            st->GetTwistMotorSettings().SetTorqueLimit(torque);
            st->SetSwingMotorState(state);
            st->SetTwistMotorState(state);
        } else if (c->GetSubType() == JPH::EConstraintSubType::Hinge) {
            JPH::HingeConstraint *h = static_cast<JPH::HingeConstraint *>(c);

            h->GetMotorSettings().SetTorqueLimit(torque);
            h->SetMotorState(state);
        }
    }
}

// Every joint's motors aimed at the pose the body has now.
static void CG_JoltRagdollAimTone(prRagdoll_t *pr)
{
    for (int i = 0; i < PR_NUM_PARTS; i++) {
        const int index = pr->settings->GetConstraintIndexForBodyIndex(i);

        if (index < 0) {
            continue;
        }

        JPH::TwoBodyConstraint *c = pr->ragdoll->GetConstraint(index);

        if (c->GetSubType() == JPH::EConstraintSubType::SwingTwist) {
            JPH::SwingTwistConstraint *st = static_cast<JPH::SwingTwistConstraint *>(c);

            st->GetSwingMotorSettings() = JPH::MotorSettings(PR_TONE_FREQUENCY, 1.0f);
            st->GetTwistMotorSettings() = JPH::MotorSettings(PR_TONE_FREQUENCY, 1.0f);
            st->SetTargetOrientationCS(st->GetRotationInConstraintSpace());
        } else if (c->GetSubType() == JPH::EConstraintSubType::Hinge) {
            JPH::HingeConstraint *h = static_cast<JPH::HingeConstraint *>(c);

            h->GetMotorSettings() = JPH::MotorSettings(PR_TONE_FREQUENCY, 1.0f);
            h->SetTargetAngle(h->GetCurrentAngle());
        }
    }
}

int CG_JoltRagdollCreate(
    const vec3_t p[RD_NUM_JOINTS], const vec3_t v[RD_NUM_JOINTS], const float radius[RD_NUM_JOINTS], float toneTime
)
{
    JPH::Vec3                     jp[RD_NUM_JOINTS], jv[RD_NUM_JOINTS];
    JPH::Vec3                     pelvisUp, pelvisRight, pelvisFwd, chestUp, chestRight, chestFwd;
    JPH::Mat44                    world[PR_NUM_PARTS];
    JPH::Ref<JPH::RagdollSettings> settings;
    prRagdoll_t                  *pr = NULL;
    int                           slot, i, j;

    if (!phys_system) {
        return 0;
    }

    CG_JoltRagdollMapJoints();

    for (slot = 0; slot < PR_MAX_RAGDOLLS; slot++) {
        if (!pr_ragdolls[slot].used) {
            pr = &pr_ragdolls[slot];
            break;
        }
    }
    if (!pr) {
        return 0;
    }

    for (j = 0; j < RD_NUM_JOINTS; j++) {
        jp[j] = PhysToJolt(p[j]);
        jv[j] = PhysToJolt(v[j]);
    }

    // The frames the hips and shoulders are measured in.
    pelvisUp    = PR_Normalized(jp[RD_SPINE1] - jp[RD_PELVIS], JPH::Vec3::sAxisZ());
    pelvisRight = PR_Across(jp[RD_RTHIGH] - jp[RD_LTHIGH], pelvisUp, PR_AnySquare(pelvisUp));
    pelvisFwd   = pelvisUp.Cross(pelvisRight);
    chestUp     = PR_Normalized(jp[RD_NECK] - jp[RD_SPINE2], pelvisUp);
    chestRight  = PR_Across(jp[RD_RUARM] - jp[RD_LUARM], chestUp, pelvisRight);
    chestFwd    = chestUp.Cross(chestRight);

    settings            = new JPH::RagdollSettings;
    settings->mSkeleton = new JPH::Skeleton;
    settings->mParts.resize(PR_NUM_PARTS);

    pr->totalMass = 0.0f;

    for (i = 0; i < PR_NUM_PARTS; i++) {
        const prPartDef_t          *def  = &pr_parts[i];
        JPH::RagdollSettings::Part &part = settings->mParts[i];
        const JPH::Vec3             a    = jp[def->a];
        const JPH::Vec3             b    = jp[def->b];
        const JPH::Vec3             dir  = b - a;
        const float                 len  = dir.Length();
        JPH::Quat                   rot  = JPH::Quat::sIdentity();
        JPH::Vec3                   centre;
        JPH::Mat44                  toLocal;
        float                       r;

        if (len < 0.01f * PHYS_UNITS_TO_METRES) {
            return 0;
        }

        settings->mSkeleton->AddJoint(def->name, def->parent);

        rot     = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), dir / len);
        centre  = (a + b) * 0.5f;
        world[i] = JPH::Mat44::sRotationTranslation(rot, centre);
        toLocal  = world[i].InversedRotationTranslation();

        pr->localA[i] = toLocal * a;
        pr->localB[i] = toLocal * b;

        // As thick at each end as the rig says the joint there is, as the
        // particle solver's spheres are.
        const float ra = Q_max(radius[def->a], 1.0f) * PHYS_UNITS_TO_METRES;
        const float rb = Q_max(radius[def->b], 1.0f) * PHYS_UNITS_TO_METRES;
        r              = Q_max(ra, rb);

        // The pelvis also spans the hips and the chest the shoulders, which is
        // where the legs and arms hang from.
        if (i == PR_PELVIS || i == PR_CHEST) {
            const int                        l = i == PR_PELVIS ? RD_LTHIGH : RD_LUARM;
            const int                        rr = i == PR_PELVIS ? RD_RTHIGH : RD_RUARM;
            JPH::StaticCompoundShapeSettings compound;
            JPH::ShapeRefC                   main  = PR_Capsule(pr->localA[i], pr->localB[i], ra, rb);
            const float                      rs    = Q_max(radius[l], 1.0f) * 0.9f * PHYS_UNITS_TO_METRES;
            JPH::ShapeRefC                   across = PR_Capsule(toLocal * jp[l], toLocal * jp[rr], rs, rs);

            if (!main || !across) {
                return 0;
            }
            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), main);
            compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), across);

            JPH::ShapeSettings::ShapeResult result = compound.Create();
            if (result.HasError()) {
                return 0;
            }
            part.SetShape(result.Get());
        } else {
            JPH::ShapeRefC shape = PR_Capsule(pr->localA[i], pr->localB[i], ra, rb);

            if (!shape) {
                return 0;
            }
            part.SetShape(shape);
        }

        part.mPosition                       = JPH::RVec3(centre);
        part.mRotation                       = rot;
        part.mMotionType                     = JPH::EMotionType::Dynamic;
        part.mObjectLayer                    = PhysLayers::RAGDOLL;
        part.mOverrideMassProperties         = JPH::EOverrideMassProperties::CalculateInertia;
        part.mMassPropertiesOverride.mMass   = def->mass;
        part.mFriction                       = 0.8f;
        part.mRestitution                    = 0.05f;
        part.mLinearDamping                  = 0.05f;
        part.mAngularDamping                 = 0.3f;
        part.mMotionQuality                  = JPH::EMotionQuality::LinearCast;
        part.mUserData                       = PHYS_USERDATA_RAGDOLL_BASE + slot * 32 + i;
        pr->totalMass += def->mass;

        if (def->parent < 0) {
            continue;
        }

        // The joint to the parent, at this part's first joint.
        {
            const prPartDef_t *par       = &pr_parts[def->parent];
            const JPH::RVec3   pivot     = JPH::RVec3(a);
            const JPH::Vec3    child     = dir / len;
            const JPH::Vec3    parentDir = PR_Normalized(jp[par->b] - jp[par->a], child);
            const qboolean     arm       = (def->kind == PR_ELBOW || def->kind == PR_WRIST) ? qtrue : qfalse;
            const JPH::Vec3    right     = arm ? chestRight : pelvisRight;

            switch (def->kind) {
            case PR_BACK:
                part.mToParent = PR_SwingTwist(pivot, parentDir, pelvisRight, child, 12.0f, 10.0f, 6.0f, PR_FRICTION_BACK);
                break;
            case PR_NECKJ:
                // The neck on the chest, and the head on the neck.
                if (i == PR_NECK) {
                    part.mToParent = PR_SwingTwist(pivot, parentDir, chestRight, child, 15.0f, 12.0f, 15.0f, PR_FRICTION_NECK);
                } else {
                    part.mToParent = PR_SwingTwist(pivot, parentDir, chestRight, child, 22.0f, 18.0f, 30.0f, PR_FRICTION_NECK);
                }
                break;
            case PR_SHOULDER:
                part.mToParent = PR_SwingTwist(
                    pivot,
                    PR_RangeNeutral(chestUp, chestFwd, chestRight, def->left, PR_SHOULDER_FLEX_MIN, PR_SHOULDER_FLEX_MAX, PR_SHOULDER_ABD_MIN, PR_SHOULDER_ABD_MAX),
                    chestRight,
                    child,
                    (PR_SHOULDER_FLEX_MAX - PR_SHOULDER_FLEX_MIN) * 0.5f,
                    (PR_SHOULDER_ABD_MAX - PR_SHOULDER_ABD_MIN) * 0.5f,
                    45.0f,
                    PR_FRICTION_LIMB
                );
                break;
            case PR_HIP:
                part.mToParent = PR_SwingTwist(
                    pivot,
                    PR_RangeNeutral(pelvisUp, pelvisFwd, pelvisRight, def->left, PR_HIP_FLEX_MIN, PR_HIP_FLEX_MAX, PR_HIP_ABD_MIN, PR_HIP_ABD_MAX),
                    pelvisRight,
                    child,
                    (PR_HIP_FLEX_MAX - PR_HIP_FLEX_MIN) * 0.5f,
                    (PR_HIP_ABD_MAX - PR_HIP_ABD_MIN) * 0.5f,
                    10.0f,
                    PR_FRICTION_LIMB
                );
                break;
            case PR_ELBOW:
            case PR_KNEE: {
                // The way a man's bends, about the body's own side to side
                // axis made square to the bone above: an elbow forward (about
                // the right), a knee back (about the left).
                const JPH::Vec3 anatomical = PR_Across(def->kind == PR_ELBOW ? right : -right, parentDir, PR_AnySquare(parentDir));
                const JPH::Vec3 bentAbout  = PR_Normalized(parentDir.Cross(child), anatomical);
                const float     bend       = acosf(Q_clamp_float(parentDir.Dot(child), -1.0f, 1.0f));
                JPH::Vec3       axis       = anatomical;

                // Bent enough to show its plane, that is the plane. An elbow
                // bends whichever way the upper arm has turned; a knee only
                // the way a knee goes. One bent the other way at the hand-over
                // (the particles let a knee overshoot) starts outside its range
                // and is brought round by the soft limit.
                if (bend > DEG2RAD(15.0f) && (def->kind == PR_ELBOW || bentAbout.Dot(anatomical) > 0.0f)) {
                    axis = bentAbout;
                }
                part.mToParent = PR_Hinge(pivot, axis, parentDir, child, def->kind == PR_KNEE ? PR_KNEE_MAX : PR_ELBOW_MAX, PR_FRICTION_HINGE);
                break;
            }
            case PR_WRIST:
                part.mToParent = PR_SwingTwist(pivot, parentDir, right, child, 50.0f, 30.0f, 30.0f, PR_FRICTION_END);
                break;
            case PR_ANKLE:
                part.mToParent = PR_SwingTwist(pivot, child, right, child, 35.0f, 20.0f, 10.0f, PR_FRICTION_END);
                break;
            default:
                break;
            }
        }
    }

    settings->mSkeleton->CalculateParentJointIndices();
    if (!settings->Stabilize()) {
        return 0;
    }
    // Parts touching as the body starts do not collide with each other, or
    // they would be flung apart; nor does anything with its parent.
    settings->DisableParentChildCollisions(world, 0.005f);
    settings->CalculateBodyIndexToConstraintIndex();
    settings->CalculateConstraintIndexToBodyIdxPair();

    pr->ragdoll = settings->CreateRagdoll(pr_nextGroup++, 0, phys_system);
    if (!pr->ragdoll) {
        return 0;
    }

    pr->settings = settings;
    pr->ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

    // Moving as the particles were, and each part known for what it is
    // (CreateRagdoll gives every body the ragdoll's own user data).
    {
        JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

        for (i = 0; i < PR_NUM_PARTS; i++) {
            bodies.SetUserData(pr->ragdoll->GetBodyID(i), PHYS_USERDATA_RAGDOLL_BASE + slot * 32 + i);
        }

        for (i = 0; i < PR_NUM_PARTS; i++) {
            const JPH::Vec3 a  = jp[pr_parts[i].a], b = jp[pr_parts[i].b];
            const JPH::Vec3 va = jv[pr_parts[i].a], vb = jv[pr_parts[i].b];
            const JPH::Vec3 d  = b - a;
            const float     l2 = Q_max(d.LengthSq(), 1e-6f);

            bodies.SetLinearAndAngularVelocity(pr->ragdoll->GetBodyID(i), (va + vb) * 0.5f, d.Cross(vb - va) / l2);
        }
    }

    pr->used        = qtrue;
    pr->holdJoint   = -1;
    pr->anchor      = JPH::BodyID();
    pr->hold        = NULL;
    pr->onBodyMask  = 0;
    pr->toneTime    = toneTime;
    pr->toneElapsed = 0.0f;
    memset(pr->contact, 0, sizeof(pr->contact));

    CG_JoltRagdollAimTone(pr);
    CG_JoltRagdollTone(pr, toneTime > 0.0f ? 1.0f : 0.0f);

    return slot + 1 + PR_MAX_RAGDOLLS * pr->generation;
}

static void CG_JoltRagdollJoint(const prRagdoll_t *pr, JPH::BodyInterface& bodies, int joint, JPH::RVec3 *point, JPH::BodyID *id);

// Lets go of the joint held.
static void CG_JoltRagdollDropHold(prRagdoll_t *pr)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    if (pr->hold) {
        phys_system->RemoveConstraint(pr->hold);
        pr->hold = NULL;
    }
    if (!pr->anchor.IsInvalid()) {
        bodies.RemoveBody(pr->anchor);
        bodies.DestroyBody(pr->anchor);
        pr->anchor = JPH::BodyID();
    }
    pr->holdJoint = -1;
}

// Ties a joint to a new anchor where it is.
static void CG_JoltRagdollTakeHold(prRagdoll_t *pr, int joint)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();
    JPH::RVec3          point;
    JPH::BodyID         part;

    CG_JoltRagdollDropHold(pr);
    CG_JoltRagdollJoint(pr, bodies, joint, &point, &part);

    {
        JPH::BodyCreationSettings settings(
            new JPH::SphereShape(0.02f), point, JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, PhysLayers::KINEMATIC
        );
        settings.mIsSensor = true;
        pr->anchor         = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    }
    if (pr->anchor.IsInvalid()) {
        return;
    }

    {
        JPH::SixDOFConstraintSettings settings;
        const float                   force = pr->totalMass * (-phys_system->GetGravity().GetZ() + PR_HOLD_EXTRA_ACCEL);

        settings.mSpace     = JPH::EConstraintSpace::WorldSpace;
        settings.mPosition1 = point;
        settings.mPosition2 = point;
        for (int a = 0; a < JPH::SixDOFConstraintSettings::EAxis::Num; a++) {
            settings.MakeFreeAxis((JPH::SixDOFConstraintSettings::EAxis)a);
        }
        for (int a = JPH::SixDOFConstraintSettings::EAxis::RotationX; a <= JPH::SixDOFConstraintSettings::EAxis::RotationZ; a++) {
            settings.mMaxFriction[a] = PR_HOLD_SPIN_FRICTION;
        }
        for (int a = JPH::SixDOFConstraintSettings::EAxis::TranslationX; a <= JPH::SixDOFConstraintSettings::EAxis::TranslationZ; a++) {
            settings.mMotorSettings[a] = JPH::MotorSettings(PR_HOLD_FREQUENCY, 1.0f, force, 0.0f);
        }

        pr->hold = static_cast<JPH::SixDOFConstraint *>(bodies.CreateConstraint(&settings, pr->anchor, part));
    }
    if (!pr->hold) {
        CG_JoltRagdollDropHold(pr);
        return;
    }

    phys_system->AddConstraint(pr->hold);
    for (int a = JPH::SixDOFConstraintSettings::EAxis::TranslationX; a <= JPH::SixDOFConstraintSettings::EAxis::TranslationZ; a++) {
        pr->hold->SetMotorState((JPH::SixDOFConstraintSettings::EAxis)a, JPH::EMotorState::Position);
    }
    pr->hold->SetTargetPositionCS(JPH::Vec3::sZero());
    pr->holdJoint = joint;
}

static void CG_JoltRagdollRelease(prRagdoll_t *pr)
{
    CG_JoltRagdollDropHold(pr);

    if (pr->ragdoll) {
        pr->ragdoll->RemoveFromPhysicsSystem();
    }
    pr->ragdoll  = NULL;
    pr->settings = NULL;
    pr->used     = qfalse;
    pr->generation++;
}

void CG_JoltRagdollDestroy(int handle)
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);

    if (pr) {
        CG_JoltRagdollRelease(pr);
    }
}

static void CG_JoltChainsUnload(void);

// The world is going: every body in it.
void CG_JoltRagdollsUnload(void)
{
    for (int i = 0; i < PR_MAX_RAGDOLLS; i++) {
        if (pr_ragdolls[i].used) {
            CG_JoltRagdollRelease(&pr_ragdolls[i]);
        }
    }
    CG_JoltChainsUnload();
}

// The world under them has changed (the physics editor): every body wakes, to
// fall or settle on what is there now.
void CG_JoltRagdollsWake(void)
{
    for (int i = 0; i < PR_MAX_RAGDOLLS; i++) {
        if (pr_ragdolls[i].used && pr_ragdolls[i].ragdoll) {
            pr_ragdolls[i].ragdoll->Activate();
        }
    }
}

//=============================================================
// Following it
//=============================================================

static void CG_JoltRagdollJoint(const prRagdoll_t *pr, JPH::BodyInterface& bodies, int joint, JPH::RVec3 *point, JPH::BodyID *id)
{
    const int part = pr_jointPart[joint];

    *id    = pr->ragdoll->GetBodyID(part);
    *point = bodies.GetWorldTransform(*id) * (pr_jointEnd[joint] ? pr->localB[part] : pr->localA[part]);
}

qboolean CG_JoltRagdollRead(
    int handle, vec3_t p[RD_NUM_JOINTS], vec3_t v[RD_NUM_JOINTS], qboolean contact[RD_NUM_JOINTS], vec3_t contactNormal[RD_NUM_JOINTS],
    int *onBodyMask
)
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);
    int          j;

    if (!pr) {
        return qfalse;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (j = 0; j < RD_NUM_JOINTS; j++) {
        JPH::RVec3  point;
        JPH::BodyID id;

        CG_JoltRagdollJoint(pr, bodies, j, &point, &id);
        PhysFromJolt(JPH::Vec3(point), p[j]);
        PhysFromJolt(bodies.GetPointVelocity(id, point), v[j]);

        contact[j] = pr->contact[j];
        VectorCopy(pr->contactNormal[j], contactNormal[j]);
    }

    *onBodyMask = pr->onBodyMask;
    return qtrue;
}

void CG_JoltRagdollAddVelocity(int handle, const vec3_t dv[RD_NUM_JOINTS])
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);
    int          i;

    if (!pr) {
        return;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    // Each part takes the velocities at its two joints: their mean as it
    // moves, and their difference as it turns.
    for (i = 0; i < PR_NUM_PARTS; i++) {
        const JPH::BodyID id = pr->ragdoll->GetBodyID(i);
        const JPH::Vec3   va = PhysToJolt(dv[pr_parts[i].a]);
        const JPH::Vec3   vb = PhysToJolt(dv[pr_parts[i].b]);
        const JPH::RMat44 t  = bodies.GetWorldTransform(id);
        const JPH::Vec3   d  = t.Multiply3x3(pr->localB[i] - pr->localA[i]);
        const float       l2 = Q_max(d.LengthSq(), 1e-6f);

        if (va.IsNearZero() && vb.IsNearZero()) {
            continue;
        }

        bodies.SetLinearAndAngularVelocity(
            id, bodies.GetLinearVelocity(id) + (va + vb) * 0.5f, bodies.GetAngularVelocity(id) + d.Cross(vb - va) / l2
        );
    }

    pr->ragdoll->Activate();
}

void CG_JoltRagdollHold(int handle, int joint, const vec3_t target)
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);

    if (!pr) {
        return;
    }

    if (joint < 0 || joint >= RD_NUM_JOINTS) {
        if (pr->holdJoint >= 0) {
            CG_JoltRagdollDropHold(pr);
        }
        return;
    }

    if (joint != pr->holdJoint || !pr->hold) {
        CG_JoltRagdollTakeHold(pr, joint);
    }
    VectorCopy(target, pr->holdTarget);
    pr->ragdoll->Activate();
}

float CG_JoltRagdollSpeed(int handle)
{
    prRagdoll_t *pr   = CG_JoltRagdollGet(handle);
    float        most = 0.0f;

    if (!pr) {
        return 0.0f;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (int i = 0; i < PR_NUM_PARTS; i++) {
        most = Q_max(most, bodies.GetLinearVelocity(pr->ragdoll->GetBodyID(i)).Length());
    }

    return most * PHYS_METRES_TO_UNITS;
}

qboolean CG_JoltRagdollAwake(int handle)
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);

    if (!pr) {
        return qfalse;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (int i = 0; i < PR_NUM_PARTS; i++) {
        if (bodies.IsActive(pr->ragdoll->GetBodyID(i))) {
            return qtrue;
        }
    }
    return qfalse;
}

void CG_JoltRagdollSleep(int handle)
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);

    if (!pr) {
        return;
    }

    const JPH::Array<JPH::BodyID>& ids = pr->ragdoll->GetBodyIDs();
    phys_system->GetBodyInterface().DeactivateBodies(ids.data(), (int)ids.size());
}

void CG_JoltRagdollWake(int handle)
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);

    if (pr) {
        pr->ragdoll->Activate();
    }
}

// Each step of the world: the hold on the death pose easing off, and the
// anchor of a body held by the grabber carried towards the end of the beam.
void CG_JoltRagdollsStep(float dt)
{
    if (!phys_system) {
        return;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (int n = 0; n < PR_MAX_RAGDOLLS; n++) {
        prRagdoll_t *pr = &pr_ragdolls[n];
        JPH::RVec3   point;
        JPH::BodyID  id;

        if (!pr->used) {
            continue;
        }

        // The hold on the death pose, easing off as the particle solver's
        // does (CG_RagdollLimpness).
        if (pr->toneTime > 0.0f) {
            float t;

            pr->toneElapsed += dt;
            t = Q_min(pr->toneElapsed / pr->toneTime, 1.0f);
            CG_JoltRagdollTone(pr, 1.0f - t * t * (3.0f - 2.0f * t));
            if (t >= 1.0f) {
                pr->toneTime = 0.0f;
            }
        }

        // What it touches is gathered afresh by the step about to be taken.
        // Only a body still moving: one asleep keeps what it was lying on.
        if (CG_JoltRagdollAwake(n + 1 + PR_MAX_RAGDOLLS * pr->generation)) {
            memset(pr->contact, 0, sizeof(pr->contact));
            pr->onBodyMask = 0;
        }

        if (pr->holdJoint < 0 || pr->anchor.IsInvalid()) {
            continue;
        }

        // The anchor after the end of the beam, no faster than a hand.
        {
            const JPH::RVec3 at     = bodies.GetPosition(pr->anchor);
            JPH::Vec3        toward = PhysToJolt(pr->holdTarget) - JPH::Vec3(at);
            const float      most   = PR_HOLD_MAX_SPEED * PHYS_UNITS_TO_METRES * dt;

            if (toward.Length() > most) {
                toward = toward.Normalized() * most;
            }
            bodies.MoveKinematic(pr->anchor, at + toward, JPH::Quat::sIdentity(), dt);
        }
        (void)point;
        (void)id;
    }
}

// From the world's contact listener: what a part of a body touched, for the
// solver's rules about resting.
void CG_JoltRagdollContact(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold)
{
    const JPH::uint64 lo = PHYS_USERDATA_RAGDOLL_BASE, hi = PHYS_USERDATA_RAGDOLL_BASE + PR_MAX_RAGDOLLS * 32;

    for (int side = 0; side < 2; side++) {
        const JPH::Body& self  = side ? b : a;
        const JPH::Body& other = side ? a : b;
        const JPH::uint64 data = self.GetUserData();
        prRagdoll_t      *pr;
        int               slot, part;
        JPH::Vec3         up;
        qboolean          onBody = qfalse;

        if (data < lo || data >= hi || other.IsSensor()) {
            continue;
        }

        slot = (int)((data - lo) / 32);
        part = (int)((data - lo) % 32);
        pr   = &pr_ragdolls[slot];
        if (!pr->used || part >= PR_NUM_PARTS) {
            continue;
        }

        // Its own parts touching are nothing to it.
        if (other.GetUserData() >= lo && other.GetUserData() < hi) {
            if ((int)((other.GetUserData() - lo) / 32) == slot) {
                continue;
            }
            onBody = qtrue;
        }

        // The normal points from the first body to the second; the way the
        // surface holds this part up is from the other one into it.
        up = side ? manifold.mWorldSpaceNormal : -manifold.mWorldSpaceNormal;

        for (int end = 0; end < 2; end++) {
            const int joint = end ? pr_parts[part].b : pr_parts[part].a;

            if (onBody && up.GetZ() > 0.7f) {
                pr->onBodyMask |= 1 << joint;
            }
            if (!pr->contact[joint] || up.GetZ() > pr->contactNormal[joint][2]) {
                pr->contact[joint] = qtrue;
                pr->contactNormal[joint][0] = up.GetX();
                pr->contactNormal[joint][1] = up.GetY();
                pr->contactNormal[joint][2] = up.GetZ();
            }
        }
    }
}

// For the harness and debugging: each joint's swing and twist (degrees) as
// Jolt measures them, and each hinge's angle, printed with print.
void CG_JoltRagdollReport(int handle, void (*print)(const char *fmt, ...))
{
    prRagdoll_t *pr = CG_JoltRagdollGet(handle);

    if (!pr) {
        return;
    }

    for (int i = 0; i < PR_NUM_PARTS; i++) {
        const int index = pr->settings->GetConstraintIndexForBodyIndex(i);

        if (index < 0) {
            continue;
        }

        const JPH::TwoBodyConstraint *c = pr->ragdoll->GetConstraint(index);

        if (c->GetSubType() == JPH::EConstraintSubType::SwingTwist) {
            const JPH::SwingTwistConstraint *st = static_cast<const JPH::SwingTwistConstraint *>(c);
            JPH::Quat                        swing, twist;

            st->GetRotationInConstraintSpace().GetSwingTwist(swing, twist);
            print(
                "  %-7s swing %5.1f (limits %5.1f %5.1f)  twist %6.1f (limit %5.1f)\n",
                pr_parts[i].name,
                RAD2DEG(2.0f * acosf(Q_min(1.0f, fabsf(swing.GetW())))),
                RAD2DEG(st->GetPlaneHalfConeAngle()),
                RAD2DEG(st->GetNormalHalfConeAngle()),
                RAD2DEG(2.0f * atan2f(twist.GetX(), twist.GetW())),
                RAD2DEG(st->GetTwistMaxAngle())
            );
        } else if (c->GetSubType() == JPH::EConstraintSubType::Hinge) {
            const JPH::HingeConstraint *h = static_cast<const JPH::HingeConstraint *>(c);

            print(
                "  %-7s hinge %6.1f (limits %6.1f %6.1f)\n",
                pr_parts[i].name,
                RAD2DEG(h->GetCurrentAngle()),
                RAD2DEG(h->GetLimitsMin()),
                RAD2DEG(h->GetLimitsMax())
            );
        }
    }
}

//=============================================================
// Chains
//=============================================================
//
// Added in OPM. A part cut off a body (cg_gore.cpp): an arm, a leg, a head,
// as a short chain of the ragdoll's capsules, joined as its limbs are but
// loosely, a swing and a twist each. On the ragdoll layer, so it falls on the
// world and the props, knocks into corpses and the other parts, and is left
// alone by the living walking through it, as corpses are.

#define PC_MAX_CHAINS 32
#define PC_MAX_SEGS   (PHYS_CHAIN_MAX_PTS - 1)
// The user data of a chain's pieces: this + its slot * 32 + the piece, on from
// the ragdolls' (a body, for the impact sounds)
#define PHYS_USERDATA_CHAIN_BASE (PHYS_USERDATA_RAGDOLL_BASE + PR_MAX_RAGDOLLS * 32)

typedef struct {
    qboolean                       used;
    int                            generation;
    int                            numSegs;
    JPH::Ref<JPH::RagdollSettings> settings;
    JPH::Ref<JPH::Ragdoll>         ragdoll;
    JPH::Vec3                      localA[PC_MAX_SEGS], localB[PC_MAX_SEGS]; // each piece's ends, metres, body space
} pcChain_t;

static pcChain_t pc_chains[PC_MAX_CHAINS];

static pcChain_t *CG_JoltChainGet(int handle)
{
    int slot, gen;

    if (handle <= 0 || !phys_system) {
        return NULL;
    }

    slot = (handle - 1) % PC_MAX_CHAINS;
    gen  = (handle - 1) / PC_MAX_CHAINS;
    if (!pc_chains[slot].used || pc_chains[slot].generation != gen) {
        return NULL;
    }
    return &pc_chains[slot];
}

static void CG_JoltChainRelease(pcChain_t *pc)
{
    if (pc->ragdoll) {
        pc->ragdoll->RemoveFromPhysicsSystem();
    }
    pc->ragdoll  = NULL;
    pc->settings = NULL;
    pc->used     = qfalse;
    pc->generation++;
}

static void CG_JoltChainsUnload(void)
{
    for (int i = 0; i < PC_MAX_CHAINS; i++) {
        if (pc_chains[i].used) {
            CG_JoltChainRelease(&pc_chains[i]);
        }
    }
}

int CG_JoltChainCreate(int numPts, const vec3_t *p, const vec3_t *v, const float *radius, float mass)
{
    JPH::Vec3                      jp[PHYS_CHAIN_MAX_PTS], jv[PHYS_CHAIN_MAX_PTS];
    JPH::Mat44                     world[PC_MAX_SEGS];
    JPH::Ref<JPH::RagdollSettings> settings;
    pcChain_t                     *pc = NULL;
    int                            slot, i, numSegs = numPts - 1;

    if (!phys_system || numSegs < 1 || numPts > PHYS_CHAIN_MAX_PTS) {
        return 0;
    }

    for (slot = 0; slot < PC_MAX_CHAINS; slot++) {
        if (!pc_chains[slot].used) {
            pc = &pc_chains[slot];
            break;
        }
    }
    if (!pc) {
        return 0;
    }

    for (i = 0; i < numPts; i++) {
        jp[i] = PhysToJolt(p[i]);
        jv[i] = PhysToJolt(v[i]);
    }

    settings            = new JPH::RagdollSettings;
    settings->mSkeleton = new JPH::Skeleton;
    settings->mParts.resize(numSegs);

    for (i = 0; i < numSegs; i++) {
        JPH::RagdollSettings::Part &part = settings->mParts[i];
        const JPH::Vec3             a    = jp[i];
        const JPH::Vec3             b    = jp[i + 1];
        const JPH::Vec3             dir  = b - a;
        const float                 len  = dir.Length();
        const float                 ra   = Q_max(radius[i], 1.0f) * PHYS_UNITS_TO_METRES;
        const float                 rb   = Q_max(radius[i + 1], 1.0f) * PHYS_UNITS_TO_METRES;
        JPH::Quat                   rot;
        JPH::Mat44                  toLocal;
        JPH::ShapeRefC              shape;

        if (len < 0.01f * PHYS_UNITS_TO_METRES) {
            return 0;
        }

        settings->mSkeleton->AddJoint(va("piece%d", i), i - 1);

        rot      = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), dir / len);
        world[i] = JPH::Mat44::sRotationTranslation(rot, (a + b) * 0.5f);
        toLocal  = world[i].InversedRotationTranslation();

        pc->localA[i] = toLocal * a;
        pc->localB[i] = toLocal * b;

        shape = PR_Capsule(pc->localA[i], pc->localB[i], ra, rb);
        if (!shape) {
            return 0;
        }
        part.SetShape(shape);
        part.mPosition                     = JPH::RVec3((a + b) * 0.5f);
        part.mRotation                     = rot;
        part.mMotionType                   = JPH::EMotionType::Dynamic;
        part.mObjectLayer                  = PhysLayers::RAGDOLL;
        part.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
        part.mMassPropertiesOverride.mMass = Q_max(mass / numSegs, 0.2f);
        // rough and heavy-feeling: a limb drags to a stop rather than
        // skating off across the snow
        part.mFriction                     = 1.2f;
        part.mRestitution                  = 0.05f;
        part.mLinearDamping                = 0.6f;
        part.mAngularDamping               = 0.8f;
        part.mMotionQuality                = JPH::EMotionQuality::LinearCast;

        if (i > 0) {
            const JPH::Vec3 parentDir = PR_Normalized(jp[i] - jp[i - 1], dir / len);

            // loose: what held it straight is gone with the rest of him
            part.mToParent = PR_SwingTwist(JPH::RVec3(a), parentDir, PR_AnySquare(parentDir), dir / len, 70.0f, 70.0f, 30.0f, PR_FRICTION_LIMB);
        }
    }

    settings->mSkeleton->CalculateParentJointIndices();
    if (!settings->Stabilize()) {
        return 0;
    }
    settings->DisableParentChildCollisions(world, 0.005f);
    settings->CalculateBodyIndexToConstraintIndex();
    settings->CalculateConstraintIndexToBodyIdxPair();

    pc->ragdoll = settings->CreateRagdoll(pr_nextGroup++, 0, phys_system);
    if (!pc->ragdoll) {
        return 0;
    }
    pc->settings = settings;
    pc->numSegs  = numSegs;
    pc->ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

    {
        JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

        for (i = 0; i < numSegs; i++) {
            const JPH::Vec3 d  = jp[i + 1] - jp[i];
            const float     l2 = Q_max(d.LengthSq(), 1e-6f);

            bodies.SetUserData(pc->ragdoll->GetBodyID(i), PHYS_USERDATA_CHAIN_BASE + slot * 32 + i);
            bodies.SetLinearAndAngularVelocity(pc->ragdoll->GetBodyID(i), (jv[i] + jv[i + 1]) * 0.5f, d.Cross(jv[i + 1] - jv[i]) / l2);
        }
    }

    pc->used = qtrue;
    return slot + 1 + PC_MAX_CHAINS * pc->generation;
}

void CG_JoltChainDestroy(int handle)
{
    pcChain_t *pc = CG_JoltChainGet(handle);

    if (pc) {
        CG_JoltChainRelease(pc);
    }
}

qboolean CG_JoltChainRead(int handle, vec3_t *p)
{
    pcChain_t *pc = CG_JoltChainGet(handle);
    int        i;

    if (!pc || !pc->ragdoll) {
        return qfalse;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterfaceNoLock();

    for (i = 0; i < pc->numSegs; i++) {
        const JPH::Mat44 m = bodies.GetWorldTransform(pc->ragdoll->GetBodyID(i));

        PhysFromJolt(m * pc->localA[i], p[i]);
        if (i == pc->numSegs - 1) {
            PhysFromJolt(m * pc->localB[i], p[i + 1]);
        }
    }
    return qtrue;
}

void CG_JoltChainAddVelocity(int handle, const vec3_t point, const vec3_t dv)
{
    pcChain_t *pc = CG_JoltChainGet(handle);
    float      best = 0;
    int        i, nearest = 0;

    if (!pc || !pc->ragdoll) {
        return;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();
    const JPH::Vec3     at     = PhysToJolt(point);

    for (i = 0; i < pc->numSegs; i++) {
        const float d = (JPH::Vec3(bodies.GetCenterOfMassPosition(pc->ragdoll->GetBodyID(i))) - at).LengthSq();

        if (!i || d < best) {
            best    = d;
            nearest = i;
        }
    }

    {
        const JPH::BodyID id   = pc->ragdoll->GetBodyID(nearest);
        const float       mass = CG_PhysicsBodyMass(id);

        if (mass > 0) {
            bodies.AddImpulse(id, PhysToJolt(dv) * mass, JPH::RVec3(at));
            bodies.ActivateBody(id);
        }
    }
}

qboolean CG_JoltChainAwake(int handle)
{
    pcChain_t *pc = CG_JoltChainGet(handle);

    if (!pc || !pc->ragdoll) {
        return qfalse;
    }
    return pc->ragdoll->IsActive() ? qtrue : qfalse;
}
