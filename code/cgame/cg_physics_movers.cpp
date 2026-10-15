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
// What moves the client's props besides bullets and blasts: whatever the server
// moves, and the grabber.
//
// The local player and every solid entity in the snapshot are kinematic bodies
// in the physics world, moved over each frame's steps: people as boxes, brush
// entities (the server's crates and barrels, doors) as the hulls of their
// brushes. So they shove props aside, and props stop against them and rest on
// them. A prop still wrapped in the clip brushes that stand in for it cannot be
// reached that way, since the player stops at the clip, so walking into one
// pushes it as the server pushes its crates; once it has moved the clip goes
// (in single player) and the box takes over.
//
// The other way, a prop that strikes one of the server's entities tells
// the server, which shoves it (physnudge; single player only, where the server
// takes such commands).
//
// The grabber (see the ragdolls' +rdgrab) carries props too. The client's own
// hang from a spring at the point taken hold of; the server's are carried by
// the server, told what to hold and how far off (physgrab, physdist, physdrop,
// physpunt), with the beam drawn here to where the entity is.

#include "cg_physics_local.h"
#include "cg_props.h"

#include <map>
#include <vector>

//=============================================================
// Whatever the server moves, as kinematic bodies
//=============================================================

// The key the local player's box is kept under; entities go by number.
#define PM_LOCAL_PLAYER -1

// Brush entities larger than this are not followed.
#define PM_MAX_MODEL_SIZE 512.0f

typedef struct {
    JPH::BodyID id;
    int         model; // inline model, or 0 for a box
    vec3_t      mins, maxs;
    JPH::RVec3  from, to;
    JPH::Quat   fromRot, toRot;
    int         seenFrame;
} pmMover_t;

static std::map<int, pmMover_t>      pm_movers;
static int                           pm_frame;
static std::vector<physInlineModel_t> pm_models;
static std::map<int, JPH::ShapeRefC> pm_modelShapes;

// The local player's box and whatever he holds do not collide: holding a prop
// close would otherwise have him forever shoving it away.
static JPH::Ref<JPH::GroupFilterTable> pm_holdFilter;

#define PM_HOLD_GROUP   1
#define PM_HOLD_HOLDER  0
#define PM_HOLD_HELD    1

// Someone walking into a prop pushes it along at up to this speed, in units a
// second; over PM_PUSH_LIGHT kilograms proportionally slower, and over
// PM_PUSH_HEAVY not at all. As the server pushes its props (g_physics.cpp).
#define PM_PUSH_SPEED 150.0f
#define PM_PUSH_LIGHT 8.0f
#define PM_PUSH_HEAVY 80.0f

static JPH::GroupFilterTable *CG_PhysicsHoldFilter(void)
{
    if (!pm_holdFilter) {
        pm_holdFilter = new JPH::GroupFilterTable(2);
        pm_holdFilter->DisableCollision(PM_HOLD_HOLDER, PM_HOLD_HELD);
    }

    return pm_holdFilter;
}

// Whether the server is this process's, in single player, and so takes the
// grabber's and the props' commands.
static qboolean CG_PhysicsServerTakesCommands(void)
{
    return (cgs.localServer && cgs.gametype == GT_SINGLE_PLAYER) ? qtrue : qfalse;
}

static JPH::ShapeRefC CG_PhysicsBoxShape(const vec3_t mins, const vec3_t maxs)
{
    vec3_t half;

    VectorSubtract(maxs, mins, half);
    VectorScale(half, 0.5f * PHYS_UNITS_TO_METRES, half);
    if (half[0] <= 0.0f || half[1] <= 0.0f || half[2] <= 0.0f) {
        return JPH::ShapeRefC();
    }

    const float radius = Q_min(JPH::cDefaultConvexRadius, 0.5f * Q_min(half[0], Q_min(half[1], half[2])));

    JPH::BoxShapeSettings           box(JPH::Vec3(half[0], half[1], half[2]), radius);
    JPH::ShapeSettings::ShapeResult result = box.Create();

    return result.HasError() ? JPH::ShapeRefC() : result.Get();
}

