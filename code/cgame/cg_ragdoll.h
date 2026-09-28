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
// Client-side ragdoll simulation for dead characters.
//
// The simulation is purely visual and never leaves the client: the server
// keeps its own bounding-box corpse, so hit detection, corpse sinking and
// removal are unaffected, and nothing extra travels over the network.

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_InitRagdoll(void);
    void CG_ShutdownRagdoll(void);

    // Called for every drawn character entity, just before it is handed to the
    // renderer. Starts, advances and installs the ragdoll pose as needed, and
    // fills in model->bone_override / model->num_bone_overrides on success.
    void CG_RagdollUpdateEntity(centity_t *cent, refEntity_t *model);

    // Called when an entity is reset (teleport, or re-entering the snapshot) so a
    // stale ragdoll cannot survive on a recycled entity slot.
    void CG_RagdollEntityReset(centity_t *cent);

    // Once a frame, after the packet entities: draws the corpses whose entities
    // the server has stopped sending because it thinks they are out of sight.
    void CG_RagdollAddUnsent(void);

    // Records a flesh hit so a body dying just after it can be pushed by the
    // shot. dir is the outward normal, as the message parser stores it.
    void CG_RagdollNoteFleshImpact(const vec3_t pos, const vec3_t dir, int large);
    void CG_RagdollNoteExplosion(const vec3_t pos, int kind);
    // A round's path, after the fact. Shoves the corpses it passes through and
    // returns qtrue, with stopAt filled in, if it went into one and stopped.
    qboolean CG_RagdollNoteBullet(const vec3_t start, const vec3_t end, int large, vec3_t stopAt);

    // The grabber, a tractor beam for handling corpses by hand. See the grabber
    // section of cg_ragdoll.cpp.
    void CG_RagdollGrabDown_f(void);
    void CG_RagdollGrabUp_f(void);
    void CG_RagdollGrabNearer_f(void);
    void CG_RagdollGrabFarther_f(void);
    void CG_RagdollPunt_f(void);

    extern cvar_t *cg_ragdoll;
    extern cvar_t *cg_ragdoll_maxcount;
    extern cvar_t *cg_ragdoll_log;
    extern cvar_t *cg_ragdoll_blendtime;
    extern cvar_t *cg_ragdoll_impulse;
    extern cvar_t *cg_ragdoll_duration;
    extern cvar_t *cg_ragdoll_physicsrate;
    extern cvar_t *cg_ragdoll_iterations;
    extern cvar_t *cg_ragdoll_damping;
    extern cvar_t *cg_ragdoll_friction;
    extern cvar_t *cg_ragdoll_bounce;
    extern cvar_t *cg_ragdoll_bodybounce;
    extern cvar_t *cg_ragdoll_limbdamp;
    extern cvar_t *cg_ragdoll_sleepvel;
    extern cvar_t *cg_ragdoll_sleeptime;
    extern cvar_t *cg_ragdoll_debug;
    extern cvar_t *cg_ragdoll_stiffness;

#ifdef __cplusplus
}
#endif
