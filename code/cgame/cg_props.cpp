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
// The map's props: its static models, with collision the engine never gave
// them.
//
// The furniture a map is dressed with -- cots, shelves, tables, crates -- is
// mostly static models, and a static model has no collision at all: the world
// the traces see is the brushes, and a prop is solid to a player only where
// the mapper put a clip brush round it. Most have none. In a room full of them
// a body dropped on a cot lay on the floor through it, its hips poking up out
// of the canvas.
//
// So each one is given a box, fitted to its drawn mesh in its own frame and
// turned with it, and the ragdoll's traces run against those as well as the
// world: the hull a Half-Life 2 prop would carry, simplified to one box. A
// table is then solid underneath as well as on top and a shelf in front of its
// shelves, which is the right way to be wrong: a body rests on it or against
// it, and never ends up inside it.
//
// Left out: foliage, lights and wire (see CG_PropSkipped); things too
// small to matter under a body; and anything so big a box could seal off a
// room.

#include "cg_props.h"
#include "cg_ragdoll.h"
#include "cg_physics.h"
#include "../qcommon/qfiles.h"


#define CG_PROP_MIN_SIZE      6.0f
#define CG_PROP_MAX_SIZE      320.0f

// The largest a prop may be, in units along its longest side, and still be
// knocked about: bottles, helmets, buckets, chairs, crates. Bookcases, bunks,
// desks and sandbags stay where they are.
#define CG_PROP_DYNAMIC_SIZE  80.0f

cgProp_t cg_props[CG_MAX_PROPS];
int      cg_numProps;

static char     cg_propsMap[MAX_QPATH];
static qboolean cg_propsLoaded;

// Scratch for skinning one prop at a time.
#define CG_PROP_SKIN_MAX_VERTS 8192
#define CG_PROP_SKIN_MAX_TRIS  12288

static skinnedVert_t cg_propVerts[CG_PROP_SKIN_MAX_VERTS];
static int           cg_propTris[CG_PROP_SKIN_MAX_TRIS * 3];


// The mesh's outermost point along each of 26 directions (the faces, edges and
// corners of a cube), for a hull that hugs the model. The vertices are already
// in the model's own frame.
static void CG_PropHull(cgProp_t *out, int numVerts)
{
    int d, i, k;

    out->numHull = 0;

    for (d = 0; d < 27; d++) {
        const float dir[3] = {(float)(d % 3) - 1.0f, (float)((d / 3) % 3) - 1.0f, (float)(d / 9) - 1.0f};
        float       best   = -1e30f;
        int         which  = -1;

        if (d == 13) {
            continue; // the centre, which is no direction
        }

        for (i = 0; i < numVerts; i++) {
            const float along = DotProduct(cg_propVerts[i].xyz, dir);

            if (along > best) {
                best  = along;
                which = i;
            }
        }

        if (which < 0) {
            continue;
        }

        for (i = 0; i < out->numHull; i++) {
            if (Distance(out->hull[i], cg_propVerts[which].xyz) < 0.1f) {
                break;
            }
        }

        if (i == out->numHull && out->numHull < CG_PROP_MAX_HULL) {
            for (k = 0; k < 3; k++) {
                out->hull[out->numHull][k] = cg_propVerts[which].xyz[k];
            }
            out->numHull++;
        }
    }
}

// The world box round the turned one.
static void CG_PropUpdateBox(cgProp_t *out)
{
    int i, k;

    ClearBounds(out->absmin, out->absmax);
    for (i = 0; i < 8; i++) {
        vec3_t corner, world;

        corner[0] = (i & 1) ? out->maxs[0] : out->mins[0];
        corner[1] = (i & 2) ? out->maxs[1] : out->mins[1];
        corner[2] = (i & 4) ? out->maxs[2] : out->mins[2];

        VectorCopy(out->origin, world);
        for (k = 0; k < 3; k++) {
            VectorMA(world, corner[k], out->axis[k], world);
        }
        AddPointToBounds(world, out->absmin, out->absmax);
    }
}

