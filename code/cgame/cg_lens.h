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
// What lands on the camera's lens (cg_lens.cpp): water, blood, dirt.

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    // Water was thrown up at a point, so much of it (a splash this wide):
    // near enough and in front, some of it lands on the lens.
    void CG_LensSplash(const vec3_t at, float size);

    // A wave broke over the side of a boat at a point: whoever is in the boat
    // is wet with it, whichever way he looks.
    void CG_LensSpray(const vec3_t at);

    // A man was hit at a point: near enough and in front, his blood lands on
    // the lens.
    void CG_LensFleshHit(const vec3_t at);

    // Something blew up at a point (kind as CG_RagdollNoteExplosion's): near
    // enough, the dirt it throws lands on the lens, or the water if it was in
    // water.
    void CG_LensBlast(const vec3_t at, int kind, qboolean inWater);

    // Once a frame: what is on the lens runs and fades, and the renderer is
    // told.
    void CG_LensFrame(void);

    extern cvar_t *cg_lens;

    void CG_InitLens(void);

#ifdef __cplusplus
}
#endif
