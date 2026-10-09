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
#include "../physics/phys_rules.h"
#include "../physics/phys_impact.h"

#include "g_local.h"
#include "entity.h"
#include "level.h"
#include "gamecvars.h"
#include "g_physics.h"
#include "g_phys.h"
#include "sentient.h"
#include "barrels.h"
#include "object.h"
#include "player.h"
#include "item.h"
#include "weapon.h"

#include <set>
#include <string>
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
    float       ruleMass; // the mass physics.txt gave it, or 0
    Entity     *refused;  // could not be shaped: not tried again
} gphysEntity_t;

static gphysEntity_t gphys_entities[MAX_GENTITIES];

// physics.txt (see code/physics/phys_rules.h), and what each entity is called
// in it: a brush model by its model (*185), anything else by where it stood
// when the physics first saw it.
#define GPHYS_RULES_FILE "physics.txt"

typedef struct {
    Entity     *ent;
    std::string key;
} gphysKey_t;

static PhysRules  gphys_rules;
static gphysKey_t gphys_keys[MAX_GENTITIES];

// The furniture in the brushwork (see code/physics/phys_furniture.cpp). The
// client makes it move; here each piece is a fixed body of its own, so that
// when the client takes its brushes out of the collision model the server shares
// with it (single player), the body goes too and what stood on it falls.
typedef struct {
    JPH::BodyID    id;
    JPH::ShapeRefC shape;
    vec3_t         probe; // inside one of its brushes
    int            nextCheck;
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

// The grabber, from a single player client (cg_physics_movers.cpp): what each
// player is carrying, by the point taken hold of, how far from his eye. The
// spring is the client's: natural frequency in radians a second, the most it
// accelerates, in units a second squared, and the most weight it holds up.
#define GPHYS_GRAB_OMEGA     14.0f
#define GPHYS_GRAB_MAX_ACCEL 4000.0f
#define GPHYS_GRAB_MAX_MASS  40.0f
#define GPHYS_GRAB_SPIN_KEEP 0.92f
#define GPHYS_GRAB_RANGE     1024.0f
#define GPHYS_GRAB_MIN_DIST  24.0f

typedef struct {
    int       entnum; // -1 for nothing
    JPH::Vec3 local;  // metres, in the body's own space
    float     dist;
} gphysGrab_t;

static gphysGrab_t gphys_grab[MAX_CLIENTS];

// A player's box and what he holds do not collide, or holding a crate close
// would have him forever shoving it away. Each player is a group of his own.
static JPH::Ref<JPH::GroupFilterTable> gphys_holdFilter;

#define GPHYS_HOLD_HOLDER 0
#define GPHYS_HOLD_HELD   1

static JPH::GroupFilterTable *G_PhysicsHoldFilter(void)
{
    if (!gphys_holdFilter) {
        gphys_holdFilter = new JPH::GroupFilterTable(2);
        gphys_holdFilter->DisableCollision(GPHYS_HOLD_HOLDER, GPHYS_HOLD_HELD);
    }

    return gphys_holdFilter;
}
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
    physSoundMat_t sound; // what it sounds like striking things (phys_impact.cpp)
} gphysMaterial_t;

static const gphysMaterial_t gphys_metal  = {{"barrel", "can", "bucket", "helmet", NULL}, 3.0f, 0.5f, 0.2f, PHYS_SND_METAL};
static const gphysMaterial_t gphys_paper  = {{"magazine", "paper", "book", "map", NULL}, 0.8f, 0.8f, 0.05f, PHYS_SND_PAPER};
static const gphysMaterial_t gphys_wood   = {{"crate", NULL}, 4.0f, 0.6f, 0.15f, PHYS_SND_WOOD};

