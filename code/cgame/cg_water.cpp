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
// cg_water.cpp: the water corpses float in.
//
// The map's water is its water brushes; their top is found by tracing down
// onto them, and the bottom by tracing on down to something solid. The waves
// are the renderer's (tr_water.c, glsl/water_fp.glsl): the same file,
// water/<map>.txt, gives both of them their direction, length, period and
// height, and the first wave train here is the one the renderer draws, so a
// body is lifted and carried as the crests it is seen in go by. This file adds
// how the surf moves what is in it:
//
//     drift 14    // how fast the surf carries things towards the shore, units a second
//     surge 70    // how hard a breaking wave shoves them on, units a second
//
// The surf is where the water is shallower than `surf` (the renderer's foam
// starts there too) and runs out to three times as deep. Out past it the water
// only goes to and fro under the swell.
//
// And a body in the water bleeds into it. Each one afloat has a cloud of blood
// round it, which spreads from when it went in, trails after it as the waves
// move it, and fades once it is out of the water or gone. The renderer draws
// them (RE_SetWaterBlood): the WATER_BLOOD_SENT nearest the view, of up to
// WATER_BLOOD_MAX, the newest kept.

#include "cg_local.h"
#include "cg_water.h"

cvar_t *cg_water_float;
cvar_t *cg_water_blood;

#define WATER_REACH    256.0f // how far above a point its water's top is looked for
#define WATER_DEEP     1024.0f // how far below it the bottom is

#define WATER_BLOOD_MAX     64      // bodies whose blood is kept
#define WATER_BLOOD_SENT    32      // as many as the renderer takes (WATER_MAX_SPOTS)
#define WATER_BLOOD_START   28.0f   // how far round a body its blood is at first, units
#define WATER_BLOOD_SPREAD  170.0f  // and how much further it goes in all
#define WATER_BLOOD_SLOW    18.0f   // seconds for it to go most of the way out
#define WATER_BLOOD_THICKEN 1.5f    // seconds for it to come up thick round a body just in
#define WATER_BLOOD_LINGER  1500    // ms a cloud stays as it was once its body is out
#define WATER_BLOOD_FADE    6000    // and then ms to fade
#define WATER_BLOOD_TRAIL   0.35f   // how fast the cloud follows its body, a share a second

typedef struct {
    char   map[MAX_QPATH];
    vec2_t dir;
    float  length, period, height;
    float  surf;
    float  drift, surge;
} cgWater_t;

static cgWater_t cg_water;

typedef struct {
    qboolean used;
    int      body, born;
    int      wentIn;   // when the body went into the water
    int      lastSeen; // when it was last in it
    vec2_t   at;       // the cloud's middle, trailing the body
    vec2_t   bodyAt;   // where the body was last in the water
} cgWaterBlood_t;

static cgWaterBlood_t cg_waterBlood[WATER_BLOOD_MAX];
static int            cg_waterBloodTime;
static int            cg_waterBloodSent;

void CG_InitWater(void)
{
    cg_water_float = cgi.Cvar_Get("cg_water_float", "1", CVAR_ARCHIVE);
    cg_water_blood = cgi.Cvar_Get("cg_water_blood", "1", CVAR_ARCHIVE);
    cg_water.map[0] = 0;
    memset(cg_waterBlood, 0, sizeof(cg_waterBlood));
    cg_waterBloodTime = 0;
    cg_waterBloodSent = 0;
}

// As the renderer has them when the map has no water file (tr_water.c)
static void CG_WaterDefaults(void)
{
    cg_water.dir[0] = 1.0f;
    cg_water.dir[1] = 0.3f;
    cg_water.length = 420.0f;
    cg_water.period = 5.5f;
    cg_water.height = 4.0f;
    cg_water.surf   = 18.0f;
    cg_water.drift  = 0.0f;
    cg_water.surge  = 0.0f;
}

static int CG_WaterNumbers(char **text, float *out, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        const char *token = COM_ParseExt(text, qfalse);

        if (!token[0]) {
            break;
        }
        out[i] = atof(token);
    }
    return i;
}

