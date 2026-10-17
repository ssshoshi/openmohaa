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
// The map's props as rigid bodies.
//
// Every prop the registry keeps (cg_props.cpp) becomes a body: the small ones
// dynamic, the rest static. A dynamic prop starts asleep, so nothing on the map
// shifts or settles as it loads; it moves only once something reaches it. Its
// pose goes back to the renderer, which draws the static model where the body
// is, and to the registry, which is what the ragdolls' traces collide with.

#include "cg_physics_local.h"
#include "cg_props.h"
#include "cg_water.h"

#include <vector>

//=============================================================
// Materials
//=============================================================

// What a prop is made of, guessed from its name. Its mass goes with the area of
// its box rather than the volume: props are thin-walled or open things -- a
// bottle or a bucket is a shell, a chair is mostly gaps -- and by volume a chair
// weighed a hundred and twenty kilograms. By area, in kilograms a square metre:
// a chair comes to about sixteen, a crate twenty five, a bottle under two.
typedef struct {
    const char *name;
    const char *words[24];
    float       arealDensity; // kg/m2 of box surface
    float       friction;
    float       restitution;
    physSoundMat_t sound; // what it sounds like striking things (phys_impact.cpp)
    // In water (CG_PhysicsPropsWater): what it is held up by all under, over
    // its weight. Over 1 it floats, riding with 1/buoyancy of it under; under
    // 1 it sinks, what it displaces still holding some of it up, and the water
    // slowing it to a sink (PP_WATER_FORM).
    float       buoyancy;
} physMaterial_t;

static const physMaterial_t phys_materials[] = {
    {"glass",
     {"bottle", "jug", "pitcher", "glass", "jar", "vase", NULL},
     1.5f, 0.4f, 0.3f, PHYS_SND_GLASS, 0.4f},
    {"metal",
     {"helmet", NULL},
     3.0f, 0.5f, 0.2f, PHYS_SND_HELMET, 0.3f},
    {"metal",
     {"mg42", "rifle", "kar98", "pistol", "gun", NULL},
     3.0f, 0.5f, 0.2f, PHYS_SND_WEAPON, 0.15f},
    {"metal",
     {"bucket", "can", "shell", "pot", "stove", "radio", "ammo", "valve", "switch", "microphone", "filter",
      "canteen", "radar", "lantern", NULL},
     3.0f, 0.5f, 0.2f, PHYS_SND_METAL, 0.3f},
    {"paper",
     {"paper", "card", "book", "map", "sack", "bag", "cloth", "blanket", "pillow", NULL},
     0.8f, 0.8f, 0.05f, PHYS_SND_PAPER, 1.15f}, // soaked, it floats low
    {"stone",
     {"sandbag", "brick", "stone", "rock", "concrete", NULL},
     25.0f, 0.8f, 0.05f, PHYS_SND_STONE, 0.45f},
    {"wood",
     {"chair", "stool", "crate", "table", "trunk", "box", "bench", "lid", "cot", "basket", "desk", "shelf", "barrel",
      NULL},
     4.0f, 0.6f, 0.15f, PHYS_SND_WOOD, 2.2f},
};

// not known by its name: it sinks
static const physMaterial_t phys_defaultMaterial = {"default", {NULL}, 3.0f, 0.5f, 0.15f, PHYS_SND_DEFAULT, 0.35f};

static const physMaterial_t *CG_PhysicsMaterial(const char *name)
{
    char   lower[64];
    size_t i;
    int    w;

    Q_strncpyz(lower, name, sizeof(lower));
    Q_strlwr(lower);

    for (i = 0; i < ARRAY_LEN(phys_materials); i++) {
        for (w = 0; phys_materials[i].words[w]; w++) {
            if (strstr(lower, phys_materials[i].words[w])) {
                return &phys_materials[i];
            }
        }
    }

    return &phys_defaultMaterial;
}

void CG_PhysicsMaterialFor(const char *name, float *arealDensity, float *friction, float *restitution)
{
    const physMaterial_t *m = CG_PhysicsMaterial(name);

    *arealDensity = m->arealDensity;
    *friction     = m->friction;
    *restitution  = m->restitution;
}

physSoundMat_t CG_PhysicsSoundMatFor(const char *name)
{
    return CG_PhysicsMaterial(name)->sound;
}

//=============================================================
// Bodies
//=============================================================

// Smaller than this along its longest side, a prop is debris: it collides with
// the world and bigger things, not with other debris.
#define PHYS_DEBRIS_SIZE 12.0f

typedef struct {
    JPH::BodyID id;
    int         prop;
    qboolean    everMoved; // its stand-ins are gone
    JPH::RVec3  prevPos, curPos;
    JPH::Quat   prevRot, curRot;
    qboolean    moving; // moved since it was last drawn where it was placed
    float       buoyancy; // its material's (CG_PhysicsPropsWater)
    float       form;     // the water's drag on it going through, over speed squared (PP_WATER_FORM)
    float       wet;      // how much of it was under in the step before
} physProp_t;

// How fast a prop comes to go with the water round it: a share of the
// difference a second, all under. Slower through it than PP_WAKE_SPEED, it
// leaves no wake.
#define PP_WATER_DRAG  2.5f
#define PP_WAKE_SPEED  12.0f
// And for what sinks, the drag that grows with the square of the speed, as
// water's does, for how much face a prop turns to the water for its mass:
// half water's density (kg/m3) by a drag coefficient (0.4) by a sixth of its
// box's area. A helmet settles, a sandbag drops: glass sinks at ~0.7 m/s,
// metal ~1, stone ~2.2. Never more than PP_WATER_FORM_MOST of its speed a
// second, so it is slowed, not stopped; not on what floats, which plunges in
// and bobs up; and not on what the grabber holds, which a hand moves freely.
#define PP_WATER_FORM      (0.5f * 1000.0f * 0.4f / 6.0f)
#define PP_WATER_FORM_MOST 15.0f
// The points of its box the water holds it up and slows it at, this far out
// from its middle to its corners
#define PP_WATER_INSET 0.7f

static std::vector<physProp_t> pp_props;
static std::vector<JPH::BodyID> pp_static;
// what each hanging lamp hangs from (CG_PropHangs)
static std::vector<JPH::Ref<JPH::Constraint>> pp_hangs;
static char                    pp_map[MAX_QPATH];

