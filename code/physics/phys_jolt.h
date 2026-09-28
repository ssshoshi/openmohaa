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
// The physics shared by the client (props, corpses) and the server (entity
// props): Jolt, its layers, and the conversion between game units and metres.
// C++ only; include it before any of the game's headers.

#pragma once

// Jolt before anything of the game's: q_shared.h defines macros (LERP, Square)
// that share names with Jolt's functions.
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "../qcommon/q_shared.h"

// Game units (about an inch) to Jolt's metres, and back.
#define PHYS_UNITS_TO_METRES 0.0254f
#define PHYS_METRES_TO_UNITS (1.0f / PHYS_UNITS_TO_METRES)

static inline JPH::Vec3 PhysToJolt(const vec3_t v)
{
    return JPH::Vec3(v[0] * PHYS_UNITS_TO_METRES, v[1] * PHYS_UNITS_TO_METRES, v[2] * PHYS_UNITS_TO_METRES);
}

static inline void PhysFromJolt(JPH::Vec3Arg v, vec3_t out)
{
    out[0] = v.GetX() * PHYS_METRES_TO_UNITS;
    out[1] = v.GetY() * PHYS_METRES_TO_UNITS;
    out[2] = v.GetZ() * PHYS_METRES_TO_UNITS;
}

//=============================================================
// Layers
//=============================================================

// What collides with what. Debris, the smallest clutter, skips other debris so
// a spilled crate of shells costs nothing between the shells themselves.
namespace PhysLayers
{
inline constexpr JPH::ObjectLayer WORLD     = 0;
inline constexpr JPH::ObjectLayer PROP      = 1;
inline constexpr JPH::ObjectLayer DEBRIS    = 2;
inline constexpr JPH::ObjectLayer RAGDOLL   = 3;
inline constexpr JPH::ObjectLayer KINEMATIC = 4;
inline constexpr JPH::ObjectLayer COUNT     = 5;
} // namespace PhysLayers

namespace PhysBroadPhase
{
inline constexpr JPH::BroadPhaseLayer STATIC(0);
inline constexpr JPH::BroadPhaseLayer MOVING(1);
inline constexpr unsigned int         COUNT = 2;
} // namespace PhysBroadPhase

// What a body is, in its user data.
#define PHYS_USERDATA_WORLD     0
#define PHYS_USERDATA_FENCE     1
// A client prop: PHYS_USERDATA_PROP_BASE + its index in the registry. A server
// entity: PHYS_USERDATA_ENTITY_BASE + its entity number.
#define PHYS_USERDATA_PROP_BASE   1000
#define PHYS_USERDATA_ENTITY_BASE 100000
// A piece of the client's brushwork furniture: this + its index.
#define PHYS_USERDATA_FURNITURE_BASE 200000

// A world of its own, with everything Jolt needs to step it.
typedef struct {
    JPH::PhysicsSystem           *system;
    JPH::TempAllocatorImpl       *temp;
    JPH::JobSystemSingleThreaded *jobs;
} physWorld_t;

// Once a process: Jolt's allocator, factory and types. The trace function
// takes its messages.
void Phys_RegisterJolt(void (*trace)(const char *fmt, ...));
qboolean Phys_CreateWorld(physWorld_t *world, int maxBodies);
void     Phys_DestroyWorld(physWorld_t *world);

// Casts that see only the static world.
class PhysStaticOnlyBroadPhase final : public JPH::BroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::BroadPhaseLayer layer) const override { return layer == PhysBroadPhase::STATIC; }
};

class PhysStaticOnlyObjects final : public JPH::ObjectLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer == PhysLayers::WORLD; }
};

// Casts that pass through the kinematic boxes of players and AI: a ray from
// someone's eye or gun starts inside his own box.
class PhysNoKinematicObjects final : public JPH::ObjectLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer != PhysLayers::KINEMATIC; }
};

// The rotation that takes a model's own x, y and z to the given axes, and back.
JPH::Quat Phys_QuatFromAxis(const vec3_t axis[3]);
void      Phys_AxisFromQuat(JPH::QuatArg q, vec3_t axis[3]);
