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
// In-game bug, idea and feedback reports.
//
// bugreport (bound to F8 by default) starts pick mode: the thing under the
// crosshair is outlined and described on screen for the chosen category. A
// second bugreport locks it and opens the report menu (ui/bugreport.urc),
// whose Submit runs br_submit. That writes a bundle to
// bugreports/<id>/ in the home path:
//
//   report.json   what the client knows: build, renderer, map, position,
//                 view, the locked target, the text typed in
//   server.txt    what the game knows of the target, the level and its
//                 scripts (single player; written by fgame, bugreport_server)
//   sounds.txt    the sounds playing (s_listactive)
//   configs/bugreports/<id>/config.cfg   the archived cvars (writeconfig,
//                 which writes under configs/)
//   pending       marks it for tools/bugreport/upload.py
//
// plus screenshots/br_<id>.jpg (clean) and br_<id>_marked.jpg (outlined), and
// in single player the savegame br_<id>, so the report can be reproduced from
// exactly where it was made. The uploader files it as a GitHub issue.

// Jolt first: cg_local.h defines a LERP macro its headers collide with.
#include "cg_physics_local.h"
#include "cg_bugreport.h"
#include "cg_props.h"

#include <ctime>
#include <string>

#define BR_RANGE      4096.0f
#define BR_WAIT_FRAMES 3 // for the menu to go and a screenshot to be taken
#define BR_MAX_SIZE    2048.0f // bigger is an animation carrier or a volume, not a thing

static cvar_t *br_type;
static cvar_t *br_category;
static cvar_t *br_title;
static cvar_t *br_desc;

enum {
    BR_IDLE,
    BR_PICK,      // following the crosshair
    BR_LOCKED,    // target fixed, the menu is up
    BR_SHOT,      // waiting to take the clean screenshot
    BR_SHOT_MARK, // waiting to take the outlined one
    BR_FINISH     // waiting for the screenshots to be written
};

enum {
    BR_TARGET_NONE,
    BR_TARGET_WORLD,
    BR_TARGET_PROP,
    BR_TARGET_ENTITY
};

typedef struct {
    int    kind;
    int    index; // prop or entity number
    vec3_t origin, axis[3], mins, maxs;
    char   model[MAX_QPATH];
} brTarget_t;

typedef struct {
    int        state;
    int        frames; // since the state began
    brTarget_t target;

    // Where the crosshair met the world, and what with.
    qboolean hit;
    vec3_t   hitPos, hitNormal;
    char     hitShader[MAX_QPATH];
    int      hitSurfaceFlags, hitContents;
    char     hitThrough[MAX_QPATH]; // the first invisible surface passed, if any

    // Where the report was made from.
    vec3_t viewOrg, viewAngles, playerOrigin;

    char id[64];
    char message[128];
    int  messageTime;
} brState_t;

static brState_t br;

static const char *br_categories[] = {"physics", "texture", "lighting", "sound", "model", "script", "gameplay", "performance", "other", NULL};

static const char *br_types[] = {"bug", "idea", "feedback", NULL};

//=============================================================
// Helpers
//=============================================================

static void CG_BugReportSay(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    Q_vsnprintf(br.message, sizeof(br.message), fmt, ap);
    va_end(ap);

    br.messageTime = cg.time;
    cgi.Printf("bugreport: %s\n", br.message);
}

static qboolean CG_BugReportInList(const char *value, const char **list)
{
    for (int i = 0; list[i]; i++) {
        if (!Q_stricmp(value, list[i])) {
            return qtrue;
        }
    }
    return qfalse;
}

static void CG_BugReportSetState(int state)
{
    br.state  = state;
    br.frames = 0;
}

static std::string CG_JsonString(const char *s)
{
    std::string out = "\"";

    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;

        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += va("\\u%04x", c);
            } else {
                out += (char)c;
            }
        }
    }

    return out + "\"";
}

static std::string CG_JsonVec(const vec3_t v)
{
    return va("[%.2f, %.2f, %.2f]", v[0], v[1], v[2]);
}

