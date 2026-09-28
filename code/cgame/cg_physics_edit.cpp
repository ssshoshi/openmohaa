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
// physics.txt on the client (see code/physics/phys_rules.h), and the physics
// editor that writes it.
//
// The rules are read with the map, before the world is built from it: they
// decide which static models are solid and which move, and which furniture in
// the brushwork moves. The server reads the same file for its crates, barrels
// and small objects (g_physics.cpp).
//
// phys_edit turns the editor on: every physics object near the view is
// outlined, green if it moves, red if it is fixed, grey if it is left out, and
// the one under the crosshair yellow, described on screen with what decided
// it. phys_toggle, phys_off, phys_mass and phys_forget change the rules for it
// (or its model), save the file and build the physics again, so a change shows
// at once. The server's objects are edited through it in single player only.

#include "cg_physics_local.h"
#include "cg_props.h"
#include "../physics/phys_rules.h"

#include <cstdarg>
#include <map>
#include <string>

#define PE_FILE        "physics.txt"
#define PE_RANGE       1024.0f
#define PE_DRAW_RANGE  600.0f
#define PE_OFF_RANGE   300.0f // left-out props (lights, foliage) crowd the view
#define PE_MAX_LINES   4000

cvar_t *cg_physics_edit;

static PhysRules pe_rules;

// The server's candidates (physlist): entity number to PHYS_RULE_*.
static std::map<int, int> pe_entities;
static int                pe_listAsked;

// What the server said of the entity under the crosshair (physinfo).
typedef struct {
    qboolean valid;
    int      entnum;
    int      state;
    float    mass;
    char     classname[64];
    char     model[MAX_QPATH];
    char     key[64];
    char     why[96];
} peInfo_t;

static peInfo_t pe_info;
static int      pe_infoAsked, pe_infoAskTime;

enum {
    PE_NONE,
    PE_PROP,
    PE_FURNITURE,
    PE_ENTITY
};

typedef struct {
    int kind;
    int index; // prop, furniture piece or entity number
} peTarget_t;

static peTarget_t pe_target;

static char pe_message[256];
static int  pe_messageTime;

//=============================================================
// The rules
//=============================================================

void CG_PhysicsRulesLoad(void)
{
    void *buf = NULL;
    long  len = cgi.FS_ReadFile(PE_FILE, &buf, qtrue);

    if (len > 0 && buf) {
        const std::string text((const char *)buf, (size_t)len);
        pe_rules.Parse(text.c_str());
    } else {
        pe_rules.Parse("");
    }

    if (buf) {
        cgi.FS_FreeFile(buf);
    }
}

void CG_PhysicsApplyPropRules(void)
{
    int ruled = 0;

    for (int i = 0; i < cg_numProps; i++) {
        cgProp_t *p = &cg_props[i];
        char      key[16];

        Com_sprintf(key, sizeof(key), "%d", p->staticIndex);
        const physRuleResult_t r = pe_rules.Resolve(cgs.mapname, "static", key, p->name, NULL);

        if (r.rule.mass > 0.0f) {
            p->mass = r.rule.mass;
        }
        if (r.rule.state == PHYS_RULE_UNSET) {
            continue;
        }

        p->solid   = r.rule.state != PHYS_RULE_OFF ? qtrue : qfalse;
        p->dynamic = r.rule.state == PHYS_RULE_MOVES ? qtrue : qfalse;
        p->why     = r.from;
        ruled++;
    }

    if (cg_physics_log->integer && ruled) {
        cgi.Printf("physics: %s decides %d of the map's static models\n", PE_FILE, ruled);
    }
}

qboolean CG_PhysicsFurnitureRule(int firstBrush, const char **why, float *mass)
{
    char key[16];

    Com_sprintf(key, sizeof(key), "%d", firstBrush);
    const physRuleResult_t r = pe_rules.Resolve(cgs.mapname, "furniture", key, NULL, NULL);

    *mass = r.rule.mass;
    if (r.rule.state == PHYS_RULE_UNSET) {
        *why = "found in the brushwork";
        return qtrue;
    }

    *why = r.from;
    return r.rule.state == PHYS_RULE_MOVES ? qtrue : qfalse;
}

static void CG_PhysicsEditSay(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    Q_vsnprintf(pe_message, sizeof(pe_message), fmt, ap);
    va_end(ap);

    pe_messageTime = cg.time;
    cgi.Printf("%s\n", pe_message);
}