static const gphysMaterial_t *G_PhysicsMaterial(Entity *ent)
{
    if (ent->IsSubclassOfCrateObject()) {
        return &gphys_wood;
    }

    // Weapons, ammunition and the first aid tins.
    if (ent->IsSubclassOfWeapon() || strstr(ent->model.c_str(), "ammo") || strstr(ent->model.c_str(), "health")) {
        return &gphys_metal;
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
// Impact sounds
//=============================================================

// What the entity props sound like striking things, as on the client (see
// code/physics/phys_impact.cpp): noted from the contacts during the steps,
// played after them, the loudest few. Everyone hears them, since the entity
// makes them.

static cvar_t           *g_physics_sounds;
static cvar_t           *g_physics_sounddebug;
static PhysImpactLimiter gphys_impacts;

// A world just made, or a game restored, is quiet this long.
#define GPHYS_QUIET_TIME 1500

// The prop an entity body is, or NULL: one of ours that moves.
static Entity *G_PhysicsPropOf(JPH::uint64 data)
{
    int entnum;

    if (data < PHYS_USERDATA_ENTITY_BASE || data >= PHYS_USERDATA_ENTITY_BASE + MAX_GENTITIES) {
        return NULL;
    }

    entnum = (int)(data - PHYS_USERDATA_ENTITY_BASE);
    if (!gphys_entities[entnum].owned) {
        return NULL;
    }

    return g_entities[entnum].entity;
}

class GPhysContacts final : public JPH::ContactListener
{
public:
    void OnContactAdded(
        const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings
    ) override
    {
        Note(a, b, manifold);
    }

    void OnContactPersisted(
        const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings
    ) override
    {
        Note(a, b, manifold);
    }

private:
    static void Note(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold)
    {
        physContact_t c;
        physImpact_t  impact;
        int           h;

        if (!g_physics_sounds || !g_physics_sounds->integer || (!a.IsDynamic() && !b.IsDynamic())) {
            return;
        }

        c = Phys_ContactFrom(a, b, manifold);
        if (c.sensor || c.closing <= 0.0f) {
            return;
        }

        if (!c.dynamic[1]) {
            h = 0;
        } else if (!c.dynamic[0]) {
            h = 1;
        } else {
            h = c.approach[0] >= c.approach[1] ? 0 : 1;
        }

        impact.hitter    = c.data[h];
        impact.other     = c.data[!h];
        impact.hitterMat = PHYS_SND_DEFAULT;
        impact.mass      = c.mass[h];
        impact.speed     = c.closing;
        impact.strength  = Phys_ImpactStrength(c.closing, c.mass[h]);
        if (impact.strength <= 0.0f) {
            return;
        }

        PhysFromJolt(JPH::Vec3(c.point), impact.point);
        PhysFromJolt(h == 0 ? c.normal : -c.normal, impact.dir);
        VectorNormalize(impact.dir);

        gphys_impacts.Offer(impact, impact.hitter);
    }
};

static GPhysContacts gphys_contacts;

// The surface below an impact, from the engine's own trace, and water where
// the point is wet.
static const char *G_PhysicsSurfaceAt(const physImpact_t *impact, Entity *hitter)
{
    trace_t tr;
    vec3_t  above;
    Vector  start, end;

    if (gi.pointcontents(impact->point, ENTITYNUM_NONE) & MASK_WATER) {
        VectorCopy(impact->point, above);
        above[2] += 16.0f;
        return (gi.pointcontents(above, ENTITYNUM_NONE) & MASK_WATER) ? "wade" : "puddle";
    }

    start = Vector(impact->point) - Vector(impact->dir) * 4.0f;
    end   = Vector(impact->point) + Vector(impact->dir) * 12.0f;
    tr    = G_Trace(start, vec_zero, vec_zero, end, hitter, MASK_SOLID, qfalse, "G_PhysicsSurfaceAt");

    if (tr.fraction >= 1.0f || tr.startsolid) {
        return "stone";
    }

    return Phys_SurfaceName(tr.surfaceFlags);
}

static void G_PhysicsPlayImpacts(void)
{
    std::vector<physImpact_t> heard;
    int                       rejected;

    gphys_impacts.Take(level.inttime, &heard, &rejected);

    for (size_t i = 0; i < heard.size(); i++) {
        physImpact_t *impact = &heard[i];
        Entity       *hitter = G_PhysicsPropOf(impact->hitter);
        Entity       *other;
        const char   *surface;
        float         volume, pitch;
        std::string   alias;

        if (!hitter) {
            continue;
        }
        impact->hitterMat = G_PhysicsMaterial(hitter)->sound;

        other = G_PhysicsPropOf(impact->other);
        if (other) {
            surface = Phys_SurfaceOfMat(G_PhysicsMaterial(other)->sound);
        } else if (impact->other >= PHYS_USERDATA_ENTITY_BASE && impact->other < PHYS_USERDATA_ENTITY_BASE + MAX_GENTITIES) {
            // Someone's box, a door: the box makes no sound of its own.
            continue;
        } else {
            surface = G_PhysicsSurfaceAt(impact, hitter);
        }

        alias = Phys_ImpactAlias(impact->hitterMat, surface, impact->mass, impact->strength, &volume, &pitch);

        if (g_physics_sounddebug->integer) {
            gi.Printf(
                "g_physics sound: %s, %s (%s) on %s, %.1f m/s, %.1f kg, volume %.2f pitch %.2f\n",
                alias.c_str(),
                Phys_SoundMatName(impact->hitterMat),
                hitter->getClassname(),
                surface,
                impact->speed,
                impact->mass,
                volume,
                pitch
            );
        }

        Vector at(impact->point);
        hitter->Sound(alias.c_str(), CHAN_AUTO, volume, -1.0f, &at, pitch, 1, 0, 0);
    }

    if (g_physics_sounddebug->integer && rejected) {
        gi.Printf("g_physics sound: %d left out\n", rejected);
    }
}

//=============================================================
// physics.txt
//=============================================================

static void G_PhysicsRulesLoad(void)
{
    void *buf = NULL;
    long  len = gi.FS_ReadFile(GPHYS_RULES_FILE, &buf, qtrue);

    if (len > 0 && buf) {
        const std::string text((const char *)buf, (size_t)len);
        gphys_rules.Parse(text.c_str());
    } else {
        gphys_rules.Parse("");
    }

    if (buf) {
        gi.FS_FreeFile(buf);
    }
}

static const char *G_PhysicsEntityKey(Entity *ent)
{
    gphysKey_t *k = &gphys_keys[ent->entnum];

    if (k->ent != ent || k->key.empty()) {
        k->ent = ent;
        if (ent->model.length() > 1 && ent->model[0] == '*') {
            k->key = ent->model.c_str();
        } else {
            k->key = va("%d,%d,%d", (int)floor(ent->origin[0] + 0.5f), (int)floor(ent->origin[1] + 0.5f), (int)floor(ent->origin[2] + 0.5f));
        }
    }

    return k->key.c_str();
}

// Whether an entity moves as a body, by its kind and by the rules: MOVES or
// FIXED (the rules' off is fixed here, as the entity was made), or UNSET for
// one that is no kind of physics object at all. With why, and the rules' mass
// (0 for its own).
static int G_PhysicsDecide(Entity *ent, float *mass, const char **why)
{
    bool        byDefault;
    const char *defaultWhy;

    *mass = 0.0f;
    *why  = "";

    if (!ent || ent->entnum < 0 || ent->entnum >= MAX_GENTITIES) {
        return PHYS_RULE_UNSET;
    }

    if (ent->IsSubclassOfCrateObject() || ent->isSubclassOf(BarrelObject)) {
        // Broken, or never to be.
        if (ent->takedamage == DAMAGE_NO) {
            return PHYS_RULE_UNSET;
        }
        byDefault  = true;
        defaultWhy = ent->IsSubclassOfCrateObject() ? "a crate" : "a barrel";
    } else if (ent->isSubclassOf(InteractObject)) {
        byDefault  = ent->size[0] <= 96 && ent->size[1] <= 96 && ent->size[2] <= 96;
        defaultWhy = byDefault ? "small enough to move" : "too big to move";
    } else if (ent->IsSubclassOfItem()) {
        // Weapons, ammunition and health lying about, not in anyone's hands
        // and not picked up; in single player only, as where a pickup lies is
        // part of a multiplayer game.
        if (g_gametype->integer != GT_SINGLE_PLAYER || ((Item *)ent)->GetOwner() || ent->edict->solid != SOLID_TRIGGER
            || ent->bindmaster || !ent->edict->tiki) {
            return PHYS_RULE_UNSET;
        }
        byDefault  = true;
        defaultWhy = ent->IsSubclassOfWeapon() ? "a weapon lying about" : "an item lying about";
    } else if (ent->isSubclassOf(HelmetObject)) {
        if (!ent->edict->tiki) {
            return PHYS_RULE_UNSET;
        }
        byDefault  = true;
        defaultWhy = "a helmet shot off";
    } else {
        return PHYS_RULE_UNSET;
    }

    const bool             brush = ent->model.length() > 1 && ent->model[0] == '*';
    const physRuleResult_t r     = gphys_rules.Resolve(
        level.m_mapfile.c_str(), "entity", G_PhysicsEntityKey(ent), brush ? NULL : ent->model.c_str(), ent->getClassID()
    );

    *mass = r.rule.mass;
    if (r.rule.state == PHYS_RULE_UNSET) {
        *why = defaultWhy;
        return byDefault ? PHYS_RULE_MOVES : PHYS_RULE_FIXED;
    }

    *why = r.from;
    return r.rule.state == PHYS_RULE_MOVES ? PHYS_RULE_MOVES : PHYS_RULE_FIXED;
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
        gphys_keys[i] = gphysKey_t();
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        gphys_grab[i].entnum = -1;
    }
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
    g_physics_sounds     = gi.Cvar_Get("g_physics_sounds", "1", CVAR_ARCHIVE);
    g_physics_sounddebug = gi.Cvar_Get("g_physics_sounddebug", "0", 0);
    gphys_impacts.Clear();
    gphys_impacts.Quiet(level.inttime + GPHYS_QUIET_TIME);

    G_PhysicsShutdown();
    G_PhysicsRulesLoad();

    // Single player only: in multiplayer the ragdolls, on each client, are the
    // only physics, and crates, barrels and helmets behave as they always have.
    if (!g_physics->integer || g_gametype->integer != GT_SINGLE_PLAYER || !mapfile || !mapfile[0]) {
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
    gphys_world.system->SetContactListener(&gphys_contacts);

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
        f.shape = result.Get();

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
    float       mass;
    const char *why;

    if (!ent || ent->edict->solid == SOLID_NOT) {
        return false;
    }

    return G_PhysicsDecide(ent, &mass, &why) == PHYS_RULE_MOVES;
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

            // An item's size is the box it is picked up by, and a helmet's a
            // token cube: the model's own bounds are the thing itself.
            const bool loose = ent->IsSubclassOfItem() || ent->isSubclassOf(HelmetObject);

            gi.TIKI_CalculateBounds(ent->edict->tiki, ent->edict->s.scale, tlo, thi);
            if (tlo[0] < thi[0] && tlo[1] < thi[1] && tlo[2] <= thi[2]
                && (loose
                    || (thi[0] - tlo[0] <= (hi[0] - lo[0]) * 2.0f + 1.0f && thi[1] - tlo[1] <= (hi[1] - lo[1]) * 2.0f + 1.0f
                        && thi[2] - tlo[2] <= (hi[2] - lo[2]) * 2.0f + 1.0f))) {
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
    float                  ruleMass;
    const char            *why;

    if (!gphys_ready || !ent || ent->entnum < 0 || ent->entnum >= MAX_GENTITIES || G_PhysicsOwns(ent)) {
        return false;
    }

    if (G_PhysicsDecide(ent, &ruleMass, &why) != PHYS_RULE_MOVES) {
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
        settings.mMassPropertiesOverride.mMass = ruleMass > 0.0f ? ruleMass : Q_clamp_float(area * material->arealDensity, 0.2f, 200.0f);
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

    gphys_entities[ent->entnum].id       = id;
    gphys_entities[ent->entnum].owned    = qtrue;
    gphys_entities[ent->entnum].ruleMass = ruleMass;

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

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (gphys_grab[i].entnum == ent->entnum) {
            gphys_grab[i].entnum = -1;
        }
    }

    // Whatever it held up.
    box.ExpandBy(JPH::Vec3::sReplicate(0.1f));
    bodies.ActivateBodiesInAABox(box, {}, {});
}

// A helmet made solid to shots, as the magazines are: weapon clip, which stops
// rounds and the grabber's beam and nothing that walks, round its model.
static void G_PhysicsMakeShootable(Entity *ent)
{
    vec3_t tlo, thi, axis[3];
    Vector lo(99999, 99999, 99999), hi(-99999, -99999, -99999);

    gi.TIKI_CalculateBounds(ent->edict->tiki, ent->edict->s.scale, tlo, thi);
    AngleVectorsLeft(ent->angles, axis[0], axis[1], axis[2]);
    for (int i = 0; i < 8; i++) {
        const Vector c((i & 1) ? thi[0] : tlo[0], (i & 2) ? thi[1] : tlo[1], (i & 4) ? thi[2] : tlo[2]);
        const Vector w = Vector(axis[0]) * c[0] + Vector(axis[1]) * c[1] + Vector(axis[2]) * c[2];

        for (int k = 0; k < 3; k++) {
            lo[k] = Q_min(lo[k], w[k]);
            hi[k] = Q_max(hi[k], w[k] + (k == 2 ? 1.0f : 0.0f));
        }
    }

    ent->setSolidType(SOLID_BBOX);
    ent->setContents(CONTENTS_WEAPONCLIP);
    if (lo[0] < hi[0] && lo[1] < hi[1]) {
        ent->setSize(lo, hi);
    }
    ent->link();
}

// An item or a helmet that has come to lie about (placed, dropped, thrown,
// knocked off) becomes a body, going as fast as it was thrown.
static void G_PhysicsAdopt(Entity *ent)
{
    gphysEntity_t *pe = &gphys_entities[ent->entnum];
    const Vector   velocity  = ent->velocity;
    const Vector   avelocity = ent->avelocity;

    if (pe->refused == ent) {
        return;
    }

    if (ent->isSubclassOf(HelmetObject)) {
        G_PhysicsMakeShootable(ent);

        // In single player it stays, as the bodies do; elsewhere it goes after
        // its few seconds, as it always has.
        if (g_gametype->integer == GT_SINGLE_PLAYER) {
            ent->CancelEventsOfType(EV_Remove);
        }
    }

    if (!G_PhysicsAddEntity(ent)) {
        pe->refused = ent;
        return;
    }

    ent->setMoveType(MOVETYPE_NONE);
    ent->velocity  = vec_zero;
    ent->avelocity = vec_zero;

    if (velocity.lengthSquared() > 1.0f || avelocity.lengthSquared() > 1.0f) {
        JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();
        vec3_t              v;

        VectorCopy(velocity, v);
        bodies.SetLinearVelocity(pe->id, PhysToJolt(v));
        // Degrees a second of pitch, yaw and roll, as spin about the world's
        // axes: near enough for something tumbling through the air.
        bodies.SetAngularVelocity(
            pe->id, JPH::Vec3(avelocity[2], avelocity[0], avelocity[1]) * (float)(M_PI / 180.0)
        );
        bodies.ActivateBody(pe->id);
    }
}

// Each frame: items and helmets as they come to lie about, and let go of as
// they are picked up (the item then belongs to whoever has it).
static void G_PhysicsLooseThings(void)
{
    for (int i = 0; i < MAX_GENTITIES; i++) {
        Entity     *e = g_entities[i].inuse ? g_entities[i].entity : NULL;
        float       mass;
        const char *why;

        if (!e || (!e->IsSubclassOfItem() && !e->isSubclassOf(HelmetObject))) {
            continue;
        }

        if (G_PhysicsOwns(e)) {
            if (G_PhysicsDecide(e, &mass, &why) != PHYS_RULE_MOVES) {
                G_PhysicsRemoveEntity(e);
            }
            continue;
        }

        if (G_PhysicsDecide(e, &mass, &why) == PHYS_RULE_MOVES) {
            G_PhysicsAdopt(e);
        }
    }
}

// A round's path, start to where it stopped: the items it passed (they are
// triggers, so it goes through them) are shoved as a hit would.
void G_PhysicsBulletPath(const Vector& start, const Vector& end, float damage, const Vector& dir, Entity *struck)
{
    JPH::RayCastResult hit;
    vec3_t             s, e;
    Vector             far;

    if (!gphys_ready || damage <= 0.0f) {
        return;
    }

    far = end + dir * 4.0f;
    VectorCopy(start, s);
    VectorCopy(far, e);

    const JPH::RRayCast ray(JPH::RVec3(PhysToJolt(s)), PhysToJolt(e) - PhysToJolt(s));
    const bool          any = gphys_world.system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, PhysNoKinematicObjects());

    if (g_physics_log->integer > 2) {
        gi.Printf(
            "g_physics: round from %.0f %.0f %.0f to %.0f %.0f %.0f (struck #%d): %s %llu at %.2f\n",
            s[0], s[1], s[2], e[0], e[1], e[2], struck ? struck->entnum : -1,
            any ? "hit" : "missed", any ? (unsigned long long)gphys_world.system->GetBodyInterface().GetUserData(hit.mBodyID) : 0ull,
            any ? hit.mFraction : 0.0f
        );
    }
    if (!any) {
        return;
    }

    const JPH::uint64 data = gphys_world.system->GetBodyInterface().GetUserData(hit.mBodyID);
    if (data < PHYS_USERDATA_ENTITY_BASE || data >= PHYS_USERDATA_ENTITY_BASE + MAX_GENTITIES) {
        return;
    }

    Entity *ent = g_entities[data - PHYS_USERDATA_ENTITY_BASE].entity;
    if (!ent || ent == struck || !ent->IsSubclassOfItem() || !G_PhysicsOwns(ent)) {
        return;
    }

    G_PhysicsDamaged(ent, damage, start + (far - start) * hit.mFraction, dir);
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
                shape, JPH::RVec3(PhysToJolt(centre)), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, PhysLayers::PEOPLE
            );
            // No friction, so someone standing on a crate does not drag it.
            settings.mFriction    = 0.0f;
            settings.mRestitution = 0.0f;
            settings.mUserData    = PHYS_USERDATA_ENTITY_BASE + ent->entnum;
            if (ent->IsSubclassOfPlayer()) {
                settings.mCollisionGroup =
                    JPH::CollisionGroup(G_PhysicsHoldFilter(), (JPH::CollisionGroup::GroupID)(ent->entnum + 1), GPHYS_HOLD_HOLDER);
            }

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
// The grabber
//=============================================================

static void G_PhysicsSetHeld(int entnum, int holder)
{
    JPH::BodyLockWrite lock(gphys_world.system->GetBodyLockInterface(), gphys_entities[entnum].id);

    if (lock.Succeeded()) {
        lock.GetBody().SetCollisionGroup(
            holder >= 0 ? JPH::CollisionGroup(G_PhysicsHoldFilter(), (JPH::CollisionGroup::GroupID)(holder + 1), GPHYS_HOLD_HELD)
                        : JPH::CollisionGroup()
        );
    }
}

static void G_PhysicsDrop(int client)
{
    gphysGrab_t *g = &gphys_grab[client];

    if (g->entnum >= 0 && gphys_entities[g->entnum].owned) {
        G_PhysicsSetHeld(g->entnum, -1);
    }
    g->entnum = -1;
}

static float G_PhysicsBodyMass(JPH::BodyID id)
{
    JPH::BodyLockRead lock(gphys_world.system->GetBodyLockInterface(), id);

    if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
        return 0.0f;
    }

    return 1.0f / Q_max(1e-4f, lock.GetBody().GetMotionProperties()->GetInverseMass());
}

// A client command's player and the physics entity it names, when both are
// fit: the entity a body, and within reach of him.
static Player *G_PhysicsCommandTarget(gentity_t *ent, int argEntity, Entity **target)
{
    Player *player;
    int     n;

    if (!gphys_ready || !ent || !ent->client || !ent->entity || !ent->entity->IsSubclassOfPlayer()) {
        return NULL;
    }

    player = (Player *)ent->entity;
    if (argEntity <= 0) {
        return player;
    }

    n = atoi(gi.Argv(argEntity));
    if (n < 0 || n >= MAX_GENTITIES || !g_entities[n].entity || !G_PhysicsOwns(g_entities[n].entity)
        || (g_entities[n].entity->centroid - player->EyePosition()).length() > GPHYS_GRAB_RANGE) {
        return player;
    }

    *target = g_entities[n].entity;
    return player;
}

// physgrab <entity> <x> <y> <z> <distance>: carry it by that point.
qboolean G_PhysicsGrabCmd(gentity_t *ent)
{
    Entity *target = NULL;
    Player *player = G_PhysicsCommandTarget(ent, 1, &target);
    int     client;
    vec3_t  point;

    if (!player || gi.Argc() < 6) {
        return qtrue;
    }

    client = player->edict - g_entities;
    G_PhysicsDrop(client);

    if (!target) {
        gi.SendServerCommand(client, "physgrab_denied");
        return qtrue;
    }

    point[0] = atof(gi.Argv(2));
    point[1] = atof(gi.Argv(3));
    point[2] = atof(gi.Argv(4));

    {
        JPH::BodyLockRead lock(gphys_world.system->GetBodyLockInterface(), gphys_entities[target->entnum].id);

        if (!lock.Succeeded()) {
            gi.SendServerCommand(client, "physgrab_denied");
            return qtrue;
        }
        gphys_grab[client].local = JPH::Vec3(lock.GetBody().GetWorldTransform().Inversed() * JPH::RVec3(PhysToJolt(point)));
    }

    gphys_grab[client].entnum = target->entnum;
    gphys_grab[client].dist   = Q_clamp_float(atof(gi.Argv(5)), GPHYS_GRAB_MIN_DIST, GPHYS_GRAB_RANGE);
    G_PhysicsSetHeld(target->entnum, player->entnum);

    if (g_physics_log->integer) {
        gi.Printf("g_physics: player %d carries #%d, %.0f units off\n", client, target->entnum, gphys_grab[client].dist);
    }
    gphys_world.system->GetBodyInterface().ActivateBody(gphys_entities[target->entnum].id);
    return qtrue;
}

// physdrop: let go.
qboolean G_PhysicsDropCmd(gentity_t *ent)
{
    Entity *target = NULL;
    Player *player = G_PhysicsCommandTarget(ent, 0, &target);

    if (player) {
        G_PhysicsDrop(player->edict - g_entities);
    }
    return qtrue;
}

// physdist <distance>: carry it nearer or farther.
qboolean G_PhysicsDistCmd(gentity_t *ent)
{
    Entity *target = NULL;
    Player *player = G_PhysicsCommandTarget(ent, 0, &target);

    if (player && gi.Argc() >= 2) {
        gphys_grab[player->edict - g_entities].dist = Q_clamp_float(atof(gi.Argv(1)), GPHYS_GRAB_MIN_DIST, GPHYS_GRAB_RANGE);
    }
    return qtrue;
}

// physpunt <entity> <x> <y> <z> <speed>: throw it along the view, from that
// point; lighter things faster. Lets go of it first.
qboolean G_PhysicsPuntCmd(gentity_t *ent)
{
    Entity *target = NULL;
    Player *player = G_PhysicsCommandTarget(ent, 1, &target);
    Vector  fwd, point;
    float   mass, speed;

    if (!player || gi.Argc() < 6) {
        return qtrue;
    }

    G_PhysicsDrop(player->edict - g_entities);
    if (!target) {
        return qtrue;
    }

    mass = G_PhysicsBodyMass(gphys_entities[target->entnum].id);
    if (mass <= 0.0f) {
        return qtrue;
    }

    point = Vector(atof(gi.Argv(2)), atof(gi.Argv(3)), atof(gi.Argv(4)));
    speed = Q_clamp_float(atof(gi.Argv(5)), 0.0f, 2000.0f);
    player->GetViewAngles().AngleVectors(&fwd);

    G_PhysicsImpulse(target, point, fwd * (speed * Q_min(1.0f, 20.0f / mass) * mass));
    return qtrue;
}

// physnudge <entity> <x> <y> <z> <ix> <iy> <iz>: one of the client's props
// struck it, with that impulse, in kilograms times units a second.
qboolean G_PhysicsNudgeCmd(gentity_t *ent)
{
    Entity *target = NULL;
    Player *player = G_PhysicsCommandTarget(ent, 1, &target);
    Vector  point, impulse;
    float   mass, most;

    if (!player || !target || gi.Argc() < 8) {
        return qtrue;
    }

    mass = G_PhysicsBodyMass(gphys_entities[target->entnum].id);
    if (mass <= 0.0f) {
        return qtrue;
    }

    point   = Vector(atof(gi.Argv(2)), atof(gi.Argv(3)), atof(gi.Argv(4)));
    impulse = Vector(atof(gi.Argv(5)), atof(gi.Argv(6)), atof(gi.Argv(7)));

    // Never more than a hard throw would give it.
    most = mass * 400.0f;
    if (impulse.length() > most) {
        impulse *= most / impulse.length();
    }

    G_PhysicsImpulse(target, point, impulse);

    if (g_physics_log->integer > 1) {
        gi.Printf("g_physics: #%d struck by a client prop, %.0f u/s (%.1f kg)\n", target->entnum, impulse.length() / mass, mass);
    }
    return qtrue;
}

//=============================================================
// The physics editor
//=============================================================

// What the editor shows of an entity: 1 a body, 0 solid but not one, 2
// neither; -1 no kind of physics object.
static int G_PhysicsEditState(Entity *ent)
{
    float       mass;
    const char *why;

    if (!ent || G_PhysicsDecide(ent, &mass, &why) == PHYS_RULE_UNSET) {
        return -1;
    }
    if (G_PhysicsOwns(ent)) {
        return PHYS_RULE_MOVES;
    }
    return ent->edict->solid == SOLID_NOT ? PHYS_RULE_OFF : PHYS_RULE_FIXED;
}

static void G_PhysicsSendList(int client)
{
    std::string part;
    bool        first = true;

    for (int i = 0; i < MAX_GENTITIES; i++) {
        const int state = g_entities[i].inuse ? G_PhysicsEditState(g_entities[i].entity) : -1;

        if (state >= 0) {
            part += va(" %d:%d", i, state);
        }

        if (part.size() > 900 || (i == MAX_GENTITIES - 1 && (first || !part.empty()))) {
            gi.SendServerCommand(client, "physlist %d%s", first ? 1 : 0, part.c_str());
            part.clear();
            first = false;
        }
    }
}

// physlist
qboolean G_PhysicsListCmd(gentity_t *ent)
{
    Entity *target = NULL;
    Player *player = G_PhysicsCommandTarget(ent, 0, &target);

    if (player) {
        G_PhysicsSendList(player->edict - g_entities);
    }
    return qtrue;
}

// physinfo <entity>
qboolean G_PhysicsInfoCmd(gentity_t *ent)
{
    Entity     *target = NULL;
    Player     *player = G_PhysicsCommandTarget(ent, 0, &target);
    int         n;
    float       ruleMass, mass = 0.0f;
    const char *why;

    if (!player || gi.Argc() < 2) {
        return qtrue;
    }

    n = atoi(gi.Argv(1));
    if (n < 0 || n >= MAX_GENTITIES || !g_entities[n].inuse || !g_entities[n].entity) {
        return qtrue;
    }

    target = g_entities[n].entity;
    if (G_PhysicsDecide(target, &ruleMass, &why) == PHYS_RULE_UNSET) {
        why = "not a kind the physics moves";
    }
    if (G_PhysicsOwns(target)) {
        mass = G_PhysicsBodyMass(gphys_entities[n].id);
    }

    gi.SendServerCommand(
        player->edict - g_entities,
        "physinfo %d %d %.1f \"%s\" \"%s\" \"%s\" \"%s\"",
        n,
        G_PhysicsEditState(target),
        mass,
        target->getClassID(),
        target->model.length() ? target->model.c_str() : "none",
        G_PhysicsEntityKey(target),
        why
    );
    return qtrue;
}

// For a bug report: what the physics makes of an entity, on one line.
str G_PhysicsDescribe(Entity *ent)
{
    static const char *names[] = {"fixed", "moves", "off"};
    float              ruleMass, mass = 0.0f;
    const char        *why;
    const int          state = G_PhysicsEditState(ent);

    if (state < 0) {
        return "not a kind the physics moves";
    }

    G_PhysicsDecide(ent, &ruleMass, &why);
    if (G_PhysicsOwns(ent)) {
        mass = G_PhysicsBodyMass(gphys_entities[ent->entnum].id);
    }
    return va("%s, %.1f kg, key %s: %s", names[state], mass, G_PhysicsEntityKey(ent), why ? why : "");
}

// physrules: the file again, and every entity as it now says.
qboolean G_PhysicsRulesCmd(gentity_t *ent)
{
    Entity *target = NULL;
    Player *player = G_PhysicsCommandTarget(ent, 0, &target);
    int     added = 0, removed = 0, remade = 0;

    if (!player) {
        return qtrue;
    }

    G_PhysicsRulesLoad();

    for (int i = 0; i < MAX_GENTITIES; i++) {
        Entity     *e = g_entities[i].inuse ? g_entities[i].entity : NULL;
        float       ruleMass;
        const char *why;
        int         state;

        if (!e) {
            continue;
        }

        state = G_PhysicsDecide(e, &ruleMass, &why);
        if (state == PHYS_RULE_UNSET) {
            continue;
        }

        if (G_PhysicsOwns(e)) {
            if (state != PHYS_RULE_MOVES) {
                // Stays where it is now, as the map's own entity would.
                G_PhysicsRemoveEntity(e);
                removed++;
            } else if (ruleMass != gphys_entities[i].ruleMass) {
                G_PhysicsRemoveEntity(e);
                if (G_PhysicsAddEntity(e)) {
                    remade++;
                }
            }
            continue;
        }

        if (state != PHYS_RULE_MOVES) {
            continue;
        }

        if (e->IsSubclassOfItem() || e->isSubclassOf(HelmetObject)) {
            // Taken up by the next frame (G_PhysicsLooseThings).
            gphys_entities[i].refused = NULL;
        } else if (e->isSubclassOf(InteractObject)) {
            // As at spawn: made solid to shots too.
            ((InteractObject *)e)->SetupPhysics(NULL);
            added += G_PhysicsOwns(e) ? 1 : 0;
        } else if (G_PhysicsAddEntity(e)) {
            e->setMoveType(MOVETYPE_NONE);
            added++;
        }
    }

    if (g_physics_log->integer) {
        gi.Printf("g_physics: %s read again: %d entities became bodies, %d stopped being, %d were weighed again\n", GPHYS_RULES_FILE, added, removed, remade);
    }

    G_PhysicsSendList(player->edict - g_entities);
    return qtrue;
}

// Each step: what each player carries is pulled after the end of his beam.
static void G_PhysicsGrabStep(float dt)
{
    JPH::BodyInterface &bodies = gphys_world.system->GetBodyInterface();

    for (int client = 0; client < game.maxclients && client < MAX_CLIENTS; client++) {
        gphysGrab_t *g = &gphys_grab[client];
        Entity      *held;
        Player      *player;
        Vector       eye, fwd, target;
        trace_t      tr;
        JPH::Vec3    point, pointVel, accel, gravity;
        float        mass, limit;

        if (g->entnum < 0) {
            continue;
        }

        held   = g_entities[g->entnum].entity;
        player = (g_entities[client].entity && g_entities[client].entity->IsSubclassOfPlayer()) ? (Player *)g_entities[client].entity : NULL;
        if (!held || !player || !G_PhysicsOwns(held) || player->health <= 0) {
            G_PhysicsDrop(client);
            continue;
        }

        eye = player->EyePosition();
        player->GetViewAngles().AngleVectors(&fwd);
        target = eye + fwd * g->dist;

        // Short of walls and other props, as the beam is drawn.
        tr = G_Trace(eye, vec_zero, vec_zero, target, held, CONTENTS_SOLID, qfalse, "physics grab");
        if (tr.fraction < 1.0f) {
            target = Vector(tr.endpos) - fwd * 4.0f;
        }

        {
            JPH::BodyLockRead lock(gphys_world.system->GetBodyLockInterface(), gphys_entities[g->entnum].id);

            if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
                continue;
            }

            const JPH::Body& body = lock.GetBody();

            point    = JPH::Vec3(body.GetWorldTransform() * g->local);
            pointVel = body.GetPointVelocity(JPH::RVec3(point));
            mass     = 1.0f / Q_max(1e-4f, body.GetMotionProperties()->GetInverseMass());
        }

        {
            vec3_t t;

            VectorCopy(target, t);
            accel = (PhysToJolt(t) - point) * (GPHYS_GRAB_OMEGA * GPHYS_GRAB_OMEGA) - pointVel * (2.0f * GPHYS_GRAB_OMEGA);
        }
        gravity = -gphys_world.system->GetGravity();

        limit = GPHYS_GRAB_MAX_ACCEL * PHYS_UNITS_TO_METRES;
        if (accel.Length() > limit) {
            accel = accel.Normalized() * limit;
        }

        {
            JPH::Vec3   force = (accel + gravity) * mass;
            const float most  = GPHYS_GRAB_MAX_MASS * (limit + gravity.Length());

            if (force.Length() > most) {
                force = force.Normalized() * most;
            }

            bodies.AddImpulse(gphys_entities[g->entnum].id, force * dt, JPH::RVec3(point));
        }

        bodies.SetAngularVelocity(gphys_entities[g->entnum].id, bodies.GetAngularVelocity(gphys_entities[g->entnum].id) * GPHYS_GRAB_SPIN_KEEP);
        bodies.ActivateBody(gphys_entities[g->entnum].id);
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

    // Furniture the client has taken out of the collision model, or put back
    // (its physics editor puts the map back as it was).
    for (size_t i = 0; i < gphys_furniture.size(); i++) {
        gphysFurniture_t *f = &gphys_furniture[i];
        bool              solid;

        if (level.inttime < f->nextCheck) {
            continue;
        }
        f->nextCheck = level.inttime + 250;
        solid        = (gi.pointcontents(f->probe, ENTITYNUM_NONE) & CONTENTS_SOLID) != 0;

        if (f->id.IsInvalid()) {
            if (solid && f->shape) {
                JPH::BodyCreationSettings settings(
                    f->shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, PhysLayers::WORLD
                );

                f->id = gphys_world.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
                if (g_physics_log->integer) {
                    gi.Printf("g_physics: furniture %d is back in the collision model; so is its body\n", (int)i);
                }
            }
            continue;
        }

        if (solid) {
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

    G_PhysicsLooseThings();
    G_PhysicsFollowSentients();

    gphys_accum = Q_min(gphys_accum + frametime, dt * GPHYS_MAX_STEPS);
    {
        const int total = Q_min((int)(gphys_accum / dt), GPHYS_MAX_STEPS);

        while (steps < total) {
            G_PhysicsMoveSentients((float)(steps + 1) / total, dt);
            G_PhysicsGrabStep(dt);
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

    G_PhysicsPlayImpacts();

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
