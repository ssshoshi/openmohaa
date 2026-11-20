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
// Panes of glass that bullets break.
//
// A pane is a surface of a model, named with its box (in the model's own
// units, as its .skd has it) and the way it faces out in opm/glass.txt
// (content/opm-lamp-states, made by tools/vehicles/opel_glass.py, which cut the
// Opel truck's glass into its panes). Any model with that surface has the pane.
//
// A bullet's path is tested against the panes of the models near it, whatever
// its trace struck (a truck's box is smaller than the truck, its windscreen
// outside it), and passes on: the glass never stops it. The first pane it
// crosses takes the hit, where it crosses the pane's plane:
//   each hit: a hole with a star of cracks round it (a decal, bound to the
//     model), small bits of glass thrown on the way the round went, into the cab;
//   the second: the pane cracked across (skin1); the third: crazed, milky (skin2);
//   the fourth: it comes down, shards falling out of the frame, which is left
//     empty (nodraw), its decals gone.
// A blast brings down the panes whose middle is within most of its radius.
// The skins and nodraw are the surface's, saved with the entity; the count of
// hits and the decals are not (after a load a cracked pane counts two hits).

#include "g_local.h"
#include "entity.h"
#include "animate.h"
#include "g_glass.h"
#include "g_phys.h"
#include "../corepp/tiki.h"

#define GLASS_MAX_PANES  32
#define GLASS_MAX_TRACKS 64  // models with hit glass followed at once
#define GLASS_MAX_DECALS 6   // holes on a pane, at most
#define GLASS_FALL       4   // the hit that brings a pane down
#define GLASS_NEAR       512.0f // a model this near a bullet's path may have glass on it
#define GLASS_PAD        4.0f   // the panes' boxes are this much bigger than their glass (opel_glass.py PAD)
#define GLASS_HOLE_SIZE  0.3f   // the crack decal's scale: about 4 units across (the game's unit square is not one unit)

typedef struct {
    char   surface[MAX_QPATH];
    vec3_t mins;
    vec3_t maxs;
    vec3_t normal; // out of the cab
} glassPane_t;

typedef struct {
    EntityPtr ent;
    int       hits[GLASS_MAX_PANES];
    EntityPtr decals[GLASS_MAX_PANES][GLASS_MAX_DECALS];
} glassTrack_t;

static glassPane_t  glassPanes[GLASS_MAX_PANES];
static int          glassNumPanes;
static qboolean     glassLoaded;
static glassTrack_t glassTracks[GLASS_MAX_TRACKS];

static void G_GlassLoad(void)
{
    union {
        char *c;
        void *v;
    } buffer;
    char       *text;
    const char *token;

    glassLoaded   = qtrue;
    glassNumPanes = 0;
    if (gi.FS_ReadFile("opm/glass.txt", &buffer.v, qtrue) <= 0) {
        return;
    }
    text = buffer.c;
    while (glassNumPanes < GLASS_MAX_PANES) {
        glassPane_t *pane = &glassPanes[glassNumPanes];
        int          k;

        token = COM_ParseExt(&text, qtrue);
        if (!token[0]) {
            break;
        }
        Q_strncpyz(pane->surface, token, sizeof(pane->surface));
        for (k = 0; k < 3; k++) {
            pane->mins[k] = atof(COM_ParseExt(&text, qfalse));
        }
        for (k = 0; k < 3; k++) {
            pane->maxs[k] = atof(COM_ParseExt(&text, qfalse));
        }
        for (k = 0; k < 3; k++) {
            pane->normal[k] = atof(COM_ParseExt(&text, qfalse));
        }
        if (VectorNormalize(pane->normal) <= 0) {
            VectorSet(pane->normal, 1, 0, 0);
        }
        glassNumPanes++;
    }
    gi.FS_FreeFile(buffer.v);
}

static float G_GlassScale(Entity *ent)
{
    return (ent->edict->s.scale > 0 ? ent->edict->s.scale : 1.0f)
         * (ent->edict->tiki && ent->edict->tiki->load_scale > 0 ? ent->edict->tiki->load_scale : 1.0f);
}

