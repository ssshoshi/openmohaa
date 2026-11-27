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
// The TL-122 flashlight a player wears on the shirt (cg_flashlight.cpp).

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_InitFlashlight(void);

    // A player's body drawn this frame (renderfx: how the flashlight on it is
    // drawn, RF_FIRST_PERSON or RF_THIRD_PERSON for the player's own): if the
    // player's flashlight is on, it goes on the chest and shines.
    void CG_Flashlight(centity_t *cent, refEntity_t *body, int renderfx);

    // The HUD's icon while the player's own flashlight is on.
    void CG_DrawFlashlight(void);

#ifdef __cplusplus
}
#endif
