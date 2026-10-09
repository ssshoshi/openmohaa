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
// What the client's bodies sound like striking things: corpses, props and the
// brushwork furniture (see code/physics/phys_impact.cpp for the choice).
//
// The contact listener only notes a contact closing fast enough; nothing is
// traced or played from inside the step. After the steps the loudest few are
// kept, the surface each struck is found by a trace of the engine's own, and
// they are played. Corpses the particle solver carries (cg_ragdoll_solver 0,
// and every corpse while it blends out of its death animation) are offered
// from their own collision, which has the trace already.

#include "cg_physics_local.h"
#include "cg_props.h"
#include "cg_commands.h"
#include "../physics/phys_impact.h"

cvar_t *cg_physics_sounds;
cvar_t *cg_physics_soundvolume;
cvar_t *cg_physics_sounddebug;

// A world that has just been made, or a game restored, is quiet this long:
// everything in it drops the last fraction of a unit onto what holds it.
#define PS_QUIET_TIME 1500

// The parts of a corpse other than its head and trunk knock this much quieter.
#define PS_LIMB_VOLUME 0.35f

// A corpse of the particle solver, told apart from the Jolt bodies.
#define PS_PARTICLE_SOURCE (1ULL << 40)

static PhysImpactLimiter ps_limiter;

// cg_commands.cpp: whose sound PlaySound makes, and whose aliases it looks in
// first. An impact is nobody's.
extern int      current_entity_number;
extern dtiki_t *current_tiki;

void CG_PhysicsSoundsInit(void)
{
    cg_physics_sounds      = cgi.Cvar_Get("cg_physics_sounds", "1", CVAR_ARCHIVE);
    cg_physics_soundvolume = cgi.Cvar_Get("cg_physics_soundvolume", "1", CVAR_ARCHIVE);
    cg_physics_sounddebug  = cgi.Cvar_Get("cg_physics_sounddebug", "0", 0);

    ps_limiter.interval = 120;
    ps_limiter.perFrame = 6;
    ps_limiter.Clear();
}

void CG_PhysicsSoundsQuiet(void)
{
    ps_limiter.Quiet(cg.time + PS_QUIET_TIME);
}

static qboolean CG_PhysicsSoundsOn(void)
{
    return (cg_physics_sounds && cg_physics_sounds->integer) ? qtrue : qfalse;
}

static qboolean PS_IsRagdoll(JPH::uint64 data)
{
    return data >= PHYS_USERDATA_RAGDOLL_BASE ? qtrue : qfalse;
}

// Whether a part of a corpse is its head or trunk (pelvis, spine, chest, neck,
// head: the first six, as cg_physics_ragdoll.cpp numbers them).
static qboolean PS_IsTrunk(JPH::uint64 data)
{
    return (data - PHYS_USERDATA_RAGDOLL_BASE) % 32 < 6 ? qtrue : qfalse;
}

// What a body is made of, from its user data; qfalse for the world and the
// server's entities, which have surfaces rather than materials.
static qboolean PS_MaterialOf(JPH::uint64 data, physSoundMat_t *mat)
{
    if (PS_IsRagdoll(data)) {
        *mat = PHYS_SND_FLESH;
        return qtrue;
    }
    if (data >= PHYS_USERDATA_FURNITURE_BASE) {
        const physFurniture_t *f = CG_PhysicsFurnitureShape((int)(data - PHYS_USERDATA_FURNITURE_BASE));

        *mat = f ? CG_PhysicsSoundMatFor(f->shader) : PHYS_SND_WOOD;
        return qtrue;
    }
    if (data >= PHYS_USERDATA_ENTITY_BASE) {
        return qfalse;
    }
    if (data >= PHYS_USERDATA_PROP_BASE) {
        const int prop = (int)(data - PHYS_USERDATA_PROP_BASE);

        *mat = prop < cg_numProps ? CG_PhysicsSoundMatFor(cg_props[prop].name) : PHYS_SND_DEFAULT;
        return qtrue;
    }
    return qfalse;
}

// From the contact listener, inside the step: only noted.
void CG_PhysicsImpactContact(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold)
{
    physContact_t c;
    physImpact_t  impact;
    int           h;
    JPH::uint64   source;

    if (!CG_PhysicsSoundsOn() || (!a.IsDynamic() && !b.IsDynamic())) {
        return;
    }

    c = Phys_ContactFrom(a, b, manifold);
    if (c.sensor || c.closing <= 0.0f) {
        return;
    }

    // Its own parts touching are nothing to a corpse.
    if (PS_IsRagdoll(c.data[0]) && PS_IsRagdoll(c.data[1])
        && (c.data[0] - PHYS_USERDATA_RAGDOLL_BASE) / 32 == (c.data[1] - PHYS_USERDATA_RAGDOLL_BASE) / 32) {
        return;
    }

    // The one that moved into the other; of two moving, the faster.
    if (!c.dynamic[1]) {
        h = 0;
    } else if (!c.dynamic[0]) {
        h = 1;
    } else {
        h = c.approach[0] >= c.approach[1] ? 0 : 1;
    }

    impact.hitter    = c.data[h];
    impact.other     = c.data[!h];
    impact.hitterMat = PHYS_SND_DEFAULT; // after the limiter, for the few kept
    impact.mass      = c.mass[h];
    impact.speed     = c.closing;
    impact.strength  = Phys_ImpactStrength(c.closing, c.mass[h]);
    if (impact.strength <= 0.0f) {
        return;
    }

    PhysFromJolt(JPH::Vec3(c.point), impact.point);
    PhysFromJolt(h == 0 ? c.normal : -c.normal, impact.dir);
    VectorNormalize(impact.dir);

    if (PS_IsRagdoll(impact.hitter)) {
        if (!PS_IsTrunk(impact.hitter)) {
            impact.strength *= PS_LIMB_VOLUME;
        }
        // One sound a corpse at a time, whichever part.
        source = PHYS_USERDATA_RAGDOLL_BASE + (impact.hitter - PHYS_USERDATA_RAGDOLL_BASE) / 32 * 32;
    } else {
        source = impact.hitter;
    }

    ps_limiter.Offer(impact, source);
}

