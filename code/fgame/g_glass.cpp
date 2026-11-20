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
// Panes of glass that bullets break: every vehicle's windows.
//
// tools/vehicles/vehicle_glass.py cuts each vehicle model's glass into panes, a
// surface each, and writes opm/glass/<model>.txt (content/opm-lamp-states): the
// .tik files on the model, then each pane's surface, the way it faces out, and
// its triangles in the model's own units (as its .skd has them).
//
// A bullet's path is tested against the panes of the models near it, whatever
// its trace struck (a truck's box is smaller than the truck, its windscreen
// outside it), and passes on: the glass never stops it. The first pane it
// goes through takes the hit, where it goes through:
//   each hit: a hole with a star of cracks round it (a decal, bound to the
//     model), small bits of glass thrown on the way the round went;
//   the second: the pane cracked across; the third: crazed, milky;
//   the fourth: it comes down, shards falling out of the frame, which is left
//     empty (nodraw), its decals gone.
// A blast brings down the panes whose middle is within most of its radius.
// The skins and nodraw are the surface's, saved with the entity; the count of
// hits and the decals are not (after a load a cracked pane counts two hits).
//
// A static model (models/static/, placed in the map's BSP) is the renderer's
// alone: nothing here can change it. Its .tik draws no glass of its own (its
// panes' skin0 is opm_glass_none); at level start a copy of it is put where it
// stands showing only its glass (G_GlassStaticModels), and that copy is hit.
// Its panes' skins are one on: whole is skin1, cracked skin2, crazed skin3.

#include "g_local.h"
#include "entity.h"
#include "animate.h"
#include "g_glass.h"
#include "g_phys.h"
#include "../corepp/tiki.h"
#include "../qcommon/qfiles.h"
#include "g_spawn.h"

#define GLASS_MAX_MODELS    48
#define GLASS_MAX_TIKIS     8   // .tik files on one model
#define GLASS_MAX_PANES     16  // on one model
#define GLASS_MAX_TRACKS    64  // models with hit glass followed at once
#define GLASS_MAX_DECALS    6   // holes on a pane, at most
#define GLASS_MAX_CACHE     128 // models' TIKIs looked up
#define GLASS_FALL          4   // the hit that brings a pane down
#define GLASS_NEAR          768.0f // a model this near a bullet's path may have glass on it
#define GLASS_HOLE_SIZE     0.3f   // the crack decal's scale: about 4 units across (the game's unit square is not one unit)

typedef struct {
    char   surface[MAX_QPATH];
    vec3_t normal; // out of the vehicle
    vec3_t mid;
    float *tris;   // 9 a triangle
    int    numTris;
} glassPane_t;

typedef struct {
    char        tikis[GLASS_MAX_TIKIS][MAX_QPATH];
    qboolean    isStatic[GLASS_MAX_TIKIS];
    int         numTikis;
    glassPane_t panes[GLASS_MAX_PANES];
    int         numPanes;
} glassModel_t;

typedef struct {
    EntityPtr ent;
    int       hits[GLASS_MAX_PANES];
    EntityPtr decals[GLASS_MAX_PANES][GLASS_MAX_DECALS];
} glassTrack_t;

static glassModel_t glassModels[GLASS_MAX_MODELS];
static int          glassNumModels;
static qboolean     glassLoaded;
static glassTrack_t glassTracks[GLASS_MAX_TRACKS];

static struct {
    dtiki_t *tiki;
    short    model;
    short    skinBase;
} glassCache[GLASS_MAX_CACHE];
static int glassNumCache;