static qboolean CG_PhysicsServerEdits(void)
{
    return (cgs.localServer && cgs.gametype == GT_SINGLE_PLAYER) ? qtrue : qfalse;
}

static void CG_PhysicsEditAskList(void)
{
    if (CG_PhysicsServerEdits()) {
        cgi.SendClientCommand("physlist");
        pe_listAsked = cg.time;
    }
}

//=============================================================
// What there is, and what is under the crosshair
//=============================================================

// A box turned by axis about origin, as the physics objects are.
typedef struct {
    vec3_t origin, axis[3], mins, maxs;
} peBox_t;

static qboolean CG_PhysicsEditPropBox(int i, peBox_t *box)
{
    const cgProp_t *p = &cg_props[i];

    VectorCopy(p->origin, box->origin);
    AxisCopy(p->axis, box->axis);
    VectorCopy(p->mins, box->mins);
    VectorCopy(p->maxs, box->maxs);
    return qtrue;
}

static qboolean CG_PhysicsEditFurnitureBox(int i, peBox_t *box, int *brush, qboolean *moves, const char **why, const char **shader)
{
    if (!CG_PhysicsFurnitureInfo(i, brush, moves, why, shader, box->mins, box->maxs)) {
        return qfalse;
    }

    VectorClear(box->origin);
    AxisClear(box->axis);
    return qtrue;
}

static qboolean CG_PhysicsEditEntityBox(int entnum, peBox_t *box)
{
    const centity_t     *cent;
    const entityState_t *es;

    if (entnum < 0 || entnum >= MAX_GENTITIES) {
        return qfalse;
    }

    cent = &cg_entities[entnum];
    es   = &cent->currentState;
    if (!cent->currentValid) {
        return qfalse;
    }

    VectorCopy(cent->lerpOrigin, box->origin);
    AnglesToAxis(cent->lerpAngles, box->axis);

    if (es->solid == SOLID_BMODEL) {
        if (es->modelindex <= 0 || es->modelindex >= MAX_MODELS) {
            return qfalse;
        }
        cgi.R_ModelBounds(cgs.inlineDrawModel[es->modelindex], box->mins, box->maxs);
    } else if (es->solid) {
        IntegerToBoundingBox(es->solid, box->mins, box->maxs);
        AxisClear(box->axis);
    } else {
        if (es->modelindex <= 0 || es->modelindex >= MAX_MODELS) {
            return qfalse;
        }
        cgi.R_ModelBounds(cgs.model_draw[es->modelindex], box->mins, box->maxs);
    }

    // Nothing to aim at: a small cube.
    if (box->maxs[0] - box->mins[0] < 1.0f || box->maxs[1] - box->mins[1] < 1.0f) {
        VectorSet(box->mins, -8.0f, -8.0f, -8.0f);
        VectorSet(box->maxs, 8.0f, 8.0f, 8.0f);
    }
    return qtrue;
}

// Where a ray enters a box, or qfalse.
static qboolean CG_PhysicsEditRayBox(const vec3_t start, const vec3_t dir, const peBox_t *box, float *enter)
{
    vec3_t rel, s, d;
    float  lo = 0.0f, hi = PE_RANGE;
    int    k;

    VectorSubtract(start, box->origin, rel);
    for (k = 0; k < 3; k++) {
        s[k] = DotProduct(rel, box->axis[k]);
        d[k] = DotProduct(dir, box->axis[k]);
    }

    for (k = 0; k < 3; k++) {
        if (fabs(d[k]) < 1e-6f) {
            if (s[k] < box->mins[k] || s[k] > box->maxs[k]) {
                return qfalse;
            }
            continue;
        }

        float t0 = (box->mins[k] - s[k]) / d[k];
        float t1 = (box->maxs[k] - s[k]) / d[k];
        if (t0 > t1) {
            const float t = t0;
            t0            = t1;
            t1            = t;
        }
        lo = Q_max(lo, t0);
        hi = Q_min(hi, t1);
        if (lo > hi) {
            return qfalse;
        }
    }

    *enter = lo;
    return qtrue;
}

static int CG_PhysicsEditPropState(const cgProp_t *p)
{
    return !p->solid ? PHYS_RULE_OFF : p->dynamic ? PHYS_RULE_MOVES : PHYS_RULE_FIXED;
}

