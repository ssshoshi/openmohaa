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
//
// Where the map has a beach (`beach` in its file: the sand the falling tide
// has left wet), a body lying on it soaks the sand under it red. The renderer
// stains the beach round each, out to a radius that grows from when the body
// came to rest, and is there from the start for the dead the map placed.
//
// The dead the map placed are static models (models/static/body_*.tik): read
// from the map as it loads, each one gets its blood at once, fully spread, in
// the water or on the sand where it lies. The game puts a real body in the
// place of each American one (Level::SpawnPlacedDead), and once he is here the
// static one goes (CG_PlacedDeadFrame): he is a ragdoll once anything reaches
// him, and bleeds and stains as any body does.

#include "cg_local.h"
#include "cg_water.h"
#include "cg_lens.h"
#include "../qcommon/qfiles.h"

cvar_t *cg_water_float;
cvar_t *cg_water_floatrange;
cvar_t *cg_water_blood;
cvar_t *cg_water_ripples;

#define WATER_REACH    256.0f // how far above a point its water's top is looked for
#define WATER_DEEP     1024.0f // how far below it the bottom is
// How far from the view anything is looked at for what it does to the water:
// half the renderer's ripple grid (WATER_RIPPLE_SPAN, round the view) and a
// hull's length
#define WATER_RIPPLE_REACH (3072.0f * 0.5f + 700.0f)

#define WATER_BLOOD_MAX     64      // bodies whose blood is kept
#define WATER_BLOOD_SENT    32      // as many as the renderer takes (WATER_MAX_SPOTS)
#define WATER_BLOOD_START   28.0f   // how far round a body its blood is at first, units
#define WATER_BLOOD_SPREAD  170.0f  // and how much further it goes in all
#define WATER_BLOOD_SLOW    18.0f   // seconds for it to go most of the way out
#define WATER_BLOOD_THICKEN 1.5f    // seconds for it to come up thick round a body just in
#define WATER_BLOOD_LINGER  1500    // ms a cloud stays as it was once its body is out
#define WATER_BLOOD_FADE    6000    // and then ms to fade
#define WATER_BLOOD_TRAIL   0.35f   // how fast the cloud follows its body, a share a second

#define WATER_STAIN_MAX     64      // bodies on the sand whose stains are kept
#define WATER_STAIN_START   0.35f   // how much of its full size a stain has at first
#define WATER_STAIN_SLOW    14.0f   // seconds for it to spread most of the way
#define WATER_STAIN_MOVED   24.0f   // how far a body moves before its stain is traced afresh
#define WATER_STAIN_FADE    4000    // ms a stain lasts after its body is gone

typedef struct {
    char   map[MAX_QPATH];
    vec2_t dir;
    float  length, period, height;
    float  surf;
    float  drift, surge;
    qboolean beach;
    qboolean hasFile;
} cgWater_t;

static cgWater_t cg_water;

typedef struct {
    qboolean used;
    int      body, born;
    int      wentIn;   // when the body went into the water
    int      lastSeen; // when it was last in it
    vec2_t   at;       // the cloud's middle, trailing the body
    vec2_t   bodyAt;   // where the body was last in the water
    qboolean placed;   // one of the dead the map placed: for good
} cgWaterBlood_t;

static cgWaterBlood_t cg_waterBlood[WATER_BLOOD_MAX];

typedef struct {
    qboolean used;
    int      body, born;
    int      since;    // when it began to spread
    int      lastSeen;
    vec3_t   at;       // on the ground under the body
    float    radius;   // how far it goes in the end
    qboolean placed;   // one of the dead the map placed: for good
} cgWaterStain_t;

static cgWaterStain_t cg_waterStains[WATER_STAIN_MAX];

// The dead the map placed, static models, and whether each has been hidden
// under the body put in its place (CG_PlacedDeadFrame)
#define PLACED_DEAD_MAX 128

typedef struct {
    int      index; // among the map's static models
    vec3_t   origin;
    qboolean hidden;
} cgPlacedDead_t;

static cgPlacedDead_t cg_placedDead[PLACED_DEAD_MAX];
static int            cg_numPlacedDead;
static int            cg_waterBloodTime;
static int            cg_waterBloodSent;

