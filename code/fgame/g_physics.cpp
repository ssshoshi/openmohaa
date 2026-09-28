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
// Server-side rigid body physics for the map's entity props.
//
// Crates, barrels, cans and the small interactive objects are the server's own
// entities: they break, they stop bullets, players bump into them. So they are
// simulated here rather than on the client, and every client, this one's or an
// unmodified one, sees them move because the entities themselves move.
//
// The world is built from the map's BSP by the same code the client uses
// (code/physics/). Each prop is a dynamic body; while it is awake its pose is
// written back to the entity every frame. A prop starts asleep, so nothing on
// the map shifts as it loads.

// Jolt, through the physics core, before any of the game's headers.
#include "../physics/phys_world.h"
#include "../physics/phys_furniture.h"

#include "g_local.h"
#include "entity.h"
#include "level.h"
#include "gamecvars.h"
#include "g_physics.h"
#include "sentient.h"
#include "barrels.h"
#include "object.h"

#include <set>
#include <vector>

static cvar_t *g_physics;
static cvar_t *g_physics_log;
static cvar_t *g_physics_hitscale;

#define GPHYS_MAX_BODIES 8192
#define GPHYS_STEP_HZ    60
#define GPHYS_MAX_STEPS  8

// A hit shoves a prop by this much for each point of damage, in kilograms times
// units a second, and never faster than GPHYS_MAX_HIT_SPEED: a rifle round
// sends a small crate skidding and a can flying.
#define GPHYS_DAMAGE_IMPULSE 12.0f
#define GPHYS_MAX_HIT_SPEED  500.0f

static physWorld_t                    gphys_world;
static std::vector<JPH::BodyID>       gphys_worldBodies;
static std::vector<physInlineModel_t> gphys_models;
static float                          gphys_accum;
static qboolean                       gphys_ready;

typedef struct {
    JPH::BodyID id;
    qboolean    owned;
} gphysEntity_t;

static gphysEntity_t gphys_entities[MAX_GENTITIES];

// The furniture in the brushwork (see code/physics/phys_furniture.cpp). The
// client makes it move; here each piece is a fixed body of its own, so that
// when the client takes its brushes out of the collision model the server shares
// with it (single player), the body goes too and what stood on it falls.
typedef struct {
    JPH::BodyID id;
    vec3_t      probe; // inside one of its brushes
    int         nextCheck;
} gphysFurniture_t;

static std::vector<gphysFurniture_t> gphys_furniture;

// Players and AI, as kinematic boxes that follow them. Props are pushed out of
// the way of someone moving into them rather than through, and a prop knocked
// towards someone stops against him instead of sealing him inside it.
typedef struct {
    Entity     *ent;
    JPH::BodyID id;
    vec3_t      size;
    JPH::RVec3  from, to; // where the box goes over this frame's steps
    qboolean    seen;
} gphysKinematic_t;

static gphysKinematic_t gphys_kinematic[MAX_GENTITIES];

// Someone walking into a prop pushes it along at up to this speed, in units a
// second. A prop over GPHYS_PUSH_LIGHT kilograms goes proportionally slower, and
// one over GPHYS_PUSH_HEAVY does not go at all.
#define GPHYS_PUSH_SPEED 150.0f
#define GPHYS_PUSH_LIGHT 8.0f
#define GPHYS_PUSH_HEAVY 80.0f
static std::set<int>                 gphys_furnitureBrushes;

static qboolean G_PhysicsSkipFurniture(void *ctx, int brushNum, const char *shader, int contents, const vec3_t mins, const vec3_t maxs)
{
    return gphys_furnitureBrushes.count(brushNum) ? qtrue : qfalse;
}

//=============================================================
// Materials and mass
//=============================================================

// As on the client (cg_physics_props.cpp): a prop's mass goes with the area of
// its box, since props are shells and frames, so a big crate is heavier than a
// small one without the volume making it a ton.
typedef struct {
    const char *words[8];
    float       arealDensity; // kg/m2
    float       friction;
    float       restitution;
} gphysMaterial_t;

static const gphysMaterial_t gphys_metal  = {{"barrel", "can", "bucket", "helmet", NULL}, 3.0f, 0.5f, 0.2f};
static const gphysMaterial_t gphys_paper  = {{"magazine", "paper", "book", "map", NULL}, 0.8f, 0.8f, 0.05f};
static const gphysMaterial_t gphys_wood   = {{"crate", NULL}, 4.0f, 0.6f, 0.15f};