static void G_GlassLoadFile(const char *path)
{
    union {
        char *c;
        void *v;
    } buffer;
    glassModel_t *m;
    glassPane_t  *pane = NULL;
    char         *text;
    const char   *token;
    int           cap  = 0, k;

    if (glassNumModels >= GLASS_MAX_MODELS || gi.FS_ReadFile(path, &buffer.v, qtrue) <= 0) {
        return;
    }
    m = &glassModels[glassNumModels];
    memset(m, 0, sizeof(*m));

    text = buffer.c;
    while (1) {
        token = COM_ParseExt(&text, qtrue);
        if (!token[0]) {
            break;
        }
        if (!Q_stricmp(token, "tiki") || !Q_stricmp(token, "static")) {
            const qboolean isStatic = !Q_stricmp(token, "static");

            token = COM_ParseExt(&text, qfalse);
            if (m->numTikis < GLASS_MAX_TIKIS) {
                Q_strncpyz(m->tikis[m->numTikis], token, MAX_QPATH);
                m->isStatic[m->numTikis++] = isStatic;
            }
        } else if (!Q_stricmp(token, "pane")) {
            if (m->numPanes >= GLASS_MAX_PANES) {
                pane = NULL;
                SkipRestOfLine(&text);
                continue;
            }
            pane = &m->panes[m->numPanes++];
            Q_strncpyz(pane->surface, COM_ParseExt(&text, qfalse), sizeof(pane->surface));
            for (k = 0; k < 3; k++) {
                pane->normal[k] = atof(COM_ParseExt(&text, qfalse));
            }
            if (VectorNormalize(pane->normal) <= 0) {
                VectorSet(pane->normal, 1, 0, 0);
            }
            cap = 0;
        } else if (!Q_stricmp(token, "tri") && pane) {
            if (pane->numTris == cap) {
                float *more;

                cap  = cap ? cap * 2 : 16;
                more = (float *)gi.Malloc(cap * 9 * sizeof(float));
                if (pane->tris) {
                    memcpy(more, pane->tris, pane->numTris * 9 * sizeof(float));
                    gi.Free(pane->tris);
                }
                pane->tris = more;
            }
            for (k = 0; k < 9; k++) {
                pane->tris[pane->numTris * 9 + k] = atof(COM_ParseExt(&text, qfalse));
            }
            pane->numTris++;
        } else {
            SkipRestOfLine(&text);
        }
    }
    gi.FS_FreeFile(buffer.v);

    for (k = 0; k < m->numPanes; k++) {
        glassPane_t *p = &m->panes[k];
        int          i;

        VectorClear(p->mid);
        for (i = 0; i < p->numTris * 3; i++) {
            VectorAdd(p->mid, &p->tris[i * 3], p->mid);
        }
        if (p->numTris) {
            VectorScale(p->mid, 1.0f / (p->numTris * 3), p->mid);
        }
    }
    if (m->numPanes && m->numTikis) {
        glassNumModels++;
    }
}

static void G_GlassLoad(void)
{
    char **list;
    int    num = 0, i;

    glassLoaded    = qtrue;
    glassNumModels = 0;
    glassNumCache  = 0;
    list           = gi.FS_ListFiles("opm/glass", ".txt", qfalse, &num);
    for (i = 0; i < num; i++) {
        G_GlassLoadFile(va("opm/glass/%s", list[i]));
    }
    gi.FS_FreeFileList(list);
}