void CG_PropSetPose(int index, const vec3_t origin, const vec3_t axis[3])
{
    cgProp_t *p;

    if (index < 0 || index >= cg_numProps) {
        return;
    }

    p = &cg_props[index];
    VectorCopy(origin, p->origin);
    AxisCopy(axis, p->axis);
    MatrixToEulerAngles((const float(*)[3])axis, p->angles);
    CG_PropUpdateBox(p);
}

void CG_PropsReset(void)
{
    cg_propsLoaded = qfalse;
    cg_numProps    = 0;
}

// Whether a static model is left without a box, by its name: foliage, whose
// box would be an invisible wall round a bush; the lights, which on a map such
// as m1l2a are nearly three hundred of its five hundred static models, most of
// them coronas and bulbs hanging in the air; and wire, whose box would be a
// solid block where there is mostly nothing.
qboolean CG_PropSkipped(const char *name)
{
    static const char *words[] = {"tree",   "bush",  "plant", "grass",  "foliage", "palm",   "vine",  "ivy",
                                  "fern",   "weed",  "flower", "hedge", "leaf",    "leaves", "branch", "shrub",
                                  "reed",   "cactus", "corona", "flare", "light",  "lamp",   "bulb",  "wire"};
    char   lower[128];
    size_t i;

    Q_strncpyz(lower, name, sizeof(lower));
    Q_strlwr(lower);

    // A lantern stands on something and is carried about, unlike the lamps
    // and lights fixed to walls and ceilings.
    if (strstr(lower, "lantern")) {
        return qfalse;
    }

    for (i = 0; i < ARRAY_LEN(words); i++) {
        if (strstr(lower, words[i])) {
            return qtrue;
        }
    }

    return qfalse;
}

// Added in OPM
//  Whether a static model is foliage, by its name.
static qboolean CG_PropFoliage(const char *name)
{
    static const char *words[] = {"tree", "bush", "plant", "foliage", "palm", "vine", "ivy", "fern",
                                  "hedge", "leaf", "leaves", "branch", "shrub"};
    char   lower[128];
    size_t i;

    Q_strncpyz(lower, name, sizeof(lower));
    Q_strlwr(lower);
    for (i = 0; i < ARRAY_LEN(words); i++) {
        if (strstr(lower, words[i])) {
            return qtrue;
        }
    }
    return qfalse;
}

/*
====================
CG_PropsFoliageNear

Added in OPM
  The map's foliage within radius of pos: a point in the upper part of each
  one's box, in the world, and how near it is (1 at pos, 0 at radius). For an
  explosion's falling leaves (cg_blast.cpp).
====================
*/
int CG_PropsFoliageNear(const vec3_t pos, float radius, vec3_t *points, float *near, int max)
{
    int i, k, n = 0;

    CG_PropsLoad();

    for (i = 0; i < cg_numProps && n < max; i++) {
        const cgProp_t *p = &cg_props[i];
        vec3_t          local, world;
        float           size, dist;

        if (pos[0] < p->absmin[0] - radius || pos[0] > p->absmax[0] + radius || pos[1] < p->absmin[1] - radius
            || pos[1] > p->absmax[1] + radius || pos[2] < p->absmin[2] - radius || pos[2] > p->absmax[2] + radius) {
            continue;
        }
        if (!CG_PropFoliage(p->name)) {
            continue;
        }

        for (k = 0; k < 3; k++) {
            local[k] = p->mins[k] + (p->maxs[k] - p->mins[k]) * (k == 2 ? 0.55f + 0.4f * random() : random());
        }
        VectorCopy(p->origin, world);
        for (k = 0; k < 3; k++) {
            VectorMA(world, local[k], p->axis[k], world);
        }

        size = 0.5f * Distance(p->absmin, p->absmax);
        dist = Distance(world, pos) - size * 0.5f;
        if (dist >= radius) {
            continue;
        }

        VectorCopy(world, points[n]);
        near[n] = 1.0f - Q_max(0.0f, dist) / radius;
        n++;
    }

    return n;
}