static void CG_PhysicsEditPick(void)
{
    const float *start = cg.refdef.vieworg;
    const float *dir   = cg.refdef.viewaxis[0];
    peTarget_t   best = {PE_NONE, -1}, bestOff = {PE_NONE, -1};
    float        bestAt, bestOffAt, at, limit;
    trace_t      tr;
    vec3_t       end;
    peBox_t      box;

    // Up to the world in the way, and a little past: props sit inside the
    // clip brushes that stand in for them.
    VectorMA(start, PE_RANGE, dir, end);
    CG_Trace(&tr, start, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, MASK_SOLID, qfalse, qfalse, "phys_edit");
    limit     = tr.fraction * PE_RANGE + 24.0f;
    bestAt    = limit;
    bestOffAt = limit;

    for (int i = 0; i < cg_numProps; i++) {
        CG_PhysicsEditPropBox(i, &box);
        if (!CG_PhysicsEditRayBox(start, dir, &box, &at)) {
            continue;
        }

        if (!cg_props[i].solid) {
            if (at < bestOffAt) {
                bestOffAt = at;
                bestOff.kind  = PE_PROP;
                bestOff.index = i;
            }
        } else if (at < bestAt) {
            bestAt     = at;
            best.kind  = PE_PROP;
            best.index = i;
        }
    }

    for (int i = 0; i < CG_PhysicsFurnitureCount(); i++) {
        int         brush;
        qboolean    moves;
        const char *why, *shader;

        if (CG_PhysicsEditFurnitureBox(i, &box, &brush, &moves, &why, &shader) && CG_PhysicsEditRayBox(start, dir, &box, &at)
            && at < bestAt) {
            bestAt     = at;
            best.kind  = PE_FURNITURE;
            best.index = i;
        }
    }

    for (std::map<int, int>::const_iterator it = pe_entities.begin(); it != pe_entities.end(); ++it) {
        if (CG_PhysicsEditEntityBox(it->first, &box) && CG_PhysicsEditRayBox(start, dir, &box, &at) && at < bestAt) {
            bestAt     = at;
            best.kind  = PE_ENTITY;
            best.index = it->first;
        }
    }

    // Something solid, unless a left-out prop (a lamp, a bush) is well in
    // front of it.
    {
        const peTarget_t was = pe_target;

        if (best.kind != PE_NONE && (bestOff.kind == PE_NONE || bestAt <= bestOffAt + 32.0f)) {
            pe_target = best;
        } else {
            pe_target = bestOff;
        }

        if (cg_physics_log->integer > 2 && (was.kind != pe_target.kind || was.index != pe_target.index)) {
            cgi.Printf(
                "phys_edit: now kind %d #%d at %.0f (world %.0f%s) from %.0f %.0f %.0f along %.2f %.2f %.2f\n",
                pe_target.kind,
                pe_target.index,
                pe_target.kind == best.kind && pe_target.index == best.index ? bestAt : bestOffAt,
                tr.fraction * PE_RANGE,
                tr.startsolid ? ", in solid" : "",
                start[0],
                start[1],
                start[2],
                dir[0],
                dir[1],
                dir[2]
            );
        }
    }

    // The server says what it knows of an entity.
    if (pe_target.kind == PE_ENTITY && CG_PhysicsServerEdits()
        && (pe_infoAsked != pe_target.index || (!pe_info.valid && cg.time - pe_infoAskTime > 1000))) {
        cgi.SendClientCommand(va("physinfo %d", pe_target.index));
        if (cg_physics_log->integer > 1) {
            cgi.Printf("phys_edit: asked the server about entity %d\n", pe_target.index);
        }
        pe_infoAsked   = pe_target.index;
        pe_infoAskTime = cg.time;
        pe_info.valid  = qfalse;
    }
}

//=============================================================
// Drawing
//=============================================================

static int pe_lines;

