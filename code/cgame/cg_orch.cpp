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
// The live orchestrator: the player talks to an agent while playing, and the
// agent answers and changes the game as it runs (tools/orchestrator/).
//
// The game's half is small. orch (F10) turns orchestrator mode on. The player
// talks while holding a key the sidecar watches (MOUSE4). orch_shot
// (MOUSE3) then takes a screenshot and writes what is under the crosshair to
// orch/events/<id>.json in the home path, which the voice sidecar puts on the
// same timeline as what was said. Replies come back as orch_msg commands,
// through the command directory (com_cmddir), and are shown in a panel.
//
// orch_freeze (F11) pauses the world and lets the player walk, or fly with
// orch_fly (N), through it: AI, scripts and physics stay where they were. The
// engine keeps making movement commands while paused (cl_freecam) without
// sending them; the camera runs Pmove on its own copy of the player state.
// orch_ghost (B) is the same camera with the world running: the engine sends
// the server commands that keep the body standing where it was. The body is
// still a target; the camera says when it is hit and comes back if it dies.
//
// A shot writes:
//   orch/events/<id>.json          where the player is and what is aimed at
//   orch/events/<id>.server.txt    what the game knows of it (single player)
//   screenshots/orch_<id>.jpg      the clean frame
//   screenshots/orch_<id>_marked.jpg  the same, the target outlined
// and in single player the savegame orch_<id>, to come back to that moment.
//
// While the mode is on, what the player does is logged for the sidecar to put
// next to what was said: where they are and look (every 250 ms, when it
// changes), what is under the crosshair (when it changes), long frames, and
// shots, freezes and the like. It goes out in batches, to a ring of files
// orch/log/client.<n>.jsonl, since cgame can't append to a file; each batch
// starts with a line naming its writer and number. The game logs its own side
// to orch/log/game.jsonl (fgame/g_orch.cpp).

// Jolt first: cg_local.h defines a LERP macro its headers collide with.
#include "cg_physics_local.h"
#include "cg_orch.h"
#include "cg_pick.h"

#include <chrono>
#include <ctime>
#include <string>

#define ORCH_MAX_MSGS    6
#define ORCH_MSG_LIFE    15000 // ms a reply stays up
#define ORCH_MSG_FADE    2000
#define ORCH_OUTLINE     1500 // ms the shot's target stays outlined
#define ORCH_MSG_LEN     512

#define ORCH_LOG_SLOTS   64   // client.<n>.jsonl files in the ring
#define ORCH_LOG_EVERY   500  // ms between batches
#define ORCH_LOG_SAMPLE  250  // ms between looks at the trail and the gaze
#define ORCH_LOG_QUIET   5000 // ms the trail goes unlogged when nothing moves
#define ORCH_HITCH_MS    50   // a frame longer than this is logged

static cvar_t *orch_active;
static cvar_t *orch_shotsave;
static cvar_t *orch_flyspeed;

enum {
    ORCH_SHOT_IDLE,
    ORCH_SHOT_CLEAN,  // waiting to take the clean screenshot
    ORCH_SHOT_MARKED, // the target outlined, waiting to take that one
};

typedef struct {
    char     text[ORCH_MSG_LEN];
    int      time; // cgi.Milliseconds(), which runs while paused
    qboolean heard; // the player's words, as the sidecar understood them
} orchMsg_t;

typedef struct {
    int    shotState;
    int    shotFrames;
    int    shotCount;
    char   shotId[64];
    pick_t pick;
    int    outlineUntil;

    orchMsg_t msgs[ORCH_MAX_MSGS];
    int       nextMsg;

    char status[32];
    char statusKey[32]; // the push-to-talk key, with status "ready"
} orchState_t;

static orchState_t orch;

// The free camera, while the world is frozen or, live, while it runs.
typedef struct {
    qboolean      active;
    qboolean      live; // the world runs, the body stands where it was
    qboolean      fly;
    playerState_t ps;     // a copy of the player's, moved by Pmove
    int           time;   // the camera's own command clock: the server's may be frozen
    int           lastMs;
    vec3_t        mins, maxs; // the player's box, from the last Pmove
    vec3_t        frozeAt;    // where the player was when frozen, for orch_return
    int           health;     // the body's, last frame, while ghosting
    int           hitMsgAt;   // when the panel last said it was hit
} orchFreecam_t;

static orchFreecam_t fc;

// The log, while the mode is on.
typedef struct {
    std::string buf;       // lines not written yet
    long long   writer;    // when this cgame started, ms: names its batches
    int         batch;
    int         flushedAt;
    int         sampledAt;
    int         trailAt;   // when the trail was last logged
    vec3_t      trailOrigin, trailAngles;
    int         trailHealth;
    std::string trailWeapon;
    std::string gaze;
    int         frameAt;   // the last frame's cgi.Milliseconds()
    int         startedAt;
    std::string map;
} orchLog_t;

