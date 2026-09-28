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
// The client's physics world: the map's collision (built by
// code/physics/phys_world.cpp), less the clip brushes that stand in for props
// that move, with outlines kept for cg_physics_debug.
//
// Player clip is left out on purpose. Mappers wrap furniture in it to keep
// players off, and the furniture is exactly what is going to move.

#include "cg_physics_local.h"
#include "cg_props.h"

#include <vector>

// For drawing: every edge of every shape, and a box round each shape's edges.
typedef struct {
    vec3_t mins, maxs;
    int    firstEdge, numEdges;
    int    kind; // 0 brush, 1 patch, 2 terrain
} pwShape_t;

static std::vector<pwShape_t>   pw_shapes;
static std::vector<float>       pw_edges; // 6 floats an edge
static std::vector<JPH::BodyID> pw_bodies;
static char                     pw_map[MAX_QPATH];
static qboolean                 pw_loaded;
static int                      pw_standIns;

// Whether a brush is a stand-in for one of the props that move: an invisible
// clip brush (common/clip, woodclip, metalclip...) that mostly lies inside the
// prop's box. Mappers wrap furniture in these so that players and bullets
// collide with it. Left in the physics world, the prop starts sealed inside
// one and cannot move.
static int CG_PhysicsBrushStandsIn(const char *shader, const vec3_t mins, const vec3_t maxs)
{
    char  lower[64];
    float volume;
    int   i, k;

    Q_strncpyz(lower, shader, sizeof(lower));
    Q_strlwr(lower);
    if (!strstr(lower, "clip")) {
        return -1;
    }

    volume = (maxs[0] - mins[0]) * (maxs[1] - mins[1]) * (maxs[2] - mins[2]);
    if (volume <= 0.0f) {
        return -1;
    }

    for (i = 0; i < cg_numProps; i++) {
        const cgProp_t *p = &cg_props[i];
        float           overlap = 1.0f;

        if (!p->dynamic) {
            continue;
        }

        for (k = 0; k < 3 && overlap > 0.0f; k++) {
            overlap *= Q_max(0.0f, Q_min(maxs[k], p->absmax[k] + 2.0f) - Q_max(mins[k], p->absmin[k] - 2.0f));
        }

        if (overlap >= volume * 0.5f) {
            return i;
        }
    }

    return -1;
}

static void CG_PhysicsAddEdges(const vec3_t *points, int numPoints, int kind)
{
    pwShape_t shape;
    int       i, k;

    shape.firstEdge = (int)(pw_edges.size() / 6);
    shape.kind      = kind;
    ClearBounds(shape.mins, shape.maxs);

    for (i = 0; i < numPoints; i++) {
        const float *a = points[i];
        const float *b = points[(i + 1) % numPoints];

        for (k = 0; k < 3; k++) {
            pw_edges.push_back(a[k]);
        }
        for (k = 0; k < 3; k++) {
            pw_edges.push_back(b[k]);
        }
        AddPointToBounds(a, shape.mins, shape.maxs);
    }

    shape.numEdges = (int)(pw_edges.size() / 6) - shape.firstEdge;
    pw_shapes.push_back(shape);
}


static void CG_PhysicsOutline(void *ctx, const vec3_t *points, int numPoints, int kind)
{
    (void)ctx;
    CG_PhysicsAddEdges(points, numPoints, kind);
}

// A clip brush standing in for a prop: recorded against the prop, and left out
// of the physics world if the prop is going to move, or it could never get out.
static qboolean CG_PhysicsSkipBrush(void *ctx, int brushNum, const char *shader, int contents, const vec3_t mins, const vec3_t maxs)
{
    const int prop = CG_PhysicsBrushStandsIn(shader, mins, maxs);

    (void)ctx;
    (void)contents;

    // Furniture in the brushwork is a body of its own.
    if (CG_PhysicsBrushIsFurniture(brushNum)) {
        return qtrue;
    }

    if (prop < 0) {
        return qfalse;
    }

    {
        cgProp_t *p = &cg_props[prop];

        p->clipped++;
        if (p->numStandIns < CG_PROP_MAX_STANDINS) {
            p->standIns[p->numStandIns++] = brushNum;
        }
    }
    pw_standIns++;

    return CG_PhysicsClippedPropsMove();
}