static void CG_PhysicsEditDrawBox(const peBox_t *box, const float *colour)
{
    vec3_t corners[8];
    int    i, k;

    if (pe_lines + 12 > PE_MAX_LINES) {
        return;
    }

    for (i = 0; i < 8; i++) {
        const vec3_t local = {
            (i & 1) ? box->maxs[0] : box->mins[0], (i & 2) ? box->maxs[1] : box->mins[1], (i & 4) ? box->maxs[2] : box->mins[2]
        };

        VectorCopy(box->origin, corners[i]);
        for (k = 0; k < 3; k++) {
            VectorMA(corners[i], local[k], box->axis[k], corners[i]);
        }
    }

    for (i = 0; i < 8; i++) {
        for (k = 0; k < 3; k++) {
            const int j = i | (1 << k);

            if (j != i) {
                cgi.R_DebugLine(corners[i], corners[j], colour[0], colour[1], colour[2], 1.0f);
                pe_lines++;
            }
        }
    }
}

static const float pe_colours[4][3] = {
    {1.0f, 0.25f, 0.2f}, // fixed
    {0.2f, 1.0f, 0.2f},  // moves
    {0.5f, 0.5f, 0.5f},  // off
    {1.0f, 1.0f, 0.2f},  // under the crosshair
};

static qboolean CG_PhysicsEditNear(const peBox_t *box, float range)
{
    vec3_t mid, world;
    int    k;

    VectorAdd(box->mins, box->maxs, mid);
    VectorScale(mid, 0.5f, mid);
    VectorCopy(box->origin, world);
    for (k = 0; k < 3; k++) {
        VectorMA(world, mid[k], box->axis[k], world);
    }

    return Distance(world, cg.refdef.vieworg) <= range ? qtrue : qfalse;
}

void CG_PhysicsEditFrame(void)
{
    peBox_t box;

    if (!cg_physics_edit || !cg_physics_edit->integer || !cg.snap) {
        pe_target.kind = PE_NONE;
        return;
    }

    // The server's list, now and then: entities come and go.
    if (CG_PhysicsServerEdits() && (!pe_listAsked || cg.time - pe_listAsked > 3000 || cg.time < pe_listAsked)) {
        CG_PhysicsEditAskList();
    }

    CG_PhysicsEditPick();
    pe_lines = 0;

    // The target first, so it is never left out for the line budget.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < cg_numProps; i++) {
            const qboolean target = (pe_target.kind == PE_PROP && pe_target.index == i) ? qtrue : qfalse;
            const int      state  = CG_PhysicsEditPropState(&cg_props[i]);

            if (target != (pass == 0)) {
                continue;
            }

            CG_PhysicsEditPropBox(i, &box);
            if (!target && !CG_PhysicsEditNear(&box, state == PHYS_RULE_OFF ? PE_OFF_RANGE : PE_DRAW_RANGE)) {
                continue;
            }
            CG_PhysicsEditDrawBox(&box, pe_colours[target ? 3 : state]);
        }

        for (int i = 0; i < CG_PhysicsFurnitureCount(); i++) {
            const qboolean target = (pe_target.kind == PE_FURNITURE && pe_target.index == i) ? qtrue : qfalse;
            int            brush;
            qboolean       moves;
            const char    *why, *shader;

            if (target != (pass == 0) || !CG_PhysicsEditFurnitureBox(i, &box, &brush, &moves, &why, &shader)) {
                continue;
            }
            if (!target && !CG_PhysicsEditNear(&box, PE_DRAW_RANGE)) {
                continue;
            }
            CG_PhysicsEditDrawBox(&box, pe_colours[target ? 3 : moves ? PHYS_RULE_MOVES : PHYS_RULE_FIXED]);
        }

        for (std::map<int, int>::const_iterator it = pe_entities.begin(); it != pe_entities.end(); ++it) {
            const qboolean target = (pe_target.kind == PE_ENTITY && pe_target.index == it->first) ? qtrue : qfalse;

            if (target != (pass == 0) || !CG_PhysicsEditEntityBox(it->first, &box)) {
                continue;
            }
            if (!target && !CG_PhysicsEditNear(&box, PE_DRAW_RANGE)) {
                continue;
            }
            CG_PhysicsEditDrawBox(&box, pe_colours[target ? 3 : (it->second >= 0 && it->second <= 2) ? it->second : PHYS_RULE_FIXED]);
        }
    }
}

static const char *CG_PhysicsEditStateName(int state)
{
    switch (state) {
    case PHYS_RULE_MOVES:
        return "MOVES";
    case PHYS_RULE_FIXED:
        return "FIXED";
    case PHYS_RULE_OFF:
        return "LEFT OUT";
    default:
        return "?";
    }
}

