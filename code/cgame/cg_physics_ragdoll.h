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
// The Jolt ragdoll (cg_physics_ragdoll.cpp), as the particle solver sees it:
// made from where its joints are and how fast they are going, it hands back
// where they are now, what they rest on, and takes pushes and the grabber. No
// Jolt types here, so cg_ragdoll.cpp (and the ragdoll harness) need none.

#pragma once

#include "cg_ragdoll_rig.h"

// A body in the physics world, from its joints (units, units a second) and
// their radii. For toneTime seconds its joints are held towards the pose it
// has now, less and less (the death pose's hold, which the particle solver
// lets go of over cg_ragdoll_limptime). Returns a handle, or 0 if it could not
// be made (no physics world, or a degenerate pose), in which case the
// particles carry on.
int CG_JoltRagdollCreate(
    const vec3_t p[RD_NUM_JOINTS], const vec3_t v[RD_NUM_JOINTS], const float radius[RD_NUM_JOINTS], float toneTime
);
void CG_JoltRagdollDestroy(int handle);

// Where the joints are now and how fast they are going; and, since the last
// read, which joints touched something (and the surface's normal there, the
// way it holds the joint up) and which lay on another body (a mask).
qboolean CG_JoltRagdollRead(
    int handle, vec3_t p[RD_NUM_JOINTS], vec3_t v[RD_NUM_JOINTS], qboolean contact[RD_NUM_JOINTS],
    vec3_t contactNormal[RD_NUM_JOINTS], int *onBodyMask
);

// A change of velocity at each joint, units a second: a shot, a blast, a punt.
void CG_JoltRagdollAddVelocity(int handle, const vec3_t dv[RD_NUM_JOINTS]);

// Carried by the grabber by this joint towards target, or let go (joint -1).
void CG_JoltRagdollHold(int handle, int joint, const vec3_t target);

// Whether any part is moving in the physics world, and how fast the fastest
// is going (units a second); putting it to sleep there; waking it.
qboolean CG_JoltRagdollAwake(int handle);
float    CG_JoltRagdollSpeed(int handle);
void     CG_JoltRagdollSleep(int handle);
void     CG_JoltRagdollWake(int handle);

// Each joint's swing, twist and hinge angle as Jolt has them, for debugging.
void CG_JoltRagdollReport(int handle, void (*print)(const char *fmt, ...));

// Added in OPM
//  A chain of capsules, joined end to end: a part cut off a body (cg_gore.cpp),
//  as the ragdoll's limbs are made. Its points run from the cut to the end of
//  the part; each piece is as thick as the larger of its two ends' radii.
#define PHYS_CHAIN_MAX_PTS 4

int      CG_JoltChainCreate(int numPts, const vec3_t *p, const vec3_t *v, const float *radius, float mass);
void     CG_JoltChainDestroy(int handle);
// The points where the pieces are now, numPts of them; qfalse if it is gone.
qboolean CG_JoltChainRead(int handle, vec3_t *p);
// A push, in game units a second, on the piece nearest point.
void     CG_JoltChainAddVelocity(int handle, const vec3_t point, const vec3_t dv);
qboolean CG_JoltChainAwake(int handle);
