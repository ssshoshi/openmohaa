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
// goes through takes the hit, where it goes through, and breaks the way
// Half-Life 2's glass does (bg_glass.h, cgame cg_glass.cpp): a hole knocked
// out there with cracks round it, the glass no longer held by the frame
// falling out, until enough rounds bring the whole pane down. A pane's hits
// are kept on an entity of its own (EF_GLASS, bound to the vehicle), made at
// its first: from then the model's own pane is not drawn (nodraw) and cgame
// draws what is left of it. A round through a hole already there goes through
// nothing. A blast brings down the panes whose middle is within most of its
// radius. A map's func_window breaks the same way (windows.cpp), its hits on
// the window itself.
//
// A static model (models/static/, placed in the map's BSP) is the renderer's
// alone: nothing here can change it. Its .tik draws no glass of its own (its
// panes' skin0 is opm_glass_none); at level start a copy of it is put where it
// stands showing only its glass (G_GlassStaticModels), and that copy is hit.
// Its panes' skin1 is the pane whole.

#include "g_local.h"
#include "entity.h"
#include "animate.h"
#include "g_glass.h"
#include "g_phys.h"
#include "../corepp/tiki.h"
#include "../qcommon/qfiles.h"
#include "g_spawn.h"
#include "bg_glass.h"

#define GLASS_MAX_MODELS    48
#define GLASS_MAX_TIKIS     8   // .tik files on one model
#define GLASS_MAX_PANES     16  // on one model
#define GLASS_MAX_CACHE     128 // models' TIKIs looked up
#define GLASS_NEAR          768.0f // a model this near a bullet's path may have glass on it
#define GLASS_HOLE_PASS     2.5f   // a round this near a hole already there goes through it

typedef struct {
    char   surface[MAX_QPATH];
    vec3_t normal; // out of the vehicle
    vec3_t mid;
    float *tris;   // 9 a triangle
    int    numTris;
    glassFrame_t frame; // as cgame has it (bg_glass.h)
} glassPane_t;

typedef struct {
    char        tikis[GLASS_MAX_TIKIS][MAX_QPATH];
    qboolean    isStatic[GLASS_MAX_TIKIS];
    int         numTikis;
    glassPane_t panes[GLASS_MAX_PANES];
    int         numPanes;
} glassModel_t;

static glassModel_t glassModels[GLASS_MAX_MODELS];
static int          glassNumModels;
static qboolean     glassLoaded;

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
        BG_GlassFrameFromTris(p->normal, p->tris, p->numTris, &p->frame);
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