// A lamp on a cord (cgProp_t.corded): the cord is a rope of a few points, its
// ends held at the anchor and at the lamp, which falls and swings under them
// and so sags when the lamp is knocked up into it, and lags as it swings.
#define PP_CORD_POINTS 40
#define PP_CORD_SMOOTH 4  // drawn pieces between two of its points, along a curve through them
#define PP_CORD_WIDTH  0.35f // half its thickness
#define PP_CORD_VIEW   3072.0f
#define PP_CORD_SHADER "opm/lampcord"

typedef struct {
    int      prop;
    vec3_t   p[PP_CORD_POINTS + 1];
    vec3_t   old[PP_CORD_POINTS + 1];
    int      lastTime;
    qboolean live;
} ppCord_t;

static std::vector<ppCord_t> pp_cords;

// Where the renderer is to draw a prop. A lamp on a cord drawn as its lamp
// without a wire (CG_PhysicsDrawCords) has its own model, wire and all, shrunk
// to a speck at the lamp's middle: it shows nothing, and the realtime light of
// the fixture, which follows it, stays in the lamp, its spot light turned as
// the lamp is (the axes keep their directions, scaled down).
#define PP_CORD_HIDDEN_SCALE 0.0001f
static void CG_PhysicsAxisFromQuat(JPH::QuatArg q, vec3_t axis[3]);

static void CG_PhysicsSendPose(const cgProp_t *p, const vec3_t origin, const vec3_t axis[3])
{
    if (p->corded && p->cordModel) {
        vec3_t light, none[3];
        int    k;

        VectorCopy(origin, light);
        for (k = 0; k < 3; k++) {
            VectorMA(light, p->cordLight[k], axis[k], light);
        }
        VectorScale(axis[0], PP_CORD_HIDDEN_SCALE, none[0]);
        VectorScale(axis[1], PP_CORD_HIDDEN_SCALE, none[1]);
        VectorScale(axis[2], PP_CORD_HIDDEN_SCALE, none[2]);
        cgi.R_SetStaticModelTransform(p->staticIndex, light, none);
        return;
    }
    cgi.R_SetStaticModelTransform(p->staticIndex, origin, axis);
}

static qboolean                pp_loaded;

static JPH::Quat CG_PhysicsQuatFromAxis(const vec3_t axis[3])
{
    // The rotation that takes the model's own x, y and z to its axes.
    const JPH::Mat44 m(
        JPH::Vec4(axis[0][0], axis[0][1], axis[0][2], 0.0f),
        JPH::Vec4(axis[1][0], axis[1][1], axis[1][2], 0.0f),
        JPH::Vec4(axis[2][0], axis[2][1], axis[2][2], 0.0f),
        JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)
    );

    return m.GetQuaternion().Normalized();
}

static void CG_PhysicsAxisFromQuat(JPH::QuatArg q, vec3_t axis[3])
{
    const JPH::Mat44 m = JPH::Mat44::sRotation(q);
    int              k;

    for (k = 0; k < 3; k++) {
        const JPH::Vec3 c = m.GetColumn3(k);

        axis[k][0] = c.GetX();
        axis[k][1] = c.GetY();
        axis[k][2] = c.GetZ();
    }
}

// A hanging lamp's mass in kilograms, and how fast the air slows its swing.
#define PP_HANGING_MASS    4.0f
#define PP_HANGING_DAMPING 0.25f

// Added in OPM
//  A hanging lamp: a box round its lower end only, the lamp and not the wire or
//  chain above it, which would grind against the ceiling it hangs from.
static JPH::ShapeRefC CG_PhysicsHangingShape(const cgProp_t *p)
{
    vec3_t    half, centre;
    JPH::Vec3 extent;
    float     height = p->maxs[2] - p->mins[2];
    float     lamp   = Q_min(height, Q_max(8.0f, Q_min(height * 0.4f, 24.0f)));
    int       k;

    for (k = 0; k < 2; k++) {
        half[k]   = Q_max(1.0f, (p->maxs[k] - p->mins[k]) * 0.5f);
        centre[k] = (p->maxs[k] + p->mins[k]) * 0.5f;
    }
    half[2]   = lamp * 0.5f;
    centre[2] = p->mins[2] + lamp * 0.5f;
    extent    = PhysToJolt(half);

    // the inner settings are held by reference, and so must not be on the stack
    JPH::Ref<JPH::BoxShapeSettings>     box = new JPH::BoxShapeSettings(extent, Q_min(JPH::cDefaultConvexRadius, extent.ReduceMin() * 0.5f));
    JPH::RotatedTranslatedShapeSettings placed(PhysToJolt(centre), JPH::Quat::sIdentity(), box);
    JPH::ShapeSettings::ShapeResult     result = placed.Create();

    return result.HasError() ? JPH::ShapeRefC() : result.Get();
}

// Added in OPM
//  A box from mins to maxs in a prop's own frame.
static JPH::ShapeRefC CG_PhysicsBoxShape(const vec3_t mins, const vec3_t maxs)
{
    vec3_t    half, centre;
    JPH::Vec3 extent;
    int       k;

    for (k = 0; k < 3; k++) {
        half[k]   = Q_max(1.0f, (maxs[k] - mins[k]) * 0.5f);
        centre[k] = (maxs[k] + mins[k]) * 0.5f;
    }
    extent = PhysToJolt(half);

    // the inner settings are held by reference, and so must not be on the stack
    JPH::Ref<JPH::BoxShapeSettings>     inner = new JPH::BoxShapeSettings(extent, Q_min(JPH::cDefaultConvexRadius, extent.ReduceMin() * 0.5f));
    JPH::RotatedTranslatedShapeSettings placed(PhysToJolt(centre), JPH::Quat::sIdentity(), inner);
    JPH::ShapeSettings::ShapeResult     result = placed.Create();

    return result.HasError() ? JPH::ShapeRefC() : result.Get();
}

