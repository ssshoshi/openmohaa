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
// What is under the crosshair (cg_pick.h).

// Jolt first: cg_local.h defines a LERP macro its headers collide with.
#include "cg_physics_local.h"
#include "cg_pick.h"
#include "cg_props.h"

#define PICK_RANGE    4096.0f
#define PICK_MAX_SIZE 2048.0f // bigger is an animation carrier or a volume, not a thing

//=============================================================
// JSON
//=============================================================

std::string CG_JsonString(const char *s)
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

std::string CG_JsonVec(const vec3_t v)
{
    return va("[%.2f, %.2f, %.2f]", v[0], v[1], v[2]);
}

const char *CG_PickETypeName(int eType)
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

std::string CG_PickModelShaders(const char *model, qboolean json)
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

const char *CG_PickMapName(void)
{
    static char map[64];

    Q_strncpyz(map, cgs.mapname, sizeof(map));
    if (!Q_stricmpn(map, "maps/", 5)) {
        memmove(map, map + 5, strlen(map + 5) + 1);
    }
    COM_StripExtension(map, map, sizeof(map));
    return map;
}

//=============================================================
// What is under the crosshair
//=============================================================

// Whether a point is inside a box turned by axis about origin.
static qboolean CG_PickInBox(const vec3_t p, const vec3_t origin, const vec3_t axis[3], const vec3_t mins, const vec3_t maxs)
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

// Anything whose box the eye is in, or that is bigger than any one object, is
// left out: nothing can be aimed at from inside it, and the invisible models
// that carry a scripted animation across the map (m1l1's truck paths, five
// thousand units wide) would otherwise be picked wherever one looked.
static qboolean CG_PickTooBig(const vec3_t mins, const vec3_t maxs)
{
    return maxs[0] - mins[0] > PICK_MAX_SIZE || maxs[1] - mins[1] > PICK_MAX_SIZE || maxs[2] - mins[2] > PICK_MAX_SIZE
             ? qtrue
             : qfalse;
}