static const gphysMaterial_t *G_PhysicsMaterial(Entity *ent)
{
    if (ent->IsSubclassOfCrateObject()) {
        return &gphys_wood;
    }

    if (strstr(ent->getClassID(), "barrel") || strstr(ent->getClassname(), "barrel")) {
        return &gphys_metal;
    }

    if (strstr(ent->getClassname(), "magazine") || strstr(ent->model.c_str(), "magazine")) {
        return &gphys_paper;
    }

    if (strstr(ent->model.c_str(), "helmet") || strstr(ent->model.c_str(), "can")) {
        return &gphys_metal;
    }

    return &gphys_wood;
}

//=============================================================
// The world
//=============================================================

static void G_PhysicsTrace(const char *fmt, ...)
{
    char    text[1024];
    va_list ap;

    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    gi.DPrintf("g_physics: %s\n", text);
}

void G_PhysicsShutdown(void)
{
    if (gphys_world.system) {
        JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();

        for (int i = 0; i < MAX_GENTITIES; i++) {
            if (gphys_entities[i].owned) {
                bodies.RemoveBody(gphys_entities[i].id);
                bodies.DestroyBody(gphys_entities[i].id);
            }
        }

        for (size_t i = 0; i < gphys_worldBodies.size(); i++) {
            bodies.RemoveBody(gphys_worldBodies[i]);
            bodies.DestroyBody(gphys_worldBodies[i]);
        }

        for (size_t i = 0; i < gphys_furniture.size(); i++) {
            if (!gphys_furniture[i].id.IsInvalid()) {
                bodies.RemoveBody(gphys_furniture[i].id);
                bodies.DestroyBody(gphys_furniture[i].id);
            }
        }

        for (int i = 0; i < MAX_GENTITIES; i++) {
            if (!gphys_kinematic[i].id.IsInvalid()) {
                bodies.RemoveBody(gphys_kinematic[i].id);
                bodies.DestroyBody(gphys_kinematic[i].id);
            }
        }

        Phys_DestroyWorld(&gphys_world);
    }

    memset(gphys_entities, 0, sizeof(gphys_entities));
    for (int i = 0; i < MAX_GENTITIES; i++) {
        gphys_kinematic[i] = gphysKinematic_t();
    }
    gphys_worldBodies.clear();
    gphys_furniture.clear();
    gphys_furnitureBrushes.clear();
    gphys_models.clear();
    gphys_accum = 0.0f;
    gphys_ready = qfalse;
}

