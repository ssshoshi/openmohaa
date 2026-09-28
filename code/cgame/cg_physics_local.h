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

extern JPH::PhysicsSystem *phys_system;

// cg_physics.cpp
qboolean CG_PhysicsCanRemoveStandIns(void);
qboolean CG_PhysicsClippedPropsMove(void);

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

void CG_PhysicsLoadFills(const void *bsp, long len);
void CG_PhysicsUnloadFills(void);
void CG_PhysicsDrawFurnitureFill(int index, const vec3_t origin, const vec3_t axis[3]);