static const char *CG_BugReportETypeName(int eType)
{
    static const char *names[] = {
        "modelanim_skel", "modelanim", "vehicle", "player", "item", "general", "missile", "mover", "beam", "multibeam",
        "portal", "event_only", "rain", "leaf", "speaker", "push_trigger", "teleport_trigger", "decal", "emitter",
        "rope", "events"
    };

    if (eType >= 0 && eType < (int)ARRAY_LEN(names)) {
        return names[eType];
    }
    return "unknown";
}

// The shaders a TIKI's surfaces are drawn with, for the texture category.
static std::string CG_BugReportModelShaders(const char *model, qboolean json)
{
    std::string out;
    qhandle_t   h;
    dtiki_t    *tiki;

    if (!model[0] || model[0] == '*') {
        return json ? "[]" : "";
    }

    h    = cgi.R_RegisterModel(model);
    tiki = h ? cgi.R_Model_GetHandle(h) : NULL;
    if (!tiki) {
        return json ? "[]" : "";
    }

    if (json) {
        out = "[";
    }
    for (int i = 0; i < tiki->num_surfaces; i++) {
        const dtikisurface_t *s      = &tiki->surfaces[i];
        const char           *shader = s->shader[0];

        // Only the name it was given in the TIKI, when it was given one.
        if (!shader[0] && s->hShader[0]) {
            shader = cgi.R_GetShaderName(s->hShader[0]);
        }

        if (json) {
            if (i) {
                out += ", ";
            }
            out += "{\"surface\": " + CG_JsonString(s->name) + ", \"shader\": " + CG_JsonString(shader) + "}";
        } else {
            if (i) {
                out += "  ";
            }
            out += va("%s=%s", s->name, shader);
        }
    }
    if (json) {
        out += "]";
    }
    return out;
}

//=============================================================
// What is under the crosshair
//=============================================================

static qboolean CG_BugReportInBox(const vec3_t p, const vec3_t origin, const vec3_t axis[3], const vec3_t mins, const vec3_t maxs);

// Anything whose box the eye is in, or that is bigger than any one object, is
// left out: nothing can be aimed at from inside it, and the invisible models
// that carry a scripted animation across the map (m1l1's truck paths, five
// thousand units wide) would otherwise be picked wherever one looked.
static qboolean CG_BugReportTooBig(const vec3_t mins, const vec3_t maxs)
{
    return maxs[0] - mins[0] > BR_MAX_SIZE || maxs[1] - mins[1] > BR_MAX_SIZE || maxs[2] - mins[2] > BR_MAX_SIZE ? qtrue : qfalse;
}