void G_PhysicsInitLevel(const char *mapfile)
{
    void            *buf = NULL;
    long             len;
    physBspOptions_t opt;
    physBspStats_t   stats;
    const int        start = gi.Milliseconds();

    g_physics     = gi.Cvar_Get("g_physics", "1", CVAR_ARCHIVE | CVAR_LATCH);
    g_physics_log = gi.Cvar_Get("g_physics_log", "0", 0);
    g_physics_hitscale = gi.Cvar_Get("g_physics_hitscale", "1", 0);

    G_PhysicsShutdown();

    if (!g_physics->integer || !mapfile || !mapfile[0]) {
        return;
    }

    len = gi.FS_ReadFile(mapfile, &buf, qtrue);
    if (len < (long)sizeof(dheader_t) || !buf) {
        if (buf) {
            gi.FS_FreeFile(buf);
        }
        return;
    }

    Phys_RegisterJolt(G_PhysicsTrace);
    Phys_CreateWorld(&gphys_world, GPHYS_MAX_BODIES);

    // Solid brushes, and fences; the clip brushes that stand in for the
    // client's static model furniture are solid too, which is what the
    // server's props should rest on. Player clip stays out.
    std::vector<physFurniture_t> furniture;

    Phys_FindFurniture(buf, len, &furniture);
    for (size_t i = 0; i < furniture.size(); i++) {
        for (size_t b = 0; b < furniture[i].brushes.size(); b++) {
            gphys_furnitureBrushes.insert(furniture[i].brushes[b]);
        }
    }

    memset(&opt, 0, sizeof(opt));
    opt.contents  = CONTENTS_SOLID | CONTENTS_FENCE;
    opt.skipBrush = G_PhysicsSkipFurniture;

    Phys_BuildBspWorld(gphys_world.system, buf, len, &opt, &gphys_worldBodies, &stats);
    Phys_ReadInlineModels(buf, len, &gphys_models);
    gi.FS_FreeFile(buf);

    for (size_t i = 0; i < furniture.size(); i++) {
        JPH::StaticCompoundShapeSettings compound;
        gphysFurniture_t                 f;
        int                              pieces = 0;

        for (size_t h = 0; h < furniture[i].hulls.size(); h++) {
            const std::vector<float>& corners = furniture[i].hulls[h];
            JPH::Array<JPH::Vec3>     points;

            for (size_t c = 0; c + 2 < corners.size(); c += 3) {
                vec3_t p = {corners[c], corners[c + 1], corners[c + 2]};
                points.push_back(PhysToJolt(p));
            }

            JPH::ConvexHullShapeSettings    hull(points, 0.005f);
            JPH::ShapeSettings::ShapeResult result = hull.Create();
            if (!result.HasError()) {
                compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), result.Get());
                pieces++;
            }
        }

        if (!pieces) {
            continue;
        }

        JPH::ShapeSettings::ShapeResult result = compound.Create();
        if (result.HasError()) {
            continue;
        }

        JPH::BodyCreationSettings settings(
            result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, PhysLayers::WORLD
        );
        f.id = gphys_world.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (f.id.IsInvalid()) {
            continue;
        }

        // The middle of its first brush.
        VectorClear(f.probe);
        {
            const std::vector<float>& corners = furniture[i].hulls[0];
            const int                 n       = (int)(corners.size() / 3);

            for (int c = 0; c < n; c++) {
                f.probe[0] += corners[c * 3] / n;
                f.probe[1] += corners[c * 3 + 1] / n;
                f.probe[2] += corners[c * 3 + 2] / n;
            }
        }
        f.nextCheck = 0;
        gphys_furniture.push_back(f);
    }

    gphys_ready = qtrue;

    if (g_physics_log->integer) {
        gi.Printf(
            "g_physics: %s: %d brushes, %d patches, %d terrain patches, %d bodies, %d brush models, %d pieces of furniture in %d ms\n",
            mapfile,
            stats.brushes,
            stats.patches,
            stats.terrain,
            stats.bodies,
            (int)gphys_models.size(),
            (int)gphys_furniture.size(),
            gi.Milliseconds() - start
        );
    }
}

//=============================================================
// Entities
//=============================================================

// Whether an entity is one the physics takes, as it spawns or as a save game
// brings it back: an unbroken crate or barrel, or a small interactive object.
static bool G_PhysicsWants(Entity *ent)
{
    if (!ent || ent->edict->solid == SOLID_NOT) {
        return false;
    }

    if (ent->IsSubclassOfCrateObject() || ent->isSubclassOf(BarrelObject)) {
        return ent->takedamage != DAMAGE_NO;
    }

    if (ent->isSubclassOf(InteractObject)) {
        return ent->size[0] <= 96 && ent->size[1] <= 96 && ent->size[2] <= 96;
    }

    return false;
}

void G_PhysicsRestoreLevel(void)
{
    int restored = 0;

    G_PhysicsInitLevel(level.m_mapfile.c_str());
    if (!gphys_ready) {
        return;
    }

    for (int i = 0; i < MAX_GENTITIES; i++) {
        Entity *ent = g_entities[i].entity;

        if (G_PhysicsWants(ent) && G_PhysicsAddEntity(ent)) {
            restored++;
        }
    }

    if (g_physics_log->integer) {
        gi.Printf("g_physics: %d entities are bodies again, from the save game\n", restored);
    }
}

bool G_PhysicsOwns(const Entity *ent)
{
    return ent && ent->entnum >= 0 && ent->entnum < MAX_GENTITIES && gphys_entities[ent->entnum].owned;
}