// The box round a static model, in its own frame: from its skinned mesh, or
// from the model's own bounds if the renderer cannot skin it.
static qboolean CG_PropFit(const cStaticModel_t *in, const char *path, cgProp_t *out)
{
    refEntity_t ent;
    qhandle_t   h;
    dtiki_t    *tiki;
    vec3_t      axis[3];
    float       scale;
    int         i, k, n = 0;

    h = cgi.R_RegisterModel(path);
    if (!h) {
        return qfalse;
    }

    tiki = cgi.R_Model_GetHandle(h);
    if (!tiki || !tiki->a) {
        return qfalse;
    }

    for (k = 0; k < 3; k++) {
        out->origin[k] = LittleFloat(in->origin[k]);
        out->angles[k] = LittleFloat(in->angles[k]);
    }
    scale = LittleFloat(in->scale);
    out->hModel = h;
    out->scale  = scale;
    AnglesToAxis(out->angles, axis);
    AxisCopy(axis, out->axis);

    ClearBounds(out->mins, out->maxs);

    if (cgi.apiversion >= 4 && cgi.R_GetSkinnedMesh) {
        memset(&ent, 0, sizeof(ent));
        ent.reType       = RT_MODEL;
        ent.hModel       = h;
        ent.tiki         = tiki;
        ent.scale        = scale;
        ent.entityNumber = ENTITYNUM_NONE;
        // Posed as the renderer poses a static model, on the first frame of
        // its first animation (TIKI_GetSkelAnimFrame). With no animation at
        // all it came out in its bind pose, turned a quarter over: a chair's
        // box lay on its side and a deck of cards stood on its edge.
        ent.frameInfo[0].index  = 0;
        ent.frameInfo[0].time   = 0.0f;
        ent.frameInfo[0].weight = 1.0f;
        ent.actionWeight        = 1.0f;
        VectorCopy(out->origin, ent.origin);
        VectorCopy(out->origin, ent.lightingOrigin);
        AxisCopy(axis, ent.axis);

        n = cgi.R_GetSkinnedMesh(&ent, cg_propVerts, CG_PROP_SKIN_MAX_VERTS, cg_propTris, CG_PROP_SKIN_MAX_TRIS, &i);

        for (i = 0; i < n; i++) {
            vec3_t d;

            VectorSubtract(cg_propVerts[i].xyz, out->origin, d);
            for (k = 0; k < 3; k++) {
                cg_propVerts[i].xyz[k] = DotProduct(d, axis[k]);
            }
            AddPointToBounds(cg_propVerts[i].xyz, out->mins, out->maxs);
        }

        CG_PropHull(out, n);
    }

    if (n <= 0) {
        for (k = 0; k < 3; k++) {
            out->mins[k] = tiki->a->mins[k] * tiki->load_scale * scale;
            out->maxs[k] = tiki->a->maxs[k] * tiki->load_scale * scale;
        }

        // The box's corners stand in for the hull.
        out->numHull = 8;
        for (i = 0; i < 8; i++) {
            out->hull[i][0] = (i & 1) ? out->maxs[0] : out->mins[0];
            out->hull[i][1] = (i & 2) ? out->maxs[1] : out->mins[1];
            out->hull[i][2] = (i & 4) ? out->maxs[2] : out->mins[2];
        }
    }

    CG_PropUpdateBox(out);

    return qtrue;
}

// Added in OPM
//  Whether a lamp hangs: a light of some kind (CG_PropSkipped leaves the lights
//  out, lanterns in) whose top is against something solid above it -- a bulb on
//  a wire, a lantern on a chain. Where it hangs from is the top of its box, in
//  the middle.
#define CG_PROP_HANG_REACH 8.0f
// the least it hangs below where it hangs from: less is a fitting on the ceiling
#define CG_PROP_HANG_DROP  16.0f

