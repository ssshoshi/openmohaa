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
// Glass that breaks a piece at a time, as Half-Life 2's does: a map's windows
// (func_window) and vehicles' panes, once hit (bg_glass.h has what the game
// sends: the hits, in order, each seeded).
//
// A hit pane is drawn here, not by its model. It is cut into small triangles
// (a grid of cells about GLASS_CELL across, its corners jittered, each cell cut
// corner to corner), clipped to the pane's own outline. Each hit knocks out
// the triangles round where it went through, a ragged hole with a few runs
// out from it in a star, and the cracks are drawn round it on the glass that
// is left. Glass no longer held by the frame (by the pane's edge, or by the
// window's bars: the parts of a window's texture that are opaque, which never
// break) falls, in shards of a few triangles that tumble, break on what they
// land on and are gone. Enough hits, or a blast, and all of it falls.
//
// The same hits give the same holes on every client and after a load: the
// cutting and the knocking out are seeded by the hit, not by the time. A pane
// first seen already hit is shown as it is, nothing falling.

#include "cg_local.h"
#include "cg_commands.h"
#include "../fgame/bg_glass.h"
#include "../qcommon/qfiles.h"

#include <vector>
#include <map>
#include <string>

#define GLASS_CELL         3.5f  // the size of a piece of glass (two triangles)
#define GLASS_MAX_CELLS    40    // across a pane, each way
#define GLASS_JITTER       0.32f // how far the cuts wander, of a cell
#define GLASS_MAX_SHARDS   400
#define GLASS_SHARD_LIFE   6.0f
#define GLASS_GRAVITY      800.0f
#define GLASS_FRAME_ALPHA  200   // a window's texture this opaque is its frame
#define GLASS_LEFT_TO_FALL 0.3f  // less of the glass left than this and it all falls
#define GLASS_UNSEEN_MS    1000  // a pane gone from view this long is shown anew

static cvar_t *cg_glass;
static cvar_t *cg_glassDebug;

//=============================================================
// A pane cut up
//=============================================================

typedef struct {
    float a, b; // across the pane and up it, in its frame
    float s, t;
} gPoint_t;

typedef struct {
    int first, count; // in points: a convex polygon
} gPiece_t;

typedef struct {
    float ca, cb; // the middle
    int   firstPiece, numPieces;
    int   corners[3]; // grid corners
    int   nb[3];      // the triangles across each edge, -1 none
    bool  frame;      // part of a window's frame: never breaks, holds glass
    bool  edge;       // at the pane's edge: held by it
    bool  full;       // whole (not cut by the pane's outline)
} gUnit_t;

typedef struct {
    glassFrame_t          f;
    glassFrame_t          hit; // the game's, which the hits are given in (a window's box: else f)
    std::vector<gPoint_t> points;
    std::vector<gPiece_t> pieces;
    std::vector<gUnit_t>  units;
    std::vector<int>      cellQuad; // a cell's two triangles whole: its four corners, in points; -1 if not
    int                   nu, nv;
    qhandle_t             shader;
    bool                  lit; // by the light where it is (else its shader's own colour)
    int                   numGlass;
    bool                  valid;
} gGeom_t;

// a pane's outline, triangles in its frame's space with texture coordinates
typedef struct {
    float xyz[3][3];
    float st[3][2];
} gTri_t;

//=============================================================
// What the map's brush models are (the BSP read once a map)
//=============================================================

typedef struct {
    std::string         shader;
    std::vector<gTri_t> tris;
    float               normal[3];
} gFace_t;

typedef struct {
    float                mins[3], maxs[3];
    std::vector<gFace_t> faces;
} gBrushModel_t;

static std::string                  glassMap;
static std::vector<gBrushModel_t>   glassBrushModels;
static std::map<int, gGeom_t>       glassWindowGeoms;  // by inline model
static std::map<int, gGeom_t>       glassVehicleGeoms; // by file and pane

//=============================================================
// Vehicles' panes (opm/glass/*.txt, as fgame g_glass.cpp reads them)
//=============================================================

typedef struct {
    float              normal[3];
    std::vector<float> tris;
} gVehiclePane_t;

typedef struct {
    std::vector<std::string>    tikis;
    std::vector<gVehiclePane_t> panes;
} gVehicle_t;

static std::vector<gVehicle_t> glassVehicles;
static bool                    glassVehiclesLoaded;

//=============================================================
// What a pane in view has come to
//=============================================================

typedef struct {
    float a, b, r, rot;
    int   shader;
} gCrack_t;

typedef struct {
    int                   key;
    gGeom_t              *geom;
    std::vector<uint8_t>  alive;
    int                   applied;
    bool                  shattered;
    std::vector<gCrack_t> cracks;
    int                   lastSeen;
    int                   numAlive; // of its glass (not its frame)
} gLive_t;

static gLive_t glassLive[MAX_GENTITIES];

//=============================================================
// Falling glass
//=============================================================

typedef struct {
    vec3_t                 origin, velocity;
    vec3_t                 axis[3];
    vec3_t                 spinAxis;
    float                  spin; // degrees a second
    float                  life;
    std::vector<float>     points; // five a point: x y z (from origin, along axis) s t
    std::vector<int>       counts; // points a polygon
    qhandle_t              shader;
    byte                   light[3];
    bool                   broken; // landed once: in bits now
} gShard_t;

static std::vector<gShard_t> glassShards;
static qhandle_t             glassCrackShaders[3];
static qhandle_t             glassTilesShader;
static int                   glassLastSound;

//=============================================================
// Small things
//=============================================================

