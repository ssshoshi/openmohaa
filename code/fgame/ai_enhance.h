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

// ai_enhance.h: switches for the enemy AI improvements.
//
// Each improvement has its own ai_* cvar, and ai_enhanced turns them all off
// at once; with it off the actors behave as in the game they come from.

#pragma once

#include "gamecvars.h"

// Whether the improvement switched by 'feature' is on.
inline bool AI_Enhanced(const cvar_t *feature)
{
    return ai_enhanced && ai_enhanced->integer && feature && feature->integer;
}

// Logs an AI decision when ai_debug is set (for testing).
void AI_Debug(const char *fmt, ...);