static void CG_BugReportPick(void)
{
    const float  *start = cg.refdef.vieworg;
    const float  *dir   = cg.refdef.viewaxis[0];
    brTarget_t    best;
    float         bestAt, at;
    trace_t       tr, first;
    vec3_t        end;
    baseshader_t *shader;

    memset(&best, 0, sizeof(best));
    best.index = -1;

    VectorMA(start, BR_RANGE, dir, end);
    CG_Trace(&tr, start, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, MASK_SHOT, qfalse, qfalse, "bugreport");

    // Anything in front of the world: props sit inside the clip brushes that
    // stand in for them, so a little past it too.
    bestAt = tr.fraction * BR_RANGE + 24.0f;

    // The surface seen, not the clip brush in front of it: on through the
    // invisible ones, remembering the first. When nothing visible is found
    // behind it (a clip volume the terrain lies inside), the invisible one is
    // what is reported.
    br.hitThrough[0] = 0;
    first = tr;
    for (int i = 0; i < 8 && tr.fraction < 1.0f && !tr.startsolid; i++) {
        vec3_t from;

        shader = cgi.GetShader(tr.shaderNum);
        if (!shader || !(shader->surfaceFlags & SURF_NODRAW)) {
            break;
        }
        if (!br.hitThrough[0]) {
            Q_strncpyz(br.hitThrough, shader->shader, sizeof(br.hitThrough));
        }
        // Out the far side of the brush: a trace from inside it starts solid.
        VectorCopy(tr.endpos, from);
        for (int step = 0; step < 64; step++) {
            VectorMA(from, 4.0f, dir, from);
            CG_Trace(&tr, from, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, MASK_SHOT, qfalse, qfalse, "bugreport");
            if (!tr.startsolid) {
                break;
            }
        }
    }

    if (tr.fraction >= 1.0f || tr.startsolid) {
        tr               = first;
        br.hitThrough[0] = 0;
    }

    br.hit = (tr.fraction < 1.0f && !tr.startsolid) ? qtrue : qfalse;
    br.hitShader[0] = 0;
    br.hitSurfaceFlags = br.hitContents = 0;
    if (br.hit) {
        VectorCopy(tr.endpos, br.hitPos);
        VectorCopy(tr.plane.normal, br.hitNormal);
        shader = cgi.GetShader(tr.shaderNum);
        if (shader) {
            Q_strncpyz(br.hitShader, shader->shader, sizeof(br.hitShader));
            br.hitSurfaceFlags = shader->surfaceFlags;
            br.hitContents     = shader->contentFlags;
        }
        best.kind = BR_TARGET_WORLD;
    }

    for (int i = 0; i < cg_numProps; i++) {
        const cgProp_t *p = &cg_props[i];

        if (CG_BugReportTooBig(p->mins, p->maxs) || CG_BugReportInBox(start, p->origin, p->axis, p->mins, p->maxs)) {
            continue;
        }
        if (CG_PhysicsRayHitsBox(start, dir, BR_RANGE, p->origin, p->axis, p->mins, p->maxs, &at) && at < bestAt) {
            bestAt     = at;
            best.kind  = BR_TARGET_PROP;
            best.index = i;
            VectorCopy(p->origin, best.origin);
            AxisCopy(p->axis, best.axis);
            VectorCopy(p->mins, best.mins);
            VectorCopy(p->maxs, best.maxs);
            Q_strncpyz(best.model, p->name, sizeof(best.model));
        }
    }

    for (int i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        vec3_t               origin, axis[3], mins, maxs;

        if (es->number == cg.snap->ps.clientNum) {
            continue;
        }
        if (!CG_PhysicsEntityBox(es->number, origin, axis, mins, maxs) || CG_BugReportTooBig(mins, maxs)
            || CG_BugReportInBox(start, origin, axis, mins, maxs)) {
            continue;
        }
        if (CG_PhysicsRayHitsBox(start, dir, BR_RANGE, origin, axis, mins, maxs, &at) && at < bestAt) {
            bestAt     = at;
            best.kind  = BR_TARGET_ENTITY;
            best.index = es->number;
            VectorCopy(origin, best.origin);
            AxisCopy(axis, best.axis);
            VectorCopy(mins, best.mins);
            VectorCopy(maxs, best.maxs);
            if (es->modelindex > 0 && es->modelindex < MAX_MODELS) {
                Q_strncpyz(best.model, CG_ConfigString(CS_MODELS + es->modelindex), sizeof(best.model));
            } else {
                best.model[0] = 0;
            }
        }
    }

    br.target = best;
}

static void CG_BugReportDrawBox(const brTarget_t *t, float r, float g, float b)
{
    vec3_t corners[8];

    for (int i = 0; i < 8; i++) {
        const vec3_t local = {
            (i & 1) ? t->maxs[0] : t->mins[0], (i & 2) ? t->maxs[1] : t->mins[1], (i & 4) ? t->maxs[2] : t->mins[2]
        };

        VectorCopy(t->origin, corners[i]);
        for (int k = 0; k < 3; k++) {
            VectorMA(corners[i], local[k], t->axis[k], corners[i]);
        }
    }

    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 3; k++) {
            const int j = i | (1 << k);

            if (j != i) {
                cgi.R_DebugLine(corners[i], corners[j], r, g, b, 1.0f);
            }
        }
    }
}

// Whether a point is inside a box turned by axis about origin.
static qboolean CG_BugReportInBox(const vec3_t p, const vec3_t origin, const vec3_t axis[3], const vec3_t mins, const vec3_t maxs)
{
    vec3_t rel;

    VectorSubtract(p, origin, rel);
    for (int k = 0; k < 3; k++) {
        const float d = DotProduct(rel, axis[k]);

        if (d < mins[k] || d > maxs[k]) {
            return qfalse;
        }
    }
    return qtrue;
}