static JPH::ShapeRefC CG_PhysicsPropShape(const cgProp_t *p)
{
    JPH::Array<JPH::Vec3> points;
    int                   i;

    for (i = 0; i < p->numHull; i++) {
        points.push_back(PhysToJolt(p->hull[i]));
    }

    {
        JPH::ConvexHullShapeSettings hull(points, JPH::cDefaultConvexRadius);

        JPH::ShapeSettings::ShapeResult result = hull.Create();
        if (!result.HasError()) {
            return result.Get();
        }
    }

    // A box, where the hull could not be made (a flat prop, say).
    {
        vec3_t    half, centre;
        JPH::Vec3 extent;
        int       k;

        for (k = 0; k < 3; k++) {
            half[k]   = Q_max(0.5f, (p->maxs[k] - p->mins[k]) * 0.5f);
            centre[k] = (p->maxs[k] + p->mins[k]) * 0.5f;
        }
        extent = PhysToJolt(half);

        JPH::BoxShapeSettings box(extent, Q_min(JPH::cDefaultConvexRadius, extent.ReduceMin() * 0.5f));

        JPH::RotatedTranslatedShapeSettings placed(PhysToJolt(centre), JPH::Quat::sIdentity(), &box);
        JPH::ShapeSettings::ShapeResult     result = placed.Create();

        return result.HasError() ? JPH::ShapeRefC() : result.Get();
    }
}

void CG_PhysicsUnloadProps(void)
{
    if (phys_system) {
        JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

        for (size_t i = 0; i < pp_hangs.size(); i++) {
            phys_system->RemoveConstraint(pp_hangs[i]);
        }
        for (size_t i = 0; i < pp_props.size(); i++) {
            bodies.RemoveBody(pp_props[i].id);
            bodies.DestroyBody(pp_props[i].id);
        }
        for (size_t i = 0; i < pp_static.size(); i++) {
            bodies.RemoveBody(pp_static[i]);
            bodies.DestroyBody(pp_static[i]);
        }
    }

    for (int i = 0; i < cg_numProps; i++) {
        cg_props[i].body = -1;
    }

    pp_hangs.clear();
    pp_cords.clear();
    pp_props.clear();
    pp_static.clear();
    pp_loaded = qfalse;
    pp_map[0] = 0;
}

void CG_PhysicsLoadProps(void)
{
    const qboolean canMove = (cgi.apiversion >= 5 && cgi.R_SetStaticModelTransform) ? qtrue : qfalse;
    int            dynamic = 0, statics = 0, failed = 0, clipped = 0;

    if (pp_loaded && !Q_stricmp(pp_map, cgs.mapname)) {
        return;
    }

    CG_PhysicsUnloadProps();
    pp_loaded = qtrue;
    Q_strncpyz(pp_map, cgs.mapname, sizeof(pp_map));

    if (!phys_system) {
        return;
    }

    CG_PropsLoad();

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (int i = 0; i < cg_numProps; i++) {
        cgProp_t             *p        = &cg_props[i];
        const physMaterial_t *material = CG_PhysicsMaterial(p->name);
        const qboolean        moves    = (p->dynamic && canMove && CG_PhysicsPropsMove() && cg_physics_props->integer
                                  && (!p->clipped || CG_PhysicsClippedPropsMove()))
                                         ? qtrue
                                         : qfalse;
        JPH::ShapeRefC        shape;
        float                 biggest  = 0.0f;
        int                   k;

        // Left out (foliage, lights, wire, the very small and big, or a rule).
        if (!p->solid) {
            continue;
        }

        shape = moves && p->corded ? CG_PhysicsBoxShape(p->cordMins, p->cordMaxs)
              : moves && p->hangs ? CG_PhysicsHangingShape(p) : CG_PhysicsPropShape(p);
        if (!shape) {
            failed++;
            continue;
        }

        if (p->dynamic && p->clipped) {
            clipped++;
        }

        for (k = 0; k < 3; k++) {
            biggest = Q_max(biggest, p->maxs[k] - p->mins[k]);
        }

        JPH::BodyCreationSettings settings(
            shape,
            JPH::RVec3(PhysToJolt(p->origin)),
            CG_PhysicsQuatFromAxis(p->axis),
            moves ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
            moves ? (biggest < PHYS_DEBRIS_SIZE ? PhysLayers::DEBRIS : PhysLayers::PROP) : PhysLayers::WORLD
        );
        settings.mFriction    = material->friction;
        settings.mRestitution = material->restitution;
        settings.mUserData    = PHYS_USERDATA_PROP_BASE + i;

        if (moves) {
            const JPH::Vec3 size = PhysToJolt(p->maxs) - PhysToJolt(p->mins);
            const float     area = 2.0f * (size.GetX() * size.GetY() + size.GetY() * size.GetZ() + size.GetZ() * size.GetX());

            settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass =
                p->mass > 0.0f ? p->mass : Q_clamp_float(area * material->arealDensity, 0.2f, 200.0f);
            settings.mLinearDamping  = 0.05f;
            settings.mAngularDamping = 0.1f;
            settings.mMotionQuality  = JPH::EMotionQuality::LinearCast;
            if (p->hangs) {
                // light, and the air slows its swing to rest in a few seconds
                settings.mMassPropertiesOverride.mMass = p->mass > 0.0f ? p->mass : PP_HANGING_MASS;
                settings.mLinearDamping                = PP_HANGING_DAMPING;
                settings.mAngularDamping               = PP_HANGING_DAMPING;
                settings.mMotionQuality                = JPH::EMotionQuality::Discrete;
            }
        }

        const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (id.IsInvalid()) {
            failed++;
            continue;
        }

        // on its cord: never farther than its length from where it hangs, and
        // free to come nearer, the cord going slack
        if (moves && p->corded) {
            JPH::DistanceConstraintSettings cord;
            vec3_t                          top;

            VectorCopy(p->origin, top);
            for (k = 0; k < 3; k++) {
                VectorMA(top, p->cordTop[k], p->axis[k], top);
            }
            cord.mSpace       = JPH::EConstraintSpace::WorldSpace;
            cord.mPoint1      = JPH::RVec3(PhysToJolt(p->cordAnchor));
            cord.mPoint2      = JPH::RVec3(PhysToJolt(top));
            cord.mMinDistance = 0.0f;
            cord.mMaxDistance = p->cordLength * PHYS_UNITS_TO_METRES;

            JPH::Constraint *c = bodies.CreateConstraint(&cord, JPH::BodyID(), id);
            if (c) {
                phys_system->AddConstraint(c);
                pp_hangs.push_back(c);
            }
        }
        // tied where it hangs
        else if (moves && p->hangs) {
            JPH::PointConstraintSettings tie;
            vec3_t                       point;

            VectorCopy(p->origin, point);
            for (k = 0; k < 3; k++) {
                VectorMA(point, p->hangPoint[k], p->axis[k], point);
            }
            tie.mSpace  = JPH::EConstraintSpace::WorldSpace;
            tie.mPoint1 = tie.mPoint2 = JPH::RVec3(PhysToJolt(point));

            JPH::Constraint *c = bodies.CreateConstraint(&tie, JPH::BodyID(), id);
            if (c) {
                phys_system->AddConstraint(c);
                pp_hangs.push_back(c);
            }
        }

        if (moves) {
            physProp_t pp;

            pp.id      = id;
            pp.prop    = i;
            pp.curPos  = pp.prevPos = settings.mPosition;
            pp.curRot  = pp.prevRot = settings.mRotation;
            pp.moving  = qfalse;
            pp.everMoved = qfalse;
            pp.buoyancy  = material->buoyancy;
            {
                const JPH::Vec3 box  = PhysToJolt(p->maxs) - PhysToJolt(p->mins);
                const float     area = 2.0f * (box.GetX() * box.GetY() + box.GetY() * box.GetZ() + box.GetZ() * box.GetX());

                pp.form = PP_WATER_FORM * area / Q_max(settings.mMassPropertiesOverride.mMass, 0.1f);
            }
            pp.wet       = 0.0f;
            p->body    = (int)pp_props.size();
            pp_props.push_back(pp);

            if (p->corded) {
                ppCord_t cord;

                memset(&cord, 0, sizeof(cord));
                cord.prop = i;
                pp_cords.push_back(cord);
                CG_PhysicsSendPose(p, p->origin, p->axis);
            }
            dynamic++;
        } else {
            pp_static.push_back(id);
            statics++;
        }
    }

    phys_system->OptimizeBroadPhase();

    if (cg_physics_log->integer) {
        cgi.Printf(
            "physics: %d props move, %d are fixed, %d could not be shaped; %d small props are wrapped in clip brushes (%s)%s\n",
            dynamic,
            statics,
            failed,
            clipped,
            CG_PhysicsCanRemoveStandIns() ? "the clip brushes go when they move"
            : cg_physics_clipped->integer ? "moving anyway, the clip brushes stay"
                                          : "kept fixed",
            !CG_PhysicsPropsMove() ? " (multiplayer, so none move)"
            : canMove              ? ""
                                   : " (this engine cannot move static models, so none move)"
        );
    }
}