static orchLog_t orchLog;

static void CG_OrchFreecamEnd(qboolean here);

//=============================================================
// Helpers
//=============================================================

static qboolean CG_OrchSinglePlayer(void)
{
    return cgs.gametype == GT_SINGLE_PLAYER ? qtrue : qfalse;
}

static void CG_OrchColor(vec4_t c, float r, float g, float b, float a)
{
    c[0] = r;
    c[1] = g;
    c[2] = b;
    c[3] = a;
}

static void CG_OrchAddMsg(const char *text, qboolean heard)
{
    orchMsg_t *m = &orch.msgs[orch.nextMsg];

    Q_strncpyz(m->text, text, sizeof(m->text));
    m->time  = cgi.Milliseconds();
    m->heard = heard;
    orch.nextMsg = (orch.nextMsg + 1) % ORCH_MAX_MSGS;
}

//=============================================================
// The log
//=============================================================

static double CG_OrchNow(void)
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() / 1000.0;
}

// One line; members are the JSON members after "t" and "type", no braces.
static void CG_OrchLog(const char *type, const std::string& members)
{
    if (!orch_active || !orch_active->integer) {
        return;
    }

    orchLog.buf += va("{\"t\": %.3f, \"type\": \"%s\"", CG_OrchNow(), type);
    if (!members.empty()) {
        orchLog.buf += ", " + members;
    }
    orchLog.buf += "}\n";
}

static void CG_OrchLogFlush(qboolean force)
{
    std::string out;

    if (orchLog.buf.empty() || (!force && cgi.Milliseconds() - orchLog.flushedAt < ORCH_LOG_EVERY)) {
        return;
    }

    out = va("{\"writer\": %lld, \"batch\": %d}\n", orchLog.writer, orchLog.batch);
    out += orchLog.buf;
    cgi.FS_WriteFile(va("orch/log/client.%02d.jsonl", orchLog.batch % ORCH_LOG_SLOTS), out.c_str(), (int)out.length());

    orchLog.batch++;
    orchLog.buf.clear();
    orchLog.flushedAt = cgi.Milliseconds();
}

static const char *CG_OrchCamMode(void)
{
    if (!fc.active) {
        return NULL;
    }
    return fc.live ? "ghost" : "frozen";
}

static void CG_OrchLogTrail(int now)
{
    const playerState_t *ps     = &cg.predicted_player_state;
    const int            health = cg.snap->ps.stats[STAT_HEALTH];
    const char          *mode   = CG_OrchCamMode();
    std::string          weapon;
    std::string          j;
    vec3_t               d;
    qboolean             moved;

    if (cg.snap->ps.activeItems[1] >= 0) {
        weapon = CG_ConfigString(CS_WEAPONS + cg.snap->ps.activeItems[1]);
    }

    // The camera's view, when it is off on its own.
    VectorSubtract(cg.refdef.vieworg, orchLog.trailOrigin, d);
    moved = VectorLength(d) > 4.0f || fabs(AngleSubtract(cg.refdefViewAngles[YAW], orchLog.trailAngles[YAW])) > 5.0f
         || fabs(AngleSubtract(cg.refdefViewAngles[PITCH], orchLog.trailAngles[PITCH])) > 5.0f;
    if (!moved && health == orchLog.trailHealth && weapon == orchLog.trailWeapon && now - orchLog.trailAt < ORCH_LOG_QUIET) {
        return;
    }

    VectorCopy(cg.refdef.vieworg, orchLog.trailOrigin);
    VectorCopy(cg.refdefViewAngles, orchLog.trailAngles);
    orchLog.trailHealth = health;
    orchLog.trailWeapon = weapon;
    orchLog.trailAt     = now;

    j += "\"origin\": " + CG_JsonVec(ps->origin);
    j += ", \"view\": " + CG_JsonVec(cg.refdef.vieworg);
    j += ", \"angles\": " + CG_JsonVec(cg.refdefViewAngles);
    j += va(", \"health\": %d", health);
    if (!weapon.empty()) {
        j += ", \"weapon\": " + CG_JsonString(weapon.c_str());
    }
    if (ps->pm_flags & PMF_DUCKED) {
        j += ", \"crouched\": true";
    }
    if (mode) {
        j += va(", \"camera\": \"%s\"", mode);
    }
    CG_OrchLog("trail", j);
}

