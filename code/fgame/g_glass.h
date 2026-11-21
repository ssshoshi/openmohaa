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
// Panes of glass on models (every vehicle's windows) and a map's windows
// (func_window) that bullets break a piece at a time, the way Half-Life 2's
// glass breaks, and blasts bring down. See g_glass.cpp and bg_glass.h.

#pragma once

class Entity;
class Vector;
#include "bg_glass.h"

// A bullet went from start to end: the first pane of glass on its way breaks
void G_GlassBulletPath(const Vector& start, const Vector& end, float damage);
// A blast of radius at origin reached ent
void G_GlassBlast(Entity *ent, const Vector& origin, float radius);
// Level start: a glass-only copy of each of the map's static vehicles
void G_GlassStaticModels(const char *mapfile);

// A pane (an entity with EF_GLASS) hit at local (in its frame's space): 0 when
// the round went through a hole already there, 1 when it made one, 2 when that
// brought the pane down. out: the round went along the frame's normal.
int G_GlassPaneHit(Entity *pane, const glassFrame_t *frame, const float *local, float damage, int out, const Vector& at);
// All of a pane's glass comes down (out: along its normal); qfalse if it was down
int G_GlassPaneShatter(Entity *pane, int out, int blast, const Vector& at);