// Everything the crosshair's line passes through, nearest first: each prop and
// entity it meets before the world, then the world surface it ends on. Out
// holds max; returns how many. Added in OPM, for picking what to mark from
// what is in front of it (cg_orch.cpp).
int CG_PickAll(pick_t *out, int max)
{
    pick_t        base;
    pick_t       *pick = &base;
    float         dist[PICK_MAX_ALL];
    int           n = 0;
    const float  *start = cg.refdef.vieworg;
    const float  *dir   = cg.refdef.viewaxis[0];
    pickTarget_t  best;
    float         bestAt, at;
    pickTarget_t  found[PICK_MAX_ALL];
    trace_t       tr, first;
    vec3_t        end;
    baseshader_t *shader;

    memset(&base, 0, sizeof(base));
    memset(&best, 0, sizeof(best));
    best.index = -1;

    VectorMA(start, PICK_RANGE, dir, end);
    CG_Trace(&tr, start, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, MASK_SHOT, qfalse, qfalse, "pick");

    // Anything in front of the world: props sit inside the clip brushes that
    // stand in for them, so a little past it too.
    bestAt = tr.fraction * PICK_RANGE + 24.0f;

    // The surface seen, not the clip brush in front of it: on through the
    // invisible ones, remembering the first. When nothing visible is found
    // behind it (a clip volume the terrain lies inside), the invisible one is
    // what is reported.
    pick->hitThrough[0] = 0;
    first               = tr;
    for (int i = 0; i < 8 && tr.fraction < 1.0f && !tr.startsolid; i++) {
        vec3_t from;

        shader = cgi.GetShader(tr.shaderNum);
        if (!shader || !(shader->surfaceFlags & SURF_NODRAW)) {
            break;
        }
        if (!pick->hitThrough[0]) {
            Q_strncpyz(pick->hitThrough, shader->shader, sizeof(pick->hitThrough));
        }
        // Out the far side of the brush: a trace from inside it starts solid.
        VectorCopy(tr.endpos, from);
        for (int step = 0; step < 64; step++) {
            VectorMA(from, 4.0f, dir, from);
            CG_Trace(&tr, from, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, MASK_SHOT, qfalse, qfalse, "pick");
            if (!tr.startsolid) {
                break;
            }
        }
    }

    if (tr.fraction >= 1.0f || tr.startsolid) {
        tr                  = first;
        pick->hitThrough[0] = 0;
    }

    pick->hit          = (tr.fraction < 1.0f && !tr.startsolid) ? qtrue : qfalse;
    pick->hitShader[0] = 0;
    pick->hitSurfaceFlags = pick->hitContents = 0;
    if (pick->hit) {
        VectorCopy(tr.endpos, pick->hitPos);
        VectorCopy(tr.plane.normal, pick->hitNormal);
        shader = cgi.GetShader(tr.shaderNum);
        if (shader) {
            Q_strncpyz(pick->hitShader, shader->shader, sizeof(pick->hitShader));
            pick->hitSurfaceFlags = shader->surfaceFlags;
            pick->hitContents     = shader->contentFlags;
        }
        best.kind = PICK_TARGET_WORLD;
    }

    for (int i = 0; i < cg_numProps; i++) {
        const cgProp_t *p = &cg_props[i];

        if (CG_PickTooBig(p->mins, p->maxs) || CG_PickInBox(start, p->origin, p->axis, p->mins, p->maxs)) {
            continue;
        }
        if (CG_PhysicsRayHitsBox(start, dir, PICK_RANGE, p->origin, p->axis, p->mins, p->maxs, &at) && at < bestAt
            && n < PICK_MAX_ALL) {
            memset(&best, 0, sizeof(best));
            best.kind  = PICK_TARGET_PROP;
            best.index = i;
            VectorCopy(p->origin, best.origin);
            AxisCopy(p->axis, best.axis);
            VectorCopy(p->mins, best.mins);
            VectorCopy(p->maxs, best.maxs);
            Q_strncpyz(best.model, p->name, sizeof(best.model));
            found[n] = best;
            dist[n++] = at;
        }
    }

    for (int i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *es = &cg.snap->entities[i];
        vec3_t               origin, axis[3], mins, maxs;

        if (es->number == cg.snap->ps.clientNum) {
            continue;
        }
        if (!CG_PhysicsEntityBox(es->number, origin, axis, mins, maxs) || CG_PickTooBig(mins, maxs)
            || CG_PickInBox(start, origin, axis, mins, maxs)) {
            continue;
        }
        if (CG_PhysicsRayHitsBox(start, dir, PICK_RANGE, origin, axis, mins, maxs, &at) && at < bestAt
            && n < PICK_MAX_ALL) {
            memset(&best, 0, sizeof(best));
            best.kind  = PICK_TARGET_ENTITY;
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
            found[n] = best;
            dist[n++] = at;
        }
    }

    // Nearest first, then the world surface the line ends on.
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && dist[j] < dist[j - 1]; j--) {
            pickTarget_t t = found[j];
            float        d = dist[j];
            found[j]       = found[j - 1];
            dist[j]        = dist[j - 1];
            found[j - 1]   = t;
            dist[j - 1]    = d;
        }
    }

    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        out[count]        = base;
        out[count].target = found[i];
        count++;
    }
    if (base.hit && count < max) {
        out[count]              = base;
        out[count].target.kind  = PICK_TARGET_WORLD;
        out[count].target.index = -1;
        count++;
    }
    return count;
}

void CG_Pick(pick_t *pick)
{
    pick_t all[PICK_MAX_ALL + 1];
    int    n = CG_PickAll(all, PICK_MAX_ALL + 1);

    if (n) {
        *pick = all[0];
        return;
    }
    memset(pick, 0, sizeof(*pick));
    pick->target.kind  = PICK_TARGET_NONE;
    pick->target.index = -1;
}

void CG_PickDrawBox(const pickTarget_t *t, float r, float g, float b)
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

