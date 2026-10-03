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
// The game's side of the live orchestrator (cgame/cg_orch.cpp). While it is
// on, in single player, damage, deaths, triggers, AI think changes and move
// orders are appended to orch/log/game.jsonl, one JSON object a line, with
// the wall clock time ("t", Unix seconds) so the voice sidecar can line them
// up with what was said.

#include "g_local.h"
#include "g_orch.h"
#include "entity.h"
#include "actor.h"
#include "player.h"
#include "trigger.h"
#include "level.h"
#include "scriptmaster.h"
#include "scriptthread.h"

#include <chrono>

#define ORCH_LOG_PATH  "orch/log/game.jsonl"
#define ORCH_LOG_LIMIT (32 * 1024 * 1024) // started over when bigger
#define ORCH_TRIGGER_GAP 2.0              // seconds before the same trigger is logged again
#define ORCH_ORDER_GAP   5.0              // and the same order to the same actor

static fileHandle_t orchLog;
static cvar_t      *orchActive;
static str          orchMap;                         // the map the last level line was for
static double       orchTriggerAt[MAX_GENTITIES];    // when each trigger was last logged
static int          orchTriggerBy[MAX_GENTITIES];    // and who by
static double       orchOrderAt[MAX_GENTITIES];      // when each actor's last order was logged
static str          orchOrderKey[MAX_GENTITIES];     // and what it was

static double G_OrchNow(void)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() / 1000.0;
}

bool G_OrchLogging(void)
{
    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        return false;
    }
    if (!orchActive) {
        // cgame's; one process in single player
        orchActive = gi.Cvar_Get("orch_active", "0", 0);
    }
    return orchActive->integer != 0;
}

void G_OrchShutdown(void)
{
    if (orchLog) {
        gi.FS_FCloseFile(orchLog);
        orchLog = 0;
    }
    orchMap = "";
}