// What the HUD says of the target: what it is, its state, and why.
static qboolean CG_PhysicsEditDescribe(char *what, size_t whatSize, int *state, char *detail, size_t detailSize)
{
    switch (pe_target.kind) {
    case PE_PROP:
        {
            const cgProp_t *p = &cg_props[pe_target.index];
            JPH::BodyID     id;
            const float     mass = CG_PhysicsPropBody(pe_target.index, &id) ? CG_PhysicsBodyMass(id) : 0.0f;

            Com_sprintf(what, whatSize, "static model %d: %s", p->staticIndex, p->name);
            *state = CG_PhysicsEditPropState(p);
            if (mass > 0.0f) {
                Com_sprintf(detail, detailSize, "%.1f kg, %s", mass, p->why ? p->why : "");
            } else {
                Com_sprintf(detail, detailSize, "%s", p->why ? p->why : "");
            }
            return qtrue;
        }

    case PE_FURNITURE:
        {
            peBox_t     box;
            int         brush;
            qboolean    moves;
            const char *why, *shader;
            JPH::BodyID id;
            vec3_t      middle;
            float       mass = 0.0f;

            if (!CG_PhysicsEditFurnitureBox(pe_target.index, &box, &brush, &moves, &why, &shader)) {
                return qfalse;
            }
            if (CG_PhysicsFurnitureBody(pe_target.index, &id, middle)) {
                mass = CG_PhysicsBodyMass(id);
            }

            Com_sprintf(what, whatSize, "furniture, brush %d: %s, %.0f x %.0f x %.0f", brush, shader,
                box.maxs[0] - box.mins[0], box.maxs[1] - box.mins[1], box.maxs[2] - box.mins[2]);
            *state = moves ? PHYS_RULE_MOVES : PHYS_RULE_FIXED;
            if (mass > 0.0f) {
                Com_sprintf(detail, detailSize, "%.1f kg, %s", mass, why ? why : "");
            } else {
                Com_sprintf(detail, detailSize, "%s", why ? why : "");
            }
            return qtrue;
        }

    case PE_ENTITY:
        {
            std::map<int, int>::const_iterator it = pe_entities.find(pe_target.index);

            *state = it != pe_entities.end() ? it->second : PHYS_RULE_UNSET;
            if (pe_info.valid && pe_info.entnum == pe_target.index) {
                Com_sprintf(what, whatSize, "entity %d: %s %s", pe_info.entnum, pe_info.classname, pe_info.model);
                *state = pe_info.state;
                if (pe_info.mass > 0.0f) {
                    Com_sprintf(detail, detailSize, "%.1f kg, %s", pe_info.mass, pe_info.why);
                } else {
                    Com_sprintf(detail, detailSize, "%s", pe_info.why);
                }
            } else {
                Com_sprintf(what, whatSize, "entity %d", pe_target.index);
                Com_sprintf(detail, detailSize, "asking the server...");
            }
            return qtrue;
        }
    }

    return qfalse;
}

void CG_PhysicsEditDraw2D(void)
{
    static const vec4_t white = {1.0f, 1.0f, 1.0f, 1.0f};
    static const vec4_t grey  = {0.75f, 0.75f, 0.75f, 1.0f};
    const float         line  = 15.0f * cgs.uiHiResScale[1];
    float               x, y;
    char                what[256], detail[256];
    int                 state;
    vec4_t              colour;

    if (!cg_physics_edit || !cg_physics_edit->integer || !cg.snap) {
        return;
    }

    x = 8.0f * cgs.uiHiResScale[0];
    y = cgs.glconfig.vidHeight * 0.3f;

#define PE_TEXT(text)                                                                                              \
    cgi.R_DrawString(cgs.media.attackerFont, (text), x / cgs.uiHiResScale[0], y / cgs.uiHiResScale[1], -1, cgs.uiHiResScale); \
    y += line

    cgi.R_SetColor(white);
    PE_TEXT(va("PHYSICS EDITOR (%s: %d rules)   green moves, red fixed, grey left out", PE_FILE, pe_rules.Count()));

    if (CG_PhysicsEditDescribe(what, sizeof(what), &state, detail, sizeof(detail))) {
        PE_TEXT(what);

        if (state >= 0 && state <= 2) {
            colour[0] = pe_colours[state][0];
            colour[1] = pe_colours[state][1];
            colour[2] = pe_colours[state][2];
            colour[3] = 1.0f;
            cgi.R_SetColor(colour);
        }
        PE_TEXT(va("%s   %s", CG_PhysicsEditStateName(state), detail));

        cgi.R_SetColor(grey);
        if (pe_target.kind == PE_FURNITURE) {
            PE_TEXT("phys_toggle   phys_mass <kg>   phys_forget");
        } else {
            PE_TEXT("phys_toggle [model|mapmodel]   phys_off [model|mapmodel]   phys_mass <kg> [model|mapmodel]   phys_forget [model|mapmodel]");
        }
    } else {
        cgi.R_SetColor(grey);
        PE_TEXT(CG_PhysicsServerEdits() ? "aim at an object" : "aim at an object (the server's crates and barrels only in single player)");
    }

    if (pe_message[0] && cg.time - pe_messageTime < 5000 && cg.time >= pe_messageTime) {
        cgi.R_SetColor(white);
        PE_TEXT(pe_message);
    }

#undef PE_TEXT

    cgi.R_SetColor(NULL);
}