static void CG_OrchLogGaze(void)
{
    pick_t      pick;
    std::string what;
    std::string j;

    CG_Pick(&pick);
    what = CG_PickSummary(&pick);
    if (what == orchLog.gaze) {
        return;
    }
    orchLog.gaze = what;

    j = "\"what\": " + CG_JsonString(what.c_str());
    if (pick.hit) {
        j += va(", \"distance\": %.0f", Distance(cg.refdef.vieworg, pick.hitPos));
        j += ", \"at\": " + CG_JsonVec(pick.hitPos);
    }
    if (pick.target.kind == PICK_TARGET_ENTITY) {
        j += va(", \"entity\": %d", pick.target.index);
    }
    CG_OrchLog("gaze", j);
}

// Every frame, while the mode is on.
static void CG_OrchLogFrame(void)
{
    const int now   = cgi.Milliseconds();
    const int frame = orchLog.frameAt ? now - orchLog.frameAt : 0;

    orchLog.frameAt = now;
    if (!orch_active->integer) {
        orchLog.buf.clear();
        return;
    }

    if (orchLog.map != CG_PickMapName()) {
        orchLog.map = CG_PickMapName();
        CG_OrchLog("level", "\"map\": " + CG_JsonString(orchLog.map.c_str()) + va(", \"server_time\": %d", cg.snap->serverTime));
    }

    // Not the load, and not the screenshots the orchestrator takes.
    if (frame > ORCH_HITCH_MS && now - orchLog.startedAt > 3000 && orch.shotState == ORCH_SHOT_IDLE) {
        CG_OrchLog(
            "hitch",
            va("\"ms\": %d, \"entities\": %d, \"server_time\": %d", frame, cg.snap->numEntities, cg.snap->serverTime)
        );
    }

    if (now - orchLog.sampledAt >= ORCH_LOG_SAMPLE) {
        orchLog.sampledAt = now;
        CG_OrchLogTrail(now);
        CG_OrchLogGaze();
    }

    CG_OrchLogFlush(qfalse);
}

static void CG_OrchMakeId(void)
{
    const time_t now = time(NULL);
    struct tm   *t   = localtime(&now);

    orch.shotCount++;
    Com_sprintf(
        orch.shotId,
        sizeof(orch.shotId),
        "%04d%02d%02d-%02d%02d%02d-%03d",
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        orch.shotCount % 1000
    );
}

// Where the player is and looks, as JSON members (no braces).
static std::string CG_OrchPlayerJson(const char *indent)
{
    std::string in = indent;
    std::string j;

    j += in + "\"map\": " + CG_JsonString(CG_PickMapName()) + ",\n";
    j += in + va("\"single_player\": %s,\n", CG_OrchSinglePlayer() ? "true" : "false");
    j += in + va("\"server_time\": %d,\n", cg.snap->serverTime);
    j += in + va("\"paused\": %d,\n", cgi.Cvar_Get("paused", "0", 0)->integer);
    j += in + "\"player_origin\": " + CG_JsonVec(cg.snap->ps.origin) + ",\n";
    j += in + "\"view_origin\": " + CG_JsonVec(cg.refdef.vieworg) + ",\n";
    j += in + "\"view_angles\": " + CG_JsonVec(cg.refdefViewAngles) + ",\n";
    j += in + va("\"health\": %d,\n", cg.snap->ps.stats[STAT_HEALTH]);
    // The view is the free camera's, not the player's, while frozen or ghosting.
    j += in + va("\"frozen\": %s,\n", fc.active && !fc.live ? "true" : "false");
    j += in + va("\"ghost\": %s", fc.active && fc.live ? "true" : "false");
    return j;
}

static void CG_OrchWriteShot(void)
{
    const qboolean sp   = CG_OrchSinglePlayer();
    const qboolean save = sp && orch_shotsave->integer ? qtrue : qfalse;
    std::string    j;

    j += "{\n";
    j += "  \"schema\": 1,\n";
    j += "  \"id\": " + CG_JsonString(orch.shotId) + ",\n";
    j += va("  \"real_ms\": %d,\n", cgi.Milliseconds());
    j += CG_OrchPlayerJson("  ") + ",\n";
    j += "  \"summary\": " + CG_JsonString(CG_PickSummary(&orch.pick).c_str()) + ",\n";
    j += "  \"aim\": {\n" + CG_PickAimJson(&orch.pick, cg.refdef.vieworg, "    ") + "\n  },\n";
    j += "  \"target\": {\n" + CG_PickTargetJson(&orch.pick, "    ") + "\n  },\n";
    j += "  \"files\": {\n";
    j += va("    \"screenshot\": \"screenshots/orch_%s.jpg\",\n", orch.shotId);
    j += va("    \"screenshot_marked\": \"screenshots/orch_%s_marked.jpg\"", orch.shotId);
    j += sp ? va(",\n    \"server\": \"orch/events/%s.server.txt\"", orch.shotId) : "";
    j += save ? va(",\n    \"savegame\": \"orch_%s\"", orch.shotId) : "";
    j += "\n  }\n";
    j += "}\n";

    cgi.FS_WriteFile(va("orch/events/%s.json", orch.shotId), j.c_str(), (int)j.length());
}