static unsigned int GlassHash(unsigned int x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static float GlassRand(unsigned int *state)
{
    *state = GlassHash(*state + 0x9e3779b9U);
    return (*state & 0xFFFFFF) / (float)0x1000000;
}

static float GlassHashFloat(unsigned int a, unsigned int b, unsigned int c)
{
    return (GlassHash(a * 73856093U ^ GlassHash(b * 19349663U ^ GlassHash(c * 83492791U))) & 0xFFFFFF)
         / (float)0x1000000;
}

static int GlassClampInt(int x, int lo, int hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static float Cross2(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

static float PolyArea(const std::vector<float>& p)
{
    float area = 0;
    for (size_t i = 0, n = p.size() / 2; i < n; i++) {
        const size_t j = (i + 1) % n;
        area += p[i * 2] * p[j * 2 + 1] - p[j * 2] * p[i * 2 + 1];
    }
    return area * 0.5f;
}

// Sutherland-Hodgman: p (x y pairs) clipped to the counter-clockwise triangle t
static void ClipToTriangle(std::vector<float>& p, const float t[3][2])
{
    for (int e = 0; e < 3 && p.size() >= 6; e++) {
        const float       *a = t[e], *b = t[(e + 1) % 3];
        std::vector<float> out;
        const size_t       n = p.size() / 2;

        for (size_t i = 0; i < n; i++) {
            const float *c  = &p[i * 2];
            const float *d  = &p[((i + 1) % n) * 2];
            const float  sc = Cross2(b[0] - a[0], b[1] - a[1], c[0] - a[0], c[1] - a[1]);
            const float  sd = Cross2(b[0] - a[0], b[1] - a[1], d[0] - a[0], d[1] - a[1]);

            if (sc >= 0) {
                out.push_back(c[0]);
                out.push_back(c[1]);
            }
            if ((sc >= 0) != (sd >= 0)) {
                const float f = sc / (sc - sd);
                out.push_back(c[0] + (d[0] - c[0]) * f);
                out.push_back(c[1] + (d[1] - c[1]) * f);
            }
        }
        p.swap(out);
    }
    if (p.size() < 6) {
        p.clear();
    }
}

// Where a point lies in a triangle (barycentric), all three >= 0 inside
static void Barycentric(const float t[3][2], float x, float y, float w[3])
{
    const float d = Cross2(t[1][0] - t[0][0], t[1][1] - t[0][1], t[2][0] - t[0][0], t[2][1] - t[0][1]);

    if (fabs(d) < 1e-9f) {
        w[0] = 1;
        w[1] = w[2] = 0;
        return;
    }
    w[1] = Cross2(x - t[0][0], y - t[0][1], t[2][0] - t[0][0], t[2][1] - t[0][1]) / d;
    w[2] = Cross2(t[1][0] - t[0][0], t[1][1] - t[0][1], x - t[0][0], y - t[0][1]) / d;
    w[0] = 1 - w[1] - w[2];
}

//=============================================================
// A window's texture, for where its frame is
//=============================================================

typedef struct {
    int                  width, height;
    std::vector<uint8_t> alpha;
} gAlpha_t;

static std::map<std::string, gAlpha_t> glassAlphas;

// Its alpha, from a .tga (uncompressed or run-length, 32 bits): none from anything else
static const gAlpha_t *GlassTextureAlpha(const char *shaderName)
{
    // its frame alone (content/opm-glass, tools/glass/window_glass.py): the
    // game's own has shards painted into its glass, half opaque
    std::string                               own  = std::string("textures/opm/glass/") + shaderName + ".tga";
    std::string                               name = std::string(shaderName) + ".tga";
    std::map<std::string, gAlpha_t>::iterator it   = glassAlphas.find(name);
    gAlpha_t                                  img;
    byte                                     *buf = NULL;
    long                                      len;

    for (size_t c = sizeof("textures/opm/glass/") - 1; c < own.size(); c++) {
        if (own[c] == '/') {
            own[c] = '_';
        }
    }

    if (it != glassAlphas.end()) {
        return it->second.width ? &it->second : NULL;
    }
    img.width = img.height = 0;

    len = cgi.FS_ReadFile(own.c_str(), (void **)&buf, qtrue);
    if (len <= 18 || !buf) {
        if (buf) {
            cgi.FS_FreeFile(buf);
            buf = NULL;
        }
        len = cgi.FS_ReadFile(name.c_str(), (void **)&buf, qtrue);
    }
    if (len > 18 && buf) {
        const int type = buf[2], w = buf[12] | (buf[13] << 8), h = buf[14] | (buf[15] << 8), bpp = buf[16];
        const int topDown = buf[17] & 0x20;
        long      pos     = 18 + buf[0];

        if ((type == 2 || type == 10) && bpp == 32 && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            std::vector<uint8_t> a(w * h);
            int                  n = 0;

            while (n < w * h && pos < len) {
                if (type == 2) {
                    if (pos + 4 > len) {
                        break;
                    }
                    a[n++] = buf[pos + 3];
                    pos += 4;
                } else {
                    const int  head  = buf[pos++];
                    const int  count = (head & 0x7F) + 1;
                    const bool run   = (head & 0x80) != 0;

                    for (int i = 0; i < count && n < w * h; i++) {
                        if (pos + 4 > len) {
                            break;
                        }
                        a[n++] = buf[pos + 3];
                        if (!run) {
                            pos += 4;
                        }
                    }
                    if (run) {
                        pos += 4;
                    }
                }
            }
            if (n == w * h) {
                img.width  = w;
                img.height = h;
                img.alpha.resize(w * h);
                // rows top first
                for (int y = 0; y < h; y++) {
                    const int from = topDown ? y : h - 1 - y;
                    memcpy(&img.alpha[y * w], &a[from * w], w);
                }
            }
        }
    }
    if (buf) {
        cgi.FS_FreeFile(buf);
    }
    glassAlphas[name] = img;
    return img.width ? &glassAlphas[name] : NULL;
}

static int AlphaAt(const gAlpha_t *img, float s, float t)
{
    int x = (int)floor(s * img->width), y = (int)floor(t * img->height);

    x %= img->width;
    y %= img->height;
    if (x < 0) {
        x += img->width;
    }
    if (y < 0) {
        y += img->height;
    }
    return img->alpha[y * img->width + x];
}

//=============================================================
// Cutting a pane up
//=============================================================

// outline: the pane, in its frame's space; texture: for a window's frame
static void GlassBuild(gGeom_t *g, const std::vector<gTri_t>& outline, const gAlpha_t *texture)
{
    std::vector<float> tri2d; // the outline's triangles across and up the pane, six floats each
    float              a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f, cu, cv;
    int                i, j, k;

    g->valid = false;
    g->points.clear();
    g->pieces.clear();
    g->units.clear();
    g->cellQuad.clear();
    g->numGlass = 0;

    for (i = 0; i < (int)outline.size(); i++) {
        float t[3][2];
        for (k = 0; k < 3; k++) {
            t[k][0] = DotProduct(outline[i].xyz[k], g->f.u);
            t[k][1] = DotProduct(outline[i].xyz[k], g->f.v);
            a0      = Q_min(a0, t[k][0]);
            a1      = Q_max(a1, t[k][0]);
            b0      = Q_min(b0, t[k][1]);
            b1      = Q_max(b1, t[k][1]);
        }
        // counter-clockwise, for the clipping
        if (Cross2(t[1][0] - t[0][0], t[1][1] - t[0][1], t[2][0] - t[0][0], t[2][1] - t[0][1]) < 0) {
            float s;
            for (k = 0; k < 2; k++) {
                s       = t[1][k];
                t[1][k] = t[2][k];
                t[2][k] = s;
            }
            gTri_t &o = const_cast<gTri_t &>(outline[i]);
            for (k = 0; k < 3; k++) {
                s           = o.xyz[1][k];
                o.xyz[1][k] = o.xyz[2][k];
                o.xyz[2][k] = s;
            }
            for (k = 0; k < 2; k++) {
                s          = o.st[1][k];
                o.st[1][k] = o.st[2][k];
                o.st[2][k] = s;
            }
        }
        for (k = 0; k < 3; k++) {
            tri2d.push_back(t[k][0]);
            tri2d.push_back(t[k][1]);
        }
    }
    if (outline.empty() || a1 - a0 < 1 || b1 - b0 < 1) {
        return;
    }

    g->nu = GlassClampInt((int)ceil((a1 - a0) / GLASS_CELL), 1, GLASS_MAX_CELLS);
    g->nv = GlassClampInt((int)ceil((b1 - b0) / GLASS_CELL), 1, GLASS_MAX_CELLS);
    cu    = (a1 - a0) / g->nu;
    cv    = (b1 - b0) / g->nv;

    // the grid's corners, wandering a little (along the edge only, on it)
    std::vector<float> corner((g->nu + 1) * (g->nv + 1) * 2);
    for (j = 0; j <= g->nv; j++) {
        for (i = 0; i <= g->nu; i++) {
            float *c  = &corner[(j * (g->nu + 1) + i) * 2];
            float  ja = (GlassHashFloat(i, j, 1) - 0.5f) * 2 * GLASS_JITTER;
            float  jb = (GlassHashFloat(i, j, 2) - 0.5f) * 2 * GLASS_JITTER;

            if (i == 0 || i == g->nu) {
                ja = 0;
            }
            if (j == 0 || j == g->nv) {
                jb = 0;
            }
            c[0] = a0 + (i + ja) * cu;
            c[1] = b0 + (j + jb) * cv;
        }
    }

    // a point in the pane: its texture coordinates, from the outline's triangle it is in
    auto pointAt = [&](float x, float y) {
        gPoint_t p;
        int      best  = 0;
        float    bestW = -1e30f, w[3];

        for (int t = 0; t < (int)outline.size(); t++) {
            float tt[3][2], m;
            for (int q = 0; q < 3; q++) {
                tt[q][0] = tri2d[t * 6 + q * 2];
                tt[q][1] = tri2d[t * 6 + q * 2 + 1];
            }
            Barycentric(tt, x, y, w);
            m = Q_min(w[0], Q_min(w[1], w[2]));
            if (m > bestW) {
                bestW = m;
                best  = t;
            }
        }
        {
            float tt[3][2];
            for (int q = 0; q < 3; q++) {
                tt[q][0] = tri2d[best * 6 + q * 2];
                tt[q][1] = tri2d[best * 6 + q * 2 + 1];
            }
            Barycentric(tt, x, y, w);
        }
        p.a = x;
        p.b = y;
        p.s = w[0] * outline[best].st[0][0] + w[1] * outline[best].st[1][0] + w[2] * outline[best].st[2][0];
        p.t = w[0] * outline[best].st[0][1] + w[1] * outline[best].st[1][1] + w[2] * outline[best].st[2][1];
        return p;
    };

    std::map<std::pair<int, int>, std::pair<int, int>> edges; // grid corners -> (unit, which edge)

    g->cellQuad.assign(g->nu * g->nv, -1);
    for (j = 0; j < g->nv; j++) {
        for (i = 0; i < g->nu; i++) {
            const int c00 = j * (g->nu + 1) + i, c10 = c00 + 1, c01 = c00 + g->nu + 1, c11 = c01 + 1;
            const int diag = GlassHash(i * 31 + j * 977 + 5) & 1;
            int       tris[2][3];
            bool      bothFull = true;

            if (!diag) {
                tris[0][0] = c00, tris[0][1] = c10, tris[0][2] = c11;
                tris[1][0] = c00, tris[1][1] = c11, tris[1][2] = c01;
            } else {
                tris[0][0] = c00, tris[0][1] = c10, tris[0][2] = c01;
                tris[1][0] = c10, tris[1][1] = c11, tris[1][2] = c01;
            }

            for (int h = 0; h < 2; h++) {
                gUnit_t u;
                float   area, sum = 0;

                u.firstPiece = g->pieces.size();
                u.numPieces  = 0;
                u.frame      = false;
                u.full       = false;
                u.edge       = i == 0 || j == 0 || i == g->nu - 1 || j == g->nv - 1;
                u.ca = u.cb = 0;
                for (k = 0; k < 3; k++) {
                    const int c = tris[h][k];
                    u.corners[k] = c;
                    u.nb[k]      = -1;
                    u.ca += corner[c * 2] / 3;
                    u.cb += corner[c * 2 + 1] / 3;
                }
                {
                    std::vector<float> whole;
                    for (k = 0; k < 3; k++) {
                        whole.push_back(corner[tris[h][k] * 2]);
                        whole.push_back(corner[tris[h][k] * 2 + 1]);
                    }
                    area = fabs(PolyArea(whole));
                }

                // what of it is in the pane
                for (int t = 0; t < (int)outline.size(); t++) {
                    std::vector<float> p;
                    float              tt[3][2];

                    for (k = 0; k < 3; k++) {
                        p.push_back(corner[tris[h][k] * 2]);
                        p.push_back(corner[tris[h][k] * 2 + 1]);
                        tt[k][0] = tri2d[t * 6 + k * 2];
                        tt[k][1] = tri2d[t * 6 + k * 2 + 1];
                    }
                    if (PolyArea(p) < 0) {
                        std::swap(p[2], p[4]);
                        std::swap(p[3], p[5]);
                    }
                    ClipToTriangle(p, tt);
                    if (p.empty() || fabs(PolyArea(p)) < 0.01f) {
                        continue;
                    }
                    sum += fabs(PolyArea(p));

                    gPiece_t piece;
                    piece.first = g->points.size();
                    piece.count = p.size() / 2;
                    for (size_t q = 0; q < p.size() / 2; q++) {
                        float    w[3];
                        gPoint_t pt;
                        Barycentric(tt, p[q * 2], p[q * 2 + 1], w);
                        pt.a = p[q * 2];
                        pt.b = p[q * 2 + 1];
                        pt.s = w[0] * outline[t].st[0][0] + w[1] * outline[t].st[1][0] + w[2] * outline[t].st[2][0];
                        pt.t = w[0] * outline[t].st[0][1] + w[1] * outline[t].st[1][1] + w[2] * outline[t].st[2][1];
                        g->points.push_back(pt);
                    }
                    g->pieces.push_back(piece);
                    u.numPieces++;
                }

                if (!u.numPieces) {
                    // outside the pane: kept as a place, never drawn, holding nothing
                    u.edge = false;
                } else {
                    u.full = sum > area * 0.99f;
                    if (!u.full) {
                        u.edge = true; // cut by the outline: at the pane's edge
                    }
                    if (texture) {
                        // the frame if any of it is: its middle and halfway to each corner
                        const gPoint_t mid = pointAt(u.ca, u.cb);
                        int            most = AlphaAt(texture, mid.s, mid.t);
                        for (k = 0; k < 3; k++) {
                            const gPoint_t p = pointAt(
                                (u.ca + corner[tris[h][k] * 2]) * 0.5f, (u.cb + corner[tris[h][k] * 2 + 1]) * 0.5f
                            );
                            most = Q_max(most, AlphaAt(texture, p.s, p.t));
                        }
                        u.frame = most >= GLASS_FRAME_ALPHA;
                    }
                    if (!u.frame) {
                        g->numGlass++;
                    }
                }
                if (!u.full) {
                    bothFull = false;
                }

                // neighbours, by the edges they share
                const int index = g->units.size();
                for (k = 0; k < 3; k++) {
                    int a = tris[h][k], b = tris[h][(k + 1) % 3];
                    if (a > b) {
                        std::swap(a, b);
                    }
                    std::map<std::pair<int, int>, std::pair<int, int>>::iterator e = edges.find(std::make_pair(a, b));
                    if (e != edges.end()) {
                        u.nb[k]                                       = e->second.first;
                        g->units[e->second.first].nb[e->second.second] = index;
                    } else {
                        edges[std::make_pair(a, b)] = std::make_pair(index, k);
                    }
                }
                g->units.push_back(u);
            }

            if (bothFull) {
                const int cs[4] = {c00, c10, c11, c01};
                g->cellQuad[j * g->nu + i] = g->points.size();
                for (k = 0; k < 4; k++) {
                    g->points.push_back(pointAt(corner[cs[k] * 2], corner[cs[k] * 2 + 1]));
                }
            }
        }
    }

    g->valid = g->numGlass > 0;
}

//=============================================================
// The map's brush models
//=============================================================

static const void *GlassLump(const void *data, long len, dheader_t *header, int lumpNum, int size, int *count)
{
    const lump_t *lump = Q_GetLumpByVersion(header, lumpNum);

    *count = 0;
    if (lump->fileofs < 0 || lump->filelen <= 0 || lump->fileofs + lump->filelen > len) {
        return NULL;
    }
    *count = lump->filelen / size;
    return (const byte *)data + lump->fileofs;
}

static void GlassLoadMap(void)
{
    void             *buf = NULL;
    long              len;
    dheader_t         header;
    const dshader_t  *shaders;
    const dsurface_t *surfaces;
    const drawVert_t *verts;
    const int        *indexes;
    const dmodel_t   *models;
    int               numShaders, numSurfaces, numVerts, numIndexes, numModels, i, k, s;

    glassMap = cgs.mapname;
    glassBrushModels.clear();
    glassWindowGeoms.clear();
    glassVehicleGeoms.clear();
    glassAlphas.clear();

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
    shaders  = (const dshader_t *)GlassLump(buf, len, &header, LUMP_SHADERS, sizeof(dshader_t), &numShaders);
    surfaces = (const dsurface_t *)GlassLump(buf, len, &header, LUMP_SURFACES, sizeof(dsurface_t), &numSurfaces);
    verts    = (const drawVert_t *)GlassLump(buf, len, &header, LUMP_DRAWVERTS, sizeof(drawVert_t), &numVerts);
    indexes  = (const int *)GlassLump(buf, len, &header, LUMP_DRAWINDEXES, sizeof(int), &numIndexes);
    models   = (const dmodel_t *)GlassLump(buf, len, &header, LUMP_MODELS, sizeof(dmodel_t), &numModels);

    if (shaders && surfaces && verts && indexes && models) {
        glassBrushModels.resize(numModels);
        for (i = 1; i < numModels; i++) {
            gBrushModel_t *bm = &glassBrushModels[i];

            VectorCopy(models[i].mins, bm->mins);
            VectorCopy(models[i].maxs, bm->maxs);
            // a window is a few faces: anything bigger is not one
            if (models[i].numSurfaces <= 0 || models[i].numSurfaces > 16) {
                continue;
            }
            for (s = models[i].firstSurface; s < models[i].firstSurface + models[i].numSurfaces; s++) {
                const dsurface_t *surf = &surfaces[s];
                gFace_t           face;

                if (s < 0 || s >= numSurfaces || surf->surfaceType != MST_PLANAR || surf->shaderNum < 0
                    || surf->shaderNum >= numShaders) {
                    continue;
                }
                if ((shaders[surf->shaderNum].surfaceFlags & SURF_NODRAW)
                    || Q_stristr(shaders[surf->shaderNum].shader, "common/")) {
                    continue;
                }
                if (surf->firstVert < 0 || surf->firstVert + surf->numVerts > numVerts || surf->firstIndex < 0
                    || surf->firstIndex + surf->numIndexes > numIndexes) {
                    continue;
                }
                face.shader = shaders[surf->shaderNum].shader;
                VectorCopy(surf->lightmapVecs[2], face.normal);
                for (k = 0; k + 2 < surf->numIndexes; k += 3) {
                    gTri_t t;
                    for (int q = 0; q < 3; q++) {
                        const drawVert_t *v = &verts[surf->firstVert + indexes[surf->firstIndex + k + q]];
                        VectorCopy(v->xyz, t.xyz[q]);
                        t.st[q][0] = v->st[0];
                        t.st[q][1] = v->st[1];
                    }
                    face.tris.push_back(t);
                }
                if (!face.tris.empty()) {
                    bm->faces.push_back(face);
                }
            }
        }
    }
    cgi.FS_FreeFile(buf);
}

static gGeom_t *GlassWindowGeom(int inlineModel)
{
    std::map<int, gGeom_t>::iterator it = glassWindowGeoms.find(inlineModel);
    gGeom_t                         *g;

    if (it != glassWindowGeoms.end()) {
        return it->second.valid ? &it->second : NULL;
    }
    g        = &glassWindowGeoms[inlineModel];
    g->valid = false;
    if (inlineModel <= 0 || inlineModel >= (int)glassBrushModels.size()) {
        return NULL;
    }

    const gBrushModel_t *bm = &glassBrushModels[inlineModel];
    std::vector<gTri_t>  outline;
    const char          *shader = NULL;

    // the game gives the hits in the window's box (bg_glass.h); the pane is
    // its biggest face (and those lying with it), which may be tilted
    BG_GlassFrameFromBounds(bm->mins, bm->maxs, &g->hit);
    {
        const gFace_t *big     = NULL;
        float          bigArea = 0;

        for (size_t f = 0; f < bm->faces.size(); f++) {
            float area = 0;
            for (size_t k = 0; k < bm->faces[f].tris.size(); k++) {
                const gTri_t *tr = &bm->faces[f].tris[k];
                vec3_t        e1, e2, c;
                VectorSubtract(tr->xyz[1], tr->xyz[0], e1);
                VectorSubtract(tr->xyz[2], tr->xyz[0], e2);
                CrossProduct(e1, e2, c);
                area += VectorLength(c) * 0.5f;
            }
            if (area > bigArea) {
                bigArea = area;
                big     = &bm->faces[f];
            }
        }
        if (!big) {
            return NULL;
        }
        shader = big->shader.c_str();
        // one side only: the other face of the brush is the same glass
        for (size_t f = 0; f < bm->faces.size(); f++) {
            if (DotProduct(bm->faces[f].normal, big->normal) > 0.99f && !Q_stricmp(shader, bm->faces[f].shader.c_str())) {
                outline.insert(outline.end(), bm->faces[f].tris.begin(), bm->faces[f].tris.end());
            }
        }
        {
            std::vector<float> flat;
            for (size_t k = 0; k < outline.size(); k++) {
                for (int q = 0; q < 3; q++) {
                    flat.push_back(outline[k].xyz[q][0]);
                    flat.push_back(outline[k].xyz[q][1]);
                    flat.push_back(outline[k].xyz[q][2]);
                }
            }
            BG_GlassFrameFromTris(big->normal, flat.data(), outline.size(), &g->f);
        }
    }
    if (outline.empty()) {
        return NULL;
    }

    GlassBuild(g, outline, GlassTextureAlpha(shader));

    // its own glass, drawn as these pieces (content/opm-glass): else plain glass
    g->shader = cgi.R_RegisterShader(va("%s_tiles", shader));
    g->lit    = true;
    if (!g->shader) {
        g->shader = glassTilesShader;
        g->lit    = false;
    }

    if (cg_glassDebug->integer) {
        cgi.Printf(
            "glass: window *%d (%s): %d x %d, %d pieces of glass, %d of frame\n",
            inlineModel,
            shader,
            g->nu,
            g->nv,
            g->numGlass,
            (int)g->units.size() - g->numGlass
        );
    }
    return g->valid ? g : NULL;
}

static void GlassLoadVehicles(void)
{
    char **list;
    int    num = 0;

    glassVehiclesLoaded = true;
    glassVehicles.clear();

    list = cgi.FS_ListFilteredFiles("opm/glass", "txt", NULL, qfalse, &num, qfalse);
    for (int i = 0; i < num; i++) {
        char           *buf = NULL, *text;
        const char     *tok;
        gVehicle_t      v;
        gVehiclePane_t *pane = NULL;

        if (cgi.FS_ReadFile(va("opm/glass/%s", list[i]), (void **)&buf, qtrue) <= 0 || !buf) {
            continue;
        }
        text = buf;
        while (1) {
            tok = COM_ParseExt(&text, qtrue);
            if (!tok[0]) {
                break;
            }
            if (!Q_stricmp(tok, "tiki") || !Q_stricmp(tok, "static")) {
                v.tikis.push_back(COM_ParseExt(&text, qfalse));
            } else if (!Q_stricmp(tok, "pane")) {
                v.panes.push_back(gVehiclePane_t());
                pane = &v.panes.back();
                COM_ParseExt(&text, qfalse); // its surface
                for (int k = 0; k < 3; k++) {
                    pane->normal[k] = atof(COM_ParseExt(&text, qfalse));
                }
            } else if (!Q_stricmp(tok, "tri") && pane) {
                for (int k = 0; k < 9; k++) {
                    pane->tris.push_back(atof(COM_ParseExt(&text, qfalse)));
                }
            } else {
                SkipRestOfLine(&text);
            }
        }
        cgi.FS_FreeFile(buf);
        glassVehicles.push_back(v);
    }
    cgi.FS_FreeFileList(list);
}

static int GlassVehicleFor(dtiki_t *tiki)
{
    const char *name;

    if (!glassVehiclesLoaded) {
        GlassLoadVehicles();
    }
    if (!tiki || !(name = cgi.TIKI_Name(tiki))) {
        return -1;
    }
    for (size_t i = 0; i < glassVehicles.size(); i++) {
        for (size_t k = 0; k < glassVehicles[i].tikis.size(); k++) {
            if (!Q_stricmp(glassVehicles[i].tikis[k].c_str(), name)) {
                return i;
            }
        }
    }
    return -1;
}

static gGeom_t *GlassVehicleGeom(int vehicle, int pane)
{
    const int                        key = vehicle * 64 + pane;
    std::map<int, gGeom_t>::iterator it  = glassVehicleGeoms.find(key);
    gGeom_t                         *g;

    if (it != glassVehicleGeoms.end()) {
        return it->second.valid ? &it->second : NULL;
    }
    g        = &glassVehicleGeoms[key];
    g->valid = false;
    if (vehicle < 0 || vehicle >= (int)glassVehicles.size() || pane < 0
        || pane >= (int)glassVehicles[vehicle].panes.size()) {
        return NULL;
    }

    const gVehiclePane_t *p = &glassVehicles[vehicle].panes[pane];
    std::vector<gTri_t>   outline;
    const int             numTris = p->tris.size() / 9;

    BG_GlassFrameFromTris(p->normal, p->tris.data(), numTris, &g->f);
    g->hit = g->f;
    for (int t = 0; t < numTris; t++) {
        gTri_t tri;
        for (int q = 0; q < 3; q++) {
            VectorCopy(&p->tris[t * 9 + q * 3], tri.xyz[q]);
            // the plain glass's smudges, laid flat across it
            tri.st[q][0] = (DotProduct(tri.xyz[q], g->f.u) - g->f.umin) / 64.0f;
            tri.st[q][1] = (DotProduct(tri.xyz[q], g->f.v) - g->f.vmin) / 64.0f;
        }
        outline.push_back(tri);
    }
    GlassBuild(g, outline, NULL);
    g->shader = glassTilesShader;
    g->lit    = false;
    return g->valid ? g : NULL;
}

//=============================================================
// Breaking
//=============================================================

static void GlassToWorld(const vec3_t origin, vec3_t axis[3], float scale, const vec3_t local, vec3_t out)
{
    VectorCopy(origin, out);
    VectorMA(out, local[0] * scale, axis[0], out);
    VectorMA(out, local[1] * scale, axis[1], out);
    VectorMA(out, local[2] * scale, axis[2], out);
}

static void GlassPointLocal(const gGeom_t *g, float a, float b, vec3_t out)
{
    VectorScale(g->f.u, a, out);
    VectorMA(out, b, g->f.v, out);
    VectorMA(out, g->f.d, g->f.n, out);
}

typedef struct {
    vec3_t  origin;
    vec3_t  axis[3];
    float   scale;
    vec3_t  normal; // the pane's, in the world
    byte    light[3];
} gPlace_t;

static void GlassSound(const vec3_t at, float volume)
{
    if (cg.time - glassLastSound < 60 && cg.time >= glassLastSound) {
        return;
    }
    glassLastSound = cg.time;
    commandManager.PlaySound(
        va("snd_bh_glass%d", 1 + (rand() % 4)), at, CHAN_AUTO, volume, -1, 1.15f + 0.3f * random(), 1
    );
}

// Units of a pane falling, in shards of a few: out, along its normal; blast, thrown hard
static void GlassDrop(gLive_t *l, const std::vector<int>& falling, const gPlace_t *place, bool out, bool blast, unsigned int seed)
{
    const gGeom_t    *g = l->geom;
    std::vector<int>  shardOf(g->units.size(), -1);
    unsigned int      rng = seed;
    int               next = 0;

    for (size_t f = 0; f < falling.size(); f++) {
        shardOf[falling[f]] = -2; // falling, in no shard yet
    }

    for (size_t f = 0; f < falling.size(); f++) {
        const int start = falling[f];
        if (shardOf[start] != -2) {
            continue;
        }
        // a shard: those falling next to it, a few
        const int        size = 2 + (int)(GlassRand(&rng) * (blast ? 3 : 6));
        std::vector<int> members, queue;
        queue.push_back(start);
        shardOf[start] = next;
        while (!queue.empty() && (int)members.size() < size) {
            const int u = queue.front();
            queue.erase(queue.begin());
            members.push_back(u);
            for (int k = 0; k < 3; k++) {
                const int n = g->units[u].nb[k];
                if (n >= 0 && shardOf[n] == -2 && (int)(members.size() + queue.size()) < size) {
                    shardOf[n] = next;
                    queue.push_back(n);
                }
            }
        }
        for (size_t q = 0; q < queue.size(); q++) {
            shardOf[queue[q]] = -2; // left for another
        }
        next++;

        if ((int)glassShards.size() >= GLASS_MAX_SHARDS) {
            continue; // gone, unseen
        }

        gShard_t shard;
        vec3_t   centre, side;
        int      count = 0;

        VectorClear(centre);
        for (size_t m = 0; m < members.size(); m++) {
            const gUnit_t *u = &g->units[members[m]];
            vec3_t         local, world;
            GlassPointLocal(g, u->ca, u->cb, local);
            GlassToWorld(place->origin, (vec3_t *)place->axis, place->scale, local, world);
            VectorAdd(centre, world, centre);
            count++;
        }
        VectorScale(centre, 1.0f / count, centre);
        VectorCopy(centre, shard.origin);
        AxisClear(shard.axis);
        for (size_t m = 0; m < members.size(); m++) {
            const gUnit_t *u = &g->units[members[m]];
            for (int p = u->firstPiece; p < u->firstPiece + u->numPieces; p++) {
                const gPiece_t *piece = &g->pieces[p];
                for (int q = piece->first; q < piece->first + piece->count; q++) {
                    vec3_t local, world;
                    GlassPointLocal(g, g->points[q].a, g->points[q].b, local);
                    GlassToWorld(place->origin, (vec3_t *)place->axis, place->scale, local, world);
                    VectorSubtract(world, centre, world);
                    shard.points.push_back(world[0]);
                    shard.points.push_back(world[1]);
                    shard.points.push_back(world[2]);
                    shard.points.push_back(g->points[q].s);
                    shard.points.push_back(g->points[q].t);
                }
                shard.counts.push_back(piece->count);
            }
        }

        // off the pane the way the round went, tumbling
        {
            const float speed = blast ? 220 + 260 * GlassRand(&rng) : 15 + 45 * GlassRand(&rng);
            VectorScale(place->normal, out ? speed : -speed, shard.velocity);
            PerpendicularVector(side, place->normal);
            RotatePointAroundVector(side, place->normal, side, GlassRand(&rng) * 360);
            VectorMA(shard.velocity, (GlassRand(&rng) - 0.5f) * (blast ? 160 : 40), side, shard.velocity);
            shard.velocity[2] += (blast ? 80 : 10) * GlassRand(&rng);
        }
        shard.spinAxis[0] = GlassRand(&rng) - 0.5f;
        shard.spinAxis[1] = GlassRand(&rng) - 0.5f;
        shard.spinAxis[2] = GlassRand(&rng) - 0.5f;
        if (VectorNormalize(shard.spinAxis) < 0.01f) {
            VectorSet(shard.spinAxis, 0, 0, 1);
        }
        shard.spin   = (blast ? 400 : 120) + GlassRand(&rng) * (blast ? 600 : 360);
        shard.life   = GLASS_SHARD_LIFE;
        shard.shader = g->shader;
        shard.broken = false;
        VectorCopy(place->light, shard.light);
        glassShards.push_back(shard);
    }
}

// What is still held up: by the pane's edge and the window's frame, through the glass left
static void GlassSupport(gLive_t *l, std::vector<int>& falling)
{
    const gGeom_t    *g = l->geom;
    std::vector<int>  queue;
    std::vector<char> held(g->units.size(), 0);

    for (size_t i = 0; i < g->units.size(); i++) {
        const gUnit_t *u = &g->units[i];
        if (l->alive[i] && u->numPieces && (u->edge || u->frame)) {
            held[i] = 1;
            queue.push_back(i);
        }
    }
    for (size_t q = 0; q < queue.size(); q++) {
        const gUnit_t *u = &g->units[queue[q]];
        for (int k = 0; k < 3; k++) {
            const int n = u->nb[k];
            if (n >= 0 && l->alive[n] && !held[n] && g->units[n].numPieces) {
                held[n] = 1;
                queue.push_back(n);
            }
        }
    }
    for (size_t i = 0; i < g->units.size(); i++) {
        if (l->alive[i] && !held[i] && g->units[i].numPieces) {
            falling.push_back(i);
        }
    }
}

static void GlassKill(gLive_t *l, int unit, std::vector<int>& falling)
{
    if (l->alive[unit] && !l->geom->units[unit].frame) {
        l->alive[unit] = 0;
        l->numAlive--;
        falling.push_back(unit);
    }
}

// Where a hit lies across the pane: given across the game's frame, it is the
// point of the pane's plane there, along the game's frame's normal
static void GlassDecodeHit(const gGeom_t *g, byte bu, byte bv, float *pa, float *pb)
{
    const glassFrame_t *h = &g->hit;
    float               fa, fb, denom, along;
    vec3_t              p;

    BG_GlassDecode(h, bu, bv, &fa, &fb);
    VectorScale(h->u, fa, p);
    VectorMA(p, fb, h->v, p);
    denom = DotProduct(h->n, g->f.n);
    along = fabs(denom) > 0.05f ? (g->f.d - DotProduct(p, g->f.n)) / denom : 0;
    VectorMA(p, along, h->n, p);
    *pa = DotProduct(p, g->f.u);
    *pb = DotProduct(p, g->f.v);
}

// A round through the pane: the hole it knocks out, what it leaves hanging falls
static void GlassApplyHit(gLive_t *l, int index, const byte *hit, const gPlace_t *place, bool silent)
{
    const gGeom_t   *g    = l->geom;
    const int        size = hit[2] >> GLASS_HIT_SIZE_SHIFT;
    const bool       out  = (hit[2] & GLASS_HIT_OUT) != 0;
    unsigned int     rng  = GlassHash((hit[2] & GLASS_HIT_SEED) * 2654435761U + index * 40503U + hit[0] * 31 + hit[1]);
    const float      r    = 2.2f + size * 0.5f;
    std::vector<int> knocked, falling;
    float            pa, pb;
    gCrack_t         crack;
    size_t           i;

    GlassDecodeHit(g, hit[0], hit[1], &pa, &pb);

    // ragged round where it went
    for (i = 0; i < g->units.size(); i++) {
        const gUnit_t *u = &g->units[i];
        const float    d = sqrt((u->ca - pa) * (u->ca - pa) + (u->cb - pb) * (u->cb - pb));

        if (!l->alive[i] || u->frame || !u->numPieces) {
            continue;
        }
        if (d < r * (0.6f + 0.6f * GlassHashFloat(i, index, hit[2])) || d < GLASS_CELL * 0.6f) {
            GlassKill(l, i, knocked);
        }
    }
    // and a few runs out from it, a star
    {
        const int rays = 3 + (int)(GlassRand(&rng) * 3);
        for (int k = 0; k < rays; k++) {
            const float ang = GlassRand(&rng) * 2 * M_PI;
            const float len = r * (1.3f + GlassRand(&rng));
            const float da = cos(ang), db = sin(ang);

            for (i = 0; i < g->units.size(); i++) {
                const gUnit_t *u = &g->units[i];
                const float    x = u->ca - pa, y = u->cb - pb;
                const float    along = x * da + y * db;
                const float    across = fabs(-x * db + y * da);

                if (!l->alive[i] || u->frame || along < 0 || along > len) {
                    continue;
                }
                if (across < GLASS_CELL * 0.5f && GlassHashFloat(i, index * 7 + k, 3) < 0.7f) {
                    GlassKill(l, i, knocked);
                }
            }
        }
    }

    crack.a      = pa;
    crack.b      = pb;
    crack.r      = r * 2.4f + 2;
    crack.rot    = GlassRand(&rng) * 360;
    crack.shader = GlassHash(rng) % 3;
    l->cracks.push_back(crack);

    // what nothing holds now
    GlassSupport(l, falling);
    for (i = 0; i < falling.size(); i++) {
        l->alive[falling[i]] = 0;
        l->numAlive--;
    }

    if (!silent) {
        // the bits it knocked out go the way it went, what came loose drops
        GlassDrop(l, knocked, place, out, false, rng);
        GlassDrop(l, falling, place, out, false, rng + 1);
        if (!falling.empty()) {
            vec3_t local, at;
            GlassPointLocal(g, pa, pb, local);
            GlassToWorld(place->origin, (vec3_t *)place->axis, place->scale, local, at);
            GlassSound(at, 0.6f);
        }
    }
}

// All of its glass comes down
static void GlassShatter(gLive_t *l, const gPlace_t *place, bool out, bool blast, bool silent, unsigned int seed)
{
    std::vector<int> falling;

    for (size_t i = 0; i < l->geom->units.size(); i++) {
        if (l->alive[i] && !l->geom->units[i].frame && l->geom->units[i].numPieces) {
            l->alive[i] = 0;
            falling.push_back(i);
        }
    }
    l->numAlive  = 0;
    l->shattered = true;
    l->cracks.clear();
    if (!silent) {
        GlassDrop(l, falling, place, out, blast, seed);
    }
}

static void GlassReset(gLive_t *l, gGeom_t *g, int key)
{
    l->key       = key;
    l->geom      = g;
    l->applied   = 0;
    l->shattered = false;
    l->cracks.clear();
    l->alive.assign(g ? g->units.size() : 0, 1);
    l->numAlive = g ? g->numGlass : 0;
}

//=============================================================
// Drawing
//=============================================================

static void GlassVert(polyVert_t *v, const gPlace_t *place, const gGeom_t *g, float a, float b, float s, float t, float lift)
{
    vec3_t local;

    GlassPointLocal(g, a, b, local);
    GlassToWorld(place->origin, (vec3_t *)place->axis, place->scale, local, v->xyz);
    VectorMA(v->xyz, lift, place->normal, v->xyz);
    v->st[0]       = s;
    v->st[1]       = t;
    v->modulate[0] = place->light[0];
    v->modulate[1] = place->light[1];
    v->modulate[2] = place->light[2];
    v->modulate[3] = 255;
}

static void GlassDrawPane(const gLive_t *l, const gPlace_t *place)
{
    const gGeom_t *g = l->geom;
    polyVert_t     verts[16];
    float          lift;
    vec3_t         toEye;

    // cells whole: one quad
    for (int c = 0; c < g->nu * g->nv; c++) {
        const int q = g->cellQuad[c];
        if (q >= 0 && l->alive[c * 2] && l->alive[c * 2 + 1]) {
            for (int k = 0; k < 4; k++) {
                const gPoint_t *p = &g->points[q + k];
                GlassVert(&verts[k], place, g, p->a, p->b, p->s, p->t, 0);
            }
            cgi.R_AddPolyToScene(g->shader, 4, verts, 0);
        }
    }
    // the rest, each triangle's pieces
    for (size_t i = 0; i < g->units.size(); i++) {
        const gUnit_t *u    = &g->units[i];
        const int      cell = i / 2;

        if (!l->alive[i] || !u->numPieces) {
            continue;
        }
        if (g->cellQuad[cell] >= 0 && l->alive[cell * 2] && l->alive[cell * 2 + 1]) {
            continue;
        }
        for (int p = u->firstPiece; p < u->firstPiece + u->numPieces; p++) {
            const gPiece_t *piece = &g->pieces[p];
            const int       n     = Q_min(piece->count, 16);
            for (int k = 0; k < n; k++) {
                const gPoint_t *pt = &g->points[piece->first + k];
                GlassVert(&verts[k], place, g, pt->a, pt->b, pt->s, pt->t, 0);
            }
            cgi.R_AddPolyToScene(g->shader, n, verts, 0);
        }
    }

    // the cracks, on the glass left round each hole, a little toward the eye
    VectorSubtract(cg.refdef.vieworg, place->origin, toEye);
    lift = DotProduct(toEye, place->normal) > 0 ? 0.05f : -0.05f;
    for (size_t c = 0; c < l->cracks.size(); c++) {
        const gCrack_t *cr = &l->cracks[c];
        const float     cs = cos(DEG2RAD(cr->rot)), sn = sin(DEG2RAD(cr->rot));
        const qhandle_t sh = glassCrackShaders[cr->shader];

        if (!sh) {
            continue;
        }
        for (size_t i = 0; i < g->units.size(); i++) {
            const gUnit_t *u = &g->units[i];
            const float    da = u->ca - cr->a, db = u->cb - cr->b;

            if (!l->alive[i] || u->frame || !u->numPieces || da * da + db * db > (cr->r + GLASS_CELL) * (cr->r + GLASS_CELL)) {
                continue;
            }
            for (int p = u->firstPiece; p < u->firstPiece + u->numPieces; p++) {
                const gPiece_t *piece = &g->pieces[p];
                const int       n     = Q_min(piece->count, 16);
                for (int k = 0; k < n; k++) {
                    const gPoint_t *pt = &g->points[piece->first + k];
                    const float     x  = pt->a - cr->a, y = pt->b - cr->b;
                    GlassVert(
                        &verts[k],
                        place,
                        g,
                        pt->a,
                        pt->b,
                        0.5f + (x * cs - y * sn) / (2 * cr->r),
                        0.5f + (x * sn + y * cs) / (2 * cr->r),
                        lift
                    );
                    // the cracks catch the light: brighter than the glass
                    for (int q = 0; q < 3; q++) {
                        verts[k].modulate[q] = g->lit ? (byte)Q_min(255, place->light[q] * 2 + 40) : 200;
                    }
                }
                cgi.R_AddPolyToScene(sh, n, verts, 0);
            }
        }
    }
}

static void GlassShardsFrame(void)
{
    const float dt = Q_clamp_float(cg.frametime / 1000.0f, 0.0f, 0.1f);
    polyVert_t  verts[16];

    for (size_t i = 0; i < glassShards.size();) {
        gShard_t *s = &glassShards[i];
        vec3_t    end;
        trace_t   tr;

        s->life -= dt;
        if (s->life <= 0) {
            glassShards[i] = glassShards.back();
            glassShards.pop_back();
            continue;
        }

        s->velocity[2] -= GLASS_GRAVITY * dt;
        VectorMA(s->origin, dt, s->velocity, end);
        CG_Trace(&tr, s->origin, vec3_origin, vec3_origin, end, ENTITYNUM_NONE, MASK_SOLID, qfalse, qtrue, "glass shard");
        CG_ClipMoveToModelMeshes(s->origin, end, &tr);

        if (tr.fraction < 1.0f || tr.startsolid) {
            // it breaks on what it lands on: into its pieces once, and then gone
            if (s->broken || s->counts.size() <= 1) {
                GlassSound(tr.endpos, 0.25f);
                glassShards[i] = glassShards.back();
                glassShards.pop_back();
                continue;
            }
            GlassSound(tr.endpos, 0.45f);
            {
                gShard_t  whole = *s;
                size_t    first = 0;
                for (size_t p = 0; p < whole.counts.size(); p++) {
                    if ((int)glassShards.size() >= GLASS_MAX_SHARDS) {
                        break;
                    }
                    gShard_t bit = whole;
                    vec3_t   mid = {0, 0, 0};
                    const int n  = whole.counts[p];

                    bit.points.assign(whole.points.begin() + first * 5, whole.points.begin() + (first + n) * 5);
                    bit.counts.assign(1, n);
                    // its own middle, in the world as it lay
                    for (int k = 0; k < n; k++) {
                        vec3_t w;
                        VectorScale(whole.axis[0], bit.points[k * 5], w);
                        VectorMA(w, bit.points[k * 5 + 1], whole.axis[1], w);
                        VectorMA(w, bit.points[k * 5 + 2], whole.axis[2], w);
                        VectorAdd(mid, w, mid);
                    }
                    VectorScale(mid, 1.0f / n, mid);
                    for (int k = 0; k < n; k++) {
                        vec3_t w;
                        VectorScale(whole.axis[0], bit.points[k * 5], w);
                        VectorMA(w, bit.points[k * 5 + 1], whole.axis[1], w);
                        VectorMA(w, bit.points[k * 5 + 2], whole.axis[2], w);
                        VectorSubtract(w, mid, w);
                        bit.points[k * 5]     = w[0];
                        bit.points[k * 5 + 1] = w[1];
                        bit.points[k * 5 + 2] = w[2];
                    }
                    AxisClear(bit.axis);
                    VectorAdd(tr.endpos, mid, bit.origin);
                    VectorMA(bit.origin, 0.5f, tr.plane.normal, bit.origin);
                    // bouncing off, scattered
                    VectorScale(tr.plane.normal, 40 + 60 * random(), bit.velocity);
                    bit.velocity[0] += crandom() * 70;
                    bit.velocity[1] += crandom() * 70;
                    bit.spin   = 300 + 500 * random();
                    bit.life   = Q_min(whole.life, 1.5f + random());
                    bit.broken = true;
                    glassShards.push_back(bit);
                    first += n;
                }
                // whole is gone: the bits were added after it, so it is still at i
                glassShards[i] = glassShards.back();
                glassShards.pop_back();
            }
            continue;
        }

        VectorCopy(end, s->origin);
        for (int k = 0; k < 3; k++) {
            RotatePointAroundVector(s->axis[k], s->spinAxis, s->axis[k], s->spin * dt);
        }

        // drawn
        {
            size_t first = 0;
            for (size_t p = 0; p < s->counts.size(); p++) {
                const int n = Q_min(s->counts[p], 16);
                for (int k = 0; k < n; k++) {
                    const float *pt = &s->points[(first + k) * 5];
                    VectorCopy(s->origin, verts[k].xyz);
                    VectorMA(verts[k].xyz, pt[0], s->axis[0], verts[k].xyz);
                    VectorMA(verts[k].xyz, pt[1], s->axis[1], verts[k].xyz);
                    VectorMA(verts[k].xyz, pt[2], s->axis[2], verts[k].xyz);
                    verts[k].st[0]       = pt[3];
                    verts[k].st[1]       = pt[4];
                    verts[k].modulate[0] = s->light[0];
                    verts[k].modulate[1] = s->light[1];
                    verts[k].modulate[2] = s->light[2];
                    verts[k].modulate[3] = 255;
                }
                cgi.R_AddPolyToScene(s->shader, n, verts, 0);
                first += s->counts[p];
            }
        }
        i++;
    }
}

//=============================================================
// The game's side
//=============================================================

static void GlassInitShaders(void)
{
    if (!cg_glass) {
        cg_glass      = cgi.Cvar_Get("cg_glass", "1", CVAR_ARCHIVE);
        cg_glassDebug = cgi.Cvar_Get("cg_glassDebug", "0", 0);
    }
    glassTilesShader     = cgi.R_RegisterShader("opm_glass_tiles");
    glassCrackShaders[0] = cgi.R_RegisterShader("opm_glass_tilecrack0");
    glassCrackShaders[1] = cgi.R_RegisterShader("opm_glass_tilecrack1");
    glassCrackShaders[2] = cgi.R_RegisterShader("opm_glass_tilecrack2");
}

void CG_GlassClear(void)
{
    for (int i = 0; i < MAX_GENTITIES; i++) {
        glassLive[i].key  = -1;
        glassLive[i].geom = NULL;
        glassLive[i].alive.clear();
        glassLive[i].cracks.clear();
    }
    glassShards.clear();
    glassMap.clear();
    glassBrushModels.clear();
    glassWindowGeoms.clear();
    glassVehicleGeoms.clear();
    glassAlphas.clear();
    glassVehicles.clear();
    glassVehiclesLoaded = false;
    cg_glass            = NULL;
}

// A pane (EF_GLASS): drawn here once hit. qtrue when it is, so its model is not.
qboolean CG_GlassEntity(centity_t *cent)
{
    const entityState_t *s     = &cent->currentState;
    const byte          *b     = s->surfaces;
    const int            count = Q_min(b[GLASS_S_FLAGS] & GLASS_FLAG_COUNT, GLASS_MAX_HITS);
    const bool           down  = (b[GLASS_S_FLAGS] & GLASS_FLAG_SHATTERED) != 0;
    gLive_t             *l     = &glassLive[s->number];
    gGeom_t             *g     = NULL;
    gPlace_t             place;
    int                  key;
    bool                 silent;

    if (!cg_glass) {
        GlassInitShaders();
    }
    if (!cg_glass->integer || (!count && !down)) {
        if (l->geom && !count && !down) {
            l->key = -1; // whole again (a new window in its place, or reloaded)
        }
        return qfalse;
    }
    if (glassMap != cgs.mapname) {
        GlassLoadMap();
    }

    if (b[GLASS_S_PANE] == GLASS_PANE_SELF) {
        // a window: its own brush model
        const char *name = CG_ConfigString(CS_MODELS + s->modelindex);
        int         model;

        if (!name || name[0] != '*') {
            return qfalse;
        }
        model = atoi(name + 1);
        g     = GlassWindowGeom(model);
        key   = model;
        if (!g) {
            return qfalse;
        }
        VectorCopy(cent->lerpOrigin, place.origin);
        AnglesToAxis(cent->lerpAngles, place.axis);
        place.scale = 1;
    } else {
        // a vehicle's pane, on the vehicle
        const int  owner = b[GLASS_S_OWNER] | (b[GLASS_S_OWNER + 1] << 8);
        centity_t *oc;
        dtiki_t   *tiki;
        int        vehicle;

        if (owner <= 0 || owner >= MAX_GENTITIES) {
            return qtrue;
        }
        oc = &cg_entities[owner];
        if (!oc->currentValid) {
            return qtrue;
        }
        tiki    = cgi.R_Model_GetHandle(cgs.model_draw[oc->currentState.modelindex]);
        vehicle = GlassVehicleFor(tiki);
        g       = GlassVehicleGeom(vehicle, b[GLASS_S_PANE]);
        key     = 0x10000 + owner * 64 + b[GLASS_S_PANE] + (vehicle << 24);
        if (!g) {
            return qtrue;
        }
        VectorCopy(oc->lerpOrigin, place.origin);
        AnglesToAxis(oc->lerpAngles, place.axis);
        place.scale = oc->currentState.scale * tiki->load_scale;
        if (place.scale <= 0) {
            place.scale = 1;
        }
    }

    // the pane's normal in the world, and the light on the side seen
    {
        vec3_t mid, local, toEye, at;
        VectorClear(place.normal);
        for (int k = 0; k < 3; k++) {
            VectorMA(place.normal, g->f.n[k], place.axis[k], place.normal);
        }
        VectorNormalize(place.normal);
        GlassPointLocal(g, (g->f.umin + g->f.umax) * 0.5f, (g->f.vmin + g->f.vmax) * 0.5f, local);
        GlassToWorld(place.origin, place.axis, place.scale, local, mid);
        if (g->lit) {
            vec3_t facing, light;
            VectorSubtract(cg.refdef.vieworg, mid, toEye);
            VectorCopy(place.normal, facing);
            if (DotProduct(toEye, facing) < 0) {
                VectorNegate(facing, facing);
            }
            VectorMA(mid, 2.0f, facing, at);
            cgi.R_GetLightingForDecal(light, facing, at);
            for (int k = 0; k < 3; k++) {
                place.light[k] = (byte)Q_clamp_float(light[k], 0.0f, 255.0f);
            }
        } else {
            place.light[0] = place.light[1] = place.light[2] = 255;
        }
    }

    // first seen, seen anew after a while, or another pane in its place: as it is, nothing falling
    silent = false;
    if (l->key != key || l->geom != g || count < l->applied || (down != l->shattered && !down)
        || cg.time - l->lastSeen > GLASS_UNSEEN_MS || cg.time < l->lastSeen) {
        GlassReset(l, g, key);
        silent = true;
    }
    l->lastSeen = cg.time;

    while (l->applied < count && !l->shattered) {
        GlassApplyHit(l, l->applied, &b[GLASS_S_HITS + l->applied * 3], &place, silent);
        l->applied++;
        if (l->numAlive < g->numGlass * GLASS_LEFT_TO_FALL) {
            const byte *last = &b[GLASS_S_HITS + (l->applied - 1) * 3];
            GlassShatter(l, &place, (last[2] & GLASS_HIT_OUT) != 0, false, silent, GlassHash(key + count));
        }
    }
    l->applied = count;
    if (down && !l->shattered) {
        GlassShatter(
            l,
            &place,
            (b[GLASS_S_FLAGS] & GLASS_FLAG_OUT) != 0,
            (b[GLASS_S_FLAGS] & GLASS_FLAG_BLAST) != 0,
            silent,
            GlassHash(key * 3 + count)
        );
    }

    GlassDrawPane(l, &place);
    return qtrue;
}

// Each frame: the glass falling
void CG_GlassFrame(void)
{
    if (!glassShards.empty()) {
        GlassShardsFrame();
    }
}