// What disturbs the water (see Ripples, below)
#define WATER_RIPPLE_GATHER 96
#define WATER_RIPPLE_SENT   32 // as many as the renderer takes (WATER_MAX_SPOTS)

// how deep in it a man is wading, rather than dry or swimming; and deeper
// than that, still swimming at its top rather than gone under
#define WATER_WADE_MIN 3.0f
#define WATER_WADE_MAX 70.0f
#define WATER_SWIM_MAX 110.0f
// slower through it than this (units a second), a man leaves no wake
#define WATER_WAKE_SPEED 12.0f
// the widest a thing can be and still be pushed through the water as a man is;
// wider, it has a hull (a boat)
#define WATER_WADER_SIZE 48.0f
#define WATER_HULL_SIZE  700.0f // and no hull is wider than this
#define WATER_JUMP       200.0f // further in a frame than this, a thing was moved, not going

typedef struct {
    float r[5]; // x, y, width, dip, foam
    float away; // from the view, squared
} cgWaterRipple_t;

static cgWaterRipple_t cg_waterRipples[WATER_RIPPLE_GATHER];
static int             cg_numWaterRipples;
static int             cg_waterRippleTime;

// Where something went in, the water rocks up and down there for a moment, and
// sends out a train of rings rather than one
#define WATER_RINGS      32
#define WATER_RING_TIME  0.7f  // seconds it rocks for
#define WATER_RING_DECAY 0.22f // seconds for it to die down most of the way

typedef struct {
    vec3_t at;
    float  size;
    float  push; // how hard, a second
    float  freq; // how often, a second
    int    start;
} cgWaterRing_t;

static cgWaterRing_t cg_waterRings[WATER_RINGS];

// each entity as it was the frame before
static struct {
    int      time;  // cg.time then, or 0
    int      model; // its modelindex then
    vec3_t   origin;
    qboolean wet;   // whether it was in the water
} cg_waterWas[MAX_GENTITIES];

void CG_InitWater(void)
{
    cg_water_float = cgi.Cvar_Get("cg_water_float", "1", CVAR_ARCHIVE);
    cg_water_floatrange = cgi.Cvar_Get("cg_water_floatrange", "1500", CVAR_ARCHIVE);
    cg_water_blood = cgi.Cvar_Get("cg_water_blood", "1", CVAR_ARCHIVE);
    cg_water_ripples = cgi.Cvar_Get("cg_water_ripples", "1", CVAR_ARCHIVE);
    memset(cg_waterWas, 0, sizeof(cg_waterWas));
    cg_numWaterRipples = 0;
    cg_waterRippleTime = 0;
    memset(cg_waterRings, 0, sizeof(cg_waterRings));
    cg_water.map[0] = 0;
    memset(cg_waterBlood, 0, sizeof(cg_waterBlood));
    memset(cg_waterStains, 0, sizeof(cg_waterStains));
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
    cg_water.beach  = qfalse;
    cg_water.hasFile = qfalse;
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
        } else if (!Q_stricmp(key, "beach")) {
            cg_water.beach = CG_WaterNumbers(&text, v, 2) == 2;
        }
        // the rest is the renderer's
        SkipRestOfLine(&text);
    }

    cgi.FS_FreeFile(buf);
    cg_water.hasFile = qtrue;

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

static void CG_WaterPlacedDead(void);