static JPH::ShapeRefC G_PhysicsEntityShape(Entity *ent, vec3_t boxMins, vec3_t boxMaxs)
{
    JPH::Array<JPH::Vec3> points;

    // A brush model: the hull of its brushes' corners, in its own space.
    if (ent->model.length() > 1 && ent->model[0] == '*') {
        const int n = atoi(ent->model.c_str() + 1);

        if (n > 0 && n < (int)gphys_models.size() && !gphys_models[n].corners.empty()) {
            const physInlineModel_t *m = &gphys_models[n];

            for (size_t i = 0; i + 2 < m->corners.size(); i += 3) {
                vec3_t c = {m->corners[i], m->corners[i + 1], m->corners[i + 2]};
                points.push_back(PhysToJolt(c));
            }

            VectorCopy(m->mins, boxMins);
            VectorCopy(m->maxs, boxMaxs);
        }
    }

    // Anything else: its bounds. A model's own bounds when it has them, as
    // its size is often a rough box (a magazine is an 8 unit cube).
    if (points.empty()) {
        vec3_t lo, hi;

        VectorCopy(ent->mins, lo);
        VectorCopy(ent->maxs, hi);

        if (ent->edict->tiki) {
            vec3_t tlo, thi;

            gi.TIKI_CalculateBounds(ent->edict->tiki, ent->edict->s.scale, tlo, thi);
            if (tlo[0] < thi[0] && tlo[1] < thi[1] && tlo[2] <= thi[2]
                && thi[0] - tlo[0] <= (hi[0] - lo[0]) * 2.0f + 1.0f && thi[1] - tlo[1] <= (hi[1] - lo[1]) * 2.0f + 1.0f
                && thi[2] - tlo[2] <= (hi[2] - lo[2]) * 2.0f + 1.0f) {
                VectorCopy(tlo, lo);
                VectorCopy(thi, hi);
            }
        }

        // Not thinner than a unit, for the solver.
        for (int k = 0; k < 3; k++) {
            if (hi[k] - lo[k] < 1.0f) {
                const float mid = (hi[k] + lo[k]) * 0.5f;
                lo[k]           = mid - 0.5f;
                hi[k]           = mid + 0.5f;
            }
        }

        for (int i = 0; i < 8; i++) {
            vec3_t c = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
            points.push_back(PhysToJolt(c));
        }

        VectorCopy(lo, boxMins);
        VectorCopy(hi, boxMaxs);
    }

    JPH::ConvexHullShapeSettings    hull(points, Q_min(JPH::cDefaultConvexRadius, 0.25f * PHYS_UNITS_TO_METRES));
    JPH::ShapeSettings::ShapeResult result = hull.Create();

    return result.HasError() ? JPH::ShapeRefC() : result.Get();
}

bool G_PhysicsAddEntity(Entity *ent)
{
    vec3_t                 mins, maxs, axis[3];
    const gphysMaterial_t *material;
    JPH::ShapeRefC         shape;

    if (!gphys_ready || !ent || ent->entnum < 0 || ent->entnum >= MAX_GENTITIES || G_PhysicsOwns(ent)) {
        return false;
    }

    shape = G_PhysicsEntityShape(ent, mins, maxs);
    if (!shape) {
        return false;
    }

    material = G_PhysicsMaterial(ent);
    AngleVectorsLeft(ent->angles, axis[0], axis[1], axis[2]);

    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(PhysToJolt(ent->origin)),
        Phys_QuatFromAxis(axis),
        JPH::EMotionType::Dynamic,
        PhysLayers::PROP
    );

    {
        const JPH::Vec3 size = PhysToJolt(maxs) - PhysToJolt(mins);
        const float     area = 2.0f * (size.GetX() * size.GetY() + size.GetY() * size.GetZ() + size.GetZ() * size.GetX());

        settings.mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = Q_clamp_float(area * material->arealDensity, 0.2f, 200.0f);
    }

    settings.mFriction       = material->friction;
    settings.mRestitution    = material->restitution;
    settings.mLinearDamping  = 0.05f;
    settings.mAngularDamping = 0.1f;
    settings.mMotionQuality  = JPH::EMotionQuality::LinearCast;
    settings.mUserData       = PHYS_USERDATA_ENTITY_BASE + ent->entnum;

    const JPH::BodyID id = gphys_world.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    if (id.IsInvalid()) {
        return false;
    }

    gphys_entities[ent->entnum].id    = id;
    gphys_entities[ent->entnum].owned = qtrue;

    if (g_physics_log->integer >= 2) {
        gi.Printf(
            "g_physics: #%d %s %s at %.0f %.0f %.0f, %.0fx%.0fx%.0f, %.1f kg\n",
            ent->entnum,
            ent->getClassname(),
            ent->model.c_str(),
            ent->origin[0],
            ent->origin[1],
            ent->origin[2],
            maxs[0] - mins[0],
            maxs[1] - mins[1],
            maxs[2] - mins[2],
            settings.mMassPropertiesOverride.mMass
        );
    }
    return true;
}