static void CG_WaterLoad(void)
{
    char  base[MAX_QPATH], name[MAX_QPATH];
    void *buf  = NULL;
    char *text;
    float v[5];

    Q_strncpyz(cg_water.map, cgs.mapname, sizeof(cg_water.map));
    CG_WaterDefaults();

    COM_StripExtension(COM_SkipPath(cgs.mapname), base, sizeof(base));
    Com_sprintf(name, sizeof(name), "water/%s.txt", base);
    if (cgi.FS_ReadFile(name, &buf, qtrue) <= 0 || !buf) {
        return;
    }

    text = (char *)buf;
    for (;;) {
        const char *token = COM_ParseExt(&text, qtrue);
        char        key[64];

        if (!token[0]) {
            break;
        }
        Q_strncpyz(key, token, sizeof(key));

        if (!Q_stricmp(key, "waves")) {
            if (CG_WaterNumbers(&text, v, 5) == 5) {
                cg_water.dir[0] = v[0];
                cg_water.dir[1] = v[1];
                cg_water.length = Q_max(v[2], 16.0f);
                cg_water.period = Q_max(v[3], 0.5f);
                cg_water.height = v[4];
            }
        } else if (!Q_stricmp(key, "surf")) {
            CG_WaterNumbers(&text, &cg_water.surf, 1);
        } else if (!Q_stricmp(key, "drift")) {
            CG_WaterNumbers(&text, &cg_water.drift, 1);
        } else if (!Q_stricmp(key, "surge")) {
            CG_WaterNumbers(&text, &cg_water.surge, 1);
        }
        // the rest is the renderer's
        SkipRestOfLine(&text);
    }

    cgi.FS_FreeFile(buf);

    {
        const float l = sqrt(cg_water.dir[0] * cg_water.dir[0] + cg_water.dir[1] * cg_water.dir[1]);

        if (l > 0.0f) {
            cg_water.dir[0] /= l;
            cg_water.dir[1] /= l;
        } else {
            cg_water.dir[0] = 1.0f;
            cg_water.dir[1] = 0.0f;
        }
    }
}

static void CG_WaterCheckMap(void)
{
    if (Q_stricmp(cg_water.map, cgs.mapname)) {
        CG_WaterLoad();
        memset(cg_waterBlood, 0, sizeof(cg_waterBlood));
    }
}

// Where the first wave train is at a point now, as water_fp.glsl has it
static float CG_WaterPhase(const vec3_t p)
{
    const float k = 2.0f * M_PI / cg_water.length;

    return k * (cg_water.dir[0] * p[0] + cg_water.dir[1] * p[1]) - 2.0f * M_PI * (cg.time * 0.001f) / cg_water.period;
}

qboolean CG_WaterSample(const vec3_t p, cgWaterSample_t *out)
{
    trace_t tr;
    vec3_t  start, end;

    CG_WaterCheckMap();

    // down onto the water's top from above the point
    VectorSet(start, p[0], p[1], p[2] + WATER_REACH);
    VectorSet(end, p[0], p[1], p[2] - cg_water.height * 2.0f - 16.0f);
    CG_Trace(&tr, start, vec3_origin, vec3_origin, end, ENTITYNUM_NONE, MASK_WATER, qfalse, qfalse, "water top");
    if (tr.startsolid || tr.allsolid) {
        // deeper than the reach: under all of it
        out->surface = start[2];
    } else if (tr.fraction < 1.0f) {
        out->surface = tr.endpos[2];
    } else {
        return qfalse;
    }

    // and on down to the bottom
    VectorSet(start, p[0], p[1], out->surface);
    VectorSet(end, p[0], p[1], out->surface - WATER_DEEP);
    CG_Trace(&tr, start, vec3_origin, vec3_origin, end, ENTITYNUM_NONE, MASK_SOLID, qfalse, qfalse, "water bottom");
    out->depth = tr.startsolid ? 0.0f : (out->surface - tr.endpos[2]);

    return qtrue;
}

float CG_WaterSwell(const vec3_t p)
{
    CG_WaterCheckMap();

    return cg_water.height * sin(CG_WaterPhase(p));
}

void CG_WaterFlow(const vec3_t p, float depth, vec3_t out)
{
    const float phase = CG_WaterPhase(p);
    const float s     = sin(phase);
    const float omega = 2.0f * M_PI / cg_water.period;
    float       surf, along;

    CG_WaterCheckMap();

    // in the surf, all of it from where the surf starts breaking, none of
    // it three times as deep
    surf = 1.0f;
    if (cg_water.surf > 0.0f) {
        const float x = Com_Clamp(0.0f, 1.0f, (depth - cg_water.surf) / (cg_water.surf * 2.0f));

        surf = 1.0f - x * x * (3.0f - 2.0f * x);
    }

    // Out past it the water goes to and fro under the swell. In it, a
    // breaking wave throws the water on with its crest and it runs back
    // more weakly in between, and the whole of it creeps up the shore.
    along = cg_water.height * omega * s * (1.0f - surf);
    along += surf * (cg_water.drift + cg_water.surge * (s > 0.0f ? s * s : -0.8f * s * s));

    out[0] = cg_water.dir[0] * along;
    out[1] = cg_water.dir[1] * along;
    out[2] = 0.0f;
}