// A point in the world, in the model's own units
static void G_GlassToModel(Entity *ent, const Vector& p, vec3_t out)
{
    const float scale = G_GlassScale(ent);
    vec3_t      d;
    int         k;

    VectorSubtract(p, ent->origin, d);
    for (k = 0; k < 3; k++) {
        out[k] = DotProduct(d, ent->orientation[k]) / scale;
    }
}

static Vector G_GlassToWorld(Entity *ent, const vec3_t local)
{
    const float scale = G_GlassScale(ent);
    Vector      p     = ent->origin;
    int         k;

    for (k = 0; k < 3; k++) {
        p += Vector(ent->orientation[k]) * (local[k] * scale);
    }
    return p;
}

static Vector G_GlassDirToWorld(Entity *ent, const vec3_t local)
{
    Vector d = vec_zero;
    int    k;

    for (k = 0; k < 3; k++) {
        d += Vector(ent->orientation[k]) * local[k];
    }
    d.normalize();
    return d;
}

// Where along a segment from a to b (0..1) it first enters the box, or -1
static float G_GlassSegmentBox(const vec3_t a, const vec3_t b, const vec3_t mins, const vec3_t maxs)
{
    float enter = 0, leave = 1;
    int   k;

    for (k = 0; k < 3; k++) {
        const float d = b[k] - a[k];

        if (fabs(d) < 1e-6f) {
            if (a[k] < mins[k] || a[k] > maxs[k]) {
                return -1;
            }
            continue;
        }
        float t0 = (mins[k] - a[k]) / d;
        float t1 = (maxs[k] - a[k]) / d;
        if (t0 > t1) {
            const float t = t0;
            t0            = t1;
            t1            = t;
        }
        enter = Q_max(enter, t0);
        leave = Q_min(leave, t1);
        if (enter > leave) {
            return -1;
        }
    }
    return enter;
}

static qboolean G_GlassDown(Entity *ent, int surface)
{
    return (ent->edict->s.surfaces[surface] & MDL_SURFACE_NODRAW) ? qtrue : qfalse;
}

// The model's record of its panes' hits, made when first hit
static glassTrack_t *G_GlassTrack(Entity *ent)
{
    glassTrack_t *free = NULL;
    int           i, p;

    for (i = 0; i < GLASS_MAX_TRACKS; i++) {
        if (glassTracks[i].ent == ent) {
            return &glassTracks[i];
        }
        if (!free && !glassTracks[i].ent) {
            free = &glassTracks[i];
        }
    }
    if (!free) {
        free = &glassTracks[rand() % GLASS_MAX_TRACKS];
    }
    free->ent = ent;
    for (p = 0; p < GLASS_MAX_PANES; p++) {
        const int surf = p < glassNumPanes ? gi.Surface_NameToNum(ent->edict->tiki, glassPanes[p].surface) : -1;
        const int bits = surf >= 0 ? ent->edict->s.surfaces[surf] : 0;

        // as it was saved: crazed three hits, cracked two
        free->hits[p] = (bits & MDL_SURFACE_SKINOFFSET_BIT1) ? 3 : (bits & MDL_SURFACE_SKINOFFSET_BIT0) ? 2 : 0;
        for (i = 0; i < GLASS_MAX_DECALS; i++) {
            free->decals[p][i] = NULL;
        }
    }
    return free;
}

static Animate *G_GlassEffect(const char *model, const Vector& at, const Vector& forward, float life)
{
    Animate *fx = new Animate;
    Vector   angles;

    fx->setModel(model);
    fx->setOrigin(at);
    angles = forward.toAngles();
    fx->setAngles(angles);
    fx->setSolidType(SOLID_NOT);
    fx->setMoveType(MOVETYPE_NONE);
    // its emitters are idle's client enter commands, which run when the
    // client sees the animation change: it comes in on the one before, as the
    // game's own effects do (barrel_empty_destroyed.tik's "aaaa"), and goes to
    // idle a moment after; first seen already in idle, nothing was thrown
    fx->NewAnim("aaaa");
    Event *ev = new Event(EV_SetAnim);
    ev->AddString("idle");
    fx->PostEvent(ev, 0.1f);
    if (life > 0) {
        fx->PostEvent(EV_Remove, life);
    }
    return fx;
}