// Before each step: the props in water (cg_water.cpp). The water holds each
// up at eight points through its box, by its material (a crate floats, a
// helmet sinks), each by how far under it is, eased in over half the box's
// height so that one going under is no sudden push; and slows each point as
// it goes through the water, which also stops it spinning and rocking, and
// carries it along. Going through it, it leaves a wake; falling in, splashes.
// Only props awake: one asleep where it came to rest stays so until something
// wakes it.
void CG_PhysicsPropsWater(float dt)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterfaceNoLock();
    const float         g      = -phys_system->GetGravity().GetZ();
    JPH::BodyID         held;

    if (!cg_water_float->integer) {
        return;
    }
    if (!CG_PhysicsGrabbedBody(&held)) {
        held = JPH::BodyID();
    }

    for (size_t i = 0; i < pp_props.size(); i++) {
        physProp_t     *pp = &pp_props[i];
        const cgProp_t *p  = &cg_props[pp->prop];
        cgWaterSample_t water;
        vec3_t          centre, axis[3], mid, corner, points[8], flow, v;
        float           zmin, zmax, band, share, mass, size, tops[8];
        int             k, c;

        if (!bodies.IsActive(pp->id) || p->hangs || p->corded) {
            continue;
        }

        PhysFromJolt(JPH::Vec3(bodies.GetPosition(pp->id)), centre);
        CG_PhysicsAxisFromQuat(bodies.GetRotation(pp->id), axis);

        // the middle of its box, and how low and high the box reaches
        VectorCopy(centre, mid);
        for (k = 0; k < 3; k++) {
            VectorMA(mid, 0.5f * (p->mins[k] + p->maxs[k]), axis[k], mid);
        }
        zmin = 1e9f;
        zmax = -1e9f;
        for (c = 0; c < 8; c++) {
            VectorCopy(mid, corner);
            VectorCopy(mid, points[c]);
            for (k = 0; k < 3; k++) {
                const float half = 0.5f * (p->maxs[k] - p->mins[k]) * ((c & (1 << k)) ? 1.0f : -1.0f);

                VectorMA(corner, half, axis[k], corner);
                VectorMA(points[c], half * PP_WATER_INSET, axis[k], points[c]);
            }
            zmin = Q_min(zmin, corner[2]);
            zmax = Q_max(zmax, corner[2]);
        }

        VectorSet(corner, mid[0], mid[1], zmin + 1.0f);
        if (!CG_WaterSample(corner, &water)) {
            pp->wet = 0.0f;
            continue;
        }

        // how far under each point is, eased over half the box's height
        band  = Q_max((zmax - zmin) * 0.5f, 2.0f);
        share = 0.0f;
        for (c = 0; c < 8; c++) {
            tops[c] = Com_Clamp(0.0f, 1.0f, 0.5f + (water.surface + CG_WaterSwell(points[c]) - points[c][2]) / band);
            share += tops[c] / 8.0f;
        }
        if (share <= 0.0f) {
            pp->wet = 0.0f;
            continue;
        }

        mass = CG_PhysicsBodyMass(pp->id);
        CG_WaterFlow(mid, water.depth, flow);
        PhysFromJolt(bodies.GetLinearVelocity(pp->id), v);

        // a prop that goes in falling splashes, by how big and how fast
        size = Q_max(p->maxs[0] - p->mins[0], p->maxs[1] - p->mins[1]);
        if (pp->wet <= 0.0f && v[2] < -60.0f) {
            vec3_t at;

            VectorSet(at, mid[0], mid[1], water.surface);
            CG_WaterSplash(at, Com_Clamp(4.0f, 30.0f, size * 0.6f), -v[2]);
        }
        pp->wet = share;

        for (c = 0; c < 8; c++) {
            const JPH::RVec3 point(PhysToJolt(points[c]));
            JPH::Vec3        force;

            if (tops[c] <= 0.0f) {
                continue;
            }
            const JPH::Vec3 rel  = PhysToJolt(flow) - bodies.GetPointVelocity(pp->id, point);
            const float     drag = PP_WATER_DRAG
                             + (pp->buoyancy < 1.0f && pp->id != held ? Q_min(pp->form * rel.Length(), PP_WATER_FORM_MOST) : 0.0f);

            force = rel * (mass * drag * tops[c] / 8.0f) + JPH::Vec3(0.0f, 0.0f, mass * g * pp->buoyancy * tops[c] / 8.0f);
            bodies.AddForce(pp->id, force, point);
        }

        // going through it: a wake from where it meets the water
        {
            const float speed = sqrt(Square(v[0] - flow[0]) + Square(v[1] - flow[1]));

            if (speed > PP_WAKE_SPEED && zmax > water.surface) {
                const float push = Q_min(share * 2.0f, 1.0f);
                vec3_t      at;

                VectorSet(at, mid[0] + (v[0] - flow[0]) / speed * size * 0.4f,
                    mid[1] + (v[1] - flow[1]) / speed * size * 0.4f, water.surface);
                CG_WaterRipple(at, Com_Clamp(6.0f, 40.0f, size * 0.8f), Q_min(speed, 300.0f) * 0.035f * push * dt,
                    Q_min(speed, 300.0f) / 150.0f * 0.6f * push * dt);
            }
        }
    }
}