// The hull of a brush model's brushes, in its own space.
static JPH::ShapeRefC CG_PhysicsModelShape(int model)
{
    std::map<int, JPH::ShapeRefC>::const_iterator it = pm_modelShapes.find(model);
    JPH::Array<JPH::Vec3>                         points;
    JPH::ShapeRefC                                shape;

    if (it != pm_modelShapes.end()) {
        return it->second;
    }

    if (model > 0 && model < (int)pm_models.size() && !pm_models[model].corners.empty()) {
        const physInlineModel_t *m = &pm_models[model];
        int                      k;

        for (k = 0; k < 3; k++) {
            if (m->maxs[k] - m->mins[k] > PM_MAX_MODEL_SIZE) {
                break;
            }
        }

        if (k == 3) {
            for (size_t i = 0; i + 2 < m->corners.size(); i += 3) {
                vec3_t c = {m->corners[i], m->corners[i + 1], m->corners[i + 2]};
                points.push_back(PhysToJolt(c));
            }

            JPH::ConvexHullShapeSettings    hull(points, 0.005f);
            JPH::ShapeSettings::ShapeResult result = hull.Create();

            if (!result.HasError()) {
                shape = result.Get();
            }
        }
    }

    pm_modelShapes[model] = shape;
    return shape;
}

void CG_PhysicsLoadMoverModels(const void *bsp, long len)
{
    pm_models.clear();
    pm_modelShapes.clear();
    Phys_ReadInlineModels(bsp, len, &pm_models);
}

static void CG_PhysicsDropMover(pmMover_t *m)
{
    if (phys_system && !m->id.IsInvalid()) {
        JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

        bodies.RemoveBody(m->id);
        bodies.DestroyBody(m->id);
    }
    m->id = JPH::BodyID();
}

// Where a mover is to be this frame: a box round an origin, or a brush model
// placed at an origin and angles.
static void CG_PhysicsPlaceMover(int key, int model, const vec3_t origin, const vec3_t angles, const vec3_t mins, const vec3_t maxs)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();
    pmMover_t          *m      = &pm_movers[key];
    vec3_t              centre, axis[3];
    JPH::Quat           rot;

    // Something else under the same number now.
    if (!m->id.IsInvalid() && m->model != model) {
        CG_PhysicsDropMover(m);
    }

    if (model) {
        VectorCopy(origin, centre);
        AnglesToAxis(angles, axis);
        rot = Phys_QuatFromAxis(axis);
    } else {
        VectorAdd(mins, maxs, centre);
        VectorMA(origin, 0.5f, centre, centre);
        rot = JPH::Quat::sIdentity();

        // Crouched, stood, or someone else.
        if (!m->id.IsInvalid() && (!VectorCompare(m->mins, mins) || !VectorCompare(m->maxs, maxs))) {
            JPH::ShapeRefC shape = CG_PhysicsBoxShape(mins, maxs);

            if (shape) {
                bodies.SetShape(m->id, shape, false, JPH::EActivation::DontActivate);
                VectorCopy(mins, m->mins);
                VectorCopy(maxs, m->maxs);
            }
        }
    }

    if (m->id.IsInvalid()) {
        JPH::ShapeRefC shape = model ? CG_PhysicsModelShape(model) : CG_PhysicsBoxShape(mins, maxs);

        if (!shape) {
            m->seenFrame = pm_frame;
            m->model     = model;
            return;
        }

        JPH::BodyCreationSettings settings(
            shape, JPH::RVec3(PhysToJolt(centre)), rot, JPH::EMotionType::Kinematic, model ? PhysLayers::KINEMATIC : PhysLayers::PEOPLE
        );
        // No friction, so standing on a prop does not drag it.
        settings.mFriction    = model ? 0.6f : 0.0f;
        settings.mRestitution = 0.0f;
        if (key >= 0) {
            settings.mUserData = PHYS_USERDATA_ENTITY_BASE + key;
        }
        if (key == PM_LOCAL_PLAYER) {
            settings.mCollisionGroup = JPH::CollisionGroup(CG_PhysicsHoldFilter(), PM_HOLD_GROUP, PM_HOLD_HOLDER);
        }

        m->id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (m->id.IsInvalid()) {
            return;
        }

        m->model = model;
        VectorCopy(mins, m->mins);
        VectorCopy(maxs, m->maxs);
        m->from    = m->to    = settings.mPosition;
        m->fromRot = m->toRot = rot;
    }

    m->seenFrame = pm_frame;
    m->from      = m->to;
    m->fromRot   = m->toRot;
    m->to        = JPH::RVec3(PhysToJolt(centre));
    m->toRot     = rot;

    // A teleport or a respawn: there at once, not swept through everything.
    if ((m->to - m->from).Length() > 64.0f * PHYS_UNITS_TO_METRES) {
        bodies.SetPositionAndRotation(m->id, m->to, m->toRot, JPH::EActivation::DontActivate);
        m->from    = m->to;
        m->fromRot = m->toRot;
    }
}