// A small cross where the crosshair meets the world.
static void CG_BugReportDrawHit(float r, float g, float b)
{
    for (int k = 0; k < 3; k++) {
        vec3_t a, c;

        VectorCopy(br.hitPos, a);
        VectorCopy(br.hitPos, c);
        a[k] -= 8.0f;
        c[k] += 8.0f;
        cgi.R_DebugLine(a, c, r, g, b, 1.0f);
    }
}

// The target's description for the overlay, one line per call, for the
// chosen category. Returns the number of lines written.
static int CG_BugReportDescribe(char lines[][256], int max)
{
    const char *cat = br_category->string;
    int         n   = 0;

#define BR_LINE(...)                                          \
    if (n < max) {                                            \
        Com_sprintf(lines[n++], sizeof(lines[0]), __VA_ARGS__); \
    }

    switch (br.target.kind) {
    case BR_TARGET_PROP:
        BR_LINE("static model #%d: %s", br.target.index, br.target.model);
        BR_LINE("  %s   %s", cg_props[br.target.index].solid ? (cg_props[br.target.index].dynamic ? "moves" : "fixed") : "not solid",
            cg_props[br.target.index].why ? cg_props[br.target.index].why : "");
        break;
    case BR_TARGET_ENTITY:
        BR_LINE("entity #%d (%s): %s", br.target.index, CG_BugReportETypeName(cg_entities[br.target.index].currentState.eType),
            br.target.model[0] ? br.target.model : "no model");
        break;
    case BR_TARGET_WORLD:
        BR_LINE("world");
        break;
    default:
        BR_LINE("nothing under the crosshair");
        break;
    }

    if (!Q_stricmp(cat, "texture")) {
        if (br.hit) {
            BR_LINE("surface: %s   flags 0x%x contents 0x%x", br.hitShader, br.hitSurfaceFlags, br.hitContents);
        }
        if (br.hitThrough[0]) {
            BR_LINE("  behind invisible %s", br.hitThrough);
        }
        if (br.target.kind == BR_TARGET_PROP || br.target.kind == BR_TARGET_ENTITY) {
            BR_LINE("model shaders: %s", CG_BugReportModelShaders(br.target.model, qfalse).c_str());
        }
    } else if (!Q_stricmp(cat, "lighting")) {
        vec3_t light;

        if (br.hit) {
            cgi.R_GetLightingForDecal(light, br.hitNormal, br.hitPos);
            BR_LINE("light at the crosshair: %.0f %.0f %.0f   (%.0f %.0f %.0f)", light[0], light[1], light[2],
                br.hitPos[0], br.hitPos[1], br.hitPos[2]);
        }
        cgi.R_GetLightingForSmoke(light, cg.refdef.vieworg);
        BR_LINE("light at the eye: %.0f %.0f %.0f", light[0], light[1], light[2]);
    } else if (br.hit) {
        BR_LINE("aim: %.0f %.0f %.0f on %s", br.hitPos[0], br.hitPos[1], br.hitPos[2], br.hitShader);
    }

#undef BR_LINE
    return n;
}

//=============================================================
// The bundle
//=============================================================

// maps/m1l1.bsp -> m1l1, as devmap takes it.
static const char *CG_BugReportMapName(void)
{
    static char map[64];

    Q_strncpyz(map, cgs.mapname, sizeof(map));
    if (!Q_stricmpn(map, "maps/", 5)) {
        memmove(map, map + 5, strlen(map + 5) + 1);
    }
    COM_StripExtension(map, map, sizeof(map));
    return map;
}

static void CG_BugReportMakeId(void)
{
    const time_t now = time(NULL);
    struct tm   *t   = localtime(&now);
    char         map[64], *p;

    Q_strncpyz(map, CG_BugReportMapName(), sizeof(map));
    for (p = map; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            *p = '_';
        }
    }

    Com_sprintf(
        br.id,
        sizeof(br.id),
        "%04d%02d%02d-%02d%02d%02d_%s",
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        map
    );
}

