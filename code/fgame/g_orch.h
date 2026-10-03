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

// Debug overlays (single player; the lines need the server in the process):
//   ai_showpaths     each actor's path, script goal, patrol chain, leash,
//                    cover/aim/look targets and a label with its think
//   ai_showsenses    each actor's line to its enemy (green seen, red not),
//                    the enemy's last known position, hearing and field of view
//   g_showtriggers   trigger boxes, arrows to what they target, labels
//   g_showdamage     a floating number where each hit landed, and its direction
// with ai_showpaths_dist / g_showtriggers_dist the reach from the player.
void G_OrchDebugInit(void);
void G_OrchDebugFrame(void);
// Whether Entity::Damage should report hits (the log or g_showdamage).
bool G_OrchWantsDamage(void);
void G_OrchShowDamage(const Vector& position, const Vector& direction, float damage, bool killed);
// scriptinfo [threadnum]: the script threads, or one in detail.
qboolean G_OrchScriptInfoCmd(struct gentity_s *ent);
// orch_runscript <file> [label]: runs a script now, from the top or a label.
// Files under orch/ are compiled afresh each time (the agent's snippets);
// others, a map's own script say, are run as loaded. Single player.
qboolean G_OrchRunScriptCmd(struct gentity_s *ent);