// Once a frame, before the steps: where everybody and everything is now.
void CG_PhysicsFollowMovers(void)
{
    const playerState_t *ps = &cg.predicted_player_state;
    int                  i;

    if (!phys_system || !cg.snap) {
        return;
    }

    pm_frame++;

    // The local player, as pmove sizes him (bg_pmove.cpp PM_CheckDuck).
    if (ps->pm_type == PM_NORMAL) {
        vec3_t mins = {-15.0f, -15.0f, 0.0f}, maxs = {15.0f, 15.0f, 94.0f};

        if (ps->pm_flags & PMF_VIEW_PRONE) {
            maxs[2] = 20.0f;
        } else if (ps->pm_flags & PMF_DUCKED) {
            maxs[2] = 54.0f;
        }

        CG_PhysicsPlaceMover(PM_LOCAL_PLAYER, 0, ps->origin, vec3_origin, mins, maxs);
    }

    // Everybody and everything solid the server sent.
    for (i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es   = &cg.snap->entities[i];
        const centity_t     *cent = &cg_entities[es->number];
        vec3_t               mins, maxs;

        // Not the dead: a corpse is a ragdoll, and one left solid by the
        // server would be a box its own body lies on.
        if (es->number == cg.snap->ps.clientNum || !es->solid || (es->eFlags & EF_DEAD)) {
            continue;
        }

        if (es->solid == SOLID_BMODEL) {
            if (es->modelindex <= 0) {
                continue;
            }
            CG_PhysicsPlaceMover(es->number, es->modelindex, cent->lerpOrigin, cent->lerpAngles, vec3_origin, vec3_origin);
        } else {
            IntegerToBoundingBox(es->solid, mins, maxs);
            CG_PhysicsPlaceMover(es->number, 0, cent->lerpOrigin, vec3_origin, mins, maxs);
        }
    }

    // Whatever is gone.
    for (std::map<int, pmMover_t>::iterator it = pm_movers.begin(); it != pm_movers.end();) {
        if (it->second.seenFrame != pm_frame) {
            CG_PhysicsDropMover(&it->second);
            it = pm_movers.erase(it);
        } else {
            ++it;
        }
    }
}

// Each step: the movers a part of the way along.
void CG_PhysicsMoveMovers(float frac, float dt)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (std::map<int, pmMover_t>::const_iterator it = pm_movers.begin(); it != pm_movers.end(); ++it) {
        const pmMover_t *m = &it->second;

        if (m->id.IsInvalid()) {
            continue;
        }

        if (m->from == m->to && m->fromRot == m->toRot) {
            if (bodies.GetPosition(m->id) != m->to) {
                bodies.SetPositionAndRotation(m->id, m->to, m->toRot, JPH::EActivation::DontActivate);
            }
            continue;
        }

        bodies.MoveKinematic(m->id, m->from + (m->to - m->from) * frac, m->fromRot.SLERP(m->toRot, frac).Normalized(), dt);
    }
}

// A frame with no step: the movers are already where they were going.
void CG_PhysicsMoversHeld(void)
{
    for (std::map<int, pmMover_t>::iterator it = pm_movers.begin(); it != pm_movers.end(); ++it) {
        it->second.to    = it->second.from;
        it->second.toRot = it->second.fromRot;
    }
}

