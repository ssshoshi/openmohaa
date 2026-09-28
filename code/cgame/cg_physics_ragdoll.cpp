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

// How far a knee or an elbow bends, degrees from straight.
#define PR_HINGE_MAX 150.0f

// Friction in the joints, newton metres, which is most of what stops a limp
// body flailing.
#define PR_FRICTION_BACK  3.0f
#define PR_FRICTION_NECK  1.0f
#define PR_FRICTION_LIMB  2.0f
#define PR_FRICTION_HINGE 1.0f
#define PR_FRICTION_END   0.3f

// The grabber's spring: natural frequency, radians a second (critically
// damped), and the most it accelerates the joint held, units a second squared.
#define PR_HOLD_OMEGA     16.0f
#define PR_HOLD_MAX_ACCEL 6000.0f

typedef struct {
    qboolean                      used;
    int                           generation;
    JPH::Ref<JPH::RagdollSettings> settings;
    JPH::Ref<JPH::Ragdoll>         ragdoll;
    JPH::Vec3                     localA[PR_NUM_PARTS], localB[PR_NUM_PARTS]; // the part's two joints, metres, body space
    float                         totalMass;

    int    holdJoint;
    vec3_t holdTarget;

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

// A capsule between two points of a part's own space.
static JPH::ShapeRefC PR_Capsule(JPH::Vec3Arg a, JPH::Vec3Arg b, float radius)
{
    const JPH::Vec3 d   = b - a;
    const float     len = d.Length();
    JPH::Quat       rot = JPH::Quat::sIdentity();

    if (len > 1e-5f) {
        rot = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), d / len);
    }

    JPH::RotatedTranslatedShapeSettings shape((a + b) * 0.5f, rot, new JPH::CapsuleShapeSettings(Q_max(len * 0.5f, 0.001f), radius));
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
// limited to [-3, PR_HINGE_MAX] degrees or wider to take in how it starts.
static JPH::Ref<JPH::HingeConstraintSettings> PR_Hinge(
    JPH::RVec3Arg pivot, JPH::Vec3Arg axis, JPH::Vec3Arg parent, JPH::Vec3Arg child, float friction
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
    s->mLimitsMin         = Q_clamp_float(Q_min(DEG2RAD(-3.0f), angle - DEG2RAD(5.0f)), -JPH::JPH_PI, 0.0f);
    s->mLimitsMax         = Q_clamp_float(Q_max(DEG2RAD(PR_HINGE_MAX), angle + DEG2RAD(5.0f)), 0.0f, JPH::JPH_PI);
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

int CG_JoltRagdollCreate(const vec3_t p[RD_NUM_JOINTS], const vec3_t v[RD_NUM_JOINTS], const float radius[RD_NUM_JOINTS])
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

        // As thick as the rig says: the trunk at its thicker end, a limb a
        // little under its thinner one.
        if (def->kind == PR_ROOT || def->kind == PR_BACK || def->kind == PR_NECKJ) {
            r = Q_max(radius[def->a], radius[def->b]) * 0.9f;
        } else {
            r = Q_min(radius[def->a], radius[def->b]) * 0.85f;
        }
        r = Q_max(r, 1.0f) * PHYS_UNITS_TO_METRES;

        // The pelvis also spans the hips and the chest the shoulders, which is
        // where the legs and arms hang from.
        if (i == PR_PELVIS || i == PR_CHEST) {
            const int                        l = i == PR_PELVIS ? RD_LTHIGH : RD_LUARM;
            const int                        rr = i == PR_PELVIS ? RD_RTHIGH : RD_RUARM;
            JPH::StaticCompoundShapeSettings compound;
            JPH::ShapeRefC                   main  = PR_Capsule(pr->localA[i], pr->localB[i], r);
            JPH::ShapeRefC                   across = PR_Capsule(toLocal * jp[l], toLocal * jp[rr], Q_max(radius[l], 1.0f) * 0.9f * PHYS_UNITS_TO_METRES);

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
            JPH::ShapeRefC shape = PR_Capsule(pr->localA[i], pr->localB[i], r);

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
                part.mToParent = PR_SwingTwist(pivot, parentDir, pelvisRight, child, 25.0f, 20.0f, 15.0f, PR_FRICTION_BACK);
                break;
            case PR_NECKJ:
                part.mToParent = PR_SwingTwist(pivot, parentDir, chestRight, child, 40.0f, 35.0f, 35.0f, PR_FRICTION_NECK);
                break;
            case PR_SHOULDER:
                part.mToParent = PR_SwingTwist(
                    pivot,
                    PR_RangeNeutral(chestUp, chestFwd, chestRight, def->left, PR_SHOULDER_FLEX_MIN, PR_SHOULDER_FLEX_MAX, PR_SHOULDER_ABD_MIN, PR_SHOULDER_ABD_MAX),
                    chestRight,
                    child,
                    (PR_SHOULDER_FLEX_MAX - PR_SHOULDER_FLEX_MIN) * 0.5f,
                    (PR_SHOULDER_ABD_MAX - PR_SHOULDER_ABD_MIN) * 0.5f,
                    70.0f,
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
                    35.0f,
                    PR_FRICTION_LIMB
                );
                break;
            case PR_ELBOW:
            case PR_KNEE: {
                // Bending the way it is bent, when it is; else the way a man's
                // does: an elbow forward, a knee back.
                const JPH::Vec3 fwd   = def->kind == PR_ELBOW ? chestFwd : -pelvisFwd;
                JPH::Vec3       axis  = PR_Normalized(parentDir.Cross(fwd), def->kind == PR_ELBOW ? right : -right);
                const float     bend  = acosf(Q_clamp_float(parentDir.Dot(child), -1.0f, 1.0f));

                if (bend > DEG2RAD(15.0f)) {
                    axis = PR_Normalized(parentDir.Cross(child), axis);
                }
                part.mToParent = PR_Hinge(pivot, axis, parentDir, child, PR_FRICTION_HINGE);
                break;
            }
            case PR_WRIST:
                part.mToParent = PR_SwingTwist(pivot, parentDir, right, child, 70.0f, 35.0f, 30.0f, PR_FRICTION_END);
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

    pr->used       = qtrue;
    pr->holdJoint  = -1;
    pr->onBodyMask = 0;
    memset(pr->contact, 0, sizeof(pr->contact));

    return slot + 1 + PR_MAX_RAGDOLLS * pr->generation;
}

static void CG_JoltRagdollRelease(prRagdoll_t *pr)
{
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

// The world is going: every body in it.
void CG_JoltRagdollsUnload(void)
{
    for (int i = 0; i < PR_MAX_RAGDOLLS; i++) {
        if (pr_ragdolls[i].used) {
            CG_JoltRagdollRelease(&pr_ragdolls[i]);
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

    pr->holdJoint = (joint >= 0 && joint < RD_NUM_JOINTS) ? joint : -1;
    if (pr->holdJoint >= 0) {
        VectorCopy(target, pr->holdTarget);
        pr->ragdoll->Activate();
    }
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

// Each step of the world: a body held by the grabber hangs from the joint
// taken hold of, on a spring towards the end of the beam, strong enough to lift
// the whole of it.
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
        JPH::Vec3    pointVel, accel, gravity, force;
        float        limit, mass, most;

        if (!pr->used) {
            continue;
        }

        // What it touches is gathered afresh by the step about to be taken.
        // Only a body still moving: one asleep keeps what it was lying on.
        if (CG_JoltRagdollAwake(n + 1 + PR_MAX_RAGDOLLS * pr->generation)) {
            memset(pr->contact, 0, sizeof(pr->contact));
            pr->onBodyMask = 0;
        }

        if (pr->holdJoint < 0) {
            continue;
        }

        CG_JoltRagdollJoint(pr, bodies, pr->holdJoint, &point, &id);
        pointVel = bodies.GetPointVelocity(id, point);

        accel   = (PhysToJolt(pr->holdTarget) - JPH::Vec3(point)) * (PR_HOLD_OMEGA * PR_HOLD_OMEGA) - pointVel * (2.0f * PR_HOLD_OMEGA);
        gravity = -phys_system->GetGravity();
        limit   = PR_HOLD_MAX_ACCEL * PHYS_UNITS_TO_METRES;
        if (accel.Length() > limit) {
            accel = accel.Normalized() * limit;
        }

        // The part's own weight and pull, and what is left of the strength
        // for the rest of the body hanging from it.
        {
            JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), id);

            if (!lock.Succeeded()) {
                continue;
            }
            mass = 1.0f / Q_max(1e-4f, lock.GetBody().GetMotionProperties()->GetInverseMass());
        }

        force = (accel + gravity) * pr->totalMass;
        most  = pr->totalMass * (limit + gravity.Length());
        if (force.Length() > most) {
            force = force.Normalized() * most;
        }
        (void)mass;

        bodies.AddImpulse(id, force * dt, point);
        bodies.ActivateBody(id);
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

        if (data < lo || data >= hi) {
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
