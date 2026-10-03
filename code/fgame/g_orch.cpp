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
#include "scriptexception.h"

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

//=============================================================
// Debug overlays
//=============================================================

#include "debuglines.h"

#define ORCH_DAMAGE_SHOWN 64   // hits shown at once
#define ORCH_DAMAGE_LIFE  2.0f // seconds a hit stays up

static cvar_t *ai_showpaths;
static cvar_t *ai_showpaths_dist;
static cvar_t *ai_showsenses;
static cvar_t *g_showtriggers;
static cvar_t *g_showtriggers_dist;

typedef struct {
    Vector position, direction;
    float  damage;
    float  time; // level.time
    bool   killed;
} orchHit_t;

static orchHit_t orchHits[ORCH_DAMAGE_SHOWN];
static int       orchNextHit;

void G_OrchDebugInit(void)
{
    ai_showpaths        = gi.Cvar_Get("ai_showpaths", "0", 0);
    ai_showpaths_dist   = gi.Cvar_Get("ai_showpaths_dist", "3000", 0);
    ai_showsenses       = gi.Cvar_Get("ai_showsenses", "0", 0);
    g_showtriggers      = gi.Cvar_Get("g_showtriggers", "0", 0);
    g_showtriggers_dist = gi.Cvar_Get("g_showtriggers_dist", "3000", 0);
    // g_showdamage is the game's own (gamecvars.cpp), which also prints
    // explosions' damage to the console.
    for (int i = 0; i < ORCH_DAMAGE_SHOWN; i++) {
        orchHits[i].damage = 0;
    }
}

// The colour of a think: patrol and idle blue, running cyan, attack red,
// curious yellow, the rest white.
static void G_OrchThinkColor(Actor *actor, float *c)
{
    const int state = actor->m_ThinkState;

    c[0] = c[1] = c[2] = 1.0f;
    if (state == THINKSTATE_ATTACK || state == THINKSTATE_GRENADE) {
        c[0] = 1.0f, c[1] = 0.25f, c[2] = 0.2f;
    } else if (state == THINKSTATE_CURIOUS || state == THINKSTATE_DISGUISE) {
        c[0] = 1.0f, c[1] = 0.85f, c[2] = 0.2f;
    } else if (state == THINKSTATE_IDLE) {
        switch (actor->CurrentThink()) {
        case THINK_RUNNER:
            c[0] = 0.2f, c[1] = 0.9f, c[2] = 1.0f;
            break;
        default:
            c[0] = 0.35f, c[1] = 0.5f, c[2] = 1.0f;
            break;
        }
    }
}

// A line in dashes, for what is planned rather than walked.
static void G_OrchDashed(const Vector& a, const Vector& b, float r, float g, float bl, float alpha)
{
    const Vector d   = b - a;
    const float  len = d.length();
    const int    n   = Q_min(32, (int)(len / 24.0f) | 1);

    for (int i = 0; i < n; i += 2) {
        G_DebugLine(a + d * ((float)i / n), a + d * ((float)(i + 1) / n), r, g, bl, alpha);
    }
}

// A short line from the head towards something, for where it looks or aims.
static void G_OrchPoint(const Vector& head, const Vector& at, float r, float g, float b)
{
    Vector d = at - head;

    if (d.length() > 96.0f) {
        d.normalize();
        d *= 96.0f;
    }
    G_DebugLine(head, head + d, r, g, b, 1.0f);
}

