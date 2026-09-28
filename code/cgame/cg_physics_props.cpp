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
} physMaterial_t;

static const physMaterial_t phys_materials[] = {
    {"glass",
     {"bottle", "jug", "pitcher", "glass", "jar", "vase", NULL},
     1.5f, 0.4f, 0.3f},
    {"metal",
     {"bucket", "helmet", "can", "shell", "pot", "stove", "radio", "ammo", "valve", "switch", "microphone", "filter",
      "canteen", "mg42", "radar", "lantern", NULL},
     3.0f, 0.5f, 0.2f},
    {"paper",
     {"paper", "card", "book", "map", "sack", "bag", "cloth", "blanket", "pillow", NULL},
     0.8f, 0.8f, 0.05f},
    {"stone",
     {"sandbag", "brick", "stone", "rock", "concrete", NULL},
     25.0f, 0.8f, 0.05f},
    {"wood",
     {"chair", "stool", "crate", "table", "trunk", "box", "bench", "lid", "cot", "basket", "desk", "shelf", "barrel",
      NULL},
     4.0f, 0.6f, 0.15f},
};

static const physMaterial_t phys_defaultMaterial = {"default", {NULL}, 3.0f, 0.5f, 0.15f};

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
} physProp_t;

static std::vector<physProp_t> pp_props;
static std::vector<JPH::BodyID> pp_static;
static char                    pp_map[MAX_QPATH];
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
        const qboolean        moves    = (p->dynamic && canMove && cg_physics_props->integer
                                  && (!p->clipped || CG_PhysicsClippedPropsMove()))
                                         ? qtrue
                                         : qfalse;
        JPH::ShapeRefC        shape    = CG_PhysicsPropShape(p);
        float                 biggest  = 0.0f;
        int                   k;

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
            settings.mMassPropertiesOverride.mMass = Q_clamp_float(area * material->arealDensity, 0.2f, 200.0f);
            settings.mLinearDamping  = 0.05f;
            settings.mAngularDamping = 0.1f;
            settings.mMotionQuality  = JPH::EMotionQuality::LinearCast;
        }

        const JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (id.IsInvalid()) {
            failed++;
            continue;
        }

        if (moves) {
            physProp_t pp;

            pp.id      = id;
            pp.prop    = i;
            pp.curPos  = pp.prevPos = settings.mPosition;
            pp.curRot  = pp.prevRot = settings.mRotation;
            pp.moving  = qfalse;
            pp.everMoved = qfalse;
            p->body    = (int)pp_props.size();
            pp_props.push_back(pp);
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
            canMove ? "" : " (this engine cannot move static models, so none move)"
        );
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
                    cgi.CM_DisableBrush(p->standIns[k]);
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

        cgi.R_SetStaticModelTransform(cg_props[pp->prop].staticIndex, origin, axis);

        // Settled: drawn where it came to rest, and left alone after.
        if (pp->prevPos == pp->curPos && pp->prevRot == pp->curRot
            && !phys_system->GetBodyInterfaceNoLock().IsActive(pp->id)) {
            pp->moving = qfalse;
        }
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
    if (!phys_system->GetNarrowPhaseQuery().CastRay(ray, hit)) {
        return qfalse;
    }

    *id = hit.mBodyID;
    VectorMA(cg.refdef.vieworg, range * hit.mFraction, dir, point);
    return qtrue;
}

// A body's mass in kilograms, or 0 if it does not move.
static float CG_PhysicsBodyMass(JPH::BodyID id)
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
    if (!phys_system->GetNarrowPhaseQuery().CastRay(ray, hit)) {
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
