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
// The game's side of an in-game report. cgame writes what it can see
// (cg_bugreport.cpp); this adds what only the game knows: the reported
// entity's class, targetname, animation and physics, an actor's AI state, the
// level and its script threads. Single player only, so the file lands next to
// the client's in the same home path.

#include "g_local.h"
#include "g_bugreport.h"
#include "g_physics.h"
#include "entity.h"
#include "animate.h"
#include "actor.h"
#include "player.h"
#include "level.h"
#include "scriptmaster.h"

static qboolean G_BugReportIdValid(const char *id)
{
    if (!id[0] || strlen(id) >= 64) {
        return qfalse;
    }
    for (; *id; id++) {
        if (!isalnum((unsigned char)*id) && *id != '_' && *id != '-') {
            return qfalse;
        }
    }
    return qtrue;
}

static void G_BugReportEntity(str& out, Entity *ent)
{
    const char *anim = NULL;

    if (ent->edict->tiki && ent->IsSubclassOfAnimate()) {
        anim = ((Animate *)ent)->AnimName(0);
    }

    out += va("Entity #   : %d\n", ent->entnum);
    out += va("Class ID   : %s\n", ent->getClassID());
    out += va("Classname  : %s\n", ent->getClassname());
    out += va("Targetname : %s\n", ent->TargetName().c_str());
    out += va("Modelname  : %s\n", ent->model.c_str());
    out += va("Animname   : %s\n", anim ? anim : "( N/A )");
    out += va("Origin     : ( %.2f, %.2f, %.2f )\n", ent->origin.x, ent->origin.y, ent->origin.z);
    out += va("Angles     : ( %.2f, %.2f, %.2f )\n", ent->angles.x, ent->angles.y, ent->angles.z);
    out += va(
        "Bounds     : Mins( %.2f, %.2f, %.2f ) Maxs( %.2f, %.2f, %.2f )\n",
        ent->mins.x,
        ent->mins.y,
        ent->mins.z,
        ent->maxs.x,
        ent->maxs.y,
        ent->maxs.z
    );
    out += va("Velocity   : ( %.2f, %.2f, %.2f )\n", ent->velocity.x, ent->velocity.y, ent->velocity.z);
    out += va("SVFLAGS    : %x\n", ent->edict->r.svFlags);
    out += va("Movetype   : %i\n", ent->movetype);
    out += va("Solidtype  : %i\n", ent->edict->solid);
    out += va("Contents   : %x\n", ent->edict->r.contents);
    out += va("Parent     : %i\n", ent->edict->s.parent);
    out += va("Health     : %.1f / %.1f\n", ent->health, ent->max_health);
    out += "Physics    : " + G_PhysicsDescribe(ent) + "\n";

    if (ent->IsSubclassOfActor()) {
        Actor *actor = (Actor *)ent;

        out += "\n== actor ==\n";
        out += "Think      : " + actor->ThinkStateName() + " " + actor->ThinkName() + "\n";
        out += va("Enemy      : %s\n",
            actor->m_Enemy ? va("#%d %s", actor->m_Enemy->entnum, actor->m_Enemy->TargetName().c_str()) : "none");
        out += va("Enemy on   : %s\n", actor->m_bEnableEnemy ? "yes" : "no");
        out += va("Leash      : %.0f\n", actor->m_fLeash);
        out += va("Min/max    : %.0f / %.0f\n", actor->m_fMinDistance, actor->m_fMaxDistance);
    }
}

qboolean G_BugReportServerCmd(gentity_t *ent)
{
    str         out;
    const char *id;
    int         n;

    if (!ent || !ent->entity || !ent->entity->IsSubclassOfPlayer() || gi.Argc() < 3) {
        return qtrue;
    }

    id = gi.Argv(1);
    n  = atoi(gi.Argv(2));
    if (!G_BugReportIdValid(id)) {
        return qtrue;
    }

    out += "== level ==\n";
    out += va("Map        : %s\n", level.mapname.c_str());
    out += va("Map script : %s\n", level.m_mapscript.c_str());
    out += va("Spawnpoint : %s\n", level.spawnpoint.c_str());
    out += va("Level time : %.2f (frame %d)\n", level.time, level.framenum);
    out += va("Cinematic  : %s\n", level.cinematic ? "yes" : "no");

    {
        Player *player = (Player *)ent->entity;

        out += "\n== player ==\n";
        out += va("Origin     : ( %.2f, %.2f, %.2f )\n", player->origin.x, player->origin.y, player->origin.z);
        out += va("Angles     : ( %.2f, %.2f, %.2f )\n", player->angles.x, player->angles.y, player->angles.z);
        out += va("Health     : %.1f\n", player->health);
    }

    out += "\n== target ==\n";
    if (n >= 0 && n < MAX_GENTITIES && g_entities[n].inuse && g_entities[n].entity) {
        G_BugReportEntity(out, g_entities[n].entity);
    } else {
        out += "none (not an entity)\n";
    }

    out += "\n== scripts ==\n";
    out += Director.GetStatus();

    // bugreport_server <id> <entnum> orch: a shot of the orchestrator (cgame/cg_orch.cpp)
    if (gi.Argc() > 3 && !Q_stricmp(gi.Argv(3), "orch")) {
        gi.FS_WriteFile(va("orch/events/%s.server.txt", id), out.c_str(), out.length());
    } else {
        gi.FS_WriteFile(va("bugreports/%s/server.txt", id), out.c_str(), out.length());
    }
    return qtrue;
}