static qboolean CG_PropHangs(const char *name, cgProp_t *p)
{
    static const char *lamps[] = {"light", "lamp", "bulb", "lantern"};
    static const char *never[] = {"corona", "flare", "fx/"};
    char               lower[128];
    vec3_t             top, start, end;
    trace_t            tr;
    size_t             i;
    int                k;

    Q_strncpyz(lower, name, sizeof(lower));
    Q_strlwr(lower);
    for (i = 0; i < ARRAY_LEN(never); i++) {
        if (strstr(lower, never[i])) {
            return qfalse;
        }
    }
    for (i = 0; i < ARRAY_LEN(lamps); i++) {
        if (strstr(lower, lamps[i])) {
            break;
        }
    }
    if (i == ARRAY_LEN(lamps)) {
        return qfalse;
    }
    // hanging straight down
    if (p->axis[2][2] < 0.9f) {
        return qfalse;
    }

    // Up its middle from just above its bottom: where that meets the ceiling,
    // within a little of its top, is where it hangs from. A lamp the mapper
    // sank a way into the ceiling hangs from where its wire goes in.
    {
        vec3_t local;

        local[0] = (p->mins[0] + p->maxs[0]) * 0.5f;
        local[1] = (p->mins[1] + p->maxs[1]) * 0.5f;
        local[2] = p->mins[2] + 1.0f;
        VectorCopy(p->origin, start);
        for (k = 0; k < 3; k++) {
            VectorMA(start, local[k], p->axis[k], start);
        }
        VectorCopy(start, end);
        end[2] += p->maxs[2] - local[2] + CG_PROP_HANG_REACH;
    }
    cgi.CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);

    if (cg_physics_log->integer > 1) {
        cgi.Printf("props: %s: bottom at %.0f %.0f %.0f, box z %.0f to %.0f: %s\n", name, start[0], start[1], start[2], p->mins[2], p->maxs[2],
                   tr.startsolid ? "starts in the world" : tr.fraction < 1.0f ? va("hangs from %.0f", tr.endpos[2]) : "nothing above");
    }
    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f) {
        return qfalse;
    }
    // something below where it hangs to swing
    if (tr.endpos[2] - start[2] < CG_PROP_HANG_DROP) {
        return qfalse;
    }

    VectorSubtract(tr.endpos, p->origin, top);
    for (k = 0; k < 3; k++) {
        p->hangPoint[k] = DotProduct(top, p->axis[k]);
    }
    return qtrue;
}

// Added in OPM
//  The box round a model's mesh in its own frame, at its own scale.
static qboolean CG_PropModelBox(const char *path, qhandle_t *h, vec3_t mins, vec3_t maxs)
{
    refEntity_t ent;
    dtiki_t    *tiki;
    int         i, n = 0, k;

    *h = cgi.R_RegisterModel(path);
    tiki = *h ? cgi.R_Model_GetHandle(*h) : NULL;
    if (!tiki || !tiki->a) {
        return qfalse;
    }

    ClearBounds(mins, maxs);
    if (cgi.apiversion >= 4 && cgi.R_GetSkinnedMesh) {
        memset(&ent, 0, sizeof(ent));
        ent.reType              = RT_MODEL;
        ent.hModel              = *h;
        ent.tiki                = tiki;
        ent.scale               = 1.0f;
        ent.entityNumber        = ENTITYNUM_NONE;
        ent.frameInfo[0].weight = 1.0f;
        ent.actionWeight        = 1.0f;
        AxisClear(ent.axis);

        n = cgi.R_GetSkinnedMesh(&ent, cg_propVerts, CG_PROP_SKIN_MAX_VERTS, cg_propTris, CG_PROP_SKIN_MAX_TRIS, &i);
        for (i = 0; i < n; i++) {
            AddPointToBounds(cg_propVerts[i].xyz, mins, maxs);
        }
    }
    if (n <= 0) {
        for (k = 0; k < 3; k++) {
            mins[k] = tiki->a->mins[k] * tiki->load_scale;
            maxs[k] = tiki->a->maxs[k] * tiki->load_scale;
        }
    }
    return maxs[0] > mins[0] ? qtrue : qfalse;
}

// Added in OPM
//  Whether a lamp hangs on a cord: a bulb on a wire, and in single player the
//  shaded lamps (lightbulb_covered), which most of the campaign's rooms have.
//  The wire modelled with it goes: it is drawn as the same lamp with no wire,
//  as wide as the one placed and with its bottom where that one's was, and a
//  cord from its top up to what is above it, which the physics lets go slack.
//  One with nothing solid above it within reach, or too little room under it
//  to swing, stays as it is.
#define CG_PROP_CORD_MIN   12.0f
#define CG_PROP_CORD_REACH 384.0f