static std::string CG_BugReportCvar(const char *name)
{
    cvar_t *cv = cgi.Cvar_Get(name, "", 0);
    return CG_JsonString(cv ? cv->string : "");
}

static void CG_BugReportWriteJson(void)
{
    std::string j;
    char        lines[8][256];
    const int   numLines = CG_BugReportDescribe(lines, 8);
    const float fps      = cg.frametime > 0 ? 1000.0f / cg.frametime : 0.0f;
    const qboolean sp    = cgs.gametype == GT_SINGLE_PLAYER ? qtrue : qfalse;

    j += "{\n";
    j += "  \"schema\": 1,\n";
    j += "  \"id\": " + CG_JsonString(br.id) + ",\n";
    j += "  \"type\": " + CG_JsonString(br_type->string) + ",\n";
    j += "  \"category\": " + CG_JsonString(br_category->string) + ",\n";
    j += "  \"title\": " + CG_JsonString(br_title->string) + ",\n";
    j += "  \"description\": " + CG_JsonString(br_desc->string) + ",\n";

    j += "  \"build\": {\n";
    j += "    \"version\": " + CG_BugReportCvar("version") + ",\n";
    j += "    \"renderer\": " + CG_BugReportCvar("cl_renderer") + ",\n";
    j += "    \"gl_vendor\": " + CG_JsonString(cgs.glconfig.vendor_string) + ",\n";
    j += "    \"gl_renderer\": " + CG_JsonString(cgs.glconfig.renderer_string) + ",\n";
    j += va("    \"resolution\": [%d, %d],\n", cgs.glconfig.vidWidth, cgs.glconfig.vidHeight);
    j += va("    \"fps\": %.0f,\n", fps);
    j += "    \"fs_game\": " + CG_BugReportCvar("fs_game") + "\n";
    j += "  },\n";

    j += "  \"game\": {\n";
    j += "    \"map\": " + CG_JsonString(cgs.mapname) + ",\n";
    j += va("    \"single_player\": %s,\n", sp ? "true" : "false");
    j += va("    \"server_time\": %d,\n", cg.snap->serverTime);
    j += "    \"player_origin\": " + CG_JsonVec(br.playerOrigin) + ",\n";
    j += "    \"view_origin\": " + CG_JsonVec(br.viewOrg) + ",\n";
    j += "    \"view_angles\": " + CG_JsonVec(br.viewAngles) + ",\n";
    j += va("    \"health\": %d,\n", cg.snap->ps.stats[STAT_HEALTH]);
    j += va(
        "    \"repro\": \"devmap %s; setviewpos %.0f %.0f %.0f %.0f\"",
        CG_BugReportMapName(),
        br.playerOrigin[0],
        br.playerOrigin[1],
        br.playerOrigin[2],
        br.viewAngles[YAW]
    );
    j += sp ? va(",\n    \"savegame\": \"br_%s\"\n", br.id) : "\n";
    j += "  },\n";

    j += "  \"aim\": {\n";
    j += va("    \"hit\": %s", br.hit ? "true" : "false");
    if (br.hit) {
        vec3_t light;

        cgi.R_GetLightingForDecal(light, br.hitNormal, br.hitPos);
        j += ",\n    \"position\": " + CG_JsonVec(br.hitPos) + ",\n";
        j += "    \"normal\": " + CG_JsonVec(br.hitNormal) + ",\n";
        j += "    \"shader\": " + CG_JsonString(br.hitShader) + ",\n";
        j += "    \"through_invisible\": " + CG_JsonString(br.hitThrough) + ",\n";
        j += va("    \"surface_flags\": %d,\n", br.hitSurfaceFlags);
        j += va("    \"contents\": %d,\n", br.hitContents);
        j += "    \"light\": " + CG_JsonVec(light) + ",\n";
        j += va("    \"distance\": %.0f", Distance(br.viewOrg, br.hitPos));
    }
    j += "\n  },\n";

    {
        vec3_t light;

        cgi.R_GetLightingForSmoke(light, br.viewOrg);
        j += "  \"light_at_eye\": " + CG_JsonVec(light) + ",\n";
    }

    j += "  \"target\": {\n";
    switch (br.target.kind) {
    case BR_TARGET_PROP:
        {
            const cgProp_t *p = &cg_props[br.target.index];

            j += "    \"kind\": \"static_model\",\n";
            j += va("    \"index\": %d,\n", br.target.index);
            j += va("    \"static_index\": %d,\n", p->staticIndex);
            j += va("    \"solid\": %s,\n", p->solid ? "true" : "false");
            j += va("    \"dynamic\": %s,\n", p->dynamic ? "true" : "false");
            j += "    \"physics_why\": " + CG_JsonString(p->why ? p->why : "") + ",\n";
            j += "    \"angles\": " + CG_JsonVec(p->angles) + ",\n";
        }
        break;
    case BR_TARGET_ENTITY:
        {
            const centity_t *cent = &cg_entities[br.target.index];

            j += "    \"kind\": \"entity\",\n";
            j += va("    \"entnum\": %d,\n", br.target.index);
            j += "    \"etype\": " + CG_JsonString(CG_BugReportETypeName(cent->currentState.eType)) + ",\n";
            j += "    \"angles\": " + CG_JsonVec(cent->lerpAngles) + ",\n";
        }
        break;
    case BR_TARGET_WORLD:
        j += "    \"kind\": \"world\",\n";
        break;
    default:
        j += "    \"kind\": \"none\",\n";
        break;
    }
    if (br.target.kind == BR_TARGET_PROP || br.target.kind == BR_TARGET_ENTITY) {
        j += "    \"model\": " + CG_JsonString(br.target.model) + ",\n";
        j += "    \"origin\": " + CG_JsonVec(br.target.origin) + ",\n";
        j += "    \"mins\": " + CG_JsonVec(br.target.mins) + ",\n";
        j += "    \"maxs\": " + CG_JsonVec(br.target.maxs) + ",\n";
        j += "    \"model_shaders\": " + CG_BugReportModelShaders(br.target.model, qtrue) + ",\n";
    }
    j += "    \"overlay\": [";
    for (int i = 0; i < numLines; i++) {
        j += (i ? ", " : "") + CG_JsonString(lines[i]);
    }
    j += "]\n  },\n";

    j += "  \"files\": {\n";
    j += va("    \"screenshot\": \"screenshots/br_%s.jpg\",\n", br.id);
    j += va("    \"screenshot_marked\": \"screenshots/br_%s_marked.jpg\"", br.id);
    j += sp ? ",\n    \"server\": \"server.txt\"" : "";
    // writeconfig puts everything under configs/.
    j += va(",\n    \"sounds\": \"sounds.txt\",\n    \"config\": \"configs/bugreports/%s/config.cfg\"\n", br.id);
    j += "  }\n";
    j += "}\n";

    cgi.FS_WriteFile(va("bugreports/%s/report.json", br.id), j.c_str(), (int)j.length());
}

