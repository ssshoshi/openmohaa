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
// What lands on the camera's lens, as on the camera that went ashore with the
// first wave: water thrown up by a splash close by, which beads and slides
// down; blood from a man shot in front of it, which runs; dirt from a blast,
// which falls away. Each is a spot on the view, with how big it is and how
// much of it is left; the renderer draws the frame through them
// (RE_SetLensSpots, glsl/lens_fp.glsl).

#include "cg_lens.h"

cvar_t *cg_lens;
static cvar_t *cg_lens_test;
static cvar_t *cg_lens_debug;
static cvar_t *cg_lens_rain;

#define LENS_MAX 32 // as many as the renderer takes (WATER_MAX_SPOTS)

enum {
    LENS_WATER,
    LENS_BLOOD,
    LENS_DIRT
};

typedef struct {
    qboolean used;
    int      kind;
    float    x, y;  // across and up the view, 0 to 1
    float    size;  // a share of the view's height
    float    seed;
    float    run;   // how far it has run down, a share of the height
    float    slide; // how fast it slides once it does, heights a second
    float    amount;
    int      born;
    int      life; // ms
} cgLensSpot_t;

static cgLensSpot_t cg_lensSpots[LENS_MAX];
static int          cg_lensTime;
static int          cg_lensSent;

void CG_InitLens(void)
{
    cg_lens      = cgi.Cvar_Get("cg_lens", "1", CVAR_ARCHIVE);
    cg_lens_test = cgi.Cvar_Get("cg_lens_test", "0", CVAR_CHEAT);
    cg_lens_debug = cgi.Cvar_Get("cg_lens_debug", "0", 0);
    cg_lens_rain  = cgi.Cvar_Get("cg_lens_rain", "1", CVAR_ARCHIVE);
    // a new map is dry until it rains (CG_LensRain)
    cgi.Cvar_Set("r_rainWet", "0");
    memset(cg_lensSpots, 0, sizeof(cg_lensSpots));
    cg_lensTime = 0;
    cg_lensSent = 0;
}

// A new spot: in a free place, else in the place of the one with least left
static cgLensSpot_t *CG_LensNew(int kind, float x, float y, float size, int life, float amount)
{
    cgLensSpot_t *s = NULL;
    float         least = 2.0f;
    int           i;

    for (i = 0; i < LENS_MAX; i++) {
        cgLensSpot_t *t = &cg_lensSpots[i];
        float         left;

        if (!t->used) {
            s = t;
            break;
        }
        left = t->amount * (1.0f - (float)(cg.time - t->born) / t->life);
        if (left < least) {
            least = left;
            s     = t;
        }
    }

    s->used   = qtrue;
    s->kind   = kind;
    s->x      = x;
    s->y      = y;
    s->size   = size;
    s->seed   = random() * 100.0f;
    s->run    = 0.0f;
    s->slide  = 0.0f;
    s->amount = amount;
    s->born   = cg.time;
    s->life   = life;
    return s;
}

// Where a point is in the view, 0 to 1 across and up; qfalse behind it
static qboolean CG_LensProject(const vec3_t at, float *x, float *y, float *dist)
{
    vec3_t d;
    float  ahead, tx, ty;

    VectorSubtract(at, cg.refdef.vieworg, d);
    *dist = VectorLength(d);
    ahead = DotProduct(d, cg.refdef.viewaxis[0]);
    if (ahead <= 1.0f) {
        return qfalse;
    }
    tx = tan(DEG2RAD(cg.refdef.fov_x) * 0.5f);
    ty = tan(DEG2RAD(cg.refdef.fov_y) * 0.5f);
    *x = 0.5f - 0.5f * DotProduct(d, cg.refdef.viewaxis[1]) / (ahead * tx);
    *y = 0.5f + 0.5f * DotProduct(d, cg.refdef.viewaxis[2]) / (ahead * ty);
    return qtrue;
}

static qboolean CG_LensOn(void)
{
    return cg_lens && cg_lens->integer && cgi.apiversion >= 7 && cgi.R_SetLensSpots;
}

// So much water on the lens: drops all over it, bigger the more
static void CG_LensWater(float amount)
{
    int n = (int)(3.0f + amount * 20.0f), i;

    if (cg_lens_debug->integer) {
        cgi.Printf("lens %d: water %.2f\n", cg.time, amount);
    }

    for (i = 0; i < n; i++) {
        cgLensSpot_t *s = CG_LensNew(
            LENS_WATER, 0.04f + random() * 0.92f, 0.08f + random() * 0.9f, (0.005f + random() * random() * 0.03f) * (0.7f + amount),
            2500 + (int)(random() * 2500), 0.6f + 0.4f * amount
        );

        // the big ones run, the small ones stay where they are
        s->slide = s->size > 0.018f ? 0.03f + random() * 0.1f : 0.0f;
    }
}