static qboolean CG_PropCorded(const char *name, cgProp_t *p)
{
    char        lower[128];
    const char *bare = NULL;
    qboolean    wire;
    vec3_t      top, start, end;
    trace_t     tr;
    float       reach;
    int         k;

    Q_strncpyz(lower, name, sizeof(lower));
    Q_strlwr(lower);
    if (!strstr(lower, "lightbulb_")) {
        return qfalse;
    }
    wire = strstr(lower, "on_wire") ? qtrue : qfalse;
    if (strstr(lower, "covered")) {
        if (cgs.gametype != GT_SINGLE_PLAYER) {
            return qfalse;
        }
        bare = wire ? "models/static/lightbulb_covered_nowire.tik" : NULL;
    } else if (wire) {
        bare = "models/static/lightbulb_nowire.tik";
    } else {
        return qfalse;
    }
    // hanging straight down
    if (p->axis[2][2] < 0.9f) {
        return qfalse;
    }

    p->cordModel = 0;
    p->cordScale = 1.0f;
    VectorClear(p->cordOffset);
    VectorCopy(p->mins, p->cordMins);
    VectorCopy(p->maxs, p->cordMaxs);
    if (bare) {
        vec3_t mins, maxs;
        float  width, bareWidth;

        if (!CG_PropModelBox(bare, &p->cordModel, mins, maxs)) {
            return qfalse;
        }
        // the wire is narrower than the lamp: the box's width is the lamp's
        width     = Q_max(p->maxs[0] - p->mins[0], p->maxs[1] - p->mins[1]);
        bareWidth = Q_max(maxs[0] - mins[0], maxs[1] - mins[1]);
        if (bareWidth < 0.01f || width < 0.01f) {
            return qfalse;
        }
        p->cordScale = width / bareWidth;
        for (k = 0; k < 2; k++) {
            p->cordOffset[k] = (p->mins[k] + p->maxs[k]) * 0.5f - p->cordScale * (mins[k] + maxs[k]) * 0.5f;
        }
        p->cordOffset[2] = p->mins[2] - p->cordScale * mins[2];
        for (k = 0; k < 3; k++) {
            p->cordMins[k] = p->cordOffset[k] + p->cordScale * mins[k];
            p->cordMaxs[k] = p->cordOffset[k] + p->cordScale * maxs[k];
        }
    }

    for (k = 0; k < 3; k++) {
        p->cordLight[k] = (p->cordMins[k] + p->cordMaxs[k]) * 0.5f;
    }
    VectorCopy(p->cordLight, p->cordTop);
    p->cordTop[2] = p->cordMaxs[2];

    // Up from its top: a wire's lamp to where its wire went in, a little past
    VectorCopy(p->origin, top);
    for (k = 0; k < 3; k++) {
        VectorMA(top, p->cordTop[k], p->axis[k], top);
    }
    reach = wire ? Q_min(CG_PROP_CORD_REACH, p->maxs[2] - p->cordTop[2] + 32.0f) : CG_PROP_CORD_REACH;
    VectorCopy(top, start);
    start[2] += 1.0f;
    VectorCopy(top, end);
    end[2] += reach;
    cgi.CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);

    if (cg_physics_log->integer > 1) {
        cgi.Printf("props: %s: top at %.0f %.0f %.0f: %s\n", name, top[0], top[1], top[2],
                   tr.startsolid ? "starts in the world" : tr.fraction < 1.0f ? va("a cord of %.0f", tr.endpos[2] - top[2]) : "nothing above");
    }
    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.endpos[2] - top[2] < CG_PROP_CORD_MIN) {
        return qfalse;
    }

    VectorCopy(tr.endpos, p->cordAnchor);
    p->cordLength = tr.endpos[2] - top[2];
    return qtrue;
}

