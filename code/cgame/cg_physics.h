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
// Client-side rigid body physics (Jolt) for the map's props, and later its
// corpses. Like the ragdolls it is purely visual: the server never hears of
// it, so it works against any server and changes nothing about the game.

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_InitPhysics(void);
    void CG_ShutdownPhysics(void);

    // Once a frame: advances the simulation to cg.time.
    void CG_PhysicsFrame(void);

    // A round's traced path, and an explosion (kind as for the ragdolls):
    // props in the way are knocked about.
    void CG_PhysicsNoteBullet(const vec3_t start, const vec3_t end, int large);
    void CG_PhysicsNoteExplosion(const vec3_t pos, int kind);

    // The grabber (see the ragdolls' +rdgrab) on props: the nearest dynamic
    // body along a ray and how far; taking hold of it; whether one is held,
    // letting go, how far it is carried, where it is pulled to and where the
    // point held is now; and throwing what is held or knocking what the ray
    // finds, lighter things faster.
    qboolean CG_PhysicsGrabCandidate(const vec3_t start, const vec3_t dir, float range, float *entry);
    qboolean CG_PhysicsGrabStart(const vec3_t start, const vec3_t dir, float range, float minDist);
    qboolean CG_PhysicsGrabHeld(void);
    void     CG_PhysicsGrabRelease(void);
    float   *CG_PhysicsGrabDistance(void);
    void     CG_PhysicsGrabSetTarget(const vec3_t target);
    void     CG_PhysicsGrabPoint(vec3_t out);
    qboolean CG_PhysicsPunt(const vec3_t start, const vec3_t dir, float range, float speed);

    // For each brush entity drawn: covers the sides it was built with nodraw
    // or caulk on, once it has moved from where it stood.
    void CG_PhysicsDrawModelFill(int entnum, int model, const vec3_t origin, const vec3_t angles);

    // Console: phys_selftest [rays], phys_poke [speed], phys_blast [radius] [speed]
    void CG_PhysicsSelftest_f(void);
    void CG_PhysicsPoke_f(void);
    void CG_PhysicsBlast_f(void);
    void CG_PhysicsList_f(void);

    extern cvar_t *cg_physics;
    extern cvar_t *cg_physics_log;
    extern cvar_t *cg_physics_debug;
    extern cvar_t *cg_physics_props;
    extern cvar_t *cg_physics_clipped;

#ifdef __cplusplus
}
#endif
