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
// The water bodies float in (cg_water.cpp): where its surface is, how deep it
// is, and how its waves carry what floats in it.

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct {
        float surface; // the water's top over the point, flat (the waves are CG_WaterSwell's)
        float depth;   // from there to the bottom, or more than enough when it is deep
    } cgWaterSample_t;

    // The water at a point, if it is in water or a little under its top. One
    // look a body is enough: a body is small beside a wave.
    qboolean CG_WaterSample(const vec3_t p, cgWaterSample_t *out);

    // How far a wave lifts the water at a point above its flat top, now.
    float CG_WaterSwell(const vec3_t p);

    // How fast the water moves at a point, in units a second, where it is that
    // deep: the swell's to and fro, and in the surf the push towards the shore.
    void CG_WaterFlow(const vec3_t p, float depth, vec3_t out);

    // A body is in the water now, its middle there: it bleeds into it, a red
    // cloud that spreads round it from the moment it went in, or fully spread
    // at once for a body the map placed there long dead. body is the corpse's
    // entity and born when its ragdoll began, so a slot reused for another
    // body starts a cloud of its own.
    void CG_WaterBleed(int body, int born, const vec3_t at, qboolean longDead);

    // A body is lying on the ground, its middle there. Where the map has a
    // beach (water/<map>.txt), the sand under it soaks red, spreading from
    // when it lay down; at once for a body the map placed there long dead.
    void CG_WaterStain(int body, int born, const vec3_t at, qboolean longDead);

    // Once a frame: the clouds grow and drift, those of bodies gone from the
    // water fade, and the renderer is told where they are.
    void CG_WaterBloodFrame(void);

    // Something disturbs the water at a point this frame: a dip so deep and
    // so wide pressed into it, and foam left there. The renderer spreads it
    // out in ripples (RE_AddWaterRipples).
    void CG_WaterRipple(const vec3_t at, float width, float dip, float foam);

    // Something about so big goes into the water at a point, this fast.
    void CG_WaterSplash(const vec3_t at, float size, float speed);

    // Once a frame: what is wading and sailing through the water disturbs it,
    // and what disturbed it this frame goes to the renderer.
    void CG_WaterRipplesFrame(void);

    // Once a frame: the static dead the map placed go from under the bodies
    // the game put in their place.
    void CG_PlacedDeadFrame(void);

    extern cvar_t *cg_water_float;
    extern cvar_t *cg_water_floatrange;
    extern cvar_t *cg_water_blood;
    extern cvar_t *cg_water_ripples;

    void CG_InitWater(void);

#ifdef __cplusplus
}
#endif