// Reads the map's static models, once a map.
void CG_PropsLoad(void)
{
    void                 *buf = NULL;
    long                  len;
    dheader_t             header;
    const lump_t         *lump;
    const cStaticModel_t *in;
    int                   i, count, kept = 0, skipped = 0, sized = 0, dynamic = 0;

    if (cg_propsLoaded && !Q_stricmp(cg_propsMap, cgs.mapname)) {
        return;
    }

    cg_propsLoaded = qtrue;
    Q_strncpyz(cg_propsMap, cgs.mapname, sizeof(cg_propsMap));
    cg_numProps = 0;

    if (!cgs.mapname[0] || !cgi.FS_ReadFile) {
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

    for (i = 0; i < count && cg_numProps < CG_MAX_PROPS; i++) {
        char      path[MAX_QPATH];
        char      name[sizeof(in[i].model) + 1];
        cgProp_t *p = &cg_props[cg_numProps];
        float     biggest = 0.0f;
        int       k;

        memcpy(name, in[i].model, sizeof(in[i].model));
        name[sizeof(in[i].model)] = 0;

        const qboolean left = CG_PropSkipped(name);

        if (!Q_stricmpn(name, "models", 6)) {
            Q_strncpyz(path, name, sizeof(path));
        } else {
            Com_sprintf(path, sizeof(path), "models/%s", name);
        }
        cgi.FS_CanonicalFilename(path);

        if (!CG_PropFit(&in[i], path, p)) {
            continue;
        }

        Q_strncpyz(p->name, name, sizeof(p->name));
        p->staticIndex = i;
        p->body        = -1;
        p->clipped     = 0;
        p->numStandIns = 0;
        p->mass        = 0.0f;
        p->solid       = qtrue;
        p->dynamic     = qfalse;
        p->hangs       = qfalse;
        p->corded      = qfalse;

        for (k = 0; k < 3; k++) {
            biggest = Q_max(biggest, p->maxs[k] - p->mins[k]);
        }

        // A lamp on a cord swings on it, and the cord goes slack
        if (cg_physics_hanging->integer && CG_PropCorded(name, p)) {
            p->corded  = qtrue;
            p->hangs   = qtrue;
            p->dynamic = qtrue;
            p->why     = "hangs on a cord";
            dynamic++;
            cg_numProps++;
            kept++;
            continue;
        }

        // A hanging lamp swings, whatever its size
        if (cg_physics_hanging->integer && CG_PropHangs(name, p)) {
            p->hangs   = qtrue;
            p->dynamic = qtrue;
            p->why     = "hangs: swings from where it hangs";
            dynamic++;
            cg_numProps++;
            kept++;
            continue;
        }

        // Left out, but kept for the physics editor to show.
        if (left) {
            skipped++;
            p->solid = qfalse;
            p->why   = "left out: foliage, a light or wire";
            cg_numProps++;
            continue;
        }

        if (biggest < CG_PROP_MIN_SIZE || biggest > CG_PROP_MAX_SIZE) {
            sized++;
            p->solid = qfalse;
            p->why   = biggest < CG_PROP_MIN_SIZE ? "left out: too small" : "left out: too big";
            cg_numProps++;
            continue;
        }

        p->dynamic = biggest <= CG_PROP_DYNAMIC_SIZE ? qtrue : qfalse;
        p->why     = p->dynamic ? "small enough to move" : "too big to move";
        if (p->dynamic) {
            dynamic++;
        }

        if (cg_ragdoll_log->integer > 1 || cg_physics_log->integer > 1) {
            cgi.Printf(
                "props: %d %s at %.0f %.0f %.0f, box %.0f x %.0f x %.0f%s\n",
                cg_numProps,
                name,
                p->origin[0],
                p->origin[1],
                p->origin[2],
                p->maxs[0] - p->mins[0],
                p->maxs[1] - p->mins[1],
                p->maxs[2] - p->mins[2],
                p->dynamic ? ", moves" : ""
            );
        }

        cg_numProps++;
        kept++;
    }

    cgi.FS_FreeFile(buf);

    if (cg_ragdoll_log->integer || cg_physics_log->integer) {
        cgi.Printf(
            "props: %d of %d static models on %s are solid (%d of them move); %d foliage, lights or wire, %d too small or too big\n",
            kept,
            count,
            cgs.mapname,
            dynamic,
            skipped,
            sized
        );
    }
}

