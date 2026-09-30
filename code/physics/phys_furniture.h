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
// Furniture built into the map's brushwork: tables, benches and crates made of
// world brushes. See phys_furniture.cpp.

#pragma once

#include "phys_jolt.h"
#include "../qcommon/qfiles.h"

#include <set>
#include <vector>

typedef struct {
    std::vector<int>                brushes;  // world brushes it is made of
    std::vector<int>                surfaces; // world surfaces that draw it
    std::vector<std::vector<float>> hulls;    // convex pieces, three floats a point, in the world
    vec3_t                          mins, maxs;
    char                            shader[64]; // the visible shader with the most brushes
    bool                            forced;     // a rule made it furniture, whatever its shape
} physFurniture_t;

// What the search made of one group of touching detail brushes with something
// visible on it, for the editor to say why a piece is, or is not, furniture.
typedef struct {
    int    firstBrush; // the brush a rule names it by
    int    numBrushes;
    int    other;      // the brush it touches, when that is why; else -1
    vec3_t mins, maxs;
    bool   furniture;
    char   verdict[96];
} physFurnitureCheck_t;

// One visible detail brush and the group it fell in, for naming single brushes
// when a group is the whole building.
typedef struct {
    int    num;
    int    group; // its group's firstBrush
    vec3_t mins, maxs;
    char   shader[64];
} physFurnitureBrush_t;

// The pieces of furniture in the world's brushwork. Found by shape: a group of
// touching detail brushes, small, with something seen on it, that stands on
// something and touches nothing else solid.
//
// The brushes in forced (rules said they move) are taken out of the groups
// they would join and grouped only with each other: a sign and its post come
// away from the beam they touch, and are furniture whatever their shape, as
// long as something draws them. checks, when given, gets a verdict for every
// group with something visible on it, and brushes every visible detail brush.
void Phys_FindFurniture(
    const void *bsp, long len, std::vector<physFurniture_t> *out, const std::set<int> *forced = NULL,
    std::vector<physFurnitureCheck_t> *checks = NULL, std::vector<physFurnitureBrush_t> *brushes = NULL
);
