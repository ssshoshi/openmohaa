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
// What moves the client's props besides bullets and blasts: the people in the
// world, and the grabber.
//
// The local player and every solid entity in the snapshot (players, AI, the
// server's own physics props) are kinematic boxes in the physics world, moved
// over each frame's steps, so they shove props aside and props stop against
// them. A prop still wrapped in the clip brushes that stand in for it cannot be
// reached that way, since the player stops at the clip, so walking into one
// pushes it as the server pushes its crates; once it has moved the clip goes
// (in single player) and the box takes over.
//
// The grabber (see the ragdolls' +rdgrab) carries props too: a spring at the
// point taken hold of pulls it after the end of the beam, holding its weight up
// to a limit, so light things swing from where they are held and heavy ones
// drag.

#include "cg_physics_local.h"
#include "cg_props.h"

#include <map>

//=============================================================
// People as kinematic boxes
//=============================================================

// The key the local player's box is kept under; entities go by number.
#define PM_LOCAL_PLAYER -1

typedef struct {
    JPH::BodyID id;
    vec3_t      mins, maxs;
    JPH::RVec3  from, to;
    int         seenFrame;
} pmBox_t;

static std::map<int, pmBox_t> pm_boxes;
static int                    pm_frame;

// Someone walking into a prop pushes it along at up to this speed, in units a
// second; over PM_PUSH_LIGHT kilograms proportionally slower, and over
// PM_PUSH_HEAVY not at all. As the server pushes its props (g_physics.cpp).
#define PM_PUSH_SPEED 150.0f
#define PM_PUSH_LIGHT 8.0f
#define PM_PUSH_HEAVY 80.0f

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

static void CG_PhysicsDropBox(pmBox_t *b)
{
    if (phys_system && !b->id.IsInvalid()) {
        JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

        bodies.RemoveBody(b->id);
        bodies.DestroyBody(b->id);
    }
    b->id = JPH::BodyID();
}

static void CG_PhysicsPlaceBox(int key, const vec3_t origin, const vec3_t mins, const vec3_t maxs)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();
    pmBox_t            *b      = &pm_boxes[key];
    vec3_t              centre;

    VectorAdd(mins, maxs, centre);
    VectorMA(origin, 0.5f, centre, centre);

    if (!b->id.IsInvalid() && (!VectorCompare(b->mins, mins) || !VectorCompare(b->maxs, maxs))) {
        JPH::ShapeRefC shape = CG_PhysicsBoxShape(mins, maxs);

        if (shape) {
            bodies.SetShape(b->id, shape, false, JPH::EActivation::DontActivate);
            VectorCopy(mins, b->mins);
            VectorCopy(maxs, b->maxs);
        }
    }

    if (b->id.IsInvalid()) {
        JPH::ShapeRefC shape = CG_PhysicsBoxShape(mins, maxs);

        if (!shape) {
            return;
        }

        JPH::BodyCreationSettings settings(
            shape, JPH::RVec3(PhysToJolt(centre)), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, PhysLayers::KINEMATIC
        );
        // No friction, so standing on a prop does not drag it.
        settings.mFriction    = 0.0f;
        settings.mRestitution = 0.0f;

        b->id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (b->id.IsInvalid()) {
            return;
        }

        VectorCopy(mins, b->mins);
        VectorCopy(maxs, b->maxs);
        b->from = b->to = settings.mPosition;
    }

    b->seenFrame = pm_frame;
    b->from      = b->to;
    b->to        = JPH::RVec3(PhysToJolt(centre));

    // A teleport, or a respawn: there at once, not swept through everything.
    if ((b->to - b->from).Length() > 64.0f * PHYS_UNITS_TO_METRES) {
        bodies.SetPosition(b->id, b->to, JPH::EActivation::DontActivate);
        b->from = b->to;
    }
}

// Once a frame, before the steps: where everybody is now.
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

        CG_PhysicsPlaceBox(PM_LOCAL_PLAYER, ps->origin, mins, maxs);
    }

    // Everybody and everything solid the server sent.
    for (i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        vec3_t               mins, maxs;

        if (es->number == cg.snap->ps.clientNum || !es->solid || es->solid == SOLID_BMODEL) {
            continue;
        }

        IntegerToBoundingBox(es->solid, mins, maxs);
        CG_PhysicsPlaceBox(es->number, cg_entities[es->number].lerpOrigin, mins, maxs);
    }

    // Whoever is gone.
    for (std::map<int, pmBox_t>::iterator it = pm_boxes.begin(); it != pm_boxes.end();) {
        if (it->second.seenFrame != pm_frame) {
            CG_PhysicsDropBox(&it->second);
            it = pm_boxes.erase(it);
        } else {
            ++it;
        }
    }
}

