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
// The sound of a body striking something.
//
// Both worlds listen to their contacts and hand the ones that close fast
// enough here. What is heard is chosen by what moved into what: a corpse plays
// the retail body falls, which come for every surface the maps have; a prop
// plays its own material (phys_<material>_<light|heavy|soft>, aliases of our
// own in ubersound/opm_physics.scr), dulled on soft ground. How loud goes with
// the speed and the weight, how high with the weight.
//
// The Jolt world keeps no surface flags (the brushes are merged into one shape
// a cell), so the surface struck is found by the caller with a trace of the
// engine's own at the point of contact, once the step is over.

#include "phys_impact.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Slower than this, in metres a second along the normal, nothing is heard: a
// body settling, rolling or sliding. A fall of five centimetres is one metre a
// second.
#define PI_MIN_SPEED   0.8f
// At this speed and over a knock is at its loudest for the weight.
#define PI_FULL_SPEED  5.0f
// A thing this heavy, in kilograms, knocks at full weight; lighter, quieter.
#define PI_FULL_MASS   20.0f
// Heavier than this a prop plays its heavy set.
#define PI_HEAVY_MASS  8.0f
// What soft ground leaves of a knock.
#define PI_SOFT_VOLUME 0.6f

static float PI_Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

const char *Phys_SoundMatName(physSoundMat_t mat)
{
    switch (mat) {
    case PHYS_SND_WOOD:
        return "wood";
    case PHYS_SND_METAL:
        return "metal";
    case PHYS_SND_GLASS:
        return "glass";
    case PHYS_SND_PAPER:
        return "paper";
    case PHYS_SND_STONE:
        return "stone";
    case PHYS_SND_FLESH:
        return "flesh";
    default:
        return "default";
    }
}

const char *Phys_SurfaceName(int surfaceFlags)
{
    // In the order of CG_BodyFallSound's choice, which is a switch on the whole
    // mask: a surface with two flags is stone there, and is here.
    switch (surfaceFlags & MASK_SURF_TYPE) {
    case SURF_FOLIAGE:
        return "foliage";
    case SURF_SNOW:
        return "snow";
    case SURF_CARPET:
        return "carpet";
    case SURF_SAND:
        return "sand";
    case SURF_PUDDLE:
        return "puddle";
    case SURF_GLASS:
        return "glass";
    case SURF_GRAVEL:
        return "gravel";
    case SURF_MUD:
        return "mud";
    case SURF_DIRT:
        return "dirt";
    case SURF_GRILL:
        return "grill";
    case SURF_GRASS:
        return "grass";
    case SURF_PAPER:
        return "paper";
    case SURF_WOOD:
        return "wood";
    case SURF_METAL:
        return "metal";
    case SURF_ROCK:
    default:
        return "stone";
    }
}

const char *Phys_SurfaceOfMat(physSoundMat_t mat)
{
    switch (mat) {
    case PHYS_SND_WOOD:
        return "wood";
    case PHYS_SND_METAL:
        return "metal";
    case PHYS_SND_GLASS:
        return "glass";
    case PHYS_SND_PAPER:
        return "paper";
    case PHYS_SND_FLESH:
        // Another body: a dull thud, which the dirt falls are.
        return "dirt";
    case PHYS_SND_STONE:
    default:
        return "stone";
    }
}

bool Phys_SurfaceIsSoft(const char *surface)
{
    static const char *soft[] = {"foliage", "snow", "carpet", "sand", "mud", "dirt", "grass", "paper", NULL};

    for (int i = 0; soft[i]; i++) {
        if (!strcmp(surface, soft[i])) {
            return true;
        }
    }
    return false;
}

physContact_t Phys_ContactFrom(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold)
{
    physContact_t c;
    const JPH::Body *bodies[2] = {&a, &b};

    c.point  = manifold.GetWorldSpaceContactPointOn1(0);
    c.normal = manifold.mWorldSpaceNormal;
    // The normal points from a to b, so a closing on b moves along it.
    c.approach[0] = a.GetPointVelocity(c.point).Dot(c.normal);
    c.approach[1] = -b.GetPointVelocity(c.point).Dot(c.normal);
    c.closing     = c.approach[0] + c.approach[1];
    c.sensor      = a.IsSensor() || b.IsSensor();

    for (int i = 0; i < 2; i++) {
        c.data[i]    = bodies[i]->GetUserData();
        c.dynamic[i] = bodies[i]->IsDynamic();
        c.mass[i]    = 0.0f;
        if (c.dynamic[i]) {
            const float inv = bodies[i]->GetMotionProperties()->GetInverseMass();
            c.mass[i]       = inv > 0.0f ? 1.0f / inv : 0.0f;
        }
    }

    return c;
}

