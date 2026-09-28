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
// Setting up a Jolt world, for either module.

#include "phys_jolt.h"

#include <cstdarg>

class PhysBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
{
public:
    unsigned int GetNumBroadPhaseLayers() const override { return PhysBroadPhase::COUNT; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == PhysLayers::WORLD ? PhysBroadPhase::STATIC : PhysBroadPhase::MOVING;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == PhysBroadPhase::STATIC ? "static" : "moving";
    }
#endif
};

class PhysObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override
    {
        // The world never looks for anything; moving things look at both.
        return layer != PhysLayers::WORLD || broad == PhysBroadPhase::MOVING;
    }
};

class PhysObjectPairs final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a == PhysLayers::WORLD && b == PhysLayers::WORLD) {
            return false;
        }
        if (a == PhysLayers::DEBRIS && b == PhysLayers::DEBRIS) {
            return false;
        }
        // Kinematic bodies are driven, not simulated: they push, and nothing
        // needs to push them back.
        const bool driven1 = a == PhysLayers::KINEMATIC || a == PhysLayers::PEOPLE;
        const bool driven2 = b == PhysLayers::KINEMATIC || b == PhysLayers::PEOPLE;

        if ((driven1 && (driven2 || b == PhysLayers::WORLD)) || (driven2 && a == PhysLayers::WORLD)) {
            return false;
        }
        // People walk over corpses without moving them, as they always have.
        if ((a == PhysLayers::PEOPLE && b == PhysLayers::RAGDOLL) || (b == PhysLayers::PEOPLE && a == PhysLayers::RAGDOLL)) {
            return false;
        }
        return true;
    }
};

static PhysBroadPhaseLayers   phys_broadPhaseLayers;
static PhysObjectVsBroadPhase phys_objectVsBroadPhase;
static PhysObjectPairs        phys_objectPairs;
static void (*phys_trace)(const char *fmt, ...);

static void Phys_Trace(const char *fmt, ...)
{
    char    text[1024];
    va_list ap;

    if (!phys_trace) {
        return;
    }

    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    phys_trace("%s", text);
}

void Phys_RegisterJolt(void (*trace)(const char *fmt, ...))
{
    static qboolean registered;

    phys_trace = trace;

    if (registered) {
        return;
    }

    JPH::RegisterDefaultAllocator();
    JPH::Trace              = Phys_Trace;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    registered = qtrue;
}

#define PHYS_MAX_BODY_PAIRS    16384
#define PHYS_MAX_CONTACTS      8192
#define PHYS_TEMP_ALLOCATOR_MB 16

qboolean Phys_CreateWorld(physWorld_t *world, int maxBodies)
{
    world->temp   = new JPH::TempAllocatorImpl(PHYS_TEMP_ALLOCATOR_MB * 1024 * 1024);
    world->jobs   = new JPH::JobSystemSingleThreaded(JPH::cMaxPhysicsJobs);
    world->system = new JPH::PhysicsSystem();
    world->system->Init(
        maxBodies, 0, PHYS_MAX_BODY_PAIRS, PHYS_MAX_CONTACTS, phys_broadPhaseLayers, phys_objectVsBroadPhase, phys_objectPairs
    );

    return qtrue;
}

void Phys_DestroyWorld(physWorld_t *world)
{
    delete world->system;
    delete world->jobs;
    delete world->temp;
    world->system = NULL;
    world->jobs   = NULL;
    world->temp   = NULL;
}

JPH::Quat Phys_QuatFromAxis(const vec3_t axis[3])
{
    const JPH::Mat44 m(
        JPH::Vec4(axis[0][0], axis[0][1], axis[0][2], 0.0f),
        JPH::Vec4(axis[1][0], axis[1][1], axis[1][2], 0.0f),
        JPH::Vec4(axis[2][0], axis[2][1], axis[2][2], 0.0f),
        JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)
    );

    return m.GetQuaternion().Normalized();
}

void Phys_AxisFromQuat(JPH::QuatArg q, vec3_t axis[3])
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