//=============================================================
// Commands
//=============================================================

// bugreport [category|cancel]: the first press starts pick mode, the second
// locks the target and opens the menu.
void CG_BugReport_f(void)
{
    const char *arg = cgi.Argc() > 1 ? cgi.Argv(1) : "";

    if (!cg.snap) {
        return;
    }

    if (!Q_stricmp(arg, "cancel")) {
        CG_BugReportSetState(BR_IDLE);
        cgi.UI_HideMenu("bugreport", qtrue);
        return;
    }

    if (arg[0]) {
        if (!CG_BugReportInList(arg, br_categories)) {
            cgi.Printf("bugreport: categories are physics texture lighting sound model script gameplay performance other\n");
            return;
        }
        cgi.Cvar_Set("br_category", arg);
    }

    switch (br.state) {
    case BR_IDLE:
        CG_BugReportSetState(BR_PICK);
        break;
    case BR_PICK:
        VectorCopy(cg.refdef.vieworg, br.viewOrg);
        VectorCopy(cg.refdefViewAngles, br.viewAngles);
        VectorCopy(cg.snap->ps.origin, br.playerOrigin);
        CG_BugReportSetState(BR_LOCKED);
        cgi.UI_ShowMenu("bugreport", qtrue);
        break;
    case BR_LOCKED:
        // Back to aiming.
        cgi.UI_HideMenu("bugreport", qtrue);
        CG_BugReportSetState(BR_PICK);
        break;
    default:
        // Still writing the last one.
        break;
    }
}

