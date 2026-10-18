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
// The map's props: its static models, read from the BSP, each with a box and a
// hull fitted to its drawn mesh. Ragdolls collide with them, and the physics
// sets the small ones moving.

#pragma once

#include "cg_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CG_MAX_PROPS       2048
#define CG_PROP_MAX_HULL   26
#define CG_PROP_MAX_STANDINS 16

    typedef struct {
        char   name[64];
        int    staticIndex; // in LUMP_STATICMODELDEF, which is how the renderer knows it

        vec3_t origin;
        vec3_t angles;
        vec3_t axis[3];
        vec3_t mins, maxs;     // in the model's own frame
        vec3_t absmin, absmax; // the world box round it, for a cheap first test

        // The mesh's outermost points in the model's own frame, one for each
        // of 26 directions: enough for a convex hull that hugs a bottle or a
        // helmet better than the box does.
        float hull[CG_PROP_MAX_HULL][3];
        int   numHull;

        // Solid at all: ragdolls collide with it and the physics has a body
        // for it. Foliage, lights, wire and the very small and very big are
        // kept only so the physics editor can show them and have them made
        // solid (cg_physics_edit.cpp), and why they were left out.
        qboolean    solid;
        const char *why;   // what decided solid and dynamic
        float       mass;  // kilograms from a rule, or 0 for the physics' own

        qboolean dynamic; // small enough to be knocked about
        // Added in OPM
        //  A lamp hanging from the ceiling, on a wire or a chain: it swings
        //  from where it hangs (in its own frame), and is never dropped.
        qboolean hangs;
        vec3_t   hangPoint;
        // Added in OPM
        //  A lamp on a cord (CG_PropCorded): drawn as cordModel (the same lamp
        //  with no wire, 0 for its own model) at cordOffset and cordScale in its
        //  frame, and held by a cord of cordLength from cordAnchor in the world
        //  to cordTop in its frame, which goes slack when the lamp is knocked up.
        qboolean  corded;
        qhandle_t cordModel;
        vec3_t    cordOffset;
        float     cordScale;
        vec3_t    cordMins, cordMaxs; // the lamp, without its wire, in its frame
        vec3_t    cordTop;
        vec3_t    cordLight;          // its middle, where its light is
        vec3_t    cordAnchor;
        float     cordLength;
#define CG_PROP_MAX_CORONAS 4
        int       cordCoronas[CG_PROP_MAX_CORONAS];      // the glows at its bulb (cg_coronas), carried with it
        vec3_t    cordCoronaLocal[CG_PROP_MAX_CORONAS];  // where each is in its frame
        int       numCordCoronas;
        int      clipped; // the map's invisible clip brushes that stand in for it
        int      standIns[CG_PROP_MAX_STANDINS]; // their brush numbers
        int      numStandIns;
        int      body;    // the physics body, or -1

        // Added in OPM
        //  Its model and the map's scale for it, for decals on it
        //  (cg_marks_models.cpp).
        qhandle_t hModel;
        float     scale;
    } cgProp_t;

    extern cgProp_t cg_props[CG_MAX_PROPS];
    extern int      cg_numProps;

    // Added in OPM
    //  The map's coronas (static/corona_util.tik): the glow the maps put at
    //  a lamp's bulb, a sprite and no prop, which a lamp on a cord carries.
#define CG_MAX_CORONAS 1024
    typedef struct {
        int    staticIndex;
        vec3_t origin;
        vec3_t axis[3];
    } cgCorona_t;

    extern cgCorona_t cg_coronas[CG_MAX_CORONAS];
    extern int        cg_numCoronas;

    // Reads the current map's props, once a map.
    void CG_PropsLoad(void);
    // Forgets them, so the next load reads the map again.
    void CG_PropsReset(void);
    // Moves one: its origin, axes and world box.
    void CG_PropSetPose(int index, const vec3_t origin, const vec3_t axis[3]);
    // Whether a static model gets no collision at all, by its name.
    qboolean CG_PropSkipped(const char *name);
    int      CG_PropsFoliageNear(const vec3_t pos, float radius, vec3_t *points, float *near, int max);

#ifdef __cplusplus
}
#endif