// A pane's skin: 0 whole, one on for a static model's copy
static void G_GlassSkin(Entity *ent, const char *surface, int skin)
{
    ent->SurfaceCommand(surface, (skin & 1) ? "+skin1" : "-skin1");
    ent->SurfaceCommand(surface, (skin & 2) ? "+skin2" : "-skin2");
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

// The entity keeping a vehicle's pane's hits, made at the pane's first when create
static Entity *G_GlassPaneEntity(Entity *owner, int pane, qboolean create)
{
    Entity *g;
    byte   *s;
    int     i;

    for (i = 0; i < globals.num_entities; i++) {
        gentity_t *ed = &g_entities[i];

        if (!ed->inuse || !ed->entity || !(ed->s.eFlags & EF_GLASS)) {
            continue;
        }
        s = ed->s.surfaces;
        if (s[GLASS_S_PANE] == pane && (s[GLASS_S_OWNER] | (s[GLASS_S_OWNER + 1] << 8)) == owner->entnum) {
            return ed->entity;
        }
    }
    if (!create) {
        return NULL;
    }

    g = new Entity;
    g->setSolidType(SOLID_NOT);
    g->setMoveType(MOVETYPE_NONE);
    g->setOrigin(owner->origin);
    g->setSize(owner->mins, owner->maxs); // seen wherever the vehicle is
    g->edict->r.svFlags |= SVF_SENDPVS;   // sent with no model: cgame draws the pane
    g->edict->s.eFlags |= EF_GLASS;
    s = g->edict->s.surfaces;
    memset(s, 0, sizeof(g->edict->s.surfaces));
    s[GLASS_S_PANE]      = pane;
    s[GLASS_S_OWNER]     = owner->entnum & 0xFF;
    s[GLASS_S_OWNER + 1] = (owner->entnum >> 8) & 0xFF;
    g->bind(owner);
    g->showModel();
    return g;
}

static qboolean G_GlassShattered(Entity *g)
{
    return g && (g->edict->s.surfaces[GLASS_S_FLAGS] & GLASS_FLAG_SHATTERED) ? qtrue : qfalse;
}

int G_GlassPaneShatter(Entity *g, int out, int blast, const Vector& at)
{
    byte *flags;

    if (!g || G_GlassShattered(g)) {
        return qfalse;
    }
    flags = &g->edict->s.surfaces[GLASS_S_FLAGS];
    *flags |= GLASS_FLAG_SHATTERED | (out ? GLASS_FLAG_OUT : 0) | (blast ? GLASS_FLAG_BLAST : 0);
    if (developer->integer) {
        gi.DPrintf("glass: entity %d's pane comes down%s\n", g->entnum, blast ? " (blast)" : "");
    }
    g->Sound(va("snd_glass_shatter%d", 1 + (rand() % 3)), CHAN_AUTO, -1, -1, (Vector *)&at);
    return qtrue;
}

int G_GlassPaneHit(Entity *g, const glassFrame_t *frame, const float *local, float damage, int out, const Vector& at)
{
    byte *s = g->edict->s.surfaces;
    int   count, i, size;
    float pu, pv, hu, hv;
    byte  bu, bv;

    if (G_GlassShattered(g)) {
        return 0;
    }
    count = s[GLASS_S_FLAGS] & GLASS_FLAG_COUNT;
    BG_GlassEncode(frame, local, &bu, &bv);
    BG_GlassDecode(frame, bu, bv, &pu, &pv);

    // through a hole already there: nothing to hit
    for (i = 0; i < count; i++) {
        const byte *h = &s[GLASS_S_HITS + i * 3];

        BG_GlassDecode(frame, h[0], h[1], &hu, &hv);
        if ((hu - pu) * (hu - pu) + (hv - pv) * (hv - pv) < GLASS_HOLE_PASS * GLASS_HOLE_PASS) {
            return 0;
        }
    }

    size = (int)(damage / 20.0f);
    size = size < 1 ? 1 : (size > 7 ? 7 : size);
    s[GLASS_S_HITS + count * 3]     = bu;
    s[GLASS_S_HITS + count * 3 + 1] = bv;
    s[GLASS_S_HITS + count * 3 + 2] = (rand() & GLASS_HIT_SEED) | (out ? GLASS_HIT_OUT : 0) | (size << GLASS_HIT_SIZE_SHIFT);
    count++;
    s[GLASS_S_FLAGS] = (s[GLASS_S_FLAGS] & ~GLASS_FLAG_COUNT) | count;

    if (developer->integer) {
        gi.DPrintf("glass: entity %d's pane hit %d (size %d)\n", g->entnum, count, size);
    }
    g->Sound(va("snd_bh_glass%d", 1 + (rand() % 4)), CHAN_AUTO, -1, -1, (Vector *)&at);
    if (count >= GLASS_MAX_HITS) {
        G_GlassPaneShatter(g, out, qfalse, at);
        return 2;
    }
    return 1;
}

static void G_GlassFall(Entity *ent, const glassModel_t *m, int p, qboolean out, qboolean blast)
{
    const glassPane_t *pane = &m->panes[p];
    Entity            *g    = G_GlassPaneEntity(ent, p, qtrue);

    if (!G_GlassShattered(g)) {
        ent->SurfaceCommand(pane->surface, "+nodraw");
        G_GlassPaneShatter(g, out, blast, G_GlassToWorld(ent, pane->mid));
    }
}

static void G_GlassHit(Entity *ent, const glassModel_t *m, int p, const vec3_t local, const vec3_t face, const Vector& dir, float damage)
{
    const glassPane_t *pane = &m->panes[p];
    Entity            *g    = G_GlassPaneEntity(ent, p, qtrue);
    Vector             at   = G_GlassToWorld(ent, local);
    Vector             n    = G_GlassDirToWorld(ent, pane->normal);

    // from now cgame draws the pane, as much of it as is left
    ent->SurfaceCommand(pane->surface, "+nodraw");
    if (G_GlassPaneHit(g, &pane->frame, local, damage, (n * dir) > 0 ? qtrue : qfalse, at)) {
        // the bits it knocks out, thrown on the way it went
        G_GlassEffect("models/fx/opm_glass_spray.tik", at, dir, 3.0f);
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

void G_GlassBulletPath(const Vector& start, const Vector& end, float damage)
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

            if (surf < 0 || G_GlassShattered(G_GlassPaneEntity(ent, p, qfalse))) {
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
    G_GlassHit(best, bestM, bestPane, bestLocal, bestFace, dir, damage);
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

        if (surf < 0) {
            continue;
        }
        const Vector at = G_GlassToWorld(ent, pane->mid);
        if ((at - origin).length() < radius * 0.75f) {
            const Vector n = G_GlassDirToWorld(ent, pane->normal);

            G_GlassFall(ent, m, i, ((at - origin) * n) > 0 ? qtrue : qfalse, qtrue);
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