// The glass model of a TIKI (-1 for none), and the skin its panes are whole in
static int G_GlassModel(dtiki_t *tiki, int *skinBase)
{
    const char *name;
    int         i, k, found = -1, base = 0;

    if (!glassLoaded) {
        G_GlassLoad();
    }
    *skinBase = 0;
    if (!tiki || !glassNumModels) {
        return -1;
    }
    for (i = 0; i < glassNumCache; i++) {
        if (glassCache[i].tiki == tiki) {
            *skinBase = glassCache[i].skinBase;
            return glassCache[i].model;
        }
    }
    name = tiki->name;
    for (i = 0; i < glassNumModels && found < 0; i++) {
        for (k = 0; k < glassModels[i].numTikis; k++) {
            if (!Q_stricmp(name, glassModels[i].tikis[k])) {
                found = i;
                base  = glassModels[i].isStatic[k] ? 1 : 0;
                break;
            }
        }
    }
    if (glassNumCache < GLASS_MAX_CACHE) {
        glassCache[glassNumCache].tiki     = tiki;
        glassCache[glassNumCache].model    = found;
        glassCache[glassNumCache].skinBase = base;
        glassNumCache++;
    }
    *skinBase = base;
    return found;
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

static qboolean G_GlassDown(Entity *ent, int surface)
{
    return (ent->edict->s.surfaces[surface] & MDL_SURFACE_NODRAW) ? qtrue : qfalse;
}

// A pane's skin: 0 whole, 1 cracked, 2 crazed, one on for a static model's copy
static void G_GlassSkin(Entity *ent, const char *surface, int skin)
{
    ent->SurfaceCommand(surface, (skin & 1) ? "+skin1" : "-skin1");
    ent->SurfaceCommand(surface, (skin & 2) ? "+skin2" : "-skin2");
}

// The model's record of its panes' hits, made when first hit
static glassTrack_t *G_GlassTrack(Entity *ent, const glassModel_t *m, int skinBase)
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
        const int surf = p < m->numPanes ? gi.Surface_NameToNum(ent->edict->tiki, m->panes[p].surface) : -1;
        const int bits = surf >= 0 ? ent->edict->s.surfaces[surf] : 0;
        const int skin = ((bits & MDL_SURFACE_SKINOFFSET_BIT0) ? 1 : 0) | ((bits & MDL_SURFACE_SKINOFFSET_BIT1) ? 2 : 0);

        // as it was saved: crazed three hits, cracked two
        free->hits[p] = skin - skinBase >= 2 ? 3 : skin - skinBase == 1 ? 2 : 0;
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

static void G_GlassFall(Entity *ent, const glassModel_t *m, glassTrack_t *track, int p)
{
    const glassPane_t *pane = &m->panes[p];
    const int          surf = gi.Surface_NameToNum(ent->edict->tiki, pane->surface);
    int                i;

    if (surf < 0 || G_GlassDown(ent, surf)) {
        return; // down already (a blast and a round together)
    }
    if (developer->integer) {
        gi.DPrintf("glass: entity %d's %s comes down\n", ent->entnum, pane->surface);
    }
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

    Vector at = G_GlassToWorld(ent, pane->mid);
    G_GlassEffect("models/fx/opm_glass_collapse.tik", at, G_GlassDirToWorld(ent, pane->normal), 5.0f);
    ent->Sound(va("snd_glass_shatter%d", 1 + (rand() % 3)), CHAN_AUTO, -1, -1, &at);
}

static void G_GlassHit(Entity *ent, const glassModel_t *m, int skinBase, int p, const vec3_t local, const vec3_t face, const Vector& dir)
{
    const glassPane_t *pane  = &m->panes[p];
    glassTrack_t      *track = G_GlassTrack(ent, m, skinBase);
    const int          hits  = ++track->hits[p];
    Vector             at    = G_GlassToWorld(ent, local);
    Vector             out   = G_GlassDirToWorld(ent, face);
    int                i;

    if (hits >= GLASS_FALL) {
        G_GlassFall(ent, m, track, p);
        return;
    }
    if (developer->integer) {
        gi.DPrintf("glass: entity %d's %s hit %d\n", ent->entnum, pane->surface, hits);
    }

    // the hole and its star of cracks, lying on the pane where it was struck,
    // facing the shooter, bound to the model
    if (out * dir > 0) {
        out = out * -1;
    }
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

    // the bits it knocks out, thrown on the way it went
    G_GlassEffect("models/fx/opm_glass_spray.tik", at, dir, 3.0f);
    ent->Sound(va("snd_bh_glass%d", 1 + (rand() % 4)), CHAN_AUTO, -1, -1, &at);

    if (hits == 2 || hits == 3) {
        G_GlassSkin(ent, pane->surface, skinBase + hits - 1);
    }
}

// Where along a segment (from s along d, 0..1) it goes through a triangle, or -1
static float G_GlassSegmentTri(const vec3_t s, const vec3_t d, const float *t, vec3_t normal)
{
    vec3_t e1, e2, p, q, tv;
    float  det, inv, u, v, f;

    VectorSubtract(t + 3, t, e1);
    VectorSubtract(t + 6, t, e2);
    CrossProduct(d, e2, p);
    det = DotProduct(e1, p);
    if (fabs(det) < 1e-9f) {
        return -1;
    }
    inv = 1.0f / det;
    VectorSubtract(s, t, tv);
    u = DotProduct(tv, p) * inv;
    if (u < 0 || u > 1) {
        return -1;
    }
    CrossProduct(tv, e1, q);
    v = DotProduct(d, q) * inv;
    if (v < 0 || u + v > 1) {
        return -1;
    }
    f = DotProduct(e2, q) * inv;
    if (f < 0 || f > 1) {
        return -1;
    }
    CrossProduct(e1, e2, normal);
    VectorNormalize(normal);
    return f;
}

void G_GlassBulletPath(const Vector& start, const Vector& end)
{
    Entity             *best     = NULL;
    const glassModel_t *bestM    = NULL;
    int                 bestPane = -1, bestBase = 0;
    float               bestT    = 2.0f;
    vec3_t              bestLocal, bestFace;
    const Vector        seg  = end - start;
    const float         len2 = seg * seg;
    int                 i, p, t, k;

    if (!glassLoaded) {
        G_GlassLoad();
    }
    if (!glassNumModels || len2 < 1.0f) {
        return;
    }

    // the bullet's path, not what its trace struck: a truck's box (its TIKI's
    // setsize) is lower and shorter than the truck, its windscreen outside it,
    // and the clip brushes standing in for it are not it
    for (i = 0; i < globals.num_entities; i++) {
        gentity_t          *ed = &g_entities[i];
        Entity             *ent;
        const glassModel_t *m;
        int                 base, mi;
        vec3_t              a, b, d;

        if (!ed->inuse || !ed->entity || !ed->tiki) {
            continue;
        }
        mi = G_GlassModel(ed->tiki, &base);
        if (mi < 0) {
            continue;
        }
        ent = ed->entity;
        {
            const Vector o = ent->origin - start;
            float        f = (o * seg) / len2;

            f = Q_max(0.0f, Q_min(1.0f, f));
            if ((ent->origin - (start + seg * f)).length() > GLASS_NEAR) {
                continue;
            }
        }
        m = &glassModels[mi];
        G_GlassToModel(ent, start, a);
        G_GlassToModel(ent, end, b);
        VectorSubtract(b, a, d);
        for (p = 0; p < m->numPanes; p++) {
            const int surf = gi.Surface_NameToNum(ed->tiki, m->panes[p].surface);

            if (surf < 0 || G_GlassDown(ent, surf)) {
                continue;
            }
            for (t = 0; t < m->panes[p].numTris; t++) {
                vec3_t      n;
                const float f = G_GlassSegmentTri(a, d, &m->panes[p].tris[t * 9], n);

                if (f >= 0 && f < bestT) {
                    bestT    = f;
                    best     = ent;
                    bestM    = m;
                    bestPane = p;
                    bestBase = base;
                    VectorCopy(n, bestFace);
                    for (k = 0; k < 3; k++) {
                        bestLocal[k] = a[k] + d[k] * f;
                    }
                }
            }
        }
    }
    if (!best) {
        return;
    }

    Vector dir = seg;
    dir.normalize();
    G_GlassHit(best, bestM, bestBase, bestPane, bestLocal, bestFace, dir);
}

void G_GlassBlast(Entity *ent, const Vector& origin, float radius)
{
    const glassModel_t *m;
    int                 i, base, mi;

    if (!ent || !ent->edict->tiki) {
        return;
    }
    mi = G_GlassModel(ent->edict->tiki, &base);
    if (mi < 0) {
        return;
    }
    m = &glassModels[mi];

    for (i = 0; i < m->numPanes; i++) {
        const glassPane_t *pane = &m->panes[i];
        const int          surf = gi.Surface_NameToNum(ent->edict->tiki, pane->surface);

        if (surf < 0 || G_GlassDown(ent, surf)) {
            continue;
        }
        const Vector at = G_GlassToWorld(ent, pane->mid);
        if ((at - origin).length() < radius * 0.75f) {
            glassTrack_t *track = NULL;
            int           t;

            for (t = 0; t < GLASS_MAX_TRACKS; t++) {
                if (glassTracks[t].ent == ent) {
                    track = &glassTracks[t];
                }
            }
            G_GlassFall(ent, m, track, i);
        }
    }
}

void G_GlassStaticModels(const char *mapfile)
{
    union {
        byte *b;
        void *v;
    } buf;
    dheader_t             header;
    const lump_t         *lump;
    const cStaticModel_t *in;
    int                   len, count, i, k, spawned = 0;

    glassLoaded = qfalse; // the paks may be others than the last map's
    G_GlassLoad();
    if (!glassNumModels || !mapfile || !mapfile[0]) {
        return;
    }

    buf.v = NULL;
    len   = gi.FS_ReadFile(mapfile, &buf.v, qtrue);
    if (len < (int)sizeof(dheader_t) || !buf.v) {
        if (buf.v) {
            gi.FS_FreeFile(buf.v);
        }
        return;
    }
    memcpy(&header, buf.b, sizeof(header));
    for (i = 0; i < (int)(sizeof(dheader_t) / 4); i++) {
        ((int *)&header)[i] = LittleLong(((int *)&header)[i]);
    }
    lump  = Q_GetLumpByVersion(&header, LUMP_STATICMODELDEF);
    count = (lump->fileofs > 0 && lump->filelen > 0 && lump->fileofs + lump->filelen <= len)
              ? lump->filelen / (int)sizeof(cStaticModel_t)
              : 0;
    in    = (const cStaticModel_t *)(buf.b + lump->fileofs);

    for (i = 0; i < count; i++) {
        char      name[MAX_QPATH];
        char      raw[sizeof(in[i].model) + 1];
        int       m, j, w = 0;
        qboolean  found = qfalse;
        SpawnArgs args;
        Listener *l;

        memcpy(raw, in[i].model, sizeof(in[i].model));
        raw[sizeof(in[i].model)] = 0;
        // as a .tik's name: "static//vehicle_jeep.tik" is models/static/vehicle_jeep.tik
        if (Q_stricmpn(raw, "models/", 7)) {
            Q_strncpyz(name, "models/", sizeof(name));
            w = 7;
        }
        for (j = 0; raw[j] && w < MAX_QPATH - 1; j++) {
            if (raw[j] == '\\') {
                raw[j] = '/';
            }
            if (raw[j] == '/' && w > 0 && name[w - 1] == '/') {
                continue;
            }
            name[w++] = raw[j];
        }
        name[w] = 0;

        for (m = 0; m < glassNumModels && !found; m++) {
            for (k = 0; k < glassModels[m].numTikis; k++) {
                if (glassModels[m].isStatic[k] && !Q_stricmp(glassModels[m].tikis[k], name)) {
                    found = qtrue;
                    break;
                }
            }
        }
        if (!found) {
            continue;
        }

        args.setArg("classname", "script_model");
        args.setArg("model", name);
        args.setArg("origin", va("%f %f %f", in[i].origin[0], in[i].origin[1], in[i].origin[2]));
        args.setArg("angles", va("%f %f %f", in[i].angles[0], in[i].angles[1], in[i].angles[2]));
        args.setArg("scale", va("%f", in[i].scale > 0.0f ? in[i].scale : 1.0f));
        args.setArg("targetname", "opm_static_glass");

        l = args.Spawn();
        if (l && l->isSubclassOf(Entity)) {
            Entity  *e    = (Entity *)l;
            dtiki_t *tiki = e->edict->tiki;
            int      base, mi = G_GlassModel(tiki, &base), s;

            e->setSolidType(SOLID_NOT);
            if (mi < 0 || !tiki) {
                e->PostEvent(EV_Remove, 0);
                continue;
            }
            // its glass alone, whole (skin1): the map's own draws the rest
            for (s = 0; s < tiki->num_surfaces; s++) {
                const char *sname = gi.Surface_NumToName(tiki, s);
                qboolean    pane  = qfalse;

                for (k = 0; k < glassModels[mi].numPanes; k++) {
                    if (!Q_stricmp(sname, glassModels[mi].panes[k].surface)) {
                        pane = qtrue;
                    }
                }
                if (pane) {
                    G_GlassSkin(e, sname, base);
                } else {
                    e->SurfaceCommand(sname, "+nodraw");
                }
            }
            e->edict->s.renderfx &= ~RF_SHADOW; // the map's own casts its shadow
            spawned++;
        }
    }
    gi.FS_FreeFile(buf.v);

    if (spawned) {
        gi.DPrintf("%d of the map's vehicles have their glass to break\n", spawned);
    }
}
