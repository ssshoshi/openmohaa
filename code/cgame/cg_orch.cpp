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
// The game's half is small. orch (F10) turns orchestrator mode on. orch_shot
// (MOUSE3) then takes a screenshot and writes what is under the crosshair to
// orch/events/<id>.json in the home path, which the voice sidecar puts on the
// same timeline as what was said. Replies come back as orch_msg commands,
// through the command directory (com_cmddir), and are shown in a panel.
//
// A shot writes:
//   orch/events/<id>.json          where the player is and what is aimed at
//   orch/events/<id>.server.txt    what the game knows of it (single player)
//   screenshots/orch_<id>.jpg      the clean frame
//   screenshots/orch_<id>_marked.jpg  the same, the target outlined
// and in single player the savegame orch_<id>, to come back to that moment.

// Jolt first: cg_local.h defines a LERP macro its headers collide with.
#include "cg_physics_local.h"
#include "cg_orch.h"
#include "cg_pick.h"

#include <ctime>
#include <string>

#define ORCH_MAX_MSGS    6
#define ORCH_MSG_LIFE    15000 // ms a reply stays up
#define ORCH_MSG_FADE    2000
#define ORCH_OUTLINE     1500 // ms the shot's target stays outlined
#define ORCH_MSG_LEN     512

static cvar_t *orch_active;
static cvar_t *orch_shotsave;

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
} orchState_t;

static orchState_t orch;

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
    j += in + va("\"health\": %d", cg.snap->ps.stats[STAT_HEALTH]);
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
// Every frame
//=============================================================

// Before the scene is drawn, so the outline is in the frame the marked
// screenshot is taken from.
void CG_OrchFrame(void)
{
    if (!cg.snap) {
        return;
    }

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
        } else if (!Q_stricmp(orch.status, "muted")) {
            label = "ORCH  [MUTED]";
        } else if (!orch.status[0] || !Q_stricmp(orch.status, "off")) {
            label = "ORCH  [no voice]";
        }

        cgi.R_SetColor(color);
        cgi.R_DrawString(font, label, x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale);
        y += line * 1.4f;
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

    // Once, on the first run, on keys the stock binds leave free. Rebinding
    // or unbinding them afterwards sticks.
    if (!cgi.Cvar_Get("orch_bound", "0", CVAR_ARCHIVE)->integer) {
        cgi.Cmd_Execute(EXEC_APPEND, "bind F10 orch\nbind MOUSE3 orch_shot\nseta orch_bound 1\n");
    }

    memset(&orch, 0, sizeof(orch));
}