//=============================================================
// Commands
//=============================================================

void CG_Orch_f(void)
{
    const char *arg = cgi.Argc() > 1 ? cgi.Argv(1) : "";
    int         on;

    if (!Q_stricmp(arg, "on") || !Q_stricmp(arg, "1")) {
        on = 1;
    } else if (!Q_stricmp(arg, "off") || !Q_stricmp(arg, "0")) {
        on = 0;
    } else {
        on = !orch_active->integer;
    }

    if (!on && fc.active) {
        CG_OrchFreecamEnd(qfalse);
    }
    cgi.Cvar_Set("orch_active", on ? "1" : "0");
    CG_OrchAddMsg(on ? "Orchestrator on" : "Orchestrator off", qfalse);
    cgi.Printf("orch: %s\n", on ? "on" : "off");
}

void CG_OrchShot_f(void)
{
    if (!cg.snap || !orch_active->integer) {
        return;
    }
    if (orch.shotState != ORCH_SHOT_IDLE) {
        // Still taking the last one.
        return;
    }

    CG_Pick(&orch.pick);
    CG_OrchMakeId();
    CG_OrchWriteShot();

    orch.shotState  = ORCH_SHOT_CLEAN;
    orch.shotFrames = 0;
    cgi.Printf("orch: shot %s %s\n", orch.shotId, CG_PickSummary(&orch.pick).c_str());
    CG_OrchLog("shot", "\"id\": " + CG_JsonString(orch.shotId) + ", \"what\": " + CG_JsonString(CG_PickSummary(&orch.pick).c_str()));
}

void CG_OrchMsg_f(void)
{
    qboolean    heard = qfalse;
    std::string text;

    for (int i = 1; i < cgi.Argc(); i++) {
        const char *a = cgi.Argv(i);

        if (i == 1 && !Q_stricmp(a, "-heard")) {
            heard = qtrue;
            continue;
        }
        if (!text.empty()) {
            text += " ";
        }
        text += a;
    }

    if (!text.empty()) {
        CG_OrchAddMsg(text.c_str(), heard);
    }
}

void CG_OrchStatus_f(void)
{
    Q_strncpyz(orch.status, cgi.Argc() > 1 ? cgi.Argv(1) : "", sizeof(orch.status));
    Q_strncpyz(orch.statusKey, cgi.Argc() > 2 ? cgi.Argv(2) : "", sizeof(orch.statusKey));
}

void CG_OrchState_f(void)
{
    std::string j;
    pick_t      pick;

    if (!cg.snap) {
        cgi.Printf("{\"in_game\": false}\n");
        return;
    }

    CG_Pick(&pick);
    j = "{\"in_game\": true, " + CG_OrchPlayerJson("") + ", ";
    j += va("\"orch\": %d, ", orch_active->integer);
    j += "\"looking_at\": " + CG_JsonString(CG_PickSummary(&pick).c_str());
    if (pick.hit) {
        j += ", \"aim_position\": " + CG_JsonVec(pick.hitPos);
    }
    j += "}";

    // Newlines from the JSON helpers would split the line.
    for (size_t i = 0; i < j.length(); i++) {
        if (j[i] == '\n') {
            j[i] = ' ';
        }
    }
    cgi.Printf("%s\n", j.c_str());
}

//=============================================================
// Freezing the world
//=============================================================

static qboolean CG_OrchPaused(void)
{
    return cgi.Cvar_Get("paused", "0", 0)->integer ? qtrue : qfalse;
}

