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
// Building a physics world from a BSP. See phys_world.cpp.

#pragma once

#include "phys_jolt.h"
#include "../qcommon/qfiles.h"

#include <vector>

typedef struct {
    // The brush contents that become collision.
    int contents;

    // Asked of every world brush before it is added: true leaves it out. May
    // be NULL.
    qboolean (*skipBrush)(void *ctx, int brushNum, const char *shader, int contents, const vec3_t mins, const vec3_t maxs);

    // Every brush face, patch quad and terrain square, for drawing. May be
    // NULL. kind: 0 brush, 1 patch, 2 terrain.
    void (*outline)(void *ctx, const vec3_t *points, int numPoints, int kind);

    void *ctx;
} physBspOptions_t;

typedef struct {
    int brushes, brushFails, skipped, patches, terrain, triangles, bodies;
} physBspStats_t;

// The world model's brushes, patches and terrain, as static bodies added to
// system; their ids are appended to bodies.
void Phys_BuildBspWorld(
    JPH::PhysicsSystem       *system,
    const void               *bsp,
    long                      len,
    const physBspOptions_t   *opt,
    std::vector<JPH::BodyID> *bodies,
    physBspStats_t           *stats
);

// The brush models other than the world (doors, crates, barrels...): the
// corners of their brushes, in the model's own space.
typedef struct {
    std::vector<float> corners; // three floats a corner
    vec3_t             mins, maxs;
} physInlineModel_t;

// models[n] is inline model *n; models[0], the world, is left empty.
void Phys_ReadInlineModels(const void *bsp, long len, std::vector<physInlineModel_t> *models);

// The corners of a brush: where three of its planes meet, inside the rest.
int Phys_BrushCorners(const dplane_t *planes, const int *sides, int numSides, vec3_t *out, int maxOut);
