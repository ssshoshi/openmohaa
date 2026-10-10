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
// Gore, after Soldier of Fortune II: wounds that stay where the round went in
// (and out) on a body, alive or dead, bleeding, and pools under the dead.
// Client side and visual only, like the ragdolls.

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_GoreInit(void);
    // A new map or a restart: every wound, drop and pool goes.
    void CG_GoreClear(void);

    // Every character handed to the renderer, as it was handed over. dead is
    // whether it is a corpse.
    void CG_GoreAddEntity(centity_t *cent, const refEntity_t *model, qboolean dead);
    // Just before: folds away the parts he has lost and dents his head.
    void CG_GoreModifyEntity(centity_t *cent, refEntity_t *model, qboolean dead);
    // Whether an entity on parentEntity's tag goes with a part he has lost (a
    // rifle in a hand that is gone).
    qboolean CG_GoreHidesAttachment(int parentEntity, int tag);

    // A round into flesh: where, and the way it was going.
    void CG_GoreNoteHit(const vec3_t pos, const vec3_t dir, int large);
    void CG_GoreNoteExplosion(const vec3_t pos, int kind);
    // A round's path: the parts cut off that it goes through are knocked along it.
    void CG_GoreNoteBullet(const vec3_t start, const vec3_t end, int large);
    // The grabber (cg_ragdoll_grab) and the parts cut off: one along the aim,
    // how far; taking hold of it; knocking it along the aim.
    qboolean CG_GoreGrabCandidate(const vec3_t start, const vec3_t dir, float range, float *entry);
    qboolean CG_GoreGrabStart(const vec3_t start, const vec3_t dir, float range, float minDist);
    qboolean CG_GorePunt(const vec3_t start, const vec3_t dir, float range, float speed);

    // A corpse taken over by another entity (CG_RagdollAdopt) keeps its wounds.
    void CG_GoreTransfer(int fromEntity, int toEntity);

    // gore_sever <part|all> [explode]: cuts a part off the body nearest the
    // middle of the view, for trying it out.
    void CG_GoreSever_f(void);
    // gore_blast [kind]: what the client does with an explosion (0 grenade to 3
    // tank), where the crosshair meets the world: bodies and parts thrown, limbs
    // blown off. Nothing is hurt; for testing.
    void CG_GoreBlast_f(void);

    // Once a frame, after every entity: places new wounds and draws them all.
    void CG_GoreAddToScene(void);

#ifdef __cplusplus
}
#endif