void CG_PhysicsUnloadMovers(void)
{
    for (std::map<int, pmMover_t>::iterator it = pm_movers.begin(); it != pm_movers.end(); ++it) {
        CG_PhysicsDropMover(&it->second);
    }
    pm_movers.clear();
}

//=============================================================
// Props striking the server's brush entities
//=============================================================

// A prop has to be closing on one this fast, in metres a second, to shove it.
#define PM_NUDGE_MIN_SPEED 0.5f

// No more often than this for any one entity, in milliseconds.
#define PM_NUDGE_INTERVAL 100

typedef struct {
    int        entnum;
    JPH::RVec3 point;
    JPH::Vec3  impulse; // kilograms times metres a second
} pmNudge_t;

static std::vector<pmNudge_t> pm_nudges;
static std::map<int, int>     pm_nudgeTimes;

class CG_PhysicsContacts final : public JPH::ContactListener
{
public:
    void OnContactAdded(
        const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings
    ) override
    {
        Note(a, b, manifold);
        CG_JoltRagdollContact(a, b, manifold);
        CG_PhysicsImpactContact(a, b, manifold);
    }

    void OnContactPersisted(
        const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings
    ) override
    {
        Note(a, b, manifold);
        CG_JoltRagdollContact(a, b, manifold);
        CG_PhysicsImpactContact(a, b, manifold);
    }

private:
    static bool IsEntity(const JPH::Body& body)
    {
        const JPH::uint64 data = body.GetUserData();

        return body.IsKinematic() && data >= PHYS_USERDATA_ENTITY_BASE && data < PHYS_USERDATA_ENTITY_BASE + MAX_GENTITIES;
    }

    static void Note(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold)
    {
        const JPH::Body *prop, *ent;
        JPH::Vec3        normal; // from the prop to the entity

        if (a.IsDynamic() && IsEntity(b)) {
            prop   = &a;
            ent    = &b;
            normal = manifold.mWorldSpaceNormal;
        } else if (b.IsDynamic() && IsEntity(a)) {
            prop   = &b;
            ent    = &a;
            normal = -manifold.mWorldSpaceNormal;
        } else {
            return;
        }

        const JPH::RVec3 point   = manifold.GetWorldSpaceContactPointOn1(0);
        const float      closing = (prop->GetPointVelocity(point) - ent->GetPointVelocity(point)).Dot(normal);

        if (closing < PM_NUDGE_MIN_SPEED) {
            return;
        }

        pmNudge_t n;

        n.entnum  = (int)(ent->GetUserData() - PHYS_USERDATA_ENTITY_BASE);
        n.point   = point;
        n.impulse = normal * (closing * 1.2f / Q_max(1e-4f, prop->GetMotionProperties()->GetInverseMass()));
        pm_nudges.push_back(n);
    }
};

static CG_PhysicsContacts pm_contacts;

void CG_PhysicsListenForContacts(void)
{
    if (phys_system) {
        phys_system->SetContactListener(&pm_contacts);
    }
}

// After the steps: what struck the server's entities, told to the server.
void CG_PhysicsSendNudges(void)
{
    std::map<int, pmNudge_t> sum;

    if (pm_nudges.empty()) {
        return;
    }

    if (!CG_PhysicsServerTakesCommands()) {
        pm_nudges.clear();
        return;
    }

    for (size_t i = 0; i < pm_nudges.size(); i++) {
        std::map<int, pmNudge_t>::iterator it = sum.find(pm_nudges[i].entnum);

        if (it == sum.end()) {
            sum[pm_nudges[i].entnum] = pm_nudges[i];
        } else {
            it->second.impulse += pm_nudges[i].impulse;
        }
    }
    pm_nudges.clear();

    for (std::map<int, pmNudge_t>::const_iterator it = sum.begin(); it != sum.end(); ++it) {
        int   *last = &pm_nudgeTimes[it->first];
        vec3_t point, impulse;

        if (cg.time - *last < PM_NUDGE_INTERVAL && cg.time >= *last) {
            continue;
        }
        *last = cg.time;

        PhysFromJolt(JPH::Vec3(it->second.point), point);
        PhysFromJolt(it->second.impulse, impulse);
        cgi.SendClientCommand(va(
            "physnudge %d %.0f %.0f %.0f %.0f %.0f %.0f", it->first, point[0], point[1], point[2], impulse[0], impulse[1], impulse[2]
        ));
    }
}