void CG_LensSplash(const vec3_t at, float size)
{
    float x, y, dist, reach, amount;

    if (!CG_LensOn()) {
        return;
    }

    // a round's splash reaches a man standing over it, a shell's a good way
    reach = size * 6.0f + 40.0f;
    if (!CG_LensProject(at, &x, &y, &dist)) {
        // behind, it reaches only when right by him
        if (Distance(at, cg.refdef.vieworg) > reach * 0.3f) {
            return;
        }
        dist = Distance(at, cg.refdef.vieworg);
    }
    if (dist > reach) {
        return;
    }

    amount = (1.0f - dist / reach) * Com_Clamp(0.2f, 1.0f, size / 40.0f + 0.3f);
    CG_LensWater(amount);
}

void CG_LensSpray(const vec3_t at)
{
    const float reach = 220.0f;
    float       dist;

    if (!CG_LensOn()) {
        return;
    }
    dist = Distance(at, cg.refdef.vieworg);
    if (dist < reach) {
        CG_LensWater(0.25f + 0.55f * (1.0f - dist / reach));
    }
}

void CG_LensFleshHit(const vec3_t at)
{
    const float reach = 170.0f;
    float       x, y, dist, amount;
    int         n, i;

    if (!CG_LensOn() || !CG_LensProject(at, &x, &y, &dist) || dist > reach || x < -0.2f || x > 1.2f) {
        return;
    }

    // thrown at the lens from where he was hit: most of it round there,
    // some of it right across
    amount = 1.0f - dist / reach;
    if (cg_lens_debug->integer) {
        cgi.Printf("lens %d: blood %.2f, %.0f away\n", cg.time, amount, dist);
    }
    n      = (int)(1.0f + amount * 6.0f);
    for (i = 0; i < n; i++) {
        const float spread = i < n / 2 ? 0.18f : 0.5f;
        float       sx     = Com_Clamp(0.03f, 0.97f, x + crandom() * spread);
        float       sy     = Com_Clamp(0.05f, 0.97f, y + crandom() * spread);
        cgLensSpot_t *s = CG_LensNew(
            LENS_BLOOD, sx, sy, (0.012f + random() * 0.045f) * (0.6f + amount), 5000 + (int)(random() * 3000),
            0.75f + 0.25f * amount
        );

        s->slide = 0.006f + random() * 0.018f;
    }
}

void CG_LensBlast(const vec3_t at, int kind, qboolean inWater)
{
    static const float reaches[] = {500.0f, 650.0f, 800.0f, 800.0f};
    float              reach     = reaches[kind < 0 ? 0 : kind > 3 ? 3 : kind];
    float              x, y, dist, amount;
    int                n, i;

    if (!CG_LensOn()) {
        return;
    }
    if (!CG_LensProject(at, &x, &y, &dist)) {
        // behind him: it still showers him, if it is close
        dist = Distance(at, cg.refdef.vieworg);
        if (dist > reach * 0.4f) {
            return;
        }
    }
    if (dist > reach) {
        return;
    }

    amount = 1.0f - dist / reach;
    if (inWater) {
        CG_LensWater(amount);
        return;
    }

    // clods and grit thrown over it, most of it round the edges
    n = (int)(3.0f + amount * 12.0f);
    if (cg_lens_debug->integer) {
        cgi.Printf("lens %d: dirt %.2f, %.0f away\n", cg.time, amount, dist);
    }
    for (i = 0; i < n; i++) {
        float sx = random(), sy = random();

        if (random() < 0.6f) {
            if (random() < 0.5f) {
                sx = random() < 0.5f ? random() * 0.2f : 1.0f - random() * 0.2f;
            } else {
                sy = random() < 0.5f ? random() * 0.2f : 1.0f - random() * 0.2f;
            }
        }
        CG_LensNew(LENS_DIRT, sx, sy, (0.03f + random() * 0.08f) * (0.6f + amount), 1800 + (int)(random() * 1400),
            0.7f + 0.3f * amount);
    }
}