//=============================================================
// Changing the rules
//=============================================================

typedef struct {
    std::string map, kind, key;
    std::string label; // what it covers, for the message
    qboolean    ownObject; // about the target alone (not its model or class)
} peScope_t;

static std::string CG_PhysicsEditBareMap(void)
{
    return Phys_RuleMap(cgs.mapname);
}

// Which rule a command with this argument means: the target's own, or its
// model's everywhere ("model") or on this map ("mapmodel").
static qboolean CG_PhysicsEditScope(const char *arg, peScope_t *out)
{
    const qboolean model    = !Q_stricmp(arg, "model") ? qtrue : qfalse;
    const qboolean mapModel = !Q_stricmp(arg, "mapmodel") ? qtrue : qfalse;
    char           key[64];

    if (arg[0] && !model && !mapModel) {
        CG_PhysicsEditSay("phys_edit: '%s'? Nothing, 'model' (on every map) or 'mapmodel' (on this one)", arg);
        return qfalse;
    }

    out->map       = CG_PhysicsEditBareMap();
    out->ownObject = (model || mapModel) ? qfalse : qtrue;
    if (model) {
        out->map = "";
    }

    switch (pe_target.kind) {
    case PE_PROP:
        {
            const cgProp_t *p = &cg_props[pe_target.index];

            if (out->ownObject) {
                Com_sprintf(key, sizeof(key), "%d", p->staticIndex);
                out->kind  = "static";
                out->key   = key;
                out->label = va("static model %d", p->staticIndex);
            } else {
                out->kind  = "model";
                out->key   = Phys_RuleModel(p->name);
                out->label = va("every %s%s", out->key.c_str(), mapModel ? " on this map" : "");
            }
            return qtrue;
        }

    case PE_FURNITURE:
        {
            peBox_t     box;
            int         brush;
            qboolean    moves;
            const char *why, *shader;

            if (!out->ownObject) {
                CG_PhysicsEditSay("phys_edit: furniture in the brushwork has no model; only this piece");
                return qfalse;
            }
            if (!CG_PhysicsEditFurnitureBox(pe_target.index, &box, &brush, &moves, &why, &shader)) {
                return qfalse;
            }

            Com_sprintf(key, sizeof(key), "%d", brush);
            out->kind  = "furniture";
            out->key   = key;
            out->label = va("the furniture with brush %d", brush);
            return qtrue;
        }

    case PE_ENTITY:
        if (!pe_info.valid || pe_info.entnum != pe_target.index) {
            CG_PhysicsEditSay("phys_edit: the server has not said what entity %d is yet", pe_target.index);
            return qfalse;
        }

        if (out->ownObject) {
            out->kind  = "entity";
            out->key   = pe_info.key;
            out->label = va("entity %s", pe_info.key);
        } else if (pe_info.model[0] && pe_info.model[0] != '*') {
            out->kind  = "model";
            out->key   = Phys_RuleModel(pe_info.model);
            out->label = va("every %s%s", out->key.c_str(), mapModel ? " on this map" : "");
        } else {
            // A brush model's name is only a number on its map: its class.
            out->kind  = "class";
            out->key   = pe_info.classname;
            out->label = va("every %s%s", pe_info.classname, mapModel ? " on this map" : "");
        }
        return qtrue;
    }

    CG_PhysicsEditSay("phys_edit: aim at an object first");
    return qfalse;
}