//=============================================================
// Walking into props
//=============================================================

// A body up to speed along dir, the less the heavier it is; never slowed.
static void CG_PhysicsShove(JPH::BodyID id, const vec3_t dir, float speed)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();
    float               mass, target, along;

    {
        JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), id);

        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
            return;
        }
        mass = 1.0f / Q_max(1e-4f, lock.GetBody().GetMotionProperties()->GetInverseMass());
    }

    if (mass >= PM_PUSH_HEAVY) {
        return;
    }

    target = Q_min(speed, PM_PUSH_SPEED) * Q_min(1.0f, PM_PUSH_LIGHT / mass);
    along  = bodies.GetLinearVelocity(id).Dot(PhysToJolt(dir)) * PHYS_METRES_TO_UNITS;
    if (along >= target) {
        return;
    }

    bodies.AddImpulse(id, PhysToJolt(dir) * ((target - along) * mass));
    bodies.ActivateBody(id);
}

// Once a frame: the local player walking into a prop that still stands inside
// its clip brushes, which stop him short of it, pushes it the way he steers.
void CG_PhysicsPlayerPushes(void)
{
    const playerState_t *ps = &cg.predicted_player_state;
    usercmd_t            cmd;
    vec3_t               fwd, right, wish, angles, mins, maxs;
    float                amount;

    if (!phys_system || !cg.snap || ps->pm_type != PM_NORMAL) {
        return;
    }

    if (!cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &cmd)) {
        return;
    }

    VectorSet(angles, 0.0f, ps->viewangles[YAW], 0.0f);
    AngleVectors(angles, fwd, right, NULL);
    VectorScale(fwd, cmd.forwardmove, wish);
    VectorMA(wish, cmd.rightmove, right, wish);
    wish[2] = 0.0f;
    amount  = VectorNormalize(wish);
    if (amount < 1.0f) {
        return;
    }

    // Reaching a little past his box, to the stand-ins round the prop.
    VectorSet(mins, ps->origin[0] - 19.0f, ps->origin[1] - 19.0f, ps->origin[2]);
    VectorSet(maxs, ps->origin[0] + 19.0f, ps->origin[1] + 19.0f, ps->origin[2] + 94.0f);

    CG_PhysicsPushPropsInBox(mins, maxs, wish, PM_PUSH_SPEED * Q_min(1.0f, amount / 127.0f));
}

void CG_PhysicsPushBody(JPH::BodyID id, const vec3_t dir, float speed)
{
    CG_PhysicsShove(id, dir, speed);
}

//=============================================================
// The grabber
//=============================================================

// The spring that carries a held prop: its natural frequency in radians a
// second (critically damped), the most it accelerates the prop in units a
// second squared, and the most weight it will hold up, in kilograms. The
// server carries its own props with the same (g_physics.cpp).
#define PG_SPRING_OMEGA  14.0f
#define PG_MAX_ACCEL     4000.0f
#define PG_MAX_MASS      40.0f

// How much of a held prop's turning is kept each step, so it hangs rather
// than spins.
#define PG_SPIN_KEEP 0.92f

static struct {
    qboolean    held;
    qboolean    server; // one of the server's entities, which the server carries
    JPH::BodyID id;
    JPH::Vec3   local;       // the point held, in the body's own space, metres
    int         entnum;      // the server's entity
    vec3_t      entityLocal; // the point held, in the entity's own space
    float       dist;
    vec3_t      target;
    // the body's own collision group, given back when it is let go: the pieces
    // of a ragdoll chain (a part cut off a body) are kept from colliding with
    // each other by theirs
    JPH::CollisionGroup group;
} pg;

