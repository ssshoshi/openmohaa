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

#include <vector>

typedef struct {
    std::vector<int>                brushes;  // world brushes it is made of
    std::vector<int>                surfaces; // world surfaces that draw it
    std::vector<std::vector<float>> hulls;    // convex pieces, three floats a point, in the world
    vec3_t                          mins, maxs;
    char                            shader[64]; // the visible shader with the most brushes
} physFurniture_t;

// The pieces of furniture in the world's brushwork. Found by shape: a group of
// touching detail brushes, small, with something seen on it, that stands on
// something and touches nothing else solid.
void Phys_FindFurniture(const void *bsp, long len, std::vector<physFurniture_t> *out);