// Each step: the boxes a part of the way along.
void CG_PhysicsMoveMovers(float frac, float dt)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

    for (std::map<int, pmBox_t>::const_iterator it = pm_boxes.begin(); it != pm_boxes.end(); ++it) {
        const pmBox_t *b = &it->second;

        if (b->id.IsInvalid()) {
            continue;
        }

        if (b->from == b->to) {
            if (bodies.GetPosition(b->id) != b->to) {
                bodies.SetPosition(b->id, b->to, JPH::EActivation::DontActivate);
            }
            continue;
        }

        bodies.MoveKinematic(b->id, b->from + (b->to - b->from) * frac, JPH::Quat::sIdentity(), dt);
    }
}

// A frame with no step: the boxes are already where they were going.
void CG_PhysicsMoversHeld(void)
{
    for (std::map<int, pmBox_t>::iterator it = pm_boxes.begin(); it != pm_boxes.end(); ++it) {
        it->second.to = it->second.from;
    }
}

void CG_PhysicsUnloadMovers(void)
{
    for (std::map<int, pmBox_t>::iterator it = pm_boxes.begin(); it != pm_boxes.end(); ++it) {
        CG_PhysicsDropBox(&it->second);
    }
    pm_boxes.clear();
}

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
// second squared, and the most weight it will hold up, in kilograms.
#define PG_SPRING_OMEGA  14.0f
#define PG_MAX_ACCEL     4000.0f
#define PG_MAX_MASS      40.0f

// How much of a held prop's turning is kept each step, so it hangs rather
// than spins.
#define PG_SPIN_KEEP 0.92f

static struct {
    qboolean    held;
    JPH::BodyID id;
    JPH::Vec3   local; // the point held, in the body's own space, metres
    float       dist;
    vec3_t      target;
} pg;

// The nearest dynamic body along a ray, not behind anything static, and how far
// along the ray it is.
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

qboolean CG_PhysicsGrabCandidate(const vec3_t start, const vec3_t dir, float range, float *entry)
{
    JPH::BodyID id;

    return (phys_system && CG_PhysicsRayBody(start, dir, range, &id, entry)) ? qtrue : qfalse;
}

qboolean CG_PhysicsGrabStart(const vec3_t start, const vec3_t dir, float range, float minDist)
{
    JPH::BodyID id;
    float       entry;
    vec3_t      point;

    CG_PhysicsGrabRelease();

    if (!phys_system || !CG_PhysicsRayBody(start, dir, range, &id, &entry)) {
        return qfalse;
    }

    VectorMA(start, entry, dir, point);

    {
        JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), id);

        if (!lock.Succeeded()) {
            return qfalse;
        }
        pg.local = JPH::Vec3(lock.GetBody().GetWorldTransform().Inversed() * JPH::RVec3(PhysToJolt(point)));
    }

    pg.held = qtrue;
    pg.id   = id;
    pg.dist = Q_max(entry, minDist);
    VectorCopy(point, pg.target);
    phys_system->GetBodyInterface().ActivateBody(id);

    if (cg_physics_log->integer) {
        cgi.Printf("physics: grabbed a body %.0f units away\n", entry);
    }
    return qtrue;
}

qboolean CG_PhysicsGrabHeld(void)
{
    if (pg.held && (!phys_system || !phys_system->GetBodyInterface().IsAdded(pg.id))) {
        pg.held = qfalse;
    }

    return pg.held;
}

void CG_PhysicsGrabRelease(void)
{
    pg.held = qfalse;
}

float *CG_PhysicsGrabDistance(void)
{
    return &pg.dist;
}

void CG_PhysicsGrabSetTarget(const vec3_t target)
{
    VectorCopy(target, pg.target);
}

// Where the point held is now.
void CG_PhysicsGrabPoint(vec3_t out)
{
    JPH::BodyLockRead lock(phys_system->GetBodyLockInterface(), pg.id);

    if (!lock.Succeeded()) {
        VectorCopy(pg.target, out);
        return;
    }

    PhysFromJolt(JPH::Vec3(lock.GetBody().GetWorldTransform() * pg.local), out);
}

// Each step: the spring at the point held pulls it after the target.
void CG_PhysicsGrabStep(float dt)
{
    JPH::BodyInterface &bodies = phys_system->GetBodyInterface();
    JPH::Vec3           point, pointVel, accel, gravity;
    float               mass, limit;

    if (!CG_PhysicsGrabHeld()) {
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

// Throws what is held along dir, or knocks the body the ray finds; lighter
// things go faster.
qboolean CG_PhysicsPunt(const vec3_t start, const vec3_t dir, float range, float speed)
{
    JPH::BodyID id;
    vec3_t      point, impulse;
    float       entry, mass;

    if (!phys_system) {
        return qfalse;
    }

    if (CG_PhysicsGrabHeld()) {
        id = pg.id;
        CG_PhysicsGrabPoint(point);
        CG_PhysicsGrabRelease();
    } else if (CG_PhysicsRayBody(start, dir, range, &id, &entry)) {
        VectorMA(start, entry, dir, point);
    } else {
        return qfalse;
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