// After each step: where each awake prop is now, and where it was.
void CG_PhysicsPropsStepped(void)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterfaceNoLock();

    for (size_t i = 0; i < pp_props.size(); i++) {
        physProp_t *pp = &pp_props[i];

        pp->prevPos = pp->curPos;
        pp->prevRot = pp->curRot;

        if (!bodies.IsActive(pp->id)) {
            continue;
        }

        bodies.GetPositionAndRotation(pp->id, pp->curPos, pp->curRot);
        pp->moving = qtrue;

        // Moving for the first time: the clip brushes that stood in for it
        // are left where it was, so they go, for everybody sharing this
        // collision model -- player, bullets, AI and the ragdolls.
        if (!pp->everMoved) {
            const cgProp_t *p = &cg_props[pp->prop];

            pp->everMoved = qtrue;
            if (p->numStandIns && CG_PhysicsCanRemoveStandIns()) {
                vec3_t    was;
                int       before;
                JPH::RVec3 pos;
                JPH::Quat  rot;

                // Where it stood: the middle of its box as it was placed. The
                // registry already has its new pose, so this comes from the
                // step before it woke.
                pos = pp->prevPos;
                PhysFromJolt(JPH::Vec3(pos), was);
                was[2] += (p->mins[2] + p->maxs[2]) * 0.5f;
                before = cgi.CM_PointContents(was, 0);
                (void)rot;

                for (int k = 0; k < p->numStandIns; k++) {
                    CG_PhysicsDisableBrush(p->standIns[k]);
                }

                if (cg_physics_log->integer) {
                    cgi.Printf(
                        "physics: %s moved; its %d clip brushes are gone (contents where it stood 0x%x, now 0x%x)\n",
                        p->name,
                        p->numStandIns,
                        before,
                        cgi.CM_PointContents(was, 0)
                    );
                }
            }
        }

        // The registry takes the stepped pose: it is what the ragdolls'
        // traces collide with, and they step on the same clock.
        {
            vec3_t origin, axis[3];

            PhysFromJolt(JPH::Vec3(pp->curPos), origin);
            CG_PhysicsAxisFromQuat(pp->curRot, axis);
            CG_PropSetPose(pp->prop, origin, axis);
        }
    }
}

qboolean CG_PhysicsPropBody(int prop, JPH::BodyID *id)
{
    if (prop < 0 || prop >= cg_numProps || cg_props[prop].body < 0 || cg_props[prop].body >= (int)pp_props.size()) {
        return qfalse;
    }

    *id = pp_props[cg_props[prop].body].id;
    return qtrue;
}

void CG_PhysicsMovedStaticModels(std::vector<int> *out)
{
    out->clear();
    for (size_t i = 0; i < pp_props.size(); i++) {
        if (pp_props[i].everMoved || pp_props[i].moving) {
            out->push_back(cg_props[pp_props[i].prop].staticIndex);
        }
    }
}

// Every frame: draw the props that have moved where they are, between the last
// two steps by how far the clock is into the next.
void CG_PhysicsDrawProps(float frac)
{
    for (size_t i = 0; i < pp_props.size(); i++) {
        physProp_t *pp = &pp_props[i];
        vec3_t      origin, axis[3];

        if (!pp->moving) {
            continue;
        }

        {
            const JPH::Vec3 pos = JPH::Vec3(pp->prevPos) + (JPH::Vec3(pp->curPos) - JPH::Vec3(pp->prevPos)) * frac;
            const JPH::Quat rot = pp->prevRot.SLERP(pp->curRot, frac).Normalized();

            PhysFromJolt(pos, origin);
            CG_PhysicsAxisFromQuat(rot, axis);
        }

        CG_PhysicsSendPose(&cg_props[pp->prop], origin, axis);

        // Settled: drawn where it came to rest, and left alone after.
        if (pp->prevPos == pp->curPos && pp->prevRot == pp->curRot
            && !phys_system->GetBodyInterfaceNoLock().IsActive(pp->id)) {
            pp->moving = qfalse;
        }
    }
}


//=============================================================
// Lamps on cords
//=============================================================