static str G_OrchJson(const char *s)
{
    str out = "\"";

    for (; s && *s; s++) {
        const char c = *s;

        if (c == '"' || c == '\\') {
            out += "\\";
            out += str(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if ((unsigned char)c < 0x20) {
            out += " ";
        } else {
            out += str(c);
        }
    }
    return out + "\"";
}

static str G_OrchVec(const Vector& v)
{
    return va("[%.0f, %.0f, %.0f]", v[0], v[1], v[2]);
}

// {"num": 12, "class": "Actor", "targetname": "guy3", "model": "..."}
static str G_OrchEnt(Entity *e)
{
    str j;

    if (!e) {
        return "null";
    }
    if (e == world) {
        return "\"world\"";
    }

    j = va("{\"num\": %d, \"class\": ", e->entnum);
    j += G_OrchJson(e->getClassname());
    if (e->TargetName().length()) {
        j += ", \"targetname\": " + G_OrchJson(e->TargetName().c_str());
    }
    if (e->model.length()) {
        j += ", \"model\": " + G_OrchJson(e->model.c_str());
    }
    return j + "}";
}

// Where the running script is: "maps/m3l2.scr:210", or "" when none is.
static str G_OrchScriptPos(void)
{
    ScriptThread *thread = Director.CurrentThread();

    if (!thread) {
        return "";
    }
    return thread->SourcePos();
}

static void G_OrchWrite(const char *type, const str& members)
{
    str line;

    if (!orchLog) {
        fileHandle_t f;
        const long   size = gi.FS_FOpenFile(ORCH_LOG_PATH, &f, qtrue, qtrue);

        if (f) {
            gi.FS_FCloseFile(f);
        }
        orchLog = size > ORCH_LOG_LIMIT ? gi.FS_FOpenFileWrite(ORCH_LOG_PATH) : gi.FS_FOpenFileAppend(ORCH_LOG_PATH);
        if (!orchLog) {
            return;
        }
    }

    if (orchMap != level.mapname) {
        orchMap = level.mapname;
        line    = va("{\"t\": %.3f, \"type\": \"level\", \"map\": ", G_OrchNow());
        line += G_OrchJson(level.mapname.c_str());
        line += va(", \"level_time\": %.2f}\n", level.time);
        gi.FS_Write(line.c_str(), line.length(), orchLog);
    }

    line = va("{\"t\": %.3f, \"level_time\": %.2f, \"type\": \"%s\", ", G_OrchNow(), level.time, type);
    line += members;
    line += "}\n";
    gi.FS_Write(line.c_str(), line.length(), orchLog);
    gi.FS_Flush(orchLog);
}

void G_OrchLogDamage(
    Entity *victim, Entity *attacker, Entity *inflictor, float damage, int meansofdeath, int location, float healthBefore
)
{
    str j;

    if (!victim || victim == world || victim->takedamage == DAMAGE_NO) {
        return;
    }

    j = "\"victim\": " + G_OrchEnt(victim);
    j += ", \"attacker\": " + G_OrchEnt(attacker);
    if (inflictor && inflictor != attacker) {
        j += ", \"inflictor\": " + G_OrchEnt(inflictor);
    }
    j += va(", \"damage\": %.0f", damage);
    if (meansofdeath >= 0 && meansofdeath < MOD_TOTAL_NUMBER) {
        j += ", \"mod\": " + G_OrchJson(means_of_death_strings[meansofdeath]);
    }
    if (location >= 0) {
        j += ", \"location\": " + G_OrchJson(G_LocationNumToDispString(location));
    }
    // The HUD shows health as a percentage of max_health.
    j += va(", \"health\": [%.0f, %.0f], \"max_health\": %.0f", healthBefore, victim->health, victim->max_health);
    if (healthBefore > 0 && victim->health <= 0) {
        j += ", \"killed\": true";
    }
    j += ", \"origin\": " + G_OrchVec(victim->origin);

    G_OrchWrite("damage", j);
}

void G_OrchLogTrigger(Trigger *trigger, Entity *activator)
{
    const double now = G_OrchNow();
    const int    by  = activator ? activator->entnum : -1;
    str          j;

    // Standing in a trigger fires it over and over.
    if (orchTriggerBy[trigger->entnum] == by && now - orchTriggerAt[trigger->entnum] < ORCH_TRIGGER_GAP) {
        return;
    }
    orchTriggerAt[trigger->entnum] = now;
    orchTriggerBy[trigger->entnum] = by;

    j = "\"trigger\": " + G_OrchEnt(trigger);
    if (trigger->Target().length()) {
        j += ", \"target\": " + G_OrchJson(trigger->Target().c_str());
    }
    j += ", \"activator\": " + G_OrchEnt(activator);
    j += ", \"origin\": " + G_OrchVec(trigger->centroid);

    G_OrchWrite("trigger", j);
}

void G_OrchLogThink(Actor *actor, int oldState, int oldThink)
{
    str j;

    j = "\"actor\": " + G_OrchEnt(actor);
    j += ", \"from\": " + G_OrchJson(va(
             "%s/%s",
             Director.GetString(Actor::m_csThinkStateNames[oldState]).c_str(),
             Director.GetString(Actor::m_csThinkNames[oldThink]).c_str()
         ));
    j += ", \"to\": " + G_OrchJson(va("%s/%s", actor->ThinkStateName().c_str(), actor->ThinkName().c_str()));
    if (actor->m_Enemy) {
        j += ", \"enemy\": " + G_OrchEnt(actor->m_Enemy);
    }
    j += ", \"origin\": " + G_OrchVec(actor->origin);

    G_OrchWrite("think", j);
}

void G_OrchLogOrder(Actor *actor, const char *order, const char *anim, ScriptVariable *dest)
{
    const double now = G_OrchNow();
    str          script;
    str          key;
    str          j;

    // Follow scripts give the same order every frame.
    key = str(order) + " " + (dest ? dest->stringValue() : str());
    if (orchOrderKey[actor->entnum] == key && now - orchOrderAt[actor->entnum] < ORCH_ORDER_GAP) {
        return;
    }
    orchOrderKey[actor->entnum] = key;
    orchOrderAt[actor->entnum]  = now;

    script = G_OrchScriptPos();
    j      = "\"actor\": " + G_OrchEnt(actor);
    j += ", \"order\": " + G_OrchJson(order);
    if (anim && *anim) {
        j += ", \"anim\": " + G_OrchJson(anim);
    }
    if (dest) {
        Listener *l = dest->GetType() == VARIABLE_LISTENER ? dest->listenerValue() : NULL;

        if (l && l->isSubclassOf(SimpleEntity)) {
            SimpleEntity *se = (SimpleEntity *)l;

            j += ", \"to\": " + G_OrchJson(se->TargetName().length() ? se->TargetName().c_str() : se->getClassname());
            j += ", \"to_origin\": " + G_OrchVec(se->origin);
        } else if (dest->GetType() == VARIABLE_VECTOR) {
            j += ", \"to_origin\": " + G_OrchVec(dest->vectorValue());
        } else {
            j += ", \"to\": " + G_OrchJson(dest->stringValue().c_str());
        }
    }
    if (script.length()) {
        j += ", \"script\": " + G_OrchJson(script.c_str());
    }
    j += ", \"origin\": " + G_OrchVec(actor->origin);

    G_OrchWrite("order", j);
}
