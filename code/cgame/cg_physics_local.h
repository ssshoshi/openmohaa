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
// What the physics files share: Jolt, its layers, the world, and the unit
// conversion. C++ only.

#pragma once

#include "../physics/phys_jolt.h"
#include "../physics/phys_world.h"
#include "../physics/phys_furniture.h"

#include "cg_physics.h"

#include <set>
#include <vector>

extern JPH::PhysicsSystem *phys_system;

// cg_physics.cpp
qboolean CG_PhysicsCanRemoveStandIns(void);
qboolean CG_PhysicsPropsMove(void);
qboolean CG_PhysicsClippedPropsMove(void);

// cg_physics_world.cpp: a brush out of the collision model (a prop's stand-in
// or furniture that moved), remembered so the physics editor can put it back.
void CG_PhysicsDisableBrush(int brushNum);

// cg_physics_world.cpp
void CG_PhysicsLoadWorld(void);
void CG_PhysicsUnloadWorld(void);
void CG_PhysicsDrawWorld(void);

// cg_physics_props.cpp
void CG_PhysicsLoadProps(void);
void CG_PhysicsUnloadProps(void);
void CG_PhysicsPropsStepped(void);
void CG_PhysicsDrawProps(float frac);
void CG_PhysicsImpulse(JPH::BodyID id, const vec3_t point, const vec3_t impulse);
void CG_PhysicsBlast(const vec3_t centre, float radius, float strength);
void CG_PhysicsMaterialFor(const char *name, float *arealDensity, float *friction, float *restitution);

extern cvar_t *cg_physics_furniture;

void     CG_PhysicsFindFurniture(const void *bsp, long len);
qboolean CG_PhysicsBrushIsFurniture(int brushNum);
void     CG_PhysicsLoadFurniture(void);
void     CG_PhysicsUnloadFurniture(void);
void     CG_PhysicsFurnitureStepped(void);
void     CG_PhysicsDrawFurniture(float frac);
int      CG_PhysicsFurnitureCount(void);
qboolean CG_PhysicsFurnitureBody(int index, JPH::BodyID *id, vec3_t middle);
qboolean CG_PhysicsFurnitureInfo(
    int index, int *firstBrush, qboolean *moves, const char **why, const char **shader, vec3_t mins, vec3_t maxs
);
void CG_PhysicsForgetDetachedFurniture(void);
// What the search made of each group of detail brushes at the last load, for
// phys_furniture to say why a piece is or is not furniture.
const std::vector<physFurnitureCheck_t>& CG_PhysicsFurnitureChecks(void);
const std::vector<physFurnitureBrush_t>& CG_PhysicsFurnitureBrushes(void);

// physics.txt (cg_physics_edit.cpp): read with the map, and what it says of the
// props and the brushwork furniture.
void     CG_PhysicsRulesLoad(void);
void     CG_PhysicsApplyPropRules(void);
qboolean CG_PhysicsFurnitureRule(int firstBrush, const char **why, float *mass);
// The brushes the map's rules say move, whose groups are furniture whatever
// their shape.
void     CG_PhysicsForcedFurniture(std::set<int> *out);
void     CG_PhysicsEditFrame(void);

// A server entity's box as it is drawn (its brush model, its solid box, or its
// model's bounds), and where a ray from start along dir enters a turned box.
qboolean CG_PhysicsEntityBox(int entnum, vec3_t origin, vec3_t axis[3], vec3_t mins, vec3_t maxs);
qboolean CG_PhysicsRayHitsBox(
    const vec3_t start, const vec3_t dir, float range, const vec3_t origin, const vec3_t axis[3], const vec3_t mins,
    const vec3_t maxs, float *enter
);

// The world again, as the rules now have it, without the ragdolls in it
// going (cg_physics_world.cpp); the props go back where the map put them.
void CG_PhysicsReloadWorld(void);

// A body's mass in kilograms, or 0 if it does not move; a prop's body.
float    CG_PhysicsBodyMass(JPH::BodyID id);
qboolean CG_PhysicsPropBody(int prop, JPH::BodyID *id);
// The static models that have moved from where the map put them.
void CG_PhysicsMovedStaticModels(std::vector<int> *out);

void CG_JoltRagdollsWake(void);

const physFurniture_t *CG_PhysicsFurnitureShape(int index);

void CG_PhysicsFollowMovers(void);
void CG_PhysicsLoadMoverModels(const void *bsp, long len);
void CG_PhysicsListenForContacts(void);
void CG_PhysicsSendNudges(void);
void CG_PhysicsMoveMovers(float frac, float dt);
void CG_PhysicsMoversHeld(void);
void CG_PhysicsUnloadMovers(void);
void CG_PhysicsPlayerPushes(void);
void CG_PhysicsPushBody(JPH::BodyID id, const vec3_t dir, float speed);
void CG_PhysicsPushPropsInBox(const vec3_t mins, const vec3_t maxs, const vec3_t dir, float speed);
void CG_PhysicsGrabStep(float dt);

void CG_JoltRagdollsStep(float dt);
void CG_JoltRagdollsUnload(void);
void CG_JoltRagdollContact(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold);

void CG_PhysicsLoadFills(const void *bsp, long len);
void CG_PhysicsUnloadFills(void);
void CG_PhysicsDrawFurnitureFill(int index, const vec3_t origin, const vec3_t axis[3]);