static void CG_PhysicsCordStep(ppCord_t *c, const cgProp_t *p, const vec3_t top)
{
    const float rest = p->cordLength / PP_CORD_POINTS;
    float       dt, g;
    int         i, n;

    if (!c->live) {
        for (i = 0; i <= PP_CORD_POINTS; i++) {
            const float t = (float)i / PP_CORD_POINTS;

            VectorScale(p->cordAnchor, 1.0f - t, c->p[i]);
            VectorMA(c->p[i], t, top, c->p[i]);
            VectorCopy(c->p[i], c->old[i]);
        }
        c->lastTime = cg.time;
        c->live     = qtrue;
    }

    dt          = Q_clamp_float((cg.time - c->lastTime) * 0.001f, 0.0f, 0.05f);
    c->lastTime = cg.time;
    g           = cg.snap && cg.snap->ps.gravity ? (float)cg.snap->ps.gravity : 800.0f;

    // falling, a little slowed by the air
    for (i = 1; i < PP_CORD_POINTS; i++) {
        vec3_t v;

        VectorSubtract(c->p[i], c->old[i], v);
        VectorCopy(c->p[i], c->old[i]);
        VectorMA(c->p[i], 0.98f, v, c->p[i]);
        c->p[i][2] -= g * dt * dt;
    }

    // held at both ends, each length as long as it is
    VectorCopy(p->cordAnchor, c->p[0]);
    VectorCopy(top, c->p[PP_CORD_POINTS]);
    // both ways in turn, so that a pull at one end reaches the other
    for (n = 0; n < 40; n++) {
        for (int j = 0; j < PP_CORD_POINTS; j++) {
            const int i = (n & 1) ? PP_CORD_POINTS - 1 - j : j;
            vec3_t    d;
            float     len, share;

            VectorSubtract(c->p[i + 1], c->p[i], d);
            len = VectorLength(d);
            if (len < 0.0001f) {
                continue;
            }
            // a cord pulls but does not push: shorter, it folds and droops
            if (len <= rest) {
                continue;
            }
            share = (len - rest) / len;
            if (i == 0) {
                VectorMA(c->p[i + 1], -share, d, c->p[i + 1]);
            } else if (i + 1 == PP_CORD_POINTS) {
                VectorMA(c->p[i], share, d, c->p[i]);
            } else {
                VectorMA(c->p[i], 0.5f * share, d, c->p[i]);
                VectorMA(c->p[i + 1], -0.5f * share, d, c->p[i + 1]);
            }
        }
        VectorCopy(p->cordAnchor, c->p[0]);
        VectorCopy(top, c->p[PP_CORD_POINTS]);
    }
}

// The point t of the way from c->p[i] to c->p[i + 1], on a curve through the
// points around them (Catmull-Rom), so that the cord bends smoothly between
// its points rather than in straight pieces.
static void CG_PhysicsCordCurve(const ppCord_t *c, int i, float t, vec3_t out)
{
    const float *p0 = c->p[Q_max(i - 1, 0)];
    const float *p1 = c->p[i];
    const float *p2 = c->p[i + 1];
    const float *p3 = c->p[Q_min(i + 2, PP_CORD_POINTS)];
    const float  t2 = t * t, t3 = t2 * t;
    int          k;

    for (k = 0; k < 3; k++) {
        out[k] = 0.5f * (2.0f * p1[k] + (p2[k] - p0[k]) * t + (2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k]) * t2
                         + (3.0f * p1[k] - p0[k] - 3.0f * p2[k] + p3[k]) * t3);
    }
}

// A strip along the cord, face on to the eye wherever it is, as the grabber's
// beam is drawn (CG_RagdollDrawGrabBeam).
static void CG_PhysicsCordDraw(const ppCord_t *c, qhandle_t shader)
{
    const int  n = PP_CORD_POINTS * PP_CORD_SMOOTH;
    polyVert_t quad[4];
    vec3_t     points[PP_CORD_POINTS * PP_CORD_SMOOTH + 1];
    vec3_t     side, prevSide;
    int        i, k;

    for (i = 0; i < n; i++) {
        CG_PhysicsCordCurve(c, i / PP_CORD_SMOOTH, (float)(i % PP_CORD_SMOOTH) / PP_CORD_SMOOTH, points[i]);
    }
    VectorCopy(c->p[PP_CORD_POINTS], points[n]);

    for (i = 0; i <= n; i++) {
        vec3_t tangent, toEye;

        VectorSubtract(points[Q_min(i + 1, n)], points[Q_max(i - 1, 0)], tangent);
        VectorSubtract(cg.refdef.vieworg, points[i], toEye);
        CrossProduct(tangent, toEye, side);
        if (VectorNormalize(side) < 0.0001f) {
            VectorCopy(cg.refdef.viewaxis[1], side);
        }
        VectorScale(side, PP_CORD_WIDTH, side);

        if (i) {
            VectorAdd(points[i - 1], prevSide, quad[0].xyz);
            VectorSubtract(points[i - 1], prevSide, quad[1].xyz);
            VectorSubtract(points[i], side, quad[2].xyz);
            VectorAdd(points[i], side, quad[3].xyz);
            for (k = 0; k < 4; k++) {
                quad[k].st[0]       = (k == 0 || k == 1) ? (float)(i - 1) / n : (float)i / n;
                quad[k].st[1]       = (k == 1 || k == 2) ? 1.0f : 0.0f;
                quad[k].modulate[0] = quad[k].modulate[1] = quad[k].modulate[2] = quad[k].modulate[3] = 255;
            }
            cgi.R_AddPolyToScene(shader, 4, quad, 0);
        }
        VectorCopy(side, prevSide);
    }
}

