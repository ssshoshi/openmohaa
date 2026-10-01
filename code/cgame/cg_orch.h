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
// The live orchestrator (cg_orch.cpp): talk to an agent while playing.

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_OrchInit(void);
    // orch [on|off]: orchestrator mode.
    void CG_Orch_f(void);
    // orch_shot: a screenshot with what is under the crosshair.
    void CG_OrchShot_f(void);
    // orch_msg <text>: a reply from the agent, shown in the panel.
    void CG_OrchMsg_f(void);
    // orch_status <ready [key]|listening|thinking|speaking|muted|off>: the voice sidecar's state.
    void CG_OrchStatus_f(void);
    // orch_state: one line of JSON describing where the player is and looks.
    void CG_OrchState_f(void);
    // orch_freeze [here]: freeze the world (pause) and move a free camera
    // through it; again to resume where the player was, "here" to resume at
    // the camera.
    void CG_OrchFreeze_f(void);
    // orch_fly: walk or fly while frozen; noclip otherwise.
    void CG_OrchFly_f(void);
    // orch_return: back to where the player last froze.
    void CG_OrchReturn_f(void);
    // Whether the free camera is on: the player's body is drawn, not the view model.
    qboolean CG_OrchFreecamActive(void);
    // Moves the free camera and gives its view. qfalse when it is off.
    qboolean CG_OrchFreecamView(vec3_t origin, vec3_t angles);
    // Once a frame before the scene: outlines.
    void CG_OrchFrame(void);
    // Once a frame after it: the panel and the steps of a shot.
    void CG_OrchDraw2D(void);

#ifdef __cplusplus
}
#endif