// live: the world keeps running and the body stands where it is. Switches
// between the two when the camera is already on.
static void CG_OrchFreecamBegin(qboolean live)
{
    if (!CG_OrchSinglePlayer()) {
        CG_OrchAddMsg(live ? "Ghosting works in single player only" : "Freezing works in single player only", qfalse);
        return;
    }

    if (!fc.active) {
        fc.ps = cg.predicted_player_state;
        VectorCopy(cg.predicted_player_state.origin, fc.frozeAt);
        fc.ps.pm_flags = 0;
        VectorClear(fc.ps.velocity);
        fc.time           = 1000;
        fc.ps.commandTime = fc.time;
        fc.lastMs         = cgi.Milliseconds();
        fc.active         = qtrue;
    }
    fc.live   = live;
    fc.health = cg.snap->ps.stats[STAT_HEALTH];
    CG_OrchLog(live ? "ghost" : "freeze", "\"body\": " + CG_JsonVec(fc.frozeAt));

    if (live) {
        cgi.Cvar_Set("cl_freecam", "2");
        if (CG_OrchPaused()) {
            cgi.Cmd_Execute(EXEC_NOW, "pause\n");
        }
        CG_OrchAddMsg("Ghost: the world runs, your body stays. B comes back, F11 freezes.", qfalse);
    } else {
        cgi.Cvar_Set("cl_freecam", "1");
        if (!CG_OrchPaused()) {
            cgi.Cmd_Execute(EXEC_NOW, "pause\n");
        }
        CG_OrchAddMsg(
            fc.fly ? "Frozen, flying. F11 resumes, N walks, B lets it run." : "Frozen. F11 resumes, N flies, B lets it run.",
            qfalse
        );
    }
}

// Whether the player could stand where the camera is: not in a wall, and with
// a floor under it (flying lets the camera out of the map).
static const char *CG_OrchCantStandHere(void)
{
    trace_t tr;
    vec3_t  down, up;

    if (VectorCompare(fc.mins, fc.maxs)) {
        return NULL; // hasn't moved
    }

    CG_Trace(&tr, fc.ps.origin, fc.mins, fc.maxs, fc.ps.origin, fc.ps.clientNum, MASK_PLAYERSOLID, qfalse, qtrue, "orch");
    if (tr.startsolid || tr.allsolid) {
        return "inside something";
    }

    // A floor not far below. Under the terrain or outside the map there is
    // only the sky box's bottom, or nothing.
    VectorCopy(fc.ps.origin, down);
    down[2] -= 1024.0f;
    CG_Trace(&tr, fc.ps.origin, fc.mins, fc.maxs, down, fc.ps.clientNum, MASK_PLAYERSOLID, qfalse, qtrue, "orch");
    if (tr.fraction >= 1.0f || (tr.surfaceFlags & SURF_SKY)) {
        return "nothing to stand on";
    }

    // Under the ground, what is overhead is a floor seen from below: it faces
    // up. Ceilings face down.
    VectorCopy(fc.ps.origin, up);
    up[2] += 8192.0f;
    CG_Trace(&tr, fc.ps.origin, vec3_origin, vec3_origin, up, fc.ps.clientNum, MASK_PLAYERSOLID, qfalse, qfalse, "orch");
    if (tr.fraction < 1.0f && tr.plane.normal[2] > 0.7f && !(tr.surfaceFlags & SURF_SKY)) {
        return "under the ground";
    }
    return NULL;
}

// here: the player goes to where the camera is, looking where it looks.
static void CG_OrchFreecamEnd(qboolean here)
{
    if (here) {
        const char *why = CG_OrchCantStandHere();

        if (why) {
            CG_OrchAddMsg(va("Can't resume here: %s", why), qfalse);
            return;
        }
    }

    fc.active = qfalse;
    fc.live   = qfalse;
    cgi.Cvar_Set("cl_freecam", here ? "-1" : "0");
    if (CG_OrchPaused()) {
        cgi.Cmd_Execute(EXEC_NOW, "pause\n");
    }
    if (here) {
        // Sent with the first commands after the pause.
        cgi.SendClientCommand(va("tele %.1f %.1f %.1f", fc.ps.origin[0], fc.ps.origin[1], fc.ps.origin[2]));
    }
    CG_OrchAddMsg(here ? "Resumed here" : "Resumed", qfalse);
    CG_OrchLog("resume", here ? "\"here\": " + CG_JsonVec(fc.ps.origin) : std::string());
}

void CG_OrchFreeze_f(void)
{
    const qboolean here = cgi.Argc() > 1 && !Q_stricmp(cgi.Argv(1), "here") ? qtrue : qfalse;

    if (!cg.snap) {
        return;
    }
    if (!orch_active->integer) {
        cgi.Printf("orch_freeze: turn the orchestrator on first (orch)\n");
        return;
    }

    if (fc.active && fc.live && !here) {
        CG_OrchFreecamBegin(qfalse);
    } else if (fc.active) {
        CG_OrchFreecamEnd(here);
    } else if (!here) {
        CG_OrchFreecamBegin(qfalse);
    }
}

void CG_OrchGhost_f(void)
{
    const qboolean here = cgi.Argc() > 1 && !Q_stricmp(cgi.Argv(1), "here") ? qtrue : qfalse;

    if (!cg.snap) {
        return;
    }
    if (!orch_active->integer) {
        cgi.Printf("orch_ghost: turn the orchestrator on first (orch)\n");
        return;
    }

    if (fc.active && (fc.live || here)) {
        CG_OrchFreecamEnd(here);
    } else if (!here) {
        CG_OrchFreecamBegin(qtrue);
    }
}