// Every frame: the lamps on cords near the view, their lamps (when drawn
// without their wire) and their cords.
void CG_PhysicsDrawCords(float frac)
{
    static qhandle_t shader;

    if (pp_cords.empty()) {
        return;
    }
    if (!shader) {
        shader = cgi.R_RegisterShader(PP_CORD_SHADER);
    }

    for (size_t i = 0; i < pp_cords.size(); i++) {
        ppCord_t         *c  = &pp_cords[i];
        const cgProp_t   *p  = &cg_props[c->prop];
        const physProp_t *pp = &pp_props[p->body];
        vec3_t            origin, axis[3], top;
        int               k;

        {
            const JPH::Vec3 pos = JPH::Vec3(pp->prevPos) + (JPH::Vec3(pp->curPos) - JPH::Vec3(pp->prevPos)) * frac;
            const JPH::Quat rot = pp->prevRot.SLERP(pp->curRot, frac).Normalized();

            PhysFromJolt(pos, origin);
            CG_PhysicsAxisFromQuat(rot, axis);
        }

        // after a vid_restart the renderer has the model where the map put it
        CG_PhysicsSendPose(p, origin, axis);

        // the glows at its bulb go with it
        for (int n = 0; n < p->numCordCoronas; n++) {
            const cgCorona_t *corona = &cg_coronas[p->cordCoronas[n]];
            vec3_t            at;

            VectorCopy(origin, at);
            for (k = 0; k < 3; k++) {
                VectorMA(at, p->cordCoronaLocal[n][k], axis[k], at);
            }
            cgi.R_SetStaticModelTransform(corona->staticIndex, at, corona->axis);
        }

        if (DistanceSquared(cg.refdef.vieworg, p->cordAnchor) > Square(PP_CORD_VIEW)) {
            c->live = qfalse;
            continue;
        }

        if (p->cordModel) {
            refEntity_t ent;

            memset(&ent, 0, sizeof(ent));
            ent.reType              = RT_MODEL;
            ent.hModel              = p->cordModel;
            ent.tiki                = cgi.R_Model_GetHandle(p->cordModel);
            ent.scale               = p->cordScale;
            ent.entityNumber        = ENTITYNUM_NONE;
            ent.frameInfo[0].weight = 1.0f;
            ent.actionWeight        = 1.0f;
            // its own light is inside it
            ent.renderfx            = RF_NOSHADOW;
            VectorCopy(origin, ent.origin);
            VectorCopy(origin, ent.lightingOrigin);
            for (k = 0; k < 3; k++) {
                VectorMA(ent.origin, p->cordOffset[k], axis[k], ent.origin);
                VectorMA(ent.lightingOrigin, p->cordLight[k], axis[k], ent.lightingOrigin);
            }
            VectorCopy(ent.origin, ent.oldorigin);
            AxisCopy(axis, ent.axis);
            cgi.R_AddRefEntityToScene(&ent, ENTITYNUM_NONE);
        }

        VectorCopy(origin, top);
        for (k = 0; k < 3; k++) {
            VectorMA(top, p->cordTop[k], axis[k], top);
        }
        CG_PhysicsCordStep(c, p, top);
        CG_PhysicsCordDraw(c, shader);
    }
}

//=============================================================
// Pushing things about
//=============================================================

// The body a ray from the eye along the view first strikes, and where.
static qboolean CG_PhysicsUnderCrosshair(float range, JPH::BodyID *id, vec3_t point, vec3_t dir)
{
    JPH::RayCastResult hit;
    vec3_t             end;

    VectorCopy(cg.refdef.viewaxis[0], dir);
    VectorMA(cg.refdef.vieworg, range, dir, end);

    JPH::RRayCast ray(JPH::RVec3(PhysToJolt(cg.refdef.vieworg)), PhysToJolt(end) - PhysToJolt(cg.refdef.vieworg));
    if (!phys_system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, PhysNoKinematicObjects())) {
        return qfalse;
    }

    *id = hit.mBodyID;
    VectorMA(cg.refdef.vieworg, range * hit.mFraction, dir, point);
    return qtrue;
}

// A body's mass in kilograms, or 0 if it does not move.
float CG_PhysicsBodyMass(JPH::BodyID id)
{
    JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), id);

    if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
        return 0.0f;
    }

    const float inv = lock.GetBody().GetMotionProperties()->GetInverseMass();
    return inv > 0.0f ? 1.0f / inv : 0.0f;
}

// Pushes a body at a point, in game units a second of change for a body of a
// kilogram; heavier bodies move less.
void CG_PhysicsImpulse(JPH::BodyID id, const vec3_t point, const vec3_t impulse)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    if (bodies.GetMotionType(id) != JPH::EMotionType::Dynamic) {
        return;
    }

    bodies.AddImpulse(id, PhysToJolt(impulse), JPH::RVec3(PhysToJolt(point)));
    bodies.ActivateBody(id);
}

// Whether the static world stands between two points. Asked of the physics
// world rather than the engine's, which still has the clip brushes that stand
// in for the props themselves.
static qboolean CG_PhysicsWorldBlocks(const vec3_t a, const vec3_t b)
{
    const JPH::RRayCast      ray(JPH::RVec3(PhysToJolt(a)), PhysToJolt(b) - PhysToJolt(a));
    JPH::RayCastResult       hit;
    PhysStaticOnlyBroadPhase broad;
    PhysStaticOnlyObjects    objects;

    return phys_system->GetNarrowPhaseQuery().CastRay(ray, hit, broad, objects) ? qtrue : qfalse;
}

// Every dynamic prop within radius of centre, pushed away from it, harder the
// nearer; only where the world does not stand between.
void CG_PhysicsBlast(const vec3_t centre, float radius, float strength)
{
    for (size_t i = 0; i < pp_props.size(); i++) {
        const cgProp_t *p = &cg_props[pp_props[i].prop];
        vec3_t          mid, away, impulse;
        float           dist, fall;
        qboolean        blocked;
        int             k;

        for (k = 0; k < 3; k++) {
            mid[k] = (p->absmin[k] + p->absmax[k]) * 0.5f;
        }

        VectorSubtract(mid, centre, away);
        dist = VectorNormalize(away);
        if (dist > radius) {
            continue;
        }

        blocked = CG_PhysicsWorldBlocks(centre, mid);
        if (cg_physics_log->integer > 1) {
            cgi.Printf("  blast: %s %.0f away, %s, %.1f kg\n", p->name, dist, blocked ? "behind the world" : "in the open", CG_PhysicsBodyMass(pp_props[i].id));
        }
        if (blocked) {
            continue;
        }

        // A little upward, as blasts throw things.
        away[2] += 0.3f;
        VectorNormalize(away);

        fall = 1.0f - dist / radius;
        {
            const float mass = CG_PhysicsBodyMass(pp_props[i].id);

            VectorScale(away, strength * fall * mass, impulse);
        }

        CG_PhysicsImpulse(pp_props[i].id, mid, impulse);
    }

    // The furniture in the brushwork.
    for (int i = 0; i < CG_PhysicsFurnitureCount(); i++) {
        JPH::BodyID id;
        vec3_t      mid, away, impulse;
        float       dist, mass;

        if (!CG_PhysicsFurnitureBody(i, &id, mid)) {
            continue;
        }

        VectorSubtract(mid, centre, away);
        dist = VectorNormalize(away);
        if (dist > radius || CG_PhysicsWorldBlocks(centre, mid)) {
            continue;
        }

        away[2] += 0.3f;
        VectorNormalize(away);
        mass = CG_PhysicsBodyMass(id);
        VectorScale(away, strength * (1.0f - dist / radius) * mass, impulse);
        CG_PhysicsImpulse(id, mid, impulse);
    }
}