void G_PhysicsRemoveEntity(Entity *ent)
{
    if (!G_PhysicsOwns(ent)) {
        return;
    }

    JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();
    gphysEntity_t      *pe     = &gphys_entities[ent->entnum];
    JPH::AABox          box    = bodies.GetTransformedShape(pe->id).GetWorldSpaceBounds();

    bodies.RemoveBody(pe->id);
    bodies.DestroyBody(pe->id);
    pe->owned = qfalse;

    // Whatever it held up.
    box.ExpandBy(JPH::Vec3::sReplicate(0.1f));
    bodies.ActivateBodiesInAABox(box, {}, {});
}

void G_PhysicsImpulse(Entity *ent, const Vector& point, const Vector& impulse)
{
    vec3_t p, j;

    if (!G_PhysicsOwns(ent)) {
        return;
    }

    VectorCopy(point, p);
    VectorCopy(impulse, j);
    gphys_world.system->GetBodyInterface().AddImpulse(gphys_entities[ent->entnum].id, PhysToJolt(j), JPH::RVec3(PhysToJolt(p)));
    gphys_world.system->GetBodyInterface().ActivateBody(gphys_entities[ent->entnum].id);
}

void G_PhysicsDamaged(Entity *ent, float damage, const Vector& position, const Vector& direction)
{
    float  mass, amount;
    Vector dir = direction;

    if (!G_PhysicsOwns(ent) || damage <= 0.0f || dir.length() < 0.001f) {
        return;
    }

    {
        JPH::BodyLockRead lock(gphys_world.system->GetBodyLockInterface(), gphys_entities[ent->entnum].id);
        if (!lock.Succeeded()) {
            return;
        }
        mass = 1.0f / Q_max(1e-4f, lock.GetBody().GetMotionProperties()->GetInverseMass());
    }

    dir.normalize();
    amount = Q_min(damage * GPHYS_DAMAGE_IMPULSE, mass * GPHYS_MAX_HIT_SPEED) * Q_max(0.0f, g_physics_hitscale->value);

    if (g_physics_log->integer >= 2) {
        gi.Printf(
            "g_physics: #%d %s hit for %.0f at %.0f %.0f %.0f: %.1f kg, %.0f units/s\n",
            ent->entnum,
            ent->getClassname(),
            damage,
            position[0],
            position[1],
            position[2],
            mass,
            amount / mass
        );
    }

    G_PhysicsImpulse(ent, position, dir * amount);
}

//=============================================================
// Players and AI
//=============================================================

static JPH::ShapeRefC G_PhysicsKinematicBox(const vec3_t size)
{
    vec3_t half;

    VectorScale(size, 0.5f * PHYS_UNITS_TO_METRES, half);
    const float radius = Q_min(JPH::cDefaultConvexRadius, 0.5f * Q_min(half[0], Q_min(half[1], half[2])));

    JPH::BoxShapeSettings           box(JPH::Vec3(half[0], half[1], half[2]), radius);
    JPH::ShapeSettings::ShapeResult result = box.Create();

    return result.HasError() ? JPH::ShapeRefC() : result.Get();
}

static void G_PhysicsDropKinematic(gphysKinematic_t *k)
{
    if (!k->id.IsInvalid()) {
        JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();

        bodies.RemoveBody(k->id);
        bodies.DestroyBody(k->id);
    }

    *k = gphysKinematic_t();
}