static void CG_WaterCheckMap(void)
{
    if (Q_stricmp(cg_water.map, cgs.mapname)) {
        memset(cg_waterBlood, 0, sizeof(cg_waterBlood));
        memset(cg_waterStains, 0, sizeof(cg_waterStains));
        CG_WaterLoad();
        CG_WaterPlacedDead();
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

static qboolean CG_WaterStainGround(const vec3_t at, vec3_t ground);

// The dead the map placed, static models: their blood, there from the start
static void CG_WaterPlacedDead(void)
{
    dheader_t             header;
    const lump_t         *lump;
    const cStaticModel_t *in;
    void                 *buf = NULL;
    long                  len;
    int                   count, i, nBlood = 0, nStain = 0;

    cg_numPlacedDead = 0;
    if (!cgs.mapname[0]) {
        return;
    }

    len = cgi.FS_ReadFile(cgs.mapname, &buf, qtrue);
    if (len < (long)sizeof(dheader_t) || !buf) {
        if (buf) {
            cgi.FS_FreeFile(buf);
        }
        return;
    }

    memcpy(&header, buf, sizeof(header));
    for (i = 0; i < (int)(sizeof(dheader_t) / 4); i++) {
        ((int *)&header)[i] = LittleLong(((int *)&header)[i]);
    }
    lump  = Q_GetLumpByVersion(&header, LUMP_STATICMODELDEF);
    count = (lump->fileofs > 0 && lump->filelen > 0 && lump->fileofs + lump->filelen <= len)
              ? lump->filelen / (int)sizeof(cStaticModel_t)
              : 0;
    in    = (const cStaticModel_t *)((const byte *)buf + lump->fileofs);

    for (i = 0; i < count; i++) {
        char            name[sizeof(in[i].model) + 1];
        vec3_t          at, ground;
        cgWaterSample_t water;

        memcpy(name, in[i].model, sizeof(in[i].model));
        name[sizeof(in[i].model)] = 0;
        if (!Q_stristr(name, "/body_")) {
            continue;
        }

        if (cg_numPlacedDead < PLACED_DEAD_MAX) {
            cgPlacedDead_t *pd = &cg_placedDead[cg_numPlacedDead++];

            pd->index = i;
            VectorCopy(in[i].origin, pd->origin);
            pd->hidden = qfalse;
        }

        if (!cg_water.hasFile || !cg_water_blood->integer) {
            continue;
        }

        VectorCopy(in[i].origin, at);
        at[2] += 8.0f;

        if (CG_WaterSample(at, &water) && water.surface > at[2] - 8.0f) {
            if (nBlood < WATER_BLOOD_MAX / 2) {
                cgWaterBlood_t *b = &cg_waterBlood[nBlood++];

                b->used     = qtrue;
                b->body     = -1 - i;
                b->born     = 0;
                b->wentIn   = cg.time - 600000;
                b->lastSeen = cg.time;
                b->at[0] = b->bodyAt[0] = at[0];
                b->at[1] = b->bodyAt[1] = at[1];
                b->placed = qtrue;
            }
        } else if (cg_water.beach && nStain < WATER_STAIN_MAX / 2 && CG_WaterStainGround(at, ground)) {
            cgWaterStain_t *st = &cg_waterStains[nStain++];

            st->used     = qtrue;
            st->body     = -1 - i;
            st->born     = 0;
            st->since    = cg.time - 600000;
            st->lastSeen = cg.time;
            VectorCopy(ground, st->at);
            st->radius = 62.0f + (float)((unsigned)(i * 2654435761u) % 1000) * 0.035f;
            st->placed = qtrue;
        }
    }

    cgi.FS_FreeFile(buf);
}

void CG_WaterBleed(int body, int born, const vec3_t at, qboolean longDead)
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
    slot->wentIn   = longDead ? cg.time - 600000 : cg.time;
    slot->lastSeen = cg.time;
    slot->at[0]    = at[0];
    slot->at[1]    = at[1];
    slot->bodyAt[0] = at[0];
    slot->bodyAt[1] = at[1];
    slot->placed    = qfalse;
}

// Where under the body the ground is
static qboolean CG_WaterStainGround(const vec3_t at, vec3_t ground)
{
    trace_t tr;
    vec3_t  end;

    VectorSet(end, at[0], at[1], at[2] - 64.0f);
    CG_Trace(&tr, at, vec3_origin, vec3_origin, end, ENTITYNUM_NONE, MASK_SOLID, qfalse, qfalse, "blood on the sand");
    if (tr.startsolid || tr.fraction >= 1.0f) {
        return qfalse;
    }
    VectorCopy(tr.endpos, ground);
    return qtrue;
}

void CG_WaterStain(int body, int born, const vec3_t at, qboolean longDead)
{
    cgWaterStain_t *st, *slot = NULL, *oldest = NULL;
    vec3_t          ground;
    int             i;

    if (!cg_water_blood->integer) {
        return;
    }
    CG_WaterCheckMap();
    if (!cg_water.beach) {
        return;
    }

    for (i = 0; i < WATER_STAIN_MAX; i++) {
        st = &cg_waterStains[i];
        if (st->used && st->body == body && st->born == born) {
            st->lastSeen = cg.time;
            // moved on (the waves, a blast): it bleeds where it lies now
            if (Square(at[0] - st->at[0]) + Square(at[1] - st->at[1]) > Square(WATER_STAIN_MOVED)
                && CG_WaterStainGround(at, ground)) {
                VectorCopy(ground, st->at);
            }
            return;
        }
        if (!st->used) {
            if (!slot) {
                slot = st;
            }
        } else if (!oldest || st->lastSeen < oldest->lastSeen) {
            oldest = st;
        }
    }

    if (!CG_WaterStainGround(at, ground)) {
        return;
    }
    if (!slot) {
        if (cg.time - oldest->lastSeen <= WATER_BLOOD_LINGER) {
            return;
        }
        slot = oldest;
    }

    slot->used     = qtrue;
    slot->body     = body;
    slot->born     = born;
    slot->since    = longDead ? cg.time - 600000 : cg.time;
    slot->lastSeen = cg.time;
    VectorCopy(ground, slot->at);
    // each his own size, from where and when he was
    slot->radius = 62.0f + (float)((unsigned)(body * 2654435761u + born) % 1000) * 0.035f;
    slot->placed = qfalse;
}

// The body's blood follows it, a share of the way a second, so a body the
// waves carry leaves a smear of it behind
static void CG_WaterBloodFollow(cgWaterBlood_t *b, float dt)
{
    const float share = Q_min(1.0f, WATER_BLOOD_TRAIL * dt);

    b->at[0] += (b->bodyAt[0] - b->at[0]) * share;
    b->at[1] += (b->bodyAt[1] - b->at[1]) * share;
}

// The stains to send, the nearest the view first: x, y, the ground's height, how far
static int CG_WaterStainsFrame(float stains[WATER_STAIN_MAX][4])
{
    float away[WATER_STAIN_MAX];
    int   n = 0, i;

    for (i = 0; i < WATER_STAIN_MAX; i++) {
        cgWaterStain_t *st = &cg_waterStains[i];
        float           grown, gone = 1.0f;

        if (!st->used) {
            continue;
        }
        if (st->placed) {
            st->lastSeen = cg.time;
        }
        if (cg.time < st->lastSeen) {
            st->used = qfalse; // a game restored to before it
            continue;
        }
        // its body taken away: the stain goes too, a little after
        if (cg.time - st->lastSeen > WATER_BLOOD_LINGER) {
            gone = 1.0f - (float)(cg.time - st->lastSeen - WATER_BLOOD_LINGER) / WATER_STAIN_FADE;
            if (gone <= 0.0f) {
                st->used = qfalse;
                continue;
            }
        }

        grown = WATER_STAIN_START + (1.0f - WATER_STAIN_START) * (1.0f - exp(-(cg.time - st->since) * 0.001f / WATER_STAIN_SLOW));

        stains[n][0] = st->at[0];
        stains[n][1] = st->at[1];
        stains[n][2] = st->at[2];
        stains[n][3] = st->radius * grown * gone;
        away[n]      = Square(st->at[0] - cg.refdef.vieworg[0]) + Square(st->at[1] - cg.refdef.vieworg[1]);
        n++;
    }

    while (n > WATER_BLOOD_SENT) {
        int far = 0;

        for (i = 1; i < n; i++) {
            if (away[i] > away[far]) {
                far = i;
            }
        }
        n--;
        memcpy(stains[far], stains[n], sizeof(stains[far]));
        away[far] = away[n];
    }
    return n;
}

void CG_WaterBloodFrame(void)
{
    float spots[WATER_BLOOD_MAX][4];
    float stains[WATER_STAIN_MAX][4];
    float away[WATER_BLOOD_MAX];
    int   n = 0, numStains, i;
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
        if (b->placed) {
            b->lastSeen = cg.time;
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

    numStains = CG_WaterStainsFrame(stains);

    if (!cg_water_blood->integer) {
        n         = 0;
        numStains = 0;
    }
    // nothing to say twice when there is none
    if (n || numStains || cg_waterBloodSent) {
        cgi.R_SetWaterBlood(&spots[0][0], n, &stains[0][0], numStains);
    }
    cg_waterBloodSent = n + numStains;
}

//=============================================================
// Ripples
//=============================================================
//
// What goes through the water disturbs it, and the renderer spreads that out
// in ripples and wakes (RE_AddWaterRipples, glsl/ripple_fp.glsl). Each frame
// gathers what disturbs it: a dip pressed into the water at a point, so wide,
// and foam left there. A man wading pushes a little in front of him as he goes;
// a boat a bow wave, at its shoulders and a wash from its stern; a bullet, a
// blast or anything falling in, one dip at once. The nearest to the view are
// sent.



void CG_WaterRipple(const vec3_t at, float width, float dip, float foam)
{
    cgWaterRipple_t *r;
    float            away;
    int              i;

    if (!cg_water_ripples || !cg_water_ripples->integer || width <= 0.0f || (dip == 0.0f && foam <= 0.0f)) {
        return;
    }

    away = Square(at[0] - cg.refdef.vieworg[0]) + Square(at[1] - cg.refdef.vieworg[1]);

    // where one is already gathered close by (a body's parts, the steps of
    // one frame), the two are one: so a few things in the water cannot take
    // every place there is
    for (i = 0; i < cg_numWaterRipples; i++) {
        r = &cg_waterRipples[i];
        if (Square(r->r[0] - at[0]) + Square(r->r[1] - at[1]) < Square(Q_min(r->r[2], width) * 0.5f)) {
            const float wa = fabs(r->r[3]) + r->r[4], wb = fabs(dip) + foam;

            // the middle between them, by how much each pushes
            if (wa + wb > 0.0f) {
                r->r[0] += (at[0] - r->r[0]) * wb / (wa + wb);
                r->r[1] += (at[1] - r->r[1]) * wb / (wa + wb);
            }
            r->r[2] = Q_max(r->r[2], width);
            r->r[3] += dip;
            r->r[4] += foam;
            return;
        }
    }

    if (cg_numWaterRipples < WATER_RIPPLE_GATHER) {
        r = &cg_waterRipples[cg_numWaterRipples++];
    } else {
        // in place of the furthest, if this is nearer
        r = &cg_waterRipples[0];
        for (i = 1; i < WATER_RIPPLE_GATHER; i++) {
            if (cg_waterRipples[i].away > r->away) {
                r = &cg_waterRipples[i];
            }
        }
        if (r->away <= away) {
            return;
        }
    }

    r->r[0] = at[0];
    r->r[1] = at[1];
    r->r[2] = width;
    r->r[3] = dip;
    r->r[4] = foam;
    r->away = away;
}

void CG_WaterSplash(const vec3_t at, float size, float speed)
{
    const float    hard = Com_Clamp(0.3f, 2.5f, speed / 300.0f);
    const float    dip  = size * 0.18f * hard;
    cgWaterRing_t *ring = &cg_waterRings[0];
    int            i;

    CG_WaterRipple(at, size, dip, Q_min(1.0f, size * 0.04f * hard + 0.15f));
    // and close by, some of it on the lens
    CG_LensSplash(at, size);

    // and it rocks on: in the oldest place, if all are taken
    for (i = 1; i < WATER_RINGS && ring->start; i++) {
        if (!cg_waterRings[i].start || cg_waterRings[i].start < ring->start) {
            ring = &cg_waterRings[i];
        }
    }
    VectorCopy(at, ring->at);
    ring->size  = size;
    ring->push  = dip * 9.0f;
    ring->freq  = Com_Clamp(2.5f, 8.0f, 40.0f / size);
    ring->start = cg.time ? cg.time : 1;
}

// The water where things went in, rocking
static void CG_WaterRings(float dt)
{
    int i;

    for (i = 0; i < WATER_RINGS; i++) {
        cgWaterRing_t *ring = &cg_waterRings[i];
        float          age;

        if (!ring->start) {
            continue;
        }
        age = (cg.time - ring->start) * 0.001f;
        if (age < 0.0f || age > WATER_RING_TIME) {
            ring->start = 0;
            continue;
        }
        CG_WaterRipple(ring->at, ring->size * 0.6f,
            ring->push * sin(2.0f * M_PI * ring->freq * age) * exp(-age / WATER_RING_DECAY) * dt, 0.0f);
    }
}

// A man (or anything about his size) in the water to his waist or so: he
// rocks it as he sways and strides, more the faster he goes, and as he goes
// pushes the water in front of him, which trails off behind him as a wake.
// Rocked where he stands, not pressed down: pressed steadily, it would sink
// into a hole round him. Swimming, deeper, he only leaves the wake.
static void CG_WaterWade(int who, const vec3_t origin, const vec3_t velocity, float dt)
{
    cgWaterSample_t water;
    vec3_t          feet, at;
    float           deep, speed, push;

    VectorSet(feet, origin[0], origin[1], origin[2] + 2.0f);
    if (!CG_WaterSample(feet, &water)) {
        return;
    }
    deep = water.surface - origin[2];
    if (deep < WATER_WADE_MIN || deep > WATER_SWIM_MAX) {
        return;
    }

    speed = sqrt(Square(velocity[0]) + Square(velocity[1]));
    push  = Com_Clamp(0.4f, 1.5f, Q_min(deep, WATER_WADE_MAX) / 24.0f);
    VectorSet(at, origin[0], origin[1], water.surface);
    if (speed > 1.0f) {
        // a little ahead of him, where he meets it
        at[0] += velocity[0] / speed * 6.0f;
        at[1] += velocity[1] / speed * 6.0f;
    }

    if (deep <= WATER_WADE_MAX) {
        const float stride = 1.2f + speed / 150.0f;
        const float rock   = sin(2.0f * M_PI * stride * cg.time * 0.001f + who * 1.7f);

        CG_WaterRipple(at, 8.0f + deep * 0.1f, ((0.8f + speed * 0.02f) * rock + speed * 0.004f) * push * dt,
            speed / 150.0f * 0.8f * dt);
    }

    // the wake: the water he goes into pushed aside, as a body dragged
    // through it is (CG_JoltRagdollFloat), from where he meets it
    if (speed > WATER_WAKE_SPEED) {
        const float pace = Q_min(speed, 320.0f);

        VectorSet(at, origin[0] + velocity[0] / speed * 10.0f, origin[1] + velocity[1] / speed * 10.0f, water.surface);
        CG_WaterRipple(at, 16.0f, pace * 0.03f * Q_min(push, 1.0f) * dt, pace / 150.0f * 0.6f * dt);
    }
}

// A hull going through the water: a bow wave where it meets it, the water
// thrown off its shoulders, and the wash from its stern; and rings round it
// as it rises and falls.
static void CG_WaterHull(const vec3_t origin, const vec3_t angles, const vec3_t mins, const vec3_t maxs,
    const vec3_t velocity, float dt)
{
    cgWaterSample_t water;
    vec3_t          axis[3], mid, centre, low, at, dir, side;
    float           speed, along, across, half[3], rate;
    int             i;

    AnglesToAxis(angles, axis);
    for (i = 0; i < 3; i++) {
        mid[i]  = 0.5f * (mins[i] + maxs[i]);
        half[i] = 0.5f * (maxs[i] - mins[i]);
    }
    VectorCopy(origin, centre);
    for (i = 0; i < 3; i++) {
        VectorMA(centre, mid[i], axis[i], centre);
    }

    // in the water: its keel under it, its top out of it
    VectorSet(low, centre[0], centre[1], centre[2] - half[2] + 2.0f);
    if (!CG_WaterSample(low, &water) || water.surface > centre[2] + half[2] || water.depth <= 0.0f) {
        return;
    }

    speed = sqrt(Square(velocity[0]) + Square(velocity[1]));
    if (speed > 5.0f) {
        VectorSet(dir, velocity[0] / speed, velocity[1] / speed, 0.0f);
        VectorSet(side, -dir[1], dir[0], 0.0f);
        along  = fabs(half[0] * DotProduct(axis[0], dir)) + fabs(half[1] * DotProduct(axis[1], dir));
        across = fabs(half[0] * DotProduct(axis[0], side)) + fabs(half[1] * DotProduct(axis[1], side));
        rate   = speed * 0.05f;

        VectorMA(centre, along * 0.9f, dir, at);
        at[2] = water.surface;
        CG_WaterRipple(at, across * 0.7f, rate * dt, speed / 90.0f * 1.5f * dt);

        for (i = -1; i <= 1; i += 2) {
            VectorMA(centre, along * 0.55f, dir, at);
            VectorMA(at, i * across * 0.9f, side, at);
            CG_WaterRipple(at, across * 0.35f, rate * 0.45f * dt, speed / 90.0f * 1.2f * dt);
        }

        VectorMA(centre, -along * 0.95f, dir, at);
        CG_WaterRipple(at, across * 0.8f, rate * 0.15f * dt, speed / 90.0f * 1.2f * dt);
    }

    if (fabs(velocity[2]) > 2.0f) {
        CG_WaterRipple(centre, Q_max(half[0], half[1]) * 0.6f, -velocity[2] * 0.05f * dt, 0.0f);
    }
}

static qboolean CG_WaterWet(const vec3_t p)
{
    return (CG_PointContents(p, 0) & MASK_WATER) != 0;
}

void CG_WaterRipplesFrame(void)
{
    float out[WATER_RIPPLE_SENT][5];
    float dt;
    int   n, i;

    if (!cgi.R_AddWaterRipples || cgi.apiversion < 7) {
        return;
    }
    CG_WaterCheckMap();

    dt = cg_waterRippleTime ? (cg.time - cg_waterRippleTime) * 0.001f : 0.0f;
    cg_waterRippleTime = cg.time;
    if (dt <= 0.0f || dt > 0.25f) {
        // a frame the game stood still in, or a jump: none from going this time
        dt = 0.0f;
    }

    if (dt > 0.0f && cg_water_ripples->integer && cg.snap) {
        vec3_t mins, maxs, velocity;

        // the player
        CG_WaterRings(dt);
        CG_WaterWade(cg.snap->ps.clientNum, cg.predicted_player_state.origin, cg.predicted_player_state.velocity, dt);

        for (i = 0; i < cg.snap->numEntities; i++) {
            const int  num  = cg.snap->entities[i].number;
            centity_t *cent = &cg_entities[num];
            const entityState_t *s = &cent->currentState;
            float      size;
            qboolean   wet;

            if (num == cg.snap->ps.clientNum || s->modelindex <= 0) {
                cg_waterWas[num].time = 0;
                continue;
            }

            // nothing it does reaches the ripples' grid, round the view
            if (fabs(cent->lerpOrigin[0] - cg.refdef.vieworg[0]) > WATER_RIPPLE_REACH
                || fabs(cent->lerpOrigin[1] - cg.refdef.vieworg[1]) > WATER_RIPPLE_REACH) {
                cg_waterWas[num].time = 0;
                continue;
            }

            if (s->solid == SOLID_BMODEL) {
                cgi.R_ModelBounds(cgs.inlineDrawModel[s->modelindex], mins, maxs);
            } else {
                // a model's bounds, worked out from its animations once
                static qhandle_t boundsOf[MAX_MODELS];
                static vec3_t    boundsMins[MAX_MODELS], boundsMaxs[MAX_MODELS];
                const qhandle_t  h     = cgs.model_draw[s->modelindex];
                const float      scale = s->scale > 0.0f ? s->scale : 1.0f;

                if (boundsOf[s->modelindex] != h || !h) {
                    dtiki_t *tiki = cgi.R_Model_GetHandle(h);

                    if (!tiki) {
                        cg_waterWas[num].time = 0;
                        continue;
                    }
                    cgi.TIKI_CalculateBounds(tiki, 1.0f, boundsMins[s->modelindex], boundsMaxs[s->modelindex]);
                    boundsOf[s->modelindex] = h;
                }
                VectorScale(boundsMins[s->modelindex], scale, mins);
                VectorScale(boundsMaxs[s->modelindex], scale, maxs);
            }
            size = Q_max(maxs[0] - mins[0], maxs[1] - mins[1]);

            // how it has moved since the frame before, if it was there then
            if (cg_waterWas[num].time && cg_waterWas[num].model == s->modelindex
                && Distance(cg_waterWas[num].origin, cent->lerpOrigin) < WATER_JUMP) {
                VectorSubtract(cent->lerpOrigin, cg_waterWas[num].origin, velocity);
                VectorScale(velocity, 1.0f / ((cg.time - cg_waterWas[num].time) * 0.001f), velocity);
            } else {
                VectorClear(velocity);
                cg_waterWas[num].wet = CG_WaterWet(cent->lerpOrigin);
            }

            if (size >= WATER_WADER_SIZE) {
                if (size < WATER_HULL_SIZE) {
                    CG_WaterHull(cent->lerpOrigin, cent->lerpAngles, mins, maxs, velocity, dt);
                }
            } else if (s->eType == ET_PLAYER || s->eType == ET_MODELANIM_SKEL
                       || (s->eType == ET_MODELANIM && maxs[2] - mins[2] > 48.0f)) {
                CG_WaterWade(num, cent->lerpOrigin, velocity, dt);
            } else {
                // a thing falling in: a grenade, a helmet, what was dropped
                wet = CG_WaterWet(cent->lerpOrigin);
                if (wet && !cg_waterWas[num].wet && velocity[2] < -40.0f) {
                    cgWaterSample_t water;

                    if (CG_WaterSample(cent->lerpOrigin, &water)) {
                        vec3_t at;

                        VectorSet(at, cent->lerpOrigin[0], cent->lerpOrigin[1], water.surface);
                        CG_WaterSplash(at, Com_Clamp(4.0f, 24.0f, size), -velocity[2]);
                    }
                }
                cg_waterWas[num].wet = wet;
            }

            cg_waterWas[num].time  = cg.time;
            cg_waterWas[num].model = s->modelindex;
            VectorCopy(cent->lerpOrigin, cg_waterWas[num].origin);
        }
    }

    // the nearest, to the renderer
    n = 0;
    while (cg_numWaterRipples > 0 && n < WATER_RIPPLE_SENT) {
        int near = 0;

        for (i = 1; i < cg_numWaterRipples; i++) {
            if (cg_waterRipples[i].away < cg_waterRipples[near].away) {
                near = i;
            }
        }
        memcpy(out[n++], cg_waterRipples[near].r, sizeof(out[0]));
        cg_waterRipples[near] = cg_waterRipples[--cg_numWaterRipples];
    }
    cg_numWaterRipples = 0;

    if (n) {
        cgi.R_AddWaterRipples(&out[0][0], n);
    }
}

//=============================================================
// The dead the map placed
//=============================================================
//
// The game puts a body where each static one lies (Level::SpawnPlacedDead): a
// man lying dead (models/animate/*_dead.tik), which the client makes a ragdoll
// of once something reaches him. Once he is here, the static one under him
// goes. The stain it had stays, as the sand under him is soaked; the cloud it
// had in the water goes, as he bleeds into the water as any body does,
// wherever the waves take him.

void CG_PlacedDeadFrame(void)
{
    static const vec3_t gone[3] = {
        {0.001f, 0.0f,   0.0f  },
        {0.0f,   0.001f, 0.0f  },
        {0.0f,   0.0f,   0.001f}
    };
    int i, j, k;

    CG_WaterCheckMap();
    if (!cg_numPlacedDead || !cg.snap || cgi.apiversion < 5 || !cgi.R_SetStaticModelTransform) {
        return;
    }

    for (i = 0; i < cg.snap->numEntities; i++) {
        const entityState_t *s = &cg_entities[cg.snap->entities[i].number].currentState;
        const char          *name;

        if (s->modelindex <= 0 || s->solid == SOLID_BMODEL) {
            continue;
        }
        name = CG_ConfigString(CS_MODELS + s->modelindex);
        if (Q_stricmpn(name, "models/animate/", 15) || !Q_stristr(name, "_dead")) {
            continue;
        }

        for (j = 0; j < cg_numPlacedDead; j++) {
            cgPlacedDead_t *pd = &cg_placedDead[j];

            if (pd->hidden || Distance(pd->origin, s->origin) > 8.0f) {
                continue;
            }

            cgi.R_SetStaticModelTransform(pd->index, pd->origin, gone);
            pd->hidden = qtrue;

            for (k = 0; k < WATER_BLOOD_MAX; k++) {
                if (cg_waterBlood[k].used && cg_waterBlood[k].placed && cg_waterBlood[k].body == -1 - pd->index) {
                    cg_waterBlood[k].used = qfalse;
                }
            }
            break;
        }
    }
}