// How hard a round shoves a prop, in kilogram units a second (a 1 kg prop
// takes it all as speed), and the fastest a round can send one.
#define PP_BULLET_IMPULSE       900.0f
#define PP_BULLET_IMPULSE_LARGE 1400.0f
#define PP_BULLET_MAX_SPEED     500.0f

// A round's path as the engine traced it. The engine still has the clip brushes
// that stand in for props that have not moved, so its trace stops at the
// prop's face; the ray goes a little past to reach the body itself.
void CG_PhysicsNoteBullet(const vec3_t start, const vec3_t end, int large)
{
    JPH::RayCastResult hit;
    vec3_t             dir, far, point, impulse;
    float              len, mass, speed;

    if (!phys_system || pp_props.empty()) {
        return;
    }

    VectorSubtract(end, start, dir);
    len = VectorNormalize(dir);
    if (len < 1.0f) {
        return;
    }
    VectorMA(end, 8.0f, dir, far);

    const JPH::RRayCast ray(JPH::RVec3(PhysToJolt(start)), PhysToJolt(far) - PhysToJolt(start));
    if (!phys_system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, PhysNoKinematicObjects())) {
        return;
    }

    mass = CG_PhysicsBodyMass(hit.mBodyID);
    if (mass <= 0.0f) {
        return;
    }

    VectorMA(start, (len + 8.0f) * hit.mFraction, dir, point);
    speed = Q_min((large ? PP_BULLET_IMPULSE_LARGE : PP_BULLET_IMPULSE) / mass, PP_BULLET_MAX_SPEED);
    VectorScale(dir, speed * mass, impulse);

    if (cg_physics_log->integer > 1) {
        cgi.Printf("physics: round hit a prop at %.0f %.0f %.0f: %.1f kg, %.0f u/s\n", point[0], point[1], point[2], mass, speed);
    }

    CG_PhysicsImpulse(hit.mBodyID, point, impulse);
}

// An explosion, of the same kinds as the ragdolls know: grenade, bazooka,
// heavy shell, tank.
void CG_PhysicsNoteExplosion(const vec3_t pos, int kind)
{
    static const float blasts[4][2] = {
        {220.0f, 520.0f },
        {280.0f, 680.0f },
        {360.0f, 840.0f },
        {440.0f, 1000.0f},
    };

    if (!phys_system || pp_props.empty()) {
        return;
    }

    kind = kind < 0 ? 0 : kind > 3 ? 3 : kind;
    CG_PhysicsBlast(pos, blasts[kind][0], blasts[kind][1]);
}

// Someone reaching into the box round a prop (its stand-ins stop him short of
// it) while going along dir, towards it and not standing on it: it is pushed.
void CG_PhysicsPushPropsInBox(const vec3_t mins, const vec3_t maxs, const vec3_t dir, float speed)
{
    for (size_t i = 0; i < pp_props.size(); i++) {
        const cgProp_t *p = &cg_props[pp_props[i].prop];
        vec3_t          toward;
        int             k;

        for (k = 0; k < 3; k++) {
            if (mins[k] > p->absmax[k] || maxs[k] < p->absmin[k]) {
                break;
            }
        }
        if (k < 3 || mins[2] >= p->absmax[2] - 2.0f) {
            continue;
        }

        for (k = 0; k < 2; k++) {
            toward[k] = (p->absmin[k] + p->absmax[k]) * 0.5f - (mins[k] + maxs[k]) * 0.5f;
        }
        toward[2] = 0.0f;
        if (VectorNormalize(toward) < 0.001f || DotProduct(toward, dir) < 0.2f) {
            continue;
        }

        CG_PhysicsPushBody(pp_props[i].id, dir, speed);
    }
}

// phys_poke [speed]: knocks whatever prop is under the crosshair.
void CG_PhysicsPoke_f(void)
{
    const float speed = cgi.Argc() > 1 ? (float)atof(cgi.Argv(1)) : 200.0f;
    JPH::BodyID id;
    vec3_t      point, dir, impulse;

    if (!phys_system || !CG_PhysicsUnderCrosshair(1024.0f, &id, point, dir)) {
        return;
    }

    {
        const float mass = CG_PhysicsBodyMass(id);
        VectorScale(dir, speed * mass, impulse);
    }

    CG_PhysicsImpulse(id, point, impulse);
}

// phys_blast [radius] [speed]: an explosion where the crosshair meets the world.
void CG_PhysicsBlast_f(void)
{
    const float radius = cgi.Argc() > 1 ? (float)atof(cgi.Argv(1)) : 200.0f;
    const float speed  = cgi.Argc() > 2 ? (float)atof(cgi.Argv(2)) : 600.0f;
    JPH::BodyID id;
    vec3_t      point, dir;

    if (!phys_system || !CG_PhysicsUnderCrosshair(4096.0f, &id, point, dir)) {
        return;
    }

    // Just off the surface, so the blast is not hidden behind it.
    VectorMA(point, -8.0f, dir, point);
    if (cg_physics_log->integer) {
        cgi.Printf("phys_blast: at %.0f %.0f %.0f, radius %.0f\n", point[0], point[1], point[2], radius);
    }
    CG_PhysicsBlast(point, radius, speed);
}

// phys_list: the props that have moved, where they are and how fast.
void CG_PhysicsList_f(void)
{
    int shown = 0;

    if (!phys_system) {
        return;
    }

    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (size_t i = 0; i < pp_props.size(); i++) {
        const physProp_t *pp = &pp_props[i];
        const cgProp_t   *p  = &cg_props[pp->prop];
        vec3_t            vel;

        if (!pp->moving && !bodies.IsActive(pp->id)) {
            continue;
        }

        PhysFromJolt(bodies.GetLinearVelocity(pp->id), vel);
        cgi.Printf(
            "  %s at %.0f %.0f %.0f, %s, %.0f u/s, %.0f kg\n",
            p->name,
            p->origin[0],
            p->origin[1],
            p->origin[2],
            bodies.IsActive(pp->id) ? "awake" : "asleep",
            VectorLength(vel),
            CG_PhysicsBodyMass(pp->id)
        );
        shown++;
    }

    cgi.Printf("phys_list: %d props have moved\n", shown);
}
