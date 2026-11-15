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
// What the cold does on a map where snow falls. A map is a snow map from the
// moment snow falls on it (CG_SnowFalls, from the lens's look at the rain,
// cg_lens.cpp); the renderer has its own part (r_snow: snow lying and
// glittering, tr_water.c).
//
// Breath: every living man breathes out a puff that shows in the cold
// (models/fx/opm_breath.tik, opm-snow data), each in his own time, slowly
// at rest and quicker when he has been running; the player too, in front
// of the view.
//
// Footprints: a step on snow (the ground's own surface flag, so on any map
// with snow on the ground, falling or not) leaves the boot's print
// (opm/snow_footprint, opm-snow data), a mark like a bullet's: the player's
// stay until the marks are needed for something else, other men's fade.

#include "cg_snow.h"
#include "cg_specialfx.h"
#include "surfaceflags.h"

static cvar_t *cg_breath;
static cvar_t *cg_footprints;

static qhandle_t footprintShader;
static qboolean  footprintTried;

#define FOOTPRINT_RANGE 1200.0f

static qboolean snowMap;

typedef struct {
    int    next;  // when he breathes out next
    int    last;  // when he last did
    vec3_t where; // and where he was then
} breath_t;

static breath_t breaths[MAX_GENTITIES];
static breath_t ownBreath;

#define BREATH_RANGE 1400.0f // no further than this: a puff is a few pixels by then

void CG_InitSnow(void)
{
    // On a map where snow falls, men's breath shows in the cold: 0 off, 1
    // everyone's but the player's own, 2 the player's too
    cg_breath = cgi.Cvar_Get("cg_breath", "1", CVAR_ARCHIVE);
    // Steps on snow leave footprints: 0 none, 1 everyone's
    cg_footprints = cgi.Cvar_Get("cg_footprints", "1", CVAR_ARCHIVE);
    footprintShader = 0;
    footprintTried  = qfalse;
    snowMap = qfalse;
    memset(breaths, 0, sizeof(breaths));
    memset(&ownBreath, 0, sizeof(ownBreath));
}

void CG_SnowFalls(void)
{
    snowMap = qtrue;
}

qboolean CG_SnowMap(void)
{
    return snowMap;
}

// Whether it is time for a breath; if so the next is set: sooner the faster
// he has moved since the last
static qboolean CG_BreathDue(breath_t *b, const vec3_t at)
{
    float speed = 0.0f;
    int   rest;

    if (b->next > cg.time && b->next - cg.time < 10000) {
        return qfalse;
    }
    if (b->last && cg.time > b->last && cg.time - b->last < 6000) {
        speed = Distance(at, b->where) * 1000.0f / (cg.time - b->last);
    }
    // at rest a breath every three or four seconds; out of breath one in a
    // little over a second
    rest    = speed > 140.0f ? 1200 : (speed > 60.0f ? 2200 : 3200);
    b->next = cg.time + rest + (rand() % (rest / 3));
    // (the first one: at any point of his breathing, not all men at once)
    if (!b->last) {
        b->last = cg.time;
        b->next = cg.time + (rand() % (rest + rest / 3));
        VectorCopy(at, b->where);
        return qfalse;
    }
    b->last = cg.time;
    VectorCopy(at, b->where);
    return qtrue;
}

