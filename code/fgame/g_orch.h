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
// The game's side of the live orchestrator (cgame/cg_orch.cpp): while it is
// on, in single player, what happens in the game is appended to
// orch/log/game.jsonl for the voice sidecar to put next to what was said.

#pragma once

class Entity;
class Actor;
class Trigger;
class ScriptVariable;

// Whether to log: the orchestrator is on (orch_active), single player.
bool G_OrchLogging(void);
void G_OrchShutdown(void);

// healthBefore is the victim's health before the damage was applied.
void G_OrchLogDamage(
    Entity *victim, Entity *attacker, Entity *inflictor, float damage, int meansofdeath, int location, float healthBefore
);
void G_OrchLogTrigger(Trigger *trigger, Entity *activator);
// The actor's think changed from oldState/oldThink to what it is now.
void G_OrchLogThink(Actor *actor, int oldState, int oldThink);
// A move order from a script: runto, walkto, moveto, patrolpath...
void G_OrchLogOrder(Actor *actor, const char *order, const char *anim, ScriptVariable *dest);