// From the particle solver's collision: a joint of corpse index struck the
// surface of trace at speed (units a second).
void CG_PhysicsRagdollImpact(int index, qboolean trunk, const trace_t *trace, float speed)
{
    physImpact_t impact;

    if (!CG_PhysicsSoundsOn()) {
        return;
    }

    impact.hitter    = PS_PARTICLE_SOURCE + index;
    impact.other     = PHYS_USERDATA_WORLD;
    impact.hitterMat = PHYS_SND_FLESH;
    // As much of the body as a Jolt corpse's part would be.
    impact.mass      = trunk ? 12.0f : 2.5f;
    impact.speed     = speed * PHYS_UNITS_TO_METRES;
    impact.strength  = Phys_ImpactStrength(impact.speed, impact.mass) * (trunk ? 1.0f : PS_LIMB_VOLUME);
    if (impact.strength <= 0.0f) {
        return;
    }

    VectorCopy(trace->endpos, impact.point);
    VectorNegate(trace->plane.normal, impact.dir);

    ps_limiter.Offer(impact, impact.hitter);
}

// The surface below an impact: traced into the world from the point it struck,
// as the engine has it, and water where the point is wet.
static const char *PS_SurfaceAt(const physImpact_t *impact)
{
    trace_t tr;
    vec3_t  start, end, above;
    int     contents;

    contents = cgi.CM_PointContents(impact->point, 0);
    if (contents & MASK_WATER) {
        VectorCopy(impact->point, above);
        above[2] += 16.0f;
        return (cgi.CM_PointContents(above, 0) & MASK_WATER) ? "wade" : "puddle";
    }

    VectorMA(impact->point, -4.0f, impact->dir, start);
    VectorMA(impact->point, 12.0f, impact->dir, end);
    cgi.CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID | CONTENTS_FENCE, qfalse);

    if (tr.fraction >= 1.0f || tr.startsolid) {
        // The Jolt world's hulls are rounded a little past the brushes; the
        // body falls default.
        return "stone";
    }

    return Phys_SurfaceName(tr.surfaceFlags);
}

// After the steps: the few worth hearing, played.
void CG_PhysicsPlayImpacts(void)
{
    std::vector<physImpact_t> heard;
    int                       rejected;

    ps_limiter.Take(cg.time, &heard, &rejected);

    if (cg_physics_sounddebug->integer && rejected) {
        cgi.Printf("physics sound: %d left out\n", rejected);
    }

    if (!CG_PhysicsSoundsOn()) {
        return;
    }

    for (size_t i = 0; i < heard.size(); i++) {
        physImpact_t  *impact = &heard[i];
        physSoundMat_t otherMat;
        const char    *surface;
        float          volume, pitch;
        std::string    alias;

        if (impact->hitterMat == PHYS_SND_DEFAULT && !PS_MaterialOf(impact->hitter, &impact->hitterMat)) {
            continue;
        }

        if (impact->other == PHYS_USERDATA_WORLD || impact->other == PHYS_USERDATA_FENCE) {
            surface = PS_SurfaceAt(impact);
        } else if (PS_MaterialOf(impact->other, &otherMat)) {
            surface = Phys_SurfaceOfMat(otherMat);
        } else {
            // A door, a lift, a man: the server's, and not ours to sound.
            continue;
        }

        alias = Phys_ImpactAlias(impact->hitterMat, surface, impact->mass, impact->strength, &volume, &pitch);
        volume *= cg_physics_soundvolume->value;

        if (cg_physics_sounddebug->integer) {
            cgi.Printf(
                "physics sound: %s, %s on %s, %.1f m/s, %.1f kg, volume %.2f pitch %.2f at %.0f %.0f %.0f\n",
                alias.c_str(),
                Phys_SoundMatName(impact->hitterMat),
                surface,
                impact->speed,
                impact->mass,
                volume,
                pitch,
                impact->point[0],
                impact->point[1],
                impact->point[2]
            );
        }

        if (volume > 0.0f) {
            const int      oldNumber = current_entity_number;
            dtiki_t *const oldTiki   = current_tiki;

            current_entity_number = -1;
            current_tiki          = NULL;
            commandManager.PlaySound(alias.c_str(), impact->point, CHAN_AUTO, volume, -1, pitch, 1);
            current_entity_number = oldNumber;
            current_tiki          = oldTiki;
        }
    }
}