static void G_OrchShowPath(Actor *actor)
{
    const Vector up(0, 0, 8);
    const Vector head = actor->origin + Vector(0, 0, actor->maxs.z + 8);
    float        c[3];
    str          label;

    G_OrchThinkColor(actor, c);

    // The path it walks, from where it is.
    if (actor->m_Path.CurrentNode()) {
        PathInfo *pos  = actor->m_Path.CurrentNode();
        Vector    prev = actor->origin + up;

        for (;;) {
            const Vector point = Vector(pos->point) + up;

            G_DebugLine(prev, point, c[0], c[1], c[2], 1.0f);
            prev = point;
            if (pos == actor->m_Path.LastNode()) {
                break;
            }
            pos--;
        }
        G_DebugPyramid(prev, 8, c[0], c[1], c[2], 1.0f);
    }

    // Where a script sent it.
    if (actor->m_bScriptGoalValid) {
        G_DebugLine(head, actor->m_vScriptGoal + up, 1.0f, 1.0f, 0.3f, 0.6f);
        G_DebugPyramid(actor->m_vScriptGoal + up, 12, 1.0f, 1.0f, 0.3f, 1.0f);
    }

    // The patrol chain ahead, to its end or round its loop.
    if (actor->m_patrolCurrentNode) {
        SimpleEntity *node = actor->m_patrolCurrentNode;

        for (int i = 0; node && i < 32; i++) {
            SimpleEntity *next = node->Next();

            if (!next || next == actor->m_patrolCurrentNode) {
                break;
            }
            G_OrchDashed(node->origin + up, next->origin + up, 1.0f, 0.4f, 1.0f, 0.8f);
            node = next;
        }
    }

    // The leash.
    if (actor->m_fLeash > 0 && actor->m_fLeash < 8192) {
        G_DebugCircle(actor->m_vHome + up, actor->m_fLeash, 0.3f, 0.8f, 0.3f, 0.5f, qtrue);
    }

    if (actor->m_pCoverNode) {
        G_DebugLine(actor->origin + up, actor->m_pCoverNode->origin + up, 0.6f, 0.6f, 0.6f, 0.8f);
        G_DebugBBox(actor->m_pCoverNode->origin, Vector(-8, -8, 0), Vector(8, 8, 16), 0.6f, 0.6f, 0.6f, 1.0f);
    }
    if (actor->m_aimNode) {
        G_OrchPoint(head, actor->m_aimNode->origin, 1.0f, 0.5f, 0.0f);
    }
    if (actor->m_pLookEntity) {
        G_OrchPoint(head, actor->m_pLookEntity->origin, 0.8f, 0.8f, 1.0f);
    }
    if (actor->m_pTurnEntity) {
        G_OrchPoint(actor->origin + up, actor->m_pTurnEntity->origin, 0.5f, 1.0f, 0.5f);
    }

    label = va("#%d %s", actor->entnum, actor->ThinkName().c_str());
    if (actor->targetname.length()) {
        label += " $" + actor->targetname;
    }
    if (actor->m_pszDebugState && *actor->m_pszDebugState) {
        label += str(" : ") + actor->m_pszDebugState;
    }
    G_DebugString(head + Vector(0, 0, 12), 1.0f, c[0], c[1], c[2], "%s", label.c_str());
}

static void G_OrchShowSenses(Actor *actor)
{
    const Vector eye = actor->EyePosition();
    Vector       fwd, left;

    // Hearing, and the field of view as two short lines.
    if (actor->m_fHearing > 0 && actor->m_fHearing < 8192) {
        G_DebugCircle(actor->origin + Vector(0, 0, 4), actor->m_fHearing, 0.3f, 0.5f, 1.0f, 0.3f, qtrue);
    }
    if (actor->m_fFov > 0 && actor->m_fFov < 360) {
        const float half = actor->m_fFov * 0.5f;

        for (int side = -1; side <= 1; side += 2) {
            Vector a = actor->angles;

            a[YAW] += side * half;
            a.AngleVectorsLeft(&fwd, &left);
            G_DebugLine(eye, eye + fwd * 192.0f, 0.5f, 0.5f, 1.0f, 0.6f);
        }
    }

    if (!actor->m_Enemy) {
        return;
    }
    if (actor->m_bEnemyVisible) {
        G_DebugLine(eye, actor->m_Enemy->centroid, 0.2f, 1.0f, 0.2f, 1.0f);
    } else {
        G_DebugLine(eye, actor->m_Enemy->centroid, 1.0f, 0.2f, 0.2f, 0.7f);
        // Where it thinks the enemy is.
        G_DebugBBox(actor->m_vLastEnemyPos, Vector(-12, -12, 0), Vector(12, 12, 64), 0.7f, 0.7f, 0.7f, 0.7f);
        G_DebugLine(eye, actor->m_vLastEnemyPos + Vector(0, 0, 32), 0.7f, 0.7f, 0.7f, 0.4f);
    }
}

static void G_OrchShowTrigger(Trigger *trigger)
{
    const bool live = trigger->IsTriggerable() && trigger->TriggerCount() != 0;
    const char *target = trigger->Target().c_str();
    const Vector top   = Vector((trigger->absmin.x + trigger->absmax.x) * 0.5f, (trigger->absmin.y + trigger->absmax.y) * 0.5f, trigger->absmax.z);
    str          label;

    if (live) {
        G_DebugBBox(vec_zero, trigger->absmin, trigger->absmax, 1.0f, 0.55f, 0.0f, 1.0f);
    } else {
        G_DebugBBox(vec_zero, trigger->absmin, trigger->absmax, 0.5f, 0.35f, 0.2f, 0.5f);
    }

    for (Entity *ent = G_FindTarget(NULL, target); ent; ent = G_FindTarget(ent, target)) {
        G_DebugLine(trigger->centroid, ent->centroid, 1.0f, 0.55f, 0.0f, 0.6f);
        G_DebugPyramid(ent->centroid, 6, 1.0f, 0.55f, 0.0f, 1.0f);
    }

    label = trigger->getClassname();
    if (trigger->TargetName().length()) {
        label += " $" + trigger->TargetName();
    }
    if (*target) {
        label += str(" -> ") + target;
    }
    if (!live) {
        label += " (off)";
    }
    G_DebugString(top + Vector(0, 0, 8), 0.8f, 1.0f, 0.6f, 0.1f, "%s", label.c_str());
}

