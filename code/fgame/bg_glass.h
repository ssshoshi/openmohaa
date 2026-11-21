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
// Glass that breaks the way Half-Life 2's does, a piece at a time: what the
// game (fgame windows.cpp for a map's func_window, g_glass.cpp for a
// vehicle's panes) and cgame (cg_glass.cpp) agree on.
//
// An entity with EF_GLASS is a pane. The game writes where each round went
// through it into the entity's surfaces[] (unused by a brush model, and a
// vehicle's pane is an entity of its own, with no model), so a pane's damage
// is sent and saved with it; cgame cuts the pane into small pieces and,
// replaying the hits in order (each seeded), knocks out the same holes and
// drops the same pieces for everyone.
//
//   surfaces[0]   the pane: GLASS_PANE_SELF, the entity's own brush model, or
//                 the number of the pane in its vehicle's opm/glass file
//   surfaces[1]   hits (GLASS_FLAG_COUNT), GLASS_FLAG_SHATTERED, GLASS_FLAG_OUT
//   surfaces[2-3] the vehicle (entity number, low byte first)
//   then three bytes a hit: where across the pane and up it (0-255 over its
//   extent in its frame, below) and GLASS_HIT_* (size, side, seed)

#pragma once

#include <math.h>

#define GLASS_PANE_SELF      0xFF
#define GLASS_S_PANE         0
#define GLASS_S_FLAGS        1
#define GLASS_S_OWNER        2
#define GLASS_S_HITS         4
#define GLASS_MAX_HITS       9
#define GLASS_FLAG_COUNT     0x0F
#define GLASS_FLAG_SHATTERED 0x80 // all its glass comes down
#define GLASS_FLAG_OUT       0x40 // ...along its normal (else against it)
#define GLASS_FLAG_BLAST     0x20 // ...thrown hard (a blast)

#define GLASS_HIT_SEED       0x0F
#define GLASS_HIT_OUT        0x10 // the round went along the normal
#define GLASS_HIT_SIZE_SHIFT 5    // 0-7: how big a hole

// A pane's frame: its plane (normal n, d along it) and the two ways across it,
// with how far the pane reaches along each. In the pane's own space (the brush
// model's, or the vehicle model's units).
typedef struct {
    float n[3], u[3], v[3];
    float d;
    float umin, umax, vmin, vmax;
} glassFrame_t;

// A brush model's pane: flat along its thinnest axis, across the other two in
// order (a window standing in the xz plane runs across x and up z).
static inline void BG_GlassFrameFromBounds(const float *mins, const float *maxs, glassFrame_t *f)
{
    float size[3];
    int   k, thin = 0, a, b;

    for (k = 0; k < 3; k++) {
        size[k] = maxs[k] - mins[k];
        f->n[k] = f->u[k] = f->v[k] = 0;
    }
    for (k = 1; k < 3; k++) {
        if (size[k] < size[thin]) {
            thin = k;
        }
    }
    a          = thin == 0 ? 1 : 0;
    b          = thin == 2 ? 1 : 2;
    f->n[thin] = 1;
    f->u[a]    = 1;
    f->v[b]    = 1;
    f->d       = (mins[thin] + maxs[thin]) * 0.5f;
    f->umin    = mins[a];
    f->umax    = maxs[a];
    f->vmin    = mins[b];
    f->vmax    = maxs[b];
}

// A model's pane, from the way it faces and its triangles (9 floats each):
// across it level, up it as near the model's up as it goes.
static inline void BG_GlassFrameFromTris(const float *normal, const float *tris, int numTris, glassFrame_t *f)
{
    float len;
    int   i, k;

    for (k = 0; k < 3; k++) {
        f->n[k] = normal[k];
    }
    len = (float)sqrt(f->n[0] * f->n[0] + f->n[1] * f->n[1] + f->n[2] * f->n[2]);
    if (len < 1e-6f) {
        f->n[0] = 1;
        f->n[1] = f->n[2] = 0;
        len             = 1;
    }
    for (k = 0; k < 3; k++) {
        f->n[k] /= len;
    }
    // u = up x n (level across the pane), or x x n on a pane lying flat
    if (fabs(f->n[2]) < 0.95f) {
        f->u[0] = -f->n[1];
        f->u[1] = f->n[0];
        f->u[2] = 0;
    } else {
        f->u[0] = 0;
        f->u[1] = -f->n[2];
        f->u[2] = f->n[1];
    }
    len = (float)sqrt(f->u[0] * f->u[0] + f->u[1] * f->u[1] + f->u[2] * f->u[2]);
    for (k = 0; k < 3; k++) {
        f->u[k] /= len;
    }
    // v = n x u
    f->v[0] = f->n[1] * f->u[2] - f->n[2] * f->u[1];
    f->v[1] = f->n[2] * f->u[0] - f->n[0] * f->u[2];
    f->v[2] = f->n[0] * f->u[1] - f->n[1] * f->u[0];

    f->d    = 0;
    f->umin = f->vmin = 1e30f;
    f->umax = f->vmax = -1e30f;
    for (i = 0; i < numTris * 3; i++) {
        const float *p  = &tris[i * 3];
        const float  pu = p[0] * f->u[0] + p[1] * f->u[1] + p[2] * f->u[2];
        const float  pv = p[0] * f->v[0] + p[1] * f->v[1] + p[2] * f->v[2];

        f->d += p[0] * f->n[0] + p[1] * f->n[1] + p[2] * f->n[2];
        if (pu < f->umin) {
            f->umin = pu;
        }
        if (pu > f->umax) {
            f->umax = pu;
        }
        if (pv < f->vmin) {
            f->vmin = pv;
        }
        if (pv > f->vmax) {
            f->vmax = pv;
        }
    }
    if (numTris > 0) {
        f->d /= numTris * 3;
    } else {
        f->umin = f->vmin = 0;
        f->umax = f->vmax = 1;
    }
}

// A point in the pane's space to where it lies across the pane, 0-255 each way
static inline void BG_GlassEncode(const glassFrame_t *f, const float *p, unsigned char *bu, unsigned char *bv)
{
    const float pu = p[0] * f->u[0] + p[1] * f->u[1] + p[2] * f->u[2];
    const float pv = p[0] * f->v[0] + p[1] * f->v[1] + p[2] * f->v[2];
    float       x  = f->umax > f->umin ? (pu - f->umin) / (f->umax - f->umin) : 0.5f;
    float       y  = f->vmax > f->vmin ? (pv - f->vmin) / (f->vmax - f->vmin) : 0.5f;

    x   = x < 0 ? 0 : (x > 1 ? 1 : x);
    y   = y < 0 ? 0 : (y > 1 ? 1 : y);
    *bu = (unsigned char)(x * 255.0f + 0.5f);
    *bv = (unsigned char)(y * 255.0f + 0.5f);
}

// ...and back, as how far across and up (in units along u and v)
static inline void BG_GlassDecode(const glassFrame_t *f, unsigned char bu, unsigned char bv, float *pu, float *pv)
{
    *pu = f->umin + (f->umax - f->umin) * (bu / 255.0f);
    *pv = f->vmin + (f->vmax - f->vmin) * (bv / 255.0f);
}