static void G_GlassFall(Entity *ent, glassTrack_t *track, int p)
{
    const glassPane_t *pane = &glassPanes[p];
    const int          surf = gi.Surface_NameToNum(ent->edict->tiki, pane->surface);
    vec3_t             mid;
    int                i;

    if (surf < 0 || G_GlassDown(ent, surf)) {
        return; // down already (a blast and a round together)
    }
    if (developer->integer) {
        gi.DPrintf("glass: entity %d's %s comes down\n", ent->entnum, pane->surface);
    }
    ent->SurfaceCommand(pane->surface, "-skin1");
    ent->SurfaceCommand(pane->surface, "-skin2");
    ent->SurfaceCommand(pane->surface, "+nodraw");
    if (track) {
        for (i = 0; i < GLASS_MAX_DECALS; i++) {
            if (track->decals[p][i]) {
                track->decals[p][i]->PostEvent(EV_Remove, 0);
                track->decals[p][i] = NULL;
            }
        }
        track->hits[p] = GLASS_FALL;
    }

    VectorAdd(pane->mins, pane->maxs, mid);
    VectorScale(mid, 0.5f, mid);
    Vector at = G_GlassToWorld(ent, mid);
    G_GlassEffect("models/fx/opm_glass_collapse.tik", at, G_GlassDirToWorld(ent, pane->normal), 3.0f);
    ent->Sound(va("snd_glass_shatter%d", 1 + (rand() % 3)), CHAN_AUTO, -1, -1, &at);
}

static void G_GlassHit(Entity *ent, int p, const vec3_t local, const Vector& dir)
{
    const glassPane_t *pane  = &glassPanes[p];
    glassTrack_t      *track = G_GlassTrack(ent);
    const int          hits  = ++track->hits[p];
    Vector             at    = G_GlassToWorld(ent, local);
    const Vector       out   = G_GlassDirToWorld(ent, pane->normal);
    int                i;

    if (hits >= GLASS_FALL) {
        G_GlassFall(ent, track, p);
        return;
    }
    if (developer->integer) {
        gi.DPrintf("glass: entity %d's %s hit %d\n", ent->entnum, pane->surface, hits);
    }

    // the hole and its star of cracks, lying on the pane, bound to the model
    for (i = 0; i < GLASS_MAX_DECALS; i++) {
        if (!track->decals[p][i]) {
            Animate *hole = new Animate;
            Vector   angles;

            hole->setModel(va("models/fx/opm_glasscrack%d.tik", rand() % 3));
            hole->setOrigin(at + out * 0.35f);
            angles    = out.toAngles();
            angles[2] = (float)(rand() % 360);
            hole->setAngles(angles);
            hole->setScale(GLASS_HOLE_SIZE * (0.8f + 0.4f * (rand() / (float)RAND_MAX)));
            hole->setSolidType(SOLID_NOT);
            hole->setMoveType(MOVETYPE_NONE);
            hole->NewAnim("idle");
            hole->bind(ent);
            track->decals[p][i] = hole;
            break;
        }
    }

    // the bits it knocks out, thrown on into the cab
    G_GlassEffect("models/fx/opm_glass_spray.tik", at, dir, 2.0f);
    ent->Sound(va("snd_bh_glass%d", 1 + (rand() % 4)), CHAN_AUTO, -1, -1, &at);

    if (hits == 2) {
        ent->SurfaceCommand(pane->surface, "+skin1");
    } else if (hits == 3) {
        ent->SurfaceCommand(pane->surface, "-skin1");
        ent->SurfaceCommand(pane->surface, "+skin2");
    }
}

// The first pane of ent's a segment passes, how far along it (0..1), or -1
static float G_GlassFirstPane(Entity *ent, const vec3_t a, const vec3_t b, int *hit)
{
    float best = -1;
    int   i;

    for (i = 0; i < glassNumPanes; i++) {
        const int surf = gi.Surface_NameToNum(ent->edict->tiki, glassPanes[i].surface);
        float     t;

        if (surf < 0 || G_GlassDown(ent, surf)) {
            continue;
        }
        t = G_GlassSegmentBox(a, b, glassPanes[i].mins, glassPanes[i].maxs);
        if (t >= 0 && (best < 0 || t < best)) {
            best = t;
            *hit = i;
        }
    }
    return best;
}

