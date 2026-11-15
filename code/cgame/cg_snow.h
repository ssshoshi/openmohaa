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
// What the cold does on a map where snow falls (cg_snow.cpp).

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    void CG_InitSnow(void);

    // It is snowing (the rain's density, with a snowflake for its shader):
    // from then on the map is a snow map.
    void CG_SnowFalls(void);

    // Whether snow has fallen on this map.
    qboolean CG_SnowMap(void);

    // A living thing drawn this frame: a man breathes out, and his breath
    // shows in the cold.
    void CG_BreathEntity(centity_t *cent, const refEntity_t *model);

    // A step came down here: on snow it leaves its print.
    void CG_SnowFootprint(const centity_t *ent, const trace_t *trace);

    // Once a frame: the player's own breath.
    void CG_SnowFrame(void);

#ifdef __cplusplus
}
#endif
