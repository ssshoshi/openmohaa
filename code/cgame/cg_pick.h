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
// What is under the crosshair: the world surface, a static model or an
// entity, for in-game reports (cg_bugreport.cpp) and the orchestrator
// (cg_orch.cpp). C++ only.

#pragma once

#include "cg_local.h"

#include <string>

enum {
    PICK_TARGET_NONE,
    PICK_TARGET_WORLD,
    PICK_TARGET_PROP,
    PICK_TARGET_ENTITY
};

typedef struct {
    int    kind;
    int    index; // prop or entity number
    vec3_t origin, axis[3], mins, maxs;
    char   model[MAX_QPATH];
} pickTarget_t;

typedef struct {
    pickTarget_t target;

    // Where the crosshair met the world, and what with.
    qboolean hit;
    vec3_t   hitPos, hitNormal;
    char     hitShader[MAX_QPATH];
    int      hitSurfaceFlags, hitContents;
    char     hitThrough[MAX_QPATH]; // the first invisible surface passed, if any
} pick_t;

// Traces from the view along its forward axis.
// The most CG_PickAll reports in front of the world.
#define PICK_MAX_ALL 8

void CG_Pick(pick_t *pick);
int  CG_PickAll(pick_t *out, int max);

// Outlines: the target's box and a cross where the crosshair meets the world.
// Debug lines, so they must be added before the scene is drawn.
void CG_PickDrawBox(const pickTarget_t *t, float r, float g, float b);
void CG_PickDrawHit(const pick_t *pick, float r, float g, float b);

const char *CG_PickETypeName(int eType);
// The shaders a TIKI's surfaces are drawn with.
std::string CG_PickModelShaders(const char *model, qboolean json);
// One line naming the target, e.g. "entity #42 (modelanim_skel): models/human/german_wehrmacht_soldier.tik".
std::string CG_PickSummary(const pick_t *pick);

// JSON members (no braces) for the aim and the target, indented by indent.
std::string CG_PickAimJson(const pick_t *pick, const vec3_t viewOrg, const char *indent);
std::string CG_PickTargetJson(const pick_t *pick, const char *indent);

std::string CG_JsonString(const char *s);
std::string CG_JsonVec(const vec3_t v);

// maps/m1l1.bsp -> m1l1, as devmap takes it.
const char *CG_PickMapName(void);