// Before the steps: a box for everyone alive and solid, placed to move from
// where it was to where they are now.
static void G_PhysicsFollowSentients(void)
{
    JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();
    int                 i;

    for (i = 0; i < MAX_GENTITIES; i++) {
        gphys_kinematic[i].seen = qfalse;
    }

    for (i = 1; i <= SentientList.NumObjects(); i++) {
        Sentient         *ent = SentientList.ObjectAt(i);
        gphysKinematic_t *k;
        vec3_t            size, centre;

        if (!ent || ent->entnum < 0 || ent->entnum >= MAX_GENTITIES || ent->health <= 0 || ent->deadflag
            || ent->edict->solid == SOLID_NOT || ent->edict->solid == SOLID_TRIGGER) {
            continue;
        }

        k = &gphys_kinematic[ent->entnum];
        if (k->ent != ent) {
            G_PhysicsDropKinematic(k);
        }

        VectorSubtract(ent->maxs, ent->mins, size);
        if (size[0] < 1.0f || size[1] < 1.0f || size[2] < 1.0f) {
            continue;
        }

        VectorAdd(ent->mins, ent->maxs, centre);
        VectorMA(ent->origin, 0.5f, centre, centre);

        if (k->id.IsInvalid()) {
            JPH::ShapeRefC shape = G_PhysicsKinematicBox(size);

            if (!shape) {
                continue;
            }

            JPH::BodyCreationSettings settings(
                shape, JPH::RVec3(PhysToJolt(centre)), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, PhysLayers::KINEMATIC
            );
            // No friction, so someone standing on a crate does not drag it.
            settings.mFriction    = 0.0f;
            settings.mRestitution = 0.0f;
            settings.mUserData    = PHYS_USERDATA_ENTITY_BASE + ent->entnum;

            k->id = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
            if (k->id.IsInvalid()) {
                continue;
            }

            k->ent = ent;
            VectorCopy(size, k->size);
            k->from = k->to = settings.mPosition;
        } else if (fabs(size[0] - k->size[0]) > 0.5f || fabs(size[1] - k->size[1]) > 0.5f || fabs(size[2] - k->size[2]) > 0.5f) {
            // Crouched or stood.
            JPH::ShapeRefC shape = G_PhysicsKinematicBox(size);

            if (shape) {
                bodies.SetShape(k->id, shape, false, JPH::EActivation::DontActivate);
                VectorCopy(size, k->size);
            }
        }

        k->seen = qtrue;
        k->from = k->to;
        k->to   = JPH::RVec3(PhysToJolt(centre));

        // A teleport: there, not swept through everything between.
        if ((k->to - k->from).Length() > 64.0f * PHYS_UNITS_TO_METRES) {
            bodies.SetPosition(k->id, k->to, JPH::EActivation::DontActivate);
            k->from = k->to;
        }
    }

    for (i = 0; i < MAX_GENTITIES; i++) {
        if (!gphys_kinematic[i].seen && !gphys_kinematic[i].id.IsInvalid()) {
            G_PhysicsDropKinematic(&gphys_kinematic[i]);
        }
    }
}

// Each step: the boxes a part of the way along.
static void G_PhysicsMoveSentients(float frac, float dt)
{
    JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();

    for (int i = 0; i < MAX_GENTITIES; i++) {
        const gphysKinematic_t *k = &gphys_kinematic[i];

        if (k->id.IsInvalid()) {
            continue;
        }

        if (k->from == k->to) {
            // Standing still: leave it asleep, but where it is.
            if (bodies.GetPosition(k->id) != k->to) {
                bodies.SetPosition(k->id, k->to, JPH::EActivation::DontActivate);
            }
            continue;
        }

        bodies.MoveKinematic(k->id, k->from + (k->to - k->from) * frac, JPH::Quat::sIdentity(), dt);
    }
}

void G_PhysicsPushedBy(Entity *ent, Entity *pusher, const Vector& direction, float speed)
{
    vec3_t dir;
    float  mass, target, along;

    if (!G_PhysicsOwns(ent) || !pusher || speed <= 0.0f) {
        return;
    }

    // Not what someone stands on.
    if (pusher->absmin[2] >= ent->absmax[2] - 2.0f) {
        return;
    }

    VectorSet(dir, direction[0], direction[1], 0.0f);
    if (VectorNormalize(dir) < 0.001f) {
        return;
    }

    JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();
    const JPH::BodyID   id     = gphys_entities[ent->entnum].id;

    {
        JPH::BodyLockRead lock(gphys_world.system->GetBodyLockInterface(), id);
        if (!lock.Succeeded()) {
            return;
        }
        mass = 1.0f / Q_max(1e-4f, lock.GetBody().GetMotionProperties()->GetInverseMass());
    }

    if (mass >= GPHYS_PUSH_HEAVY) {
        return;
    }

    target = Q_min(speed, GPHYS_PUSH_SPEED) * Q_min(1.0f, GPHYS_PUSH_LIGHT / mass);

    // Up to the speed, never slowing what already goes faster.
    along = bodies.GetLinearVelocity(id).Dot(PhysToJolt(dir)) * PHYS_METRES_TO_UNITS;
    if (along >= target) {
        return;
    }

    bodies.AddImpulse(id, PhysToJolt(dir) * ((target - along) * mass));
    bodies.ActivateBody(id);

    if (g_physics_log->integer > 2) {
        gi.Printf(
            "g_physics: #%d pushed by #%d towards %.0f u/s (%.1f kg), at %.0f %.0f %.0f\n",
            ent->entnum,
            pusher->entnum,
            target,
            mass,
            ent->origin[0],
            ent->origin[1],
            ent->origin[2]
        );
    }
}