float Phys_ImpactStrength(float speed, float mass)
{
    float bySpeed, byMass;

    if (speed < PI_MIN_SPEED) {
        return 0.0f;
    }

    // Even the slowest knock that is heard is heard.
    bySpeed = 0.15f + 0.85f * PI_Clamp((speed - PI_MIN_SPEED) / (PI_FULL_SPEED - PI_MIN_SPEED), 0.0f, 1.0f);
    byMass  = PI_Clamp(0.3f + 0.7f * sqrtf(Q_max(mass, 0.0f) / PI_FULL_MASS), 0.3f, 1.0f);

    return bySpeed * byMass;
}

std::string Phys_ImpactAlias(physSoundMat_t mat, const char *surface, float mass, float strength, float *volume, float *pitch)
{
    const bool  water = !strcmp(surface, "wade") || !strcmp(surface, "puddle");
    const bool  soft  = Phys_SurfaceIsSoft(surface);
    std::string alias;

    *volume = strength;

    if (mat == PHYS_SND_FLESH) {
        *pitch = 1.0f + 0.05f * crandom();
        return std::string("snd_bodyfall_") + surface;
    }

    // Small things ring higher: an octave of weight is a few semitones.
    *pitch = PI_Clamp(1.15f - 0.075f * log2f(Q_max(mass, 0.1f)), 0.85f, 1.3f) * (1.0f + 0.06f * crandom());

    if (water) {
        return "grenade_bounce_water";
    }

    alias = std::string("phys_") + Phys_SoundMatName(mat) + "_";
    if (soft) {
        *volume *= PI_SOFT_VOLUME;
        alias += "soft";
    } else {
        alias += mass >= PI_HEAVY_MASS ? "heavy" : "light";
    }

    return alias;
}

//=============================================================
// Keeping them few
//=============================================================

PhysImpactLimiter::PhysImpactLimiter()
    : interval(120)
    , perFrame(6)
    , quietUntil(0)
{}

void PhysImpactLimiter::Quiet(int untilTime)
{
    quietUntil = untilTime;
    offered.clear();
}

void PhysImpactLimiter::Offer(const physImpact_t& impact, JPH::uint64 source)
{
    Offered o;

    o.impact = impact;
    o.source = source;
    offered.push_back(o);
}

void PhysImpactLimiter::Take(int now, std::vector<physImpact_t> *out, int *rejected)
{
    *rejected = 0;
    out->clear();

    if (offered.empty()) {
        return;
    }

    if (now < quietUntil) {
        *rejected = (int)offered.size();
        offered.clear();
        return;
    }

    std::stable_sort(offered.begin(), offered.end(), [](const Offered& x, const Offered& y) {
        return x.impact.strength > y.impact.strength;
    });

    for (size_t i = 0; i < offered.size(); i++) {
        std::map<JPH::uint64, int>::iterator it = lastTime.find(offered[i].source);

        // A clock that went back is a new game.
        if (it != lastTime.end() && it->second <= now && now - it->second < interval) {
            (*rejected)++;
            continue;
        }
        if ((int)out->size() >= perFrame) {
            (*rejected)++;
            continue;
        }

        lastTime[offered[i].source] = now;
        out->push_back(offered[i].impact);
    }
    offered.clear();

    // What has not sounded for a while needs no remembering.
    if (lastTime.size() > 1024) {
        for (std::map<JPH::uint64, int>::iterator it = lastTime.begin(); it != lastTime.end();) {
            if (it->second > now || now - it->second >= interval) {
                it = lastTime.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void PhysImpactLimiter::Clear()
{
    offered.clear();
    lastTime.clear();
    quietUntil = 0;
}