void CG_OrchFly_f(void)
{
    if (!cg.snap || !orch_active->integer) {
        return;
    }

    if (fc.active) {
        fc.fly = !fc.fly;
        CG_OrchAddMsg(fc.fly ? "Flying" : "Walking", qfalse);
    } else {
        // The real thing; the server says whether it's on.
        cgi.SendClientCommand("noclip");
    }
}

// orch_return: back to where the player last froze, after a "here" gone wrong.
void CG_OrchReturn_f(void)
{
    if (!cg.snap || !orch_active->integer || fc.active || VectorCompare(fc.frozeAt, vec3_origin)) {
        return;
    }
    cgi.SendClientCommand(va("tele %.1f %.1f %.1f", fc.frozeAt[0], fc.frozeAt[1], fc.frozeAt[2]));
    CG_OrchAddMsg("Back to where you froze", qfalse);
    CG_OrchLog("return", "\"to\": " + CG_JsonVec(fc.frozeAt));
}

qboolean CG_OrchFreecamActive(void)
{
    return fc.active;
}

qboolean CG_OrchFreecamLive(void)
{
    return fc.active && fc.live ? qtrue : qfalse;
}

qboolean CG_OrchFreecamView(vec3_t origin, vec3_t angles)
{
    const int now = cgi.Milliseconds();
    int       dt;

    if (!fc.active) {
        return qfalse;
    }
    if (!fc.live && !CG_OrchPaused()) {
        // Unpaused some other way (the pause key, a menu).
        fc.active = qfalse;
        cgi.Cvar_Set("cl_freecam", "0");
        return qfalse;
    }

    // The body left behind is still in the fight.
    if (fc.live) {
        const int health = cg.snap->ps.stats[STAT_HEALTH];

        if (health <= 0) {
            CG_OrchFreecamEnd(qfalse);
            CG_OrchAddMsg("Your body died", qfalse);
            CG_OrchLog("body_died", "");
            return qfalse;
        }
        if (health < fc.health && now - fc.hitMsgAt > 1000) {
            CG_OrchAddMsg(va("Your body is hit: health %d", health), qfalse);
            fc.hitMsgAt = now;
        }
        fc.health = health;
    }

    dt        = now - fc.lastMs;
    fc.lastMs = now;
    if (dt > 100) {
        dt = 100;
    }

    if (dt > 0) {
        pmove_t pm;

        memset(&pm, 0, sizeof(pm));
        cgi.GetUserCmd(cgi.GetCurrentCmdNumber(), &pm.cmd);

        fc.time += dt;
        pm.cmd.serverTime = fc.time;
        fc.ps.pm_type     = fc.fly ? PM_NOCLIP : PM_NORMAL;
        fc.ps.speed       = (int)(cg.predicted_player_state.speed * (fc.fly ? orch_flyspeed->value : 1.0f));

        pm.ps            = &fc.ps;
        pm.trace         = CG_PlayerTrace;
        pm.pointcontents = CG_PointContents;
        pm.tracemask     = MASK_PLAYERSOLID;
        pm.protocol      = cg_protocol;
        Pmove(&pm);
        VectorCopy(pm.mins, fc.mins);
        VectorCopy(pm.maxs, fc.maxs);
    }

    VectorCopy(fc.ps.origin, origin);
    origin[2] += fc.ps.viewheight;
    VectorCopy(fc.ps.viewangles, angles);
    return qtrue;
}

//=============================================================
// Every frame
//=============================================================

// Before the scene is drawn, so the outline is in the frame the marked
// screenshot is taken from.
void CG_OrchFrame(void)
{
    if (!cg.snap) {
        return;
    }

    CG_OrchLogFrame();

    if (orch.outlineUntil - cgi.Milliseconds() > 0) {
        if (orch.pick.target.kind == PICK_TARGET_PROP || orch.pick.target.kind == PICK_TARGET_ENTITY) {
            CG_PickDrawBox(&orch.pick.target, 0.2f, 1.0f, 0.4f);
        }
        if (orch.pick.hit) {
            CG_PickDrawHit(&orch.pick, 1.0f, 0.4f, 0.2f);
        }
    }
}