// The nearest of the client's dynamic bodies along a ray, not behind anything
// static, and how far along the ray it is.
static qboolean CG_PhysicsRayBody(const vec3_t start, const vec3_t dir, float range, JPH::BodyID *id, float *entry)
{
    vec3_t             end;
    JPH::RayCastResult hit;

    VectorMA(start, range, dir, end);

    const JPH::RRayCast ray(JPH::RVec3(PhysToJolt(start)), PhysToJolt(end) - PhysToJolt(start));
    if (!phys_system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, PhysNoKinematicObjects())) {
        return qfalse;
    }

    if (phys_system->GetBodyInterface().GetMotionType(hit.mBodyID) != JPH::EMotionType::Dynamic) {
        return qfalse;
    }

    *id    = hit.mBodyID;
    *entry = range * hit.mFraction;
    return qtrue;
}

// The nearest of the server's solid entities along a ray, when the server takes
// the grabber's commands. It refuses anything that is not one of its physics
// props (physgrab_denied).
static qboolean CG_PhysicsRayEntity(const vec3_t start, const vec3_t dir, float range, int *entnum, float *entry)
{
    trace_t tr;
    vec3_t  end;
    float   best;
    int     bestEnt = -1;

    if (!CG_PhysicsServerTakesCommands() || !cg.snap) {
        return qfalse;
    }

    VectorMA(start, range, dir, end);
    CG_Trace(&tr, start, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, MASK_SHOT, qfalse, qtrue, "physics grab");
    best = range * tr.fraction;

    if (tr.fraction < 1.0f && tr.entityNum >= 0 && tr.entityNum < ENTITYNUM_WORLD) {
        const centity_t *cent = &cg_entities[tr.entityNum];

        if (cent->currentValid && cent->currentState.solid) {
            bestEnt = tr.entityNum;
        }
    }

    // Weapons and items lying about, which are triggers and so not in the
    // trace: their models' boxes, nearer than what the trace struck.
    for (int i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        vec3_t               origin, axis[3], mins, maxs;
        float                at;

        if (es->eType != ET_ITEM || es->parent != ENTITYNUM_NONE || !CG_PhysicsEntityBox(es->number, origin, axis, mins, maxs)) {
            continue;
        }
        if (CG_PhysicsRayHitsBox(start, dir, range, origin, axis, mins, maxs, &at) && at < best) {
            best    = at;
            bestEnt = es->number;
        }
    }

    if (bestEnt < 0) {
        return qfalse;
    }

    *entnum = bestEnt;
    *entry  = best;
    return qtrue;
}

qboolean CG_PhysicsGrabCandidate(const vec3_t start, const vec3_t dir, float range, float *entry)
{
    JPH::BodyID id;
    int         entnum;
    float       bodyEntry, entityEntry;
    qboolean    body, entity;

    if (!phys_system) {
        return qfalse;
    }

    body   = CG_PhysicsRayBody(start, dir, range, &id, &bodyEntry);
    entity = CG_PhysicsRayEntity(start, dir, range, &entnum, &entityEntry);

    if (!body && !entity) {
        return qfalse;
    }

    *entry = (body && (!entity || bodyEntry < entityEntry)) ? bodyEntry : entityEntry;
    return qtrue;
}

static void CG_PhysicsSetHeldGroup(JPH::BodyID id, qboolean held)
{
    JPH::BodyLockWrite lock(phys_system->GetBodyLockInterface(), id);

    if (!lock.Succeeded()) {
        return;
    }
    if (held) {
        pg.group = lock.GetBody().GetCollisionGroup();
        lock.GetBody().SetCollisionGroup(JPH::CollisionGroup(CG_PhysicsHoldFilter(), PM_HOLD_GROUP, PM_HOLD_HELD));
    } else {
        lock.GetBody().SetCollisionGroup(pg.group);
    }
}