// br_submit: from the menu, or the console once a target is locked. With
// arguments it can be run on its own: br_submit <type> <category> <title> [description]
void CG_BugReportSubmit_f(void)
{
    if (!cg.snap) {
        return;
    }

    if (cgi.Argc() >= 4) {
        cgi.Cvar_Set("br_type", cgi.Argv(1));
        cgi.Cvar_Set("br_category", cgi.Argv(2));
        cgi.Cvar_Set("br_title", cgi.Argv(3));
        cgi.Cvar_Set("br_desc", cgi.Argc() > 4 ? cgi.Argv(4) : "");
        if (br.state == BR_IDLE || br.state == BR_PICK) {
            CG_BugReportPick();
            VectorCopy(cg.refdef.vieworg, br.viewOrg);
            VectorCopy(cg.refdefViewAngles, br.viewAngles);
            VectorCopy(cg.snap->ps.origin, br.playerOrigin);
            br.state = BR_LOCKED;
        }
    }

    if (br.state != BR_LOCKED) {
        CG_BugReportSay("aim with bugreport first");
        return;
    }
    if (!CG_BugReportInList(br_type->string, br_types)) {
        cgi.Cvar_Set("br_type", "bug");
    }
    if (!CG_BugReportInList(br_category->string, br_categories)) {
        cgi.Cvar_Set("br_category", "other");
    }
    if (!br_title->string[0]) {
        CG_BugReportSay("give the report a title");
        return;
    }

    cgi.UI_HideMenu("bugreport", qtrue);
    br.message[0] = 0;
    CG_BugReportMakeId();
    CG_BugReportSetState(BR_SHOT);
}

static void CG_BugReportFinish(void)
{
    const qboolean sp = cgs.gametype == GT_SINGLE_PLAYER ? qtrue : qfalse;

    CG_BugReportWriteJson();

    cgi.Cmd_Execute(EXEC_NOW, va("writeconfig bugreports/%s/config.cfg\n", br.id));
    cgi.Cmd_Execute(EXEC_NOW, va("s_listactive bugreports/%s/sounds.txt\n", br.id));
    if (sp) {
        cgi.SendClientCommand(
            va("bugreport_server %s %d", br.id, br.target.kind == BR_TARGET_ENTITY ? br.target.index : -1)
        );
        cgi.Cmd_Execute(EXEC_NOW, va("savegame br_%s\n", br.id));
    }
    cgi.FS_WriteFile(va("bugreports/%s/pending", br.id), "", 0);

    CG_BugReportSay("report saved: %s", br.id);
    cgi.Cvar_Set("br_title", "");
    cgi.Cvar_Set("br_desc", "");
    CG_BugReportSetState(BR_IDLE);
}

//=============================================================
// Every frame
//=============================================================

// Before the scene is drawn, so the outline is in the same frame the marked
// screenshot is taken from (debug lines added later wait for the next one).
void CG_BugReportFrame(void)
{
    if (!cg.snap) {
        return;
    }

    if (br.state == BR_PICK) {
        CG_BugReportPick();
    }

    if (br.state == BR_PICK || br.state == BR_LOCKED || br.state == BR_SHOT_MARK) {
        if (br.target.kind == BR_TARGET_PROP || br.target.kind == BR_TARGET_ENTITY) {
            CG_BugReportDrawBox(&br.target, 1.0f, 1.0f, 0.2f);
        }
        if (br.hit) {
            CG_BugReportDrawHit(0.2f, 1.0f, 1.0f);
        }
    }
}