void CG_BreathEntity(centity_t *cent, const refEntity_t *model)
{
    const entityState_t *s = &cent->currentState;
    breath_t            *b;
    orientation_t        head;
    vec3_t               at, dir;
    int                  tag, i;

    if (!snowMap || !cg_breath->integer || !model->tiki || s->number < 0 || s->number >= MAX_GENTITIES) {
        return;
    }
    if ((s->eFlags & EF_DEAD) || (s->eType != ET_MODELANIM && s->eType != ET_PLAYER)) {
        return;
    }
    // the player's own is the frame's (CG_SnowFrame)
    if (cg.snap && s->number == cg.snap->ps.clientNum) {
        return;
    }
    b = &breaths[s->number];
    // (not yet, or not a man, or too far to see: looked at again later)
    if (b->next > cg.time && b->next - cg.time < 10000) {
        return;
    }
    if (DistanceSquared(model->origin, cg.refdef.vieworg) > BREATH_RANGE * BREATH_RANGE) {
        b->next = cg.time + 700;
        return;
    }
    tag = cgi.Tag_NumForName(model->tiki, "Bip01 Head");
    if (tag < 0) {
        b->next = cg.time + 5000;
        return;
    }
    if (!CG_BreathDue(b, model->origin)) {
        return;
    }

    // out of his mouth: in front of his head and a little under its middle,
    // the way he faces and a little down
    head = cgi.TIKI_Orientation((refEntity_t *)model, tag & TAG_MASK);
    VectorCopy(model->origin, at);
    for (i = 0; i < 3; i++) {
        VectorMA(at, head.origin[i] * model->scale, model->axis[i], at);
    }
    VectorMA(at, 6.0f, model->axis[0], at);
    VectorMA(at, 1.0f, model->axis[2], at);
    VectorCopy(model->axis[0], dir);
    dir[2] -= 0.15f;
    VectorNormalize(dir);
    sfxManager.MakeEffect_Normal(SFX_OPM_BREATH, Vector(at), Vector(dir));
}

void CG_SnowFrame(void)
{
    vec3_t at, dir;

    if (!snowMap || cg_breath->integer < 2 || !cg.snap || cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        return;
    }
    if (cg.snap->ps.pm_flags & PMF_INTERMISSION) {
        return;
    }
    if (!CG_BreathDue(&ownBreath, cg.refdef.vieworg)) {
        return;
    }
    // in front of the view and under it, going away from it
    VectorMA(cg.refdef.vieworg, 14.0f, cg.refdef.viewaxis[0], at);
    VectorMA(at, -5.0f, cg.refdef.viewaxis[2], at);
    VectorCopy(cg.refdef.viewaxis[0], dir);
    dir[2] -= 0.1f;
    VectorNormalize(dir);
    sfxManager.MakeEffect_Normal(SFX_OPM_BREATH, Vector(at), Vector(dir));
}

// The way a mark lying on normal has to be turned for its length to run along
// (CG_ImpactMark: its T axis, from a square of the normal turned so far).
static float CG_SnowMarkAngle(const vec3_t normal, const vec3_t along)
{
    vec3_t n, p, a, c;

    VectorNormalize2(normal, n);
    PerpendicularVector(p, n);
    VectorMA(along, -DotProduct(along, n), n, a);
    if (VectorNormalize(a) < 0.001f) {
        return 0.001f;
    }
    CrossProduct(p, a, c);
    return atan2(DotProduct(c, n), DotProduct(p, a)) * (180.0f / M_PI) + 0.001f;
}

void CG_SnowFootprint(const centity_t *ent, const trace_t *trace)
{
    vec3_t   angles, forward;
    qboolean own;

    if (!cg_footprints->integer || (trace->surfaceFlags & MASK_SURF_TYPE) != SURF_SNOW || trace->plane.normal[2] < 0.6f) {
        return;
    }
    if (!footprintTried) {
        footprintTried = qtrue;
        // (without the pak there is no print, not the default shader's squares)
        if (cgi.FS_ReadFile("textures/opm_snow/footprint.tga", NULL, qtrue) > 0) {
            footprintShader = cgi.R_RegisterShader("opm/snow_footprint");
        }
    }
    if (!footprintShader || DistanceSquared(trace->endpos, cg.refdef.vieworg) > FOOTPRINT_RANGE * FOOTPRINT_RANGE) {
        return;
    }
    VectorSet(angles, 0.0f, ent->lerpAngles[YAW], 0.0f);
    AngleVectors(angles, forward, NULL, NULL);
    own = (cg.snap && ent->currentState.number == cg.snap->ps.clientNum) ? qtrue : qfalse;
    CG_ImpactMark(
        footprintShader, trace->endpos, trace->plane.normal, CG_SnowMarkAngle(trace->plane.normal, forward), 2.7f, 5.8f,
        1.0f, 1.0f, 1.0f, 1.0f, own ? qfalse : qtrue, qfalse, qtrue, qfalse, 0.5f, 0.5f
    );
}