void CG_WaterBleed(int body, int born, const vec3_t at)
{
    cgWaterBlood_t *b, *oldest = NULL, *slot = NULL;
    int             i;

    if (!cg_water_blood->integer) {
        return;
    }

    for (i = 0; i < WATER_BLOOD_MAX; i++) {
        b = &cg_waterBlood[i];
        if (b->used && b->body == body && b->born == born) {
            b->lastSeen  = cg.time;
            b->bodyAt[0] = at[0];
            b->bodyAt[1] = at[1];
            return;
        }
        if (!b->used) {
            if (!slot) {
                slot = b;
            }
        } else if (!oldest || b->lastSeen < oldest->lastSeen) {
            oldest = b;
        }
    }

    // full: the one whose body has been out of the water longest gives way
    // to the newest, but only one already fading. Bodies all still in the
    // water would otherwise take each other's place every frame, and none of
    // their clouds would ever grow.
    if (!slot) {
        if (cg.time - oldest->lastSeen <= WATER_BLOOD_LINGER) {
            return;
        }
        slot = oldest;
    }

    slot->used     = qtrue;
    slot->body     = body;
    slot->born     = born;
    slot->wentIn   = cg.time;
    slot->lastSeen = cg.time;
    slot->at[0]    = at[0];
    slot->at[1]    = at[1];
    slot->bodyAt[0] = at[0];
    slot->bodyAt[1] = at[1];
}

// The body's blood follows it, a share of the way a second, so a body the
// waves carry leaves a smear of it behind
static void CG_WaterBloodFollow(cgWaterBlood_t *b, float dt)
{
    const float share = Q_min(1.0f, WATER_BLOOD_TRAIL * dt);

    b->at[0] += (b->bodyAt[0] - b->at[0]) * share;
    b->at[1] += (b->bodyAt[1] - b->at[1]) * share;
}

void CG_WaterBloodFrame(void)
{
    float spots[WATER_BLOOD_MAX][4];
    float away[WATER_BLOOD_MAX];
    int   n = 0, i;
    float dt;

    if (!cgi.R_SetWaterBlood || cgi.apiversion < 7) {
        return;
    }

    dt = cg_waterBloodTime ? (cg.time - cg_waterBloodTime) * 0.001f : 0.0f;
    if (dt < 0.0f || dt > 1.0f) {
        dt = 0.0f;
    }
    cg_waterBloodTime = cg.time;

    for (i = 0; i < WATER_BLOOD_MAX; i++) {
        cgWaterBlood_t *b = &cg_waterBlood[i];
        float           age, strength;

        if (!b->used) {
            continue;
        }
        // a game restored to before it went in
        if (cg.time < b->wentIn) {
            b->used = qfalse;
            continue;
        }

        age      = (cg.time - b->wentIn) * 0.001f;
        strength = Q_min(1.0f, age / WATER_BLOOD_THICKEN);
        if (cg.time - b->lastSeen > WATER_BLOOD_LINGER) {
            strength *= 1.0f - (float)(cg.time - b->lastSeen - WATER_BLOOD_LINGER) / WATER_BLOOD_FADE;
            if (strength <= 0.0f) {
                b->used = qfalse;
                continue;
            }
        }

        CG_WaterBloodFollow(b, dt);

        spots[n][0] = b->at[0];
        spots[n][1] = b->at[1];
        {
            const float spread = 1.0f - exp(-age / WATER_BLOOD_SLOW);

            // thinner as it spreads
            spots[n][2] = WATER_BLOOD_START + WATER_BLOOD_SPREAD * spread;
            spots[n][3] = strength * (1.0f - 0.35f * spread);
        }
        away[n]     = Square(b->at[0] - cg.refdef.vieworg[0]) + Square(b->at[1] - cg.refdef.vieworg[1]);
        n++;
    }

    // the nearest the view, if there are more than the renderer takes
    while (n > WATER_BLOOD_SENT) {
        int far = 0;

        for (i = 1; i < n; i++) {
            if (away[i] > away[far]) {
                far = i;
            }
        }
        n--;
        memcpy(spots[far], spots[n], sizeof(spots[far]));
        away[far] = away[n];
    }

    if (!cg_water_blood->integer) {
        n = 0;
    }
    // nothing to say twice when there is none
    if (n || cg_waterBloodSent) {
        cgi.R_SetWaterBlood(&spots[0][0], n);
    }
    cg_waterBloodSent = n;
}