static void CG_OrchStepShot(void)
{
    char cmd[MAX_STRING_CHARS];

    cmd[0] = 0;
    orch.shotFrames++;

    switch (orch.shotState) {
    case ORCH_SHOT_CLEAN:
        if (orch.shotFrames == 1) {
            Com_sprintf(cmd, sizeof(cmd), "screenshotJPEG orch_%s", orch.shotId);
        } else if (orch.shotFrames > 2) {
            orch.shotState    = ORCH_SHOT_MARKED;
            orch.shotFrames   = 0;
            orch.outlineUntil = cgi.Milliseconds() + ORCH_OUTLINE;
        }
        break;
    case ORCH_SHOT_MARKED:
        if (orch.shotFrames == 2) {
            Com_sprintf(cmd, sizeof(cmd), "screenshotJPEG orch_%s_marked", orch.shotId);
        } else if (orch.shotFrames > 3) {
            orch.shotState = ORCH_SHOT_IDLE;
            if (CG_OrchSinglePlayer()) {
                cgi.SendClientCommand(va(
                    "bugreport_server %s %d orch",
                    orch.shotId,
                    orch.pick.target.kind == PICK_TARGET_ENTITY ? orch.pick.target.index : -1
                ));
                if (orch_shotsave->integer) {
                    Com_sprintf(cmd, sizeof(cmd), "savegame orch_%s", orch.shotId);
                    CG_OrchLog("save", va("\"savegame\": \"orch_%s\"", orch.shotId));
                }
            }
        }
        break;
    default:
        break;
    }

    // Run at once, so it is taken with this frame: a script's wait holds the
    // command buffer.
    if (cmd[0]) {
        cgi.Cmd_Execute(EXEC_NOW, cmd);
    }
}

// Word-wraps text to width pixels, one line per step down from *y. Only
// counts the lines when draw is false. Returns the number of lines.
static int CG_OrchWrap(const char *text, float x, float *y, float width, float line, fontheader_t *font, qboolean draw)
{
    char        buf[ORCH_MSG_LEN];
    const char *p     = text;
    int         lines = 0;

    while (*p) {
        int len = 0, fit = 0;

        // As many whole words as fit.
        while (p[len]) {
            int end = len;

            while (p[end] == ' ') {
                end++;
            }
            while (p[end] && p[end] != ' ') {
                end++;
            }
            if (end >= (int)sizeof(buf)) {
                break;
            }
            memcpy(buf, p, end);
            buf[end] = 0;
            if (fit && cgi.UI_FontStringWidth(font, buf, -1) * cgs.uiHiResScale[0] > width) {
                break;
            }
            fit = len = end;
        }
        if (!fit) {
            // One word wider than the panel.
            fit = (int)strlen(p) < (int)sizeof(buf) - 1 ? (int)strlen(p) : (int)sizeof(buf) - 1;
        }

        if (draw) {
            memcpy(buf, p, fit);
            buf[fit] = 0;
            cgi.R_DrawString(font, buf, x / cgs.uiHiResScale[0], *y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale);
        }
        *y += line;
        lines++;

        p += fit;
        while (*p == ' ') {
            p++;
        }
    }

    return lines;
}