/*
The rain. Once it has rained on a map, what is out under the sky is wet for
the rest of it (the renderer's r_rainWet, tr_water.c): the map's weather
script turns the rain down indoors, and the world does not dry as a man goes
in. Out under the sky in the rain, a drop lands on the lens now and then, the
more the higher he looks: looking straight up, a dozen and more a second,
bigger, and many running down.
*/
static void CG_LensRain(float dt)
{
    static int      lastLook;
    static qboolean open;
    float           up, rate;

    if (cg.rain.density <= 0.0f) {
        return;
    }
    if (cgi.Cvar_Get("r_rainWet", "0", CVAR_TEMP)->value < 1.0f) {
        cgi.Cvar_Set("r_rainWet", "1");
    }
    if (!CG_LensOn() || !cg_lens_rain->value || dt <= 0.0f || !cg.snap) {
        return;
    }

    // nothing over him but the sky
    if (cg.time - lastLook > 250 || cg.time < lastLook) {
        trace_t tr;
        vec3_t  end;

        lastLook = cg.time;
        VectorCopy(cg.refdef.vieworg, end);
        end[2] += 4096.0f;
        CG_Trace(&tr, cg.refdef.vieworg, vec3_origin, vec3_origin, end, cg.snap->ps.clientNum, MASK_SOLID, qfalse, qtrue, "lens rain");
        open = (tr.fraction >= 1.0f || (tr.surfaceFlags & SURF_SKY)) ? qtrue : qfalse;
    }
    if (!open) {
        return;
    }

    // drops a second: a few looking ahead, many looking up, hardly any down;
    // the heavier the rain the more (its density runs from under 1 to 10)
    up   = Q_max(0.0f, cg.refdef.viewaxis[0][2] + 0.25f);
    rate = cg_lens_rain->value * (0.2f + 7.0f * up * up * up)
         * Com_Clamp(0.5f, 2.0f, 0.6f + 0.15f * cg.rain.density);
    // several a frame when it comes to that
    for (float left = rate * dt; left > 0.0f; left -= 1.0f) {
        cgLensSpot_t *s;
        float         size;

        if (left < 1.0f && random() >= left) {
            break;
        }
        // bigger ones looking up, as they gather and run together
        size = 0.007f + random() * random() * (0.02f + 0.02f * Q_min(1.0f, up));
        s    = CG_LensNew(
            LENS_WATER, 0.05f + random() * 0.9f, 0.1f + random() * 0.85f, size, 2200 + (int)(random() * 2500),
            0.75f + 0.25f * random()
        );
        s->slide = s->size > 0.014f ? 0.02f + random() * (0.06f + 0.08f * Q_min(1.0f, up)) : 0.0f;
    }
}

void CG_LensFrame(void)
{
    float out[LENS_MAX][8];
    float dt;
    int   n = 0, i;

    dt          = cg_lensTime ? (cg.time - cg_lensTime) * 0.001f : 0.0f;
    cg_lensTime = cg.time;
    if (dt < 0.0f || dt > 0.25f) {
        dt = 0.0f;
    }

    CG_LensRain(dt);
    if (!CG_LensOn()) {
        return;
    }

    // for working on it: 1 water, 2 blood, 3 dirt, as if right in front
    if (cg_lens_test->integer) {
        vec3_t at;

        VectorMA(cg.refdef.vieworg, 40.0f, cg.refdef.viewaxis[0], at);
        switch (cg_lens_test->integer) {
        case 1:
            CG_LensWater(1.0f);
            break;
        case 2:
            CG_LensFleshHit(at);
            break;
        default:
            CG_LensBlast(at, 2, qfalse);
            break;
        }
        cgi.Cvar_Set("cg_lens_test", "0");
    }

    for (i = 0; i < LENS_MAX; i++) {
        cgLensSpot_t *s = &cg_lensSpots[i];
        float         age, left;

        if (!s->used) {
            continue;
        }
        age = (float)(cg.time - s->born);
        if (age < 0.0f || age > s->life) {
            s->used = qfalse;
            continue;
        }

        // the last of it going
        left = s->amount * (1.0f - Q_max(0.0f, (age - s->life * 0.6f) / (s->life * 0.4f)));

        // it runs down, once it has gathered
        if (age > 400.0f && s->slide > 0.0f) {
            const float down = s->slide * dt;

            if (s->kind == LENS_WATER) {
                s->y -= down;
                s->run = Q_min(0.35f, s->run + down);
                s->size *= 1.0f - 0.08f * dt;
            } else {
                s->run = Q_min(0.3f, s->run + down);
            }
        }

        out[n][0] = s->x;
        out[n][1] = s->y;
        out[n][2] = s->size;
        out[n][3] = left;
        out[n][4] = (float)s->kind;
        out[n][5] = s->seed;
        out[n][6] = s->run;
        out[n][7] = 0.0f;
        n++;
    }

    if (n || cg_lensSent) {
        cgi.R_SetLensSpots(&out[0][0], n);
    }
    cg_lensSent = n;
}