void CG_PhysicsUnloadWorld(void)
{
    CG_PhysicsUnloadFills();
    CG_PhysicsUnloadFurniture();

    if (phys_system) {
        JPH::BodyInterface &bodies = phys_system->GetBodyInterface();

        for (size_t i = 0; i < pw_bodies.size(); i++) {
            bodies.RemoveBody(pw_bodies[i]);
            bodies.DestroyBody(pw_bodies[i]);
        }
    }

    pw_bodies.clear();
    pw_shapes.clear();
    pw_edges.clear();
    pw_loaded = qfalse;
    pw_map[0] = 0;
}

void CG_PhysicsLoadWorld(void)
{
    void            *buf = NULL;
    long             len;
    physBspOptions_t opt;
    physBspStats_t   stats;
    const int        startTime = cgi.Milliseconds();

    if (pw_loaded && !Q_stricmp(pw_map, cgs.mapname)) {
        return;
    }

    CG_PhysicsUnloadWorld();
    pw_loaded = qtrue;
    Q_strncpyz(pw_map, cgs.mapname, sizeof(pw_map));
    pw_standIns = 0;

    if (!cgs.mapname[0] || !phys_system) {
        return;
    }

    // The props first, for the clip brushes that stand in for them.
    CG_PropsLoad();

    len = cgi.FS_ReadFile(cgs.mapname, &buf, qtrue);
    if (len < (long)sizeof(dheader_t) || !buf) {
        if (buf) {
            cgi.FS_FreeFile(buf);
        }
        return;
    }

    memset(&opt, 0, sizeof(opt));
    opt.contents  = CONTENTS_SOLID | CONTENTS_FENCE;
    opt.skipBrush = CG_PhysicsSkipBrush;
    opt.outline   = CG_PhysicsOutline;

    CG_PhysicsFindFurniture(buf, len);
    CG_PhysicsLoadFills(buf, len);
    Phys_BuildBspWorld(phys_system, buf, len, &opt, &pw_bodies, &stats);
    cgi.FS_FreeFile(buf);
    CG_PhysicsLoadFurniture();

    if (cg_physics_log->integer) {
        cgi.Printf(
            "physics: %s: %d brushes (%d failed, %d clip brushes standing in for props), %d patches, %d terrain patches, %d triangles, %d bodies in %d ms\n",
            cgs.mapname,
            stats.brushes,
            stats.brushFails,
            pw_standIns,
            stats.patches,
            stats.terrain,
            stats.triangles,
            stats.bodies,
            cgi.Milliseconds() - startTime
        );
    }
}

//=============================================================
// Drawing
//=============================================================

#define PW_DRAW_RANGE     768.0f
#define PW_DRAW_MAX_EDGES 6000

// cg_physics_debug: the shapes near the view, brushes green, patches yellow,
// terrain brown.
void CG_PhysicsDrawWorld(void)
{
    static const float colours[3][3] = {
        {0.2f, 1.0f, 0.2f},
        {1.0f, 1.0f, 0.2f},
        {0.8f, 0.5f, 0.2f},
    };
    const float *eye   = cg.refdef.vieworg;
    int          drawn = 0;

    for (size_t i = 0; i < pw_shapes.size() && drawn < PW_DRAW_MAX_EDGES; i++) {
        const pwShape_t *s = &pw_shapes[i];
        int              e;

        if (eye[0] < s->mins[0] - PW_DRAW_RANGE || eye[0] > s->maxs[0] + PW_DRAW_RANGE
            || eye[1] < s->mins[1] - PW_DRAW_RANGE || eye[1] > s->maxs[1] + PW_DRAW_RANGE
            || eye[2] < s->mins[2] - PW_DRAW_RANGE || eye[2] > s->maxs[2] + PW_DRAW_RANGE) {
            continue;
        }

        for (e = 0; e < s->numEdges && drawn < PW_DRAW_MAX_EDGES; e++, drawn++) {
            const float *edge = &pw_edges[(s->firstEdge + e) * 6];

            cgi.R_DebugLine(edge, edge + 3, colours[s->kind][0], colours[s->kind][1], colours[s->kind][2], 1.0f);
        }
    }
}