static int CG_PhysicsEditTargetState(void)
{
    char what[256], detail[256];
    int  state = PHYS_RULE_UNSET;

    CG_PhysicsEditDescribe(what, sizeof(what), &state, detail, sizeof(detail));
    return state;
}

// Saves the file and has the physics built again by it.
static void CG_PhysicsEditApply(const peScope_t *scope)
{
    const std::string text = pe_rules.Write();

    if (cgi.FS_WriteFile(PE_FILE, text.data(), (int)text.size()) < 0) {
        CG_PhysicsEditSay("phys_edit: could not write %s", PE_FILE);
        return;
    }

    // The client's props and furniture; a model or a class can be on both.
    if (pe_target.kind != PE_ENTITY || !scope->ownObject) {
        CG_PhysicsReloadWorld();
    }

    // The server's crates, barrels and small objects.
    if (CG_PhysicsServerEdits()) {
        cgi.SendClientCommand("physrules");
        pe_info.valid = qfalse;
        pe_infoAsked  = -1;
    }
}

// Before a change: the file as it is now, in case it was edited by hand.
static qboolean CG_PhysicsEditBegin(void)
{
    if (!cg_physics_edit->integer) {
        CG_PhysicsEditSay("phys_edit: the editor is off (phys_edit turns it on)");
        return qfalse;
    }

    CG_PhysicsRulesLoad();
    if (pe_rules.Empty()) {
        pe_rules.Parse(PhysRules::Header());
    }
    return qtrue;
}

static void CG_PhysicsEditSetState(int state, const char *arg)
{
    peScope_t scope;

    if (!CG_PhysicsEditBegin() || !CG_PhysicsEditScope(arg, &scope)) {
        return;
    }

    pe_rules.SetState(scope.map.c_str(), scope.kind.c_str(), scope.key.c_str(), state);
    CG_PhysicsEditSay("%s: %s", scope.label.c_str(), state == PHYS_RULE_MOVES ? "moves" : state == PHYS_RULE_FIXED ? "fixed" : "left out");

    // A rule for this one on its own still wins over its model's.
    if (!scope.ownObject) {
        peScope_t own;

        if (CG_PhysicsEditScope("", &own)
            && pe_rules.Get(own.map.c_str(), own.kind.c_str(), own.key.c_str()).state != PHYS_RULE_UNSET) {
            const std::string said = pe_message;

            CG_PhysicsEditSay("%s (this one has a rule of its own, which still decides it: phys_forget)", said.c_str());
        }
    }

    CG_PhysicsEditApply(&scope);
}

// phys_edit: the editor on or off.
void CG_PhysicsEdit_f(void)
{
    cgi.Cvar_Set("cg_physics_edit", cg_physics_edit->integer ? "0" : "1");
    pe_message[0] = 0;
    pe_entities.clear();
    pe_listAsked = 0;
    pe_info.valid = qfalse;
    pe_infoAsked  = -1;

    if (cg_physics_edit->integer) {
        CG_PhysicsRulesLoad();
        cgi.Printf(
            "physics editor: phys_toggle [model|mapmodel] flips what is under the crosshair between moving and fixed; "
            "phys_off leaves it out; phys_mass <kg> weighs it; phys_forget takes its rule out. Changes go to %s.\n",
            PE_FILE
        );
    }
}

// phys_toggle [model|mapmodel]: moves if it did not, fixed if it did.
void CG_PhysicsToggle_f(void)
{
    const int state = CG_PhysicsEditTargetState();

    CG_PhysicsEditSetState(state == PHYS_RULE_MOVES ? PHYS_RULE_FIXED : PHYS_RULE_MOVES, cgi.Argc() > 1 ? cgi.Argv(1) : "");
}

// phys_off [model|mapmodel]: left out of the physics altogether.
void CG_PhysicsOff_f(void)
{
    CG_PhysicsEditSetState(PHYS_RULE_OFF, cgi.Argc() > 1 ? cgi.Argv(1) : "");
}