bool G_OrchWantsDamage(void)
{
    return G_OrchLogging() || (g_showdamage && g_showdamage->integer && g_gametype->integer == GT_SINGLE_PLAYER);
}

void G_OrchShowDamage(const Vector& position, const Vector& direction, float damage, bool killed)
{
    orchHit_t *hit;

    if (!g_showdamage || !g_showdamage->integer) {
        return;
    }
    hit            = &orchHits[orchNextHit++ % ORCH_DAMAGE_SHOWN];
    hit->position  = position;
    hit->direction = direction;
    hit->damage    = damage;
    hit->time      = level.time;
    hit->killed    = killed;
}

void G_OrchDebugFrame(void)
{
    Entity *player;
    float   pathDist, triggerDist;

    if (g_gametype->integer != GT_SINGLE_PLAYER || !ai_showpaths) {
        return;
    }
    if (!ai_showpaths->integer && !ai_showsenses->integer && !g_showtriggers->integer && !g_showdamage->integer) {
        return;
    }

    player      = G_GetEntity(0);
    pathDist    = Square(ai_showpaths_dist->value);
    triggerDist = Square(g_showtriggers_dist->value);

    for (gentity_t *edict = active_edicts.next; edict != &active_edicts; edict = edict->next) {
        Entity *ent = edict->entity;
        float   d2;

        if (!ent) {
            continue;
        }
        d2 = player ? (ent->centroid - player->centroid).lengthSquared() : 0;

        if (ent->IsSubclassOfActor()) {
            Actor *actor = (Actor *)ent;

            if (d2 > pathDist || actor->IsDead()) {
                continue;
            }
            if (ai_showpaths->integer) {
                G_OrchShowPath(actor);
            }
            if (ai_showsenses->integer) {
                G_OrchShowSenses(actor);
            }
        } else if (g_showtriggers->integer && ent->isSubclassOf(Trigger) && d2 <= triggerDist) {
            G_OrchShowTrigger((Trigger *)ent);
        }
    }

    if (g_showdamage->integer) {
        for (int i = 0; i < ORCH_DAMAGE_SHOWN; i++) {
            const orchHit_t *hit = &orchHits[i];
            const float      age = level.time - hit->time;
            float            fade;

            if (!hit->damage || age < 0 || age > ORCH_DAMAGE_LIFE) {
                continue;
            }
            fade = 1.0f - age / ORCH_DAMAGE_LIFE;
            // Rising as it fades.
            G_DebugString(
                hit->position + Vector(0, 0, 8 + age * 24), 0.9f, 1.0f, hit->killed ? 0.2f : 0.9f, 0.2f, hit->killed ? "%.0f KILL" : "%.0f", hit->damage
            );
            G_DebugLine(hit->position - hit->direction * 48.0f, hit->position, 1.0f, 0.9f, 0.2f, fade);
        }
    }
}

qboolean G_OrchScriptInfoCmd(gentity_t *ent)
{
    if (gi.Argc() > 1) {
        Director.PrintThread(atoi(gi.Argv(1)));
    } else {
        Director.PrintStatus();
    }
    return qtrue;
}

qboolean G_OrchRunScriptCmd(gentity_t *ent)
{
    str file, label;

    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        gi.Printf("orch_runscript: single player only\n");
        return qtrue;
    }
    if (gi.Argc() < 2) {
        gi.Printf("Usage: orch_runscript <file> [label]\n");
        return qtrue;
    }

    file  = gi.Argv(1);
    label = gi.Argc() > 2 ? gi.Argv(2) : "";

    try {
        // Recompiling a script ends its threads: only the agent's own.
        GameScript   *scr = Director.GetGameScript(file, !Q_stricmpn(file.c_str(), "orch/", 5));
        ScriptThread *thread;

        if (!scr || !scr->successCompile) {
            gi.Printf("orch_runscript: %s didn't compile\n", file.c_str());
            return qtrue;
        }
        thread = Director.CreateThread(scr, label);
        if (thread) {
            thread->Execute();
        }
        gi.Printf("orch_runscript: ran %s%s%s\n", file.c_str(), label.length() ? "::" : "", label.c_str());
    } catch (ScriptException& exc) {
        gi.Printf("orch_runscript: %s\n", exc.string.c_str());
    }
    return qtrue;
}