void CG_BugReportDraw2D(void)
{
    static const vec4_t white  = {1.0f, 1.0f, 1.0f, 1.0f};
    static const vec4_t yellow = {1.0f, 1.0f, 0.3f, 1.0f};
    static const vec4_t grey   = {0.75f, 0.75f, 0.75f, 1.0f};
    const float         line   = 15.0f * cgs.uiHiResScale[1];
    float               x, y;
    char                lines[8][256];
    int                 n;
    qboolean            overlay;
    char                shot[MAX_STRING_CHARS];

    if (!cg.snap) {
        return;
    }

    // Its own buffer: the overlay below goes through va() many times.
    shot[0] = 0;
    br.frames++;

    switch (br.state) {
    case BR_PICK:
    case BR_LOCKED:
        overlay = qtrue;
        break;
    case BR_SHOT:
        // The menu gone and nothing of the report's own on screen.
        overlay = qfalse;
        if (br.frames == BR_WAIT_FRAMES) {
            Com_sprintf(shot, sizeof(shot), "screenshotJPEG br_%s", br.id);
        } else if (br.frames > BR_WAIT_FRAMES + 1) {
            CG_BugReportSetState(BR_SHOT_MARK);
        }
        break;
    case BR_SHOT_MARK:
        overlay = qtrue;
        if (br.frames == BR_WAIT_FRAMES) {
            Com_sprintf(shot, sizeof(shot), "screenshotJPEG br_%s_marked", br.id);
        } else if (br.frames > BR_WAIT_FRAMES + 1) {
            CG_BugReportSetState(BR_FINISH);
        }
        break;
    case BR_FINISH:
        overlay = qfalse;
        if (br.frames > 1) {
            CG_BugReportFinish();
        }
        break;
    default:
        overlay = qfalse;
        break;
    }

    x = 8.0f * cgs.uiHiResScale[0];
    y = cgs.glconfig.vidHeight * 0.3f;

#define BR_TEXT(text)                                                                                                     \
    cgi.R_DrawString(cgs.media.attackerFont, (text), x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale); \
    y += line

    if (overlay) {
        cgi.R_SetColor(yellow);
        BR_TEXT(va("REPORT: %s / %s   %s", br_type->string, br_category->string,
            br.state == BR_PICK ? "aim, then press the report key again to lock" : "locked"));

        cgi.R_SetColor(white);
        n = CG_BugReportDescribe(lines, 8);
        for (int i = 0; i < n; i++) {
            BR_TEXT(lines[i]);
        }

        cgi.R_SetColor(grey);
        BR_TEXT(va("at %.0f %.0f %.0f  facing %.0f %.0f  on %s", cg.refdef.vieworg[0], cg.refdef.vieworg[1],
            cg.refdef.vieworg[2], cg.refdefViewAngles[PITCH], cg.refdefViewAngles[YAW], cgs.mapname));
    }

    if (br.message[0] && cg.time - br.messageTime < 5000 && cg.time >= br.messageTime) {
        cgi.R_SetColor(white);
        BR_TEXT(br.message);
    }

#undef BR_TEXT

    cgi.R_SetColor(NULL);

    // Run at once rather than queued, so it is taken with this frame, after
    // what was drawn above: a script's wait holds the command buffer, and the
    // two shots and the files would otherwise come in any order.
    if (shot[0]) {
        cgi.Cmd_Execute(EXEC_NOW, shot);
    }
}

void CG_BugReportInit(void)
{
    br_type     = cgi.Cvar_Get("br_type", "bug", 0);
    br_category = cgi.Cvar_Get("br_category", "other", 0);
    br_title    = cgi.Cvar_Get("br_title", "", 0);
    br_desc     = cgi.Cvar_Get("br_desc", "", 0);

    // F8 once, on the first run: next to quicksave and quickload, and free in
    // the stock binds. Rebinding or unbinding it afterwards sticks.
    if (!cgi.Cvar_Get("br_bound", "0", CVAR_ARCHIVE)->integer) {
        cgi.Cmd_Execute(EXEC_APPEND, "bind F8 bugreport\nseta br_bound 1\n");
    }
    // And F7 for the cvar browser (client/cl_uicvarbrowser.cpp), the same way.
    if (!cgi.Cvar_Get("cvb_bound", "0", CVAR_ARCHIVE)->integer) {
        cgi.Cmd_Execute(EXEC_APPEND, "bind F7 cvarbrowser\nseta cvb_bound 1\n");
    }

    memset(&br, 0, sizeof(br));
}
