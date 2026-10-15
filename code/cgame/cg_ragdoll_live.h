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
// Living bodies: a man who is shot, in single player, is for a moment a Jolt
// ragdoll that follows his animation, so the part hit gives way and comes back.
// Visual only, like the corpses: nothing reaches the server.

#pragma once

#include "cg_local.h"
#include "cg_ragdoll_rig.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_LiveRagdollInit(void);
    void CG_LiveRagdollShutdown(void);

    // Every flesh hit, as the corpses are told of them (dir into the body).
    void CG_LiveRagdollNoteHit(const vec3_t pos, const vec3_t dir, int large);

    // Every drawn character, after CG_RagdollUpdateEntity: a living one that
    // is reacting gets its pose here (model->bone_override).
    void CG_LiveRagdollUpdateEntity(centity_t *cent, refEntity_t *model);

    // The living body of a man who has just died, handed over to be his
    // corpse (a Jolt ragdoll handle), and forgotten here; 0 if he had none.
    int CG_LiveRagdollTakeBody(int entityNum);

    // From cg_ragdoll.cpp.
    qboolean CG_RagdollAnimJoints(refEntity_t *model, vec3_t p[RD_NUM_JOINTS], float radius[RD_NUM_JOINTS]);
    qboolean CG_RagdollBoneWorld(refEntity_t *model, int boneIndex, vec3_t pos, vec3_t axis[3]);
    void     CG_RagdollBoneModel(const refEntity_t *model, const vec3_t pos, const vec3_t axis[3], float out[4][3]);

#ifdef __cplusplus
}
#endif