void CG_PickDrawHit(const pick_t *pick, float r, float g, float b)
{
    for (int k = 0; k < 3; k++) {
        vec3_t a, c;

        VectorCopy(pick->hitPos, a);
        VectorCopy(pick->hitPos, c);
        a[k] -= 8.0f;
        c[k] += 8.0f;
        cgi.R_DebugLine(a, c, r, g, b, 1.0f);
    }
}

std::string CG_PickSummary(const pick_t *pick)
{
    const pickTarget_t *t = &pick->target;

    switch (t->kind) {
    case PICK_TARGET_PROP:
        return va("static model #%d: %s", t->index, t->model);
    case PICK_TARGET_ENTITY:
        return va(
            "entity #%d (%s): %s",
            t->index,
            CG_PickETypeName(cg_entities[t->index].currentState.eType),
            t->model[0] ? t->model : "no model"
        );
    case PICK_TARGET_WORLD:
        return std::string("world: ") + pick->hitShader;
    default:
        return "nothing";
    }
}

std::string CG_PickAimJson(const pick_t *pick, const vec3_t viewOrg, const char *indent)
{
    std::string j;
    std::string in = indent;

    j += in + va("\"hit\": %s", pick->hit ? "true" : "false");
    if (pick->hit) {
        vec3_t light;

        cgi.R_GetLightingForDecal(light, pick->hitNormal, pick->hitPos);
        j += ",\n" + in + "\"position\": " + CG_JsonVec(pick->hitPos) + ",\n";
        j += in + "\"normal\": " + CG_JsonVec(pick->hitNormal) + ",\n";
        j += in + "\"shader\": " + CG_JsonString(pick->hitShader) + ",\n";
        j += in + "\"through_invisible\": " + CG_JsonString(pick->hitThrough) + ",\n";
        j += in + va("\"surface_flags\": %d,\n", pick->hitSurfaceFlags);
        j += in + va("\"contents\": %d,\n", pick->hitContents);
        j += in + "\"light\": " + CG_JsonVec(light) + ",\n";
        j += in + va("\"distance\": %.0f", Distance(viewOrg, pick->hitPos));
    }
    return j;
}

std::string CG_PickTargetJson(const pick_t *pick, const char *indent)
{
    const pickTarget_t *t = &pick->target;
    std::string         j;
    std::string         in = indent;

    switch (t->kind) {
    case PICK_TARGET_PROP:
        {
            const cgProp_t *p = &cg_props[t->index];

            j += in + "\"kind\": \"static_model\",\n";
            j += in + va("\"index\": %d,\n", t->index);
            j += in + va("\"static_index\": %d,\n", p->staticIndex);
            j += in + va("\"solid\": %s,\n", p->solid ? "true" : "false");
            j += in + va("\"dynamic\": %s,\n", p->dynamic ? "true" : "false");
            j += in + "\"physics_why\": " + CG_JsonString(p->why ? p->why : "") + ",\n";
            j += in + "\"angles\": " + CG_JsonVec(p->angles);
        }
        break;
    case PICK_TARGET_ENTITY:
        {
            const centity_t *cent = &cg_entities[t->index];

            j += in + "\"kind\": \"entity\",\n";
            j += in + va("\"entnum\": %d,\n", t->index);
            j += in + "\"etype\": " + CG_JsonString(CG_PickETypeName(cent->currentState.eType)) + ",\n";
            j += in + "\"angles\": " + CG_JsonVec(cent->lerpAngles);
        }
        break;
    case PICK_TARGET_WORLD:
        j += in + "\"kind\": \"world\"";
        break;
    default:
        j += in + "\"kind\": \"none\"";
        break;
    }
    if (t->kind == PICK_TARGET_PROP || t->kind == PICK_TARGET_ENTITY) {
        j += ",\n" + in + "\"model\": " + CG_JsonString(t->model) + ",\n";
        j += in + "\"origin\": " + CG_JsonVec(t->origin) + ",\n";
        j += in + "\"mins\": " + CG_JsonVec(t->mins) + ",\n";
        j += in + "\"maxs\": " + CG_JsonVec(t->maxs) + ",\n";
        j += in + "\"model_shaders\": " + CG_PickModelShaders(t->model, qtrue);
    }
    return j;
}