//=============================================================
// Stepping
//=============================================================

void G_PhysicsFrame(float frametime)
{
    const float dt    = 1.0f / GPHYS_STEP_HZ;
    int         steps = 0;

    if (!gphys_ready) {
        return;
    }

    gphys_world.system->SetGravity(JPH::Vec3(0.0f, 0.0f, -sv_gravity->value * PHYS_UNITS_TO_METRES));

    // Furniture the client has taken out of the collision model.
    for (size_t i = 0; i < gphys_furniture.size(); i++) {
        gphysFurniture_t *f = &gphys_furniture[i];

        if (f->id.IsInvalid() || level.inttime < f->nextCheck) {
            continue;
        }
        f->nextCheck = level.inttime + 250;

        if (gi.pointcontents(f->probe, ENTITYNUM_NONE) & CONTENTS_SOLID) {
            continue;
        }

        {
            JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();
            JPH::AABox          box    = bodies.GetTransformedShape(f->id).GetWorldSpaceBounds();

            bodies.RemoveBody(f->id);
            bodies.DestroyBody(f->id);
            f->id = JPH::BodyID();

            box.ExpandBy(JPH::Vec3::sReplicate(0.1f));
            bodies.ActivateBodiesInAABox(box, {}, {});

            if (g_physics_log->integer) {
                gi.Printf("g_physics: furniture %d is gone from the collision model; so is its body\n", (int)i);
            }
        }
    }

    G_PhysicsFollowSentients();

    gphys_accum = Q_min(gphys_accum + frametime, dt * GPHYS_MAX_STEPS);
    {
        const int total = Q_min((int)(gphys_accum / dt), GPHYS_MAX_STEPS);

        while (steps < total) {
            G_PhysicsMoveSentients((float)(steps + 1) / total, dt);
            gphys_world.system->Update(dt, 1, gphys_world.temp, gphys_world.jobs);
            gphys_accum -= dt;
            steps++;
        }

        // No step this frame: the boxes are where they are going.
        if (!total) {
            for (int i = 0; i < MAX_GENTITIES; i++) {
                gphys_kinematic[i].to = gphys_kinematic[i].from;
            }
        }
    }

    if (!steps) {
        return;
    }

    if (g_physics_log->integer) {
        static int lastReport;

        if (level.inttime - lastReport >= 5000 || level.inttime < lastReport) {
            int owned = 0;

            for (int i = 0; i < MAX_GENTITIES; i++) {
                owned += gphys_entities[i].owned ? 1 : 0;
            }

            gi.Printf(
                "g_physics: %d entities are bodies, %d awake\n",
                owned,
                (int)gphys_world.system->GetNumActiveBodies(JPH::EBodyType::RigidBody)
            );
            lastReport = level.inttime;
        }
    }

    // Where the awake props are now.
    JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterfaceNoLock();

    for (int i = 0; i < MAX_GENTITIES; i++) {
        gphysEntity_t *pe = &gphys_entities[i];
        Entity        *ent;
        JPH::RVec3     pos;
        JPH::Quat      rot;
        vec3_t         origin, axis[3], angles;

        if (!pe->owned || !bodies.IsActive(pe->id)) {
            continue;
        }

        ent = g_entities[i].entity;
        if (!ent) {
            continue;
        }

        bodies.GetPositionAndRotation(pe->id, pos, rot);
        PhysFromJolt(JPH::Vec3(pos), origin);
        Phys_AxisFromQuat(rot, axis);
        MatrixToEulerAngles((const float(*)[3])axis, angles);

        ent->setOrigin(Vector(origin));
        ent->setAngles(Vector(angles));
    }
}