qboolean CG_PhysicsGrabStart(const vec3_t start, const vec3_t dir, float range, float minDist)
{
    JPH::BodyID id;
    int         entnum;
    float       bodyEntry, entityEntry;
    qboolean    body, entity;
    vec3_t      point;

    CG_PhysicsGrabRelease();

    if (!phys_system) {
        return qfalse;
    }

    body   = CG_PhysicsRayBody(start, dir, range, &id, &bodyEntry);
    entity = CG_PhysicsRayEntity(start, dir, range, &entnum, &entityEntry);

    if (entity && (!body || entityEntry < bodyEntry)) {
        const centity_t *cent = &cg_entities[entnum];
        vec3_t           axis[3], rel;

        VectorMA(start, entityEntry, dir, point);
        AnglesToAxis(cent->lerpAngles, axis);
        VectorSubtract(point, cent->lerpOrigin, rel);
        pg.entityLocal[0] = DotProduct(rel, axis[0]);
        pg.entityLocal[1] = DotProduct(rel, axis[1]);
        pg.entityLocal[2] = DotProduct(rel, axis[2]);

        pg.held   = qtrue;
        pg.server = qtrue;
        pg.entnum = entnum;
        pg.dist   = Q_max(entityEntry, minDist);
        VectorCopy(point, pg.target);

        cgi.SendClientCommand(va("physgrab %d %.1f %.1f %.1f %.0f", entnum, point[0], point[1], point[2], pg.dist));
        if (cg_physics_log->integer) {
            cgi.Printf("physics: asked the server to carry entity %d, %.0f units away\n", entnum, entityEntry);
        }
        return qtrue;
    }

    if (!body) {
        return qfalse;
    }

    VectorMA(start, bodyEntry, dir, point);
    return CG_PhysicsGrabBodyAt(id, point, Q_max(bodyEntry, minDist));
}

// Takes hold of a body at a point of it, to be carried dist along the view.
qboolean CG_PhysicsGrabBodyAt(JPH::BodyID id, const vec3_t point, float dist)
{
    CG_PhysicsGrabRelease();

    if (!phys_system) {
        return qfalse;
    }

    {
        JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), id);

        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
            return qfalse;
        }
        pg.local = JPH::Vec3(lock.GetBody().GetWorldTransform().Inversed() * JPH::RVec3(PhysToJolt(point)));
    }

    pg.held   = qtrue;
    pg.server = qfalse;
    pg.id     = id;
    pg.dist   = dist;
    VectorCopy(point, pg.target);
    CG_PhysicsSetHeldGroup(id, qtrue);
    phys_system->GetBodyInterface().ActivateBody(id);

    if (cg_physics_log->integer) {
        cgi.Printf("physics: grabbed a body %.0f units away\n", dist);
    }
    return qtrue;
}

qboolean CG_PhysicsGrabHeld(void)
{
    if (!pg.held) {
        return qfalse;
    }

    if (pg.server) {
        // Gone from the snapshot (broken, or out of sight), or picked up. An
        // item lying about is never solid.
        const entityState_t *es = &cg_entities[pg.entnum].currentState;

        if (!cg_entities[pg.entnum].currentValid || (!es->solid && es->eType != ET_ITEM) || es->parent != ENTITYNUM_NONE) {
            CG_PhysicsGrabRelease();
        }
    } else if (!phys_system || !phys_system->GetBodyInterface().IsAdded(pg.id)) {
        pg.held = qfalse;
    }

    return pg.held;
}

void CG_PhysicsGrabRelease(void)
{
    if (!pg.held) {
        return;
    }

    if (pg.server) {
        cgi.SendClientCommand("physdrop");
    } else if (phys_system && phys_system->GetBodyInterface().IsAdded(pg.id)) {
        CG_PhysicsSetHeldGroup(pg.id, qfalse);
    }

    pg.held = qfalse;
}

// The server would not carry what was taken hold of: not one of its props.
void CG_PhysicsGrabDenied(void)
{
    if (pg.held && pg.server) {
        pg.held = qfalse;
    }
}

float CG_PhysicsGrabDistance(void)
{
    return pg.dist;
}

void CG_PhysicsGrabSetDistance(float dist)
{
    if (!pg.held || dist == pg.dist) {
        return;
    }

    pg.dist = dist;
    if (pg.server) {
        cgi.SendClientCommand(va("physdist %.0f", dist));
    }
}

void CG_PhysicsGrabSetTarget(const vec3_t target)
{
    VectorCopy(target, pg.target);
}