// phys_mass <kg> [model|mapmodel]: how heavy it is when it moves; 0 for the
// physics' own guess.
void CG_PhysicsMass_f(void)
{
    peScope_t scope;
    float     mass;

    if (cgi.Argc() < 2) {
        cgi.Printf("phys_mass <kilograms> [model|mapmodel]: 0 goes back to the physics' own guess\n");
        return;
    }

    mass = (float)atof(cgi.Argv(1));
    if (!CG_PhysicsEditBegin() || !CG_PhysicsEditScope(cgi.Argc() > 2 ? cgi.Argv(2) : "", &scope)) {
        return;
    }

    pe_rules.SetMass(scope.map.c_str(), scope.kind.c_str(), scope.key.c_str(), mass);
    if (mass > 0.0f) {
        CG_PhysicsEditSay("%s: %g kg", scope.label.c_str(), mass);
    } else {
        CG_PhysicsEditSay("%s: the physics' own weight", scope.label.c_str());
    }
    CG_PhysicsEditApply(&scope);
}

// phys_forget [model|mapmodel]: the rule goes, and with it what it decided.
void CG_PhysicsForget_f(void)
{
    peScope_t scope;

    if (!CG_PhysicsEditBegin() || !CG_PhysicsEditScope(cgi.Argc() > 1 ? cgi.Argv(1) : "", &scope)) {
        return;
    }

    if (!pe_rules.Forget(scope.map.c_str(), scope.kind.c_str(), scope.key.c_str())) {
        CG_PhysicsEditSay("%s has no rule of its own to forget", scope.label.c_str());
        return;
    }

    CG_PhysicsEditSay("%s: its rule is gone", scope.label.c_str());
    CG_PhysicsEditApply(&scope);
}

// phys_reload: physics.txt read again, after editing it by hand.
void CG_PhysicsReload_f(void)
{
    CG_PhysicsRulesLoad();
    CG_PhysicsReloadWorld();
    if (CG_PhysicsServerEdits()) {
        cgi.SendClientCommand("physrules");
    }
    cgi.Printf("phys_reload: %d rules in %s\n", pe_rules.Count(), PE_FILE);
}

//=============================================================
// What the server says
//=============================================================

// physlist <first> <entity>:<state> ...: its candidates, in parts. The first
// part starts the list again.
// physinfo <entity> <state> <mass> <class> <model> <key> <why>
qboolean CG_PhysicsEditServerCommand(const char *cmd)
{
    if (!strcmp(cmd, "physlist")) {
        if (atoi(cgi.Argv(1))) {
            pe_entities.clear();
        }

        for (int i = 2; i < cgi.Argc(); i++) {
            const char *arg   = cgi.Argv(i);
            const char *colon = strchr(arg, ':');

            if (colon) {
                pe_entities[atoi(arg)] = atoi(colon + 1);
            }
        }
        return qtrue;
    }

    if (!strcmp(cmd, "physinfo")) {
        if (cg_physics_log->integer > 1) {
            cgi.Printf("phys_edit: the server says of entity %s: %d args, state %s, %s\n", cgi.Argv(1), cgi.Argc(), cgi.Argv(2), cgi.Argv(7));
        }
        if (cgi.Argc() < 8) {
            return qtrue;
        }

        pe_info.entnum = atoi(cgi.Argv(1));
        pe_info.state  = atoi(cgi.Argv(2));
        pe_info.mass   = (float)atof(cgi.Argv(3));
        Q_strncpyz(pe_info.classname, cgi.Argv(4), sizeof(pe_info.classname));
        Q_strncpyz(pe_info.model, cgi.Argv(5), sizeof(pe_info.model));
        Q_strncpyz(pe_info.key, cgi.Argv(6), sizeof(pe_info.key));
        Q_strncpyz(pe_info.why, cgi.Argv(7), sizeof(pe_info.why));
        pe_info.valid = qtrue;

        if (pe_info.state >= 0) {
            pe_entities[pe_info.entnum] = pe_info.state;
        }
        return qtrue;
    }

    return qfalse;
}

void CG_PhysicsEditInit(void)
{
    cg_physics_edit = cgi.Cvar_Get("cg_physics_edit", "0", 0);
    pe_entities.clear();
    pe_listAsked  = 0;
    pe_info.valid = qfalse;
    pe_infoAsked  = -1;
    pe_target.kind = PE_NONE;
}