static void CG_OrchDrawPanel(void)
{
    fontheader_t       *font     = cgs.media.attackerFont;
    const float         line     = 17.0f * cgs.uiHiResScale[1];
    const float         width    = cgs.glconfig.vidWidth * 0.36f;
    const float         x        = cgs.glconfig.vidWidth - width - 12.0f * cgs.uiHiResScale[0];
    const int           now      = cgi.Milliseconds();
    float               y        = cgs.glconfig.vidHeight * 0.12f;
    vec4_t              color;
    char                text[ORCH_MSG_LEN + 4];

    // The voice sidecar's state, and that the mode is on.
    if (orch_active->integer) {
        const char *label = "ORCH";

        CG_OrchColor(color, 0.7f, 0.7f, 0.7f, 1.0f);
        if (!Q_stricmp(orch.status, "listening")) {
            label = "ORCH  [REC]";
            CG_OrchColor(color, 1.0f, 0.35f, 0.3f, 1.0f);
        } else if (!Q_stricmp(orch.status, "thinking")) {
            label = "ORCH  [THINKING]";
            CG_OrchColor(color, 1.0f, 0.85f, 0.3f, 1.0f);
        } else if (!Q_stricmp(orch.status, "speaking")) {
            label = "ORCH  [SPEAKING]";
            CG_OrchColor(color, 0.4f, 0.75f, 1.0f, 1.0f);
        } else if (!Q_stricmp(orch.status, "ready")) {
            label = orch.statusKey[0] ? va("ORCH  [hold %s to talk]", orch.statusKey) : "ORCH  [ready]";
        } else if (!Q_stricmp(orch.status, "queue")) {
            // No agent answering: what is said is noted for later.
            label = orch.statusKey[0] ? va("ORCH  [QUEUE  hold %s to note]", orch.statusKey) : "ORCH  [QUEUE]";
            CG_OrchColor(color, 0.75f, 0.6f, 1.0f, 1.0f);
        } else if (!Q_stricmp(orch.status, "muted")) {
            label = "ORCH  [MUTED]";
        } else if (!orch.status[0] || !Q_stricmp(orch.status, "off")) {
            label = "ORCH  [no voice]";
        }

        cgi.R_SetColor(color);
        cgi.R_DrawString(font, label, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale);
        y += line;

        if (fc.active) {
            const char *mode;

            if (fc.live) {
                mode = fc.fly ? "GHOST  flying   B back   F11 freeze   N walk"
                              : "GHOST  walking   B back   F11 freeze   N fly";
            } else {
                mode = fc.fly ? "FROZEN  flying   F11 resume   N walk   B run"
                              : "FROZEN  walking   F11 resume   N fly   B run";
            }
            CG_OrchColor(color, 0.5f, 0.85f, 1.0f, 1.0f);
            cgi.R_SetColor(color);
            cgi.R_DrawString(
                font,
                mode,
                x / cgs.uiHiResScale[0],
                y / cgs.uiHiResScale[1],
                -1,
                cgs.uiHiResScale
            );
            y += line;
        }
        y += line * 0.4f;
    }

    // The replies, oldest first.
    for (int k = 0; k < ORCH_MAX_MSGS; k++) {
        const orchMsg_t *m   = &orch.msgs[(orch.nextMsg + k) % ORCH_MAX_MSGS];
        const int        age = now - m->time;
        float            alpha;
        float            top;

        if (!m->text[0] || age < 0 || age > ORCH_MSG_LIFE) {
            continue;
        }
        alpha = age > ORCH_MSG_LIFE - ORCH_MSG_FADE ? (float)(ORCH_MSG_LIFE - age) / ORCH_MSG_FADE : 1.0f;

        // The backing, as tall as the wrapped text.
        Q_strncpyz(text, m->heard ? va("> %s", m->text) : m->text, sizeof(text));
        top = y;
        {
            float measure = y;
            const int n   = CG_OrchWrap(text, x, &measure, width, line, font, qfalse);

            CG_OrchColor(color, 0.0f, 0.0f, 0.0f, 0.45f * alpha);
            cgi.R_SetColor(color);
            cgi.R_DrawBox(x - 6.0f, top - 3.0f, width + 12.0f, line * n + 6.0f);
        }

        if (m->heard) {
            CG_OrchColor(color, 0.75f, 0.75f, 0.75f, alpha);
        } else {
            CG_OrchColor(color, 1.0f, 1.0f, 1.0f, alpha);
        }
        cgi.R_SetColor(color);
        CG_OrchWrap(text, x, &y, width, line, font, qtrue);
        y += line * 0.5f;
    }

    cgi.R_SetColor(NULL);
}

void CG_OrchDraw2D(void)
{
    if (!cg.snap) {
        return;
    }

    if (orch.shotState != ORCH_SHOT_IDLE) {
        CG_OrchStepShot();
        // Nothing of the orchestrator's own in the shots.
        return;
    }

    CG_OrchDrawPanel();
}

void CG_OrchInit(void)
{
    orch_active   = cgi.Cvar_Get("orch_active", "0", 0);
    orch_shotsave = cgi.Cvar_Get("orch_shotsave", "1", CVAR_ARCHIVE);
    orch_flyspeed = cgi.Cvar_Get("orch_flyspeed", "2", CVAR_ARCHIVE);

    // Once, on the first run, on keys the stock binds leave free. Rebinding
    // or unbinding them afterwards sticks.
    // orch_bound counts the sets of keys bound so far.
    {
        const int bound = cgi.Cvar_Get("orch_bound", "0", CVAR_ARCHIVE)->integer;

        if (bound < 1) {
            cgi.Cmd_Execute(EXEC_APPEND, "bind F10 orch\nbind MOUSE3 orch_shot\n");
        }
        if (bound < 2) {
            cgi.Cmd_Execute(EXEC_APPEND, "bind F11 orch_freeze\nbind n orch_fly\n");
        }
        if (bound < 3) {
            cgi.Cmd_Execute(EXEC_APPEND, "bind b orch_ghost\nseta orch_bound 3\n");
        }
    }

    memset(&orch, 0, sizeof(orch));
    memset(&fc, 0, sizeof(fc));
    orchLog           = orchLog_t();
    orchLog.writer    = (long long)(CG_OrchNow() * 1000.0);
    orchLog.startedAt = cgi.Milliseconds();
    // Loads, vid_restart and map changes all start a new cgame.
    CG_OrchLog("cgame_start", "");
    // A camera left on by the last cgame (vid_restart while frozen).
    cgi.Cvar_Set("cl_freecam", "0");
}