void G_GlassBulletPath(const Vector& start, const Vector& end)
{
    Entity      *best     = NULL;
    int          bestPane = -1;
    float        bestT    = 2.0f;
    vec3_t       bestA, bestB;
    const Vector seg  = end - start;
    const float  len2 = seg * seg;
    int          i, k;

    if (!glassLoaded) {
        G_GlassLoad();
    }
    if (!glassNumPanes || len2 < 1.0f) {
        return;
    }

    // the bullet's path, not what its trace struck: a truck's box (its TIKI's
    // setsize) is lower and shorter than the truck, its windscreen outside it,
    // and the clip brushes standing in for it are not it
    for (i = 0; i < globals.num_entities; i++) {
        gentity_t *ed = &g_entities[i];
        Entity    *ent;
        int        pane = -1;
        vec3_t     a, b;
        float      t;

        if (!ed->inuse || !ed->entity || !ed->tiki) {
            continue;
        }
        ent = ed->entity;
        {
            const Vector d = ent->origin - start;
            float        f = (d * seg) / len2;

            f = Q_max(0.0f, Q_min(1.0f, f));
            if ((ent->origin - (start + seg * f)).length() > GLASS_NEAR) {
                continue;
            }
        }
        G_GlassToModel(ent, start, a);
        G_GlassToModel(ent, end, b);
        t = G_GlassFirstPane(ent, a, b, &pane);
        if (t >= 0 && t < bestT) {
            bestT    = t;
            best     = ent;
            bestPane = pane;
            VectorCopy(a, bestA);
            VectorCopy(b, bestB);
        }
    }
    if (!best) {
        return;
    }

    // where the path crosses the glass itself (the plane through the middle of
    // its box, the way it faces), not where it enters the box round it
    const glassPane_t *pane = &glassPanes[bestPane];
    vec3_t             mid, local, d;
    float              t = bestT;

    VectorAdd(pane->mins, pane->maxs, mid);
    VectorScale(mid, 0.5f, mid);
    VectorSubtract(bestB, bestA, d);
    {
        const float dn = DotProduct(d, pane->normal);

        if (fabs(dn) > 1e-6f) {
            vec3_t am;

            VectorSubtract(mid, bestA, am);
            const float tp = DotProduct(am, pane->normal) / dn;
            if (tp >= 0 && tp <= 1) {
                t = tp;
            }
        }
    }
    for (k = 0; k < 3; k++) {
        local[k] = bestA[k] + d[k] * t;
        // where it crosses the plane, on the glass itself, not the frame or the
        // roof round it the box takes in
        if (local[k] < pane->mins[k] + GLASS_PAD - 0.5f || local[k] > pane->maxs[k] - GLASS_PAD + 0.5f) {
            return;
        }
    }
    Vector dir = seg;
    dir.normalize();
    G_GlassHit(best, bestPane, local, dir);
}

void G_GlassBlast(Entity *ent, const Vector& origin, float radius)
{
    int i;

    if (!glassLoaded) {
        G_GlassLoad();
    }
    if (!glassNumPanes || !ent || !ent->edict->tiki) {
        return;
    }

    for (i = 0; i < glassNumPanes; i++) {
        const glassPane_t *pane = &glassPanes[i];
        const int          surf = gi.Surface_NameToNum(ent->edict->tiki, pane->surface);
        vec3_t             mid;

        if (surf < 0 || G_GlassDown(ent, surf)) {
            continue;
        }
        VectorAdd(pane->mins, pane->maxs, mid);
        VectorScale(mid, 0.5f, mid);
        const Vector at = G_GlassToWorld(ent, mid);
        if ((at - origin).length() < radius * 0.75f) {
            glassTrack_t *track = NULL;
            int           t;

            for (t = 0; t < GLASS_MAX_TRACKS; t++) {
                if (glassTracks[t].ent == ent) {
                    track = &glassTracks[t];
                }
            }
            G_GlassFall(ent, track, i);
        }
    }
}