// Where the point held is now.
void CG_PhysicsGrabPoint(vec3_t out)
{
    if (pg.server) {
        const centity_t *cent = &cg_entities[pg.entnum];
        vec3_t           axis[3];

        AnglesToAxis(cent->lerpAngles, axis);
        VectorCopy(cent->lerpOrigin, out);
        VectorMA(out, pg.entityLocal[0], axis[0], out);
        VectorMA(out, pg.entityLocal[1], axis[1], out);
        VectorMA(out, pg.entityLocal[2], axis[2], out);
        return;
    }

    {
        JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), pg.id);

        if (!lock.Succeeded()) {
            VectorCopy(pg.target, out);
            return;
        }

        PhysFromJolt(JPH::Vec3(lock.GetBody().GetWorldTransform() * pg.local), out);
    }
}

// Each step: the spring at the point held pulls it after the target.
void CG_PhysicsGrabStep(float dt)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();
    JPH::Vec3           point, pointVel, accel, gravity;
    float               mass, limit;

    if (!CG_PhysicsGrabHeld() || pg.server) {
        return;
    }

    {
        JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), pg.id);

        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
            pg.held = qfalse;
            return;
        }

        const JPH::Body& body = lock.GetBody();

        point    = JPH::Vec3(body.GetWorldTransform() * pg.local);
        pointVel = body.GetPointVelocity(JPH::RVec3(point));
        mass     = 1.0f / Q_max(1e-4f, body.GetMotionProperties()->GetInverseMass());
    }

    // Critically damped towards the target, with its weight held up.
    accel   = (PhysToJolt(pg.target) - point) * (PG_SPRING_OMEGA * PG_SPRING_OMEGA) - pointVel * (2.0f * PG_SPRING_OMEGA);
    gravity = -phys_system->GetGravity();

    limit = PG_MAX_ACCEL * PHYS_UNITS_TO_METRES;
    if (accel.Length() > limit) {
        accel = accel.Normalized() * limit;
    }

    // Past what it can hold, it pulls with what it has: heavy things drag.
    {
        JPH::Vec3   force = (accel + gravity) * mass;
        const float most  = PG_MAX_MASS * (limit + gravity.Length());

        if (force.Length() > most) {
            force = force.Normalized() * most;
        }

        bodies.AddImpulse(pg.id, force * dt, JPH::RVec3(point));
    }

    bodies.SetAngularVelocity(pg.id, bodies.GetAngularVelocity(pg.id) * PG_SPIN_KEEP);
    bodies.ActivateBody(pg.id);
}

// Throws what is held along dir, or knocks what the ray finds; lighter things
// go faster.
qboolean CG_PhysicsPunt(const vec3_t start, const vec3_t dir, float range, float speed)
{
    JPH::BodyID id;
    vec3_t      point, impulse;
    float       entry, entityEntry, mass;
    int         entnum;
    qboolean    body, entity;

    if (!phys_system) {
        return qfalse;
    }

    if (CG_PhysicsGrabHeld()) {
        CG_PhysicsGrabPoint(point);

        if (pg.server) {
            // The server lets go of it as it throws it.
            cgi.SendClientCommand(va("physpunt %d %.1f %.1f %.1f %.0f", pg.entnum, point[0], point[1], point[2], speed));
            pg.held = qfalse;
            return qtrue;
        }

        id = pg.id;
        CG_PhysicsGrabRelease();
    } else {
        body   = CG_PhysicsRayBody(start, dir, range, &id, &entry);
        entity = CG_PhysicsRayEntity(start, dir, range, &entnum, &entityEntry);

        if (entity && (!body || entityEntry < entry)) {
            VectorMA(start, entityEntry, dir, point);
            cgi.SendClientCommand(va("physpunt %d %.1f %.1f %.1f %.0f", entnum, point[0], point[1], point[2], speed));
            return qtrue;
        }

        if (!body) {
            return qfalse;
        }
        VectorMA(start, entry, dir, point);
    }

    {
        JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), id);

        if (!lock.Succeeded()) {
            return qfalse;
        }
        mass = 1.0f / Q_max(1e-4f, lock.GetBody().GetMotionProperties()->GetInverseMass());
    }

    VectorScale(dir, speed * Q_min(1.0f, 20.0f / mass) * mass, impulse);
    phys_system->GetBodyInterface().AddImpulse(id, PhysToJolt(impulse), JPH::RVec3(PhysToJolt(point)));
    phys_system->GetBodyInterface().ActivateBody(id);
    return qtrue;
}
