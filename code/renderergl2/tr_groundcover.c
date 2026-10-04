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

// tr_groundcover.c -- grass tufts over the ground the map calls grass
//
// Added in OPM. The maps have flat grass: a texture on terrain and brushes
// with `surfaceparm grass`. This stands tufts of grass on it near the view.
//
// Nothing is stored for the whole map. At load the grass ground is gathered
// (brush triangles and terrain patches, with their lightmaps), and binned in
// cells of GC_CELL_SIZE. A cell's tufts are made the first time it comes in
// range and freed once it is well out of it, so a map costs only what is
// around the player. The tufts are seeded by where they are, so a cell made
// again is the same grass.
//
// Each tuft takes its light from the ground's lightmap where it stands, so it
// sits in the shadows the map was compiled with, and it is left out where the
// ground texture is not grass (paths, dirt) and where something solid is over
// it. Its tip sways and, near the end of the draw distance, the tuft sinks
// into the ground instead of popping: each tuft has its own distance, and a
// cell's tufts are sorted by it, so a far cell draws a prefix of them.
//
// They are drawn on the CPU (RB_SurfaceGroundCover): the sway and the sinking
// are worked out per frame there, and the vertex cache keys on index arrays
// that a cell frees and another reuses.

#include "tr_local.h"

#define GC_CELL_SIZE        256
#define GC_MAX_TUFTS_CELL   ((SHADER_MAX_VERTEXES - 8) / GC_VERTS_TUFT)
#define GC_QUADS_TUFT       2
#define GC_VERTS_TUFT       (GC_QUADS_TUFT * 4)
#define GC_INDEXES_TUFT     (GC_QUADS_TUFT * 6)
#define GC_MAX_DRAWS        4096
#define GC_MASK_SIZE        64
#define GC_MIN_NORMAL_Z     0.7f
#define GC_BUILD_BUDGET_US  2000.0

typedef enum {
    GC_SRC_TRI,
    GC_SRC_TERRAIN
} gcSourceType_t;

// a grass triangle of a brush face
typedef struct {
    vec3_t xyz[3];
    vec2_t st[3];
    vec2_t lm[3];
    vec3_t normal;
    int    lightmap;
    int    shader;
} gcTri_t;

// a grass terrain patch
typedef struct {
    float x0, y0, z0;
    byte  heightmap[81];
    vec2_t texCoord[2][2];
    float lmS, lmT;    // texel of the patch's corner
    float lmSpan;      // texels across the patch
    int   lightmap;
    int   shader;
} gcTerrain_t;

// what a grass shader's ground texture says about where grass grows on it
typedef struct {
    qboolean loaded;
    qboolean hasMask;
    byte     mask[GC_MASK_SIZE * GC_MASK_SIZE]; // 0: bare, 255: grass
    vec3_t   tint;
} gcShader_t;

typedef struct {
    vec3_t root;
    float  fadeEnd;  // the distance it is gone at
    float  phase;
    float  flex;     // how far its tip sways
} gcTuft_t;

// a tuft while its cell is made
typedef struct {
    gcTuft_t tuft;
    vec3_t   color;
} gcBuildTuft_t;

typedef struct gcCell_s {
    int    firstRef;
    int    numRefs;
    vec3_t bounds[2];

    // made when it comes in range
    qboolean          built;
    int               numTufts;
    gcTuft_t         *tufts;    // by fadeEnd, farthest first
    srfVert_t        *verts;
    glIndex_t        *indexes;
    int               lastFrame;
    struct gcCell_s  *nextBuilt;
} gcCell_t;

// one cell drawn in one view
typedef struct {
    surfaceType_t surfaceType;
    gcCell_t     *cell;
    int           numTufts;
} srfGroundCover_t;

static struct {
    qboolean    active;
    shader_t   *shader;

    int         numLightmaps;
    byte       *lightmaps;  // the map's, as in the file: LIGHTMAP_SIZE^2 RGB each

    int         numShaders;
    int        *shaderSlot; // file shader -> slot in shaders, or -1
    vec3_t      grassAlbedo; // the mean colour of the tufts' texture
    int         numGrassShaders;
    char      (*shaderNames)[MAX_QPATH];
    gcShader_t *shaders;

    int          numTris;
    gcTri_t     *tris;
    int          numTerrain;
    gcTerrain_t *terrain;

    int         cellsX, cellsY;
    float       originX, originY;
    gcCell_t   *cells;
    int        *refs;       // GC_SRC_* << 28 | index

    gcCell_t   *builtList;
    int         numBuilt;

    float       density, height, brightness; // what the built cells were made with
    int         mask;
    qboolean    lastViewSet;
    vec3_t      lastView;

    int         backEndDraws, backEndTufts; // drawn, since the last report

    srfGroundCover_t draws[GC_MAX_DRAWS];
    int              numDraws;
    int              drawsFrame;
} gc;

// why the samples of the cells made so far were dropped (groundcoverinfo)
static int gc_dropOutside, gc_dropSlope, gc_dropMask, gc_dropCovered, gc_kept;

static cvar_t *r_groundCover;
static cvar_t *r_groundCoverDensity;
static cvar_t *r_groundCoverDistance;
static cvar_t *r_groundCoverHeight;
static cvar_t *r_groundCoverWind;
static cvar_t *r_groundCoverBrightness;
static cvar_t *r_groundCoverMask;

void R_GroundCoverRegisterCvars(void)
{
    // 0 off, 1 on (GL2 only; needs zzzzzzzzz-opm-groundcover.pk3)
    r_groundCover = ri.Cvar_Get("r_groundCover", "1", CVAR_ARCHIVE);
    // tufts per 32x32 units of grass
    r_groundCoverDensity = ri.Cvar_Get("r_groundCoverDensity", "2", CVAR_ARCHIVE);
    // how far out the farthest tufts go; most are gone well before
    r_groundCoverDistance = ri.Cvar_Get("r_groundCoverDistance", "1600", CVAR_ARCHIVE);
    r_groundCoverHeight = ri.Cvar_Get("r_groundCoverHeight", "1", CVAR_ARCHIVE);
    r_groundCoverWind = ri.Cvar_Get("r_groundCoverWind", "1", CVAR_ARCHIVE);
    // upright blades catch less of the light than the ground they stand on
    r_groundCoverBrightness = ri.Cvar_Get("r_groundCoverBrightness", "0.85", CVAR_ARCHIVE);
    // 1: no grass where the ground's texture is not green (paths, dirt)
    r_groundCoverMask = ri.Cvar_Get("r_groundCoverMask", "1", CVAR_ARCHIVE);
}

/*
=============================================================================

Seeded random numbers

=============================================================================
*/

static unsigned int GC_Hash(unsigned int x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static float GC_Rand(unsigned int *state)
{
    *state = GC_Hash(*state + 0x9e3779b9U);
    return (*state >> 8) * (1.0f / 16777216.0f);
}

/*
=============================================================================

Freeing

=============================================================================
*/

static void GC_FreeCell(gcCell_t *cell)
{
    if (cell->tufts) {
        ri.Free(cell->tufts);
    }
    if (cell->verts) {
        ri.Free(cell->verts);
    }
    if (cell->indexes) {
        ri.Free(cell->indexes);
    }
    cell->tufts    = NULL;
    cell->verts    = NULL;
    cell->indexes  = NULL;
    cell->numTufts = 0;
    cell->built    = qfalse;
}

static void GC_FreeAllCells(void)
{
    gcCell_t *cell;

    for (cell = gc.builtList; cell; cell = cell->nextBuilt) {
        GC_FreeCell(cell);
    }
    gc.builtList = NULL;
    gc.numBuilt  = 0;
}

void R_GroundCoverFree(void)
{
    // the back end may still be drawing last frame's cells
    if (gc.builtList) {
        R_IssuePendingRenderCommands();
    }

    GC_FreeAllCells();

    if (gc.lightmaps) {
        ri.Free(gc.lightmaps);
    }
    if (gc.shaderSlot) {
        ri.Free(gc.shaderSlot);
    }
    if (gc.shaderNames) {
        ri.Free(gc.shaderNames);
    }
    if (gc.shaders) {
        ri.Free(gc.shaders);
    }
    if (gc.tris) {
        ri.Free(gc.tris);
    }
    if (gc.terrain) {
        ri.Free(gc.terrain);
    }
    if (gc.cells) {
        ri.Free(gc.cells);
    }
    if (gc.refs) {
        ri.Free(gc.refs);
    }

    Com_Memset(&gc, 0, sizeof(gc));
}

/*
=============================================================================

Loading: gathering the grass ground of the map

=============================================================================
*/

static int GC_ShaderSlot(int shaderNum)
{
    if (shaderNum < 0 || shaderNum >= gc.numShaders) {
        return -1;
    }
    return gc.shaderSlot[shaderNum];
}

static void GC_CellRange(const vec3_t mins, const vec3_t maxs, int range[4])
{
    range[0] = (int)floor((mins[0] - gc.originX) / GC_CELL_SIZE);
    range[1] = (int)floor((mins[1] - gc.originY) / GC_CELL_SIZE);
    range[2] = (int)floor((maxs[0] - gc.originX) / GC_CELL_SIZE);
    range[3] = (int)floor((maxs[1] - gc.originY) / GC_CELL_SIZE);

    range[0] = Com_Clamp(0, gc.cellsX - 1, range[0]);
    range[1] = Com_Clamp(0, gc.cellsY - 1, range[1]);
    range[2] = Com_Clamp(0, gc.cellsX - 1, range[2]);
    range[3] = Com_Clamp(0, gc.cellsY - 1, range[3]);
}

static void GC_SourceBounds(int ref, vec3_t mins, vec3_t maxs)
{
    int index = ref & 0x0fffffff;
    int i;

    ClearBounds(mins, maxs);

    if ((ref >> 28) == GC_SRC_TRI) {
        const gcTri_t *tri = &gc.tris[index];
        for (i = 0; i < 3; i++) {
            AddPointToBounds(tri->xyz[i], mins, maxs);
        }
    } else {
        const gcTerrain_t *ter = &gc.terrain[index];
        int zmin = 255, zmax = 0;

        for (i = 0; i < 81; i++) {
            zmin = MIN(zmin, ter->heightmap[i]);
            zmax = MAX(zmax, ter->heightmap[i]);
        }
        mins[0] = ter->x0;
        mins[1] = ter->y0;
        mins[2] = ter->z0 + zmin * 2;
        maxs[0] = ter->x0 + 512;
        maxs[1] = ter->y0 + 512;
        maxs[2] = ter->z0 + zmax * 2;
    }
}

static void GC_BinSources(void)
{
    vec3_t mins, maxs, wmins, wmaxs;
    int    range[4];
    int    numSources = gc.numTris + gc.numTerrain;
    int    numRefs;
    int    pass, s, x, y;

    ClearBounds(wmins, wmaxs);
    for (s = 0; s < numSources; s++) {
        int ref = s < gc.numTris ? (GC_SRC_TRI << 28 | s) : (GC_SRC_TERRAIN << 28 | (s - gc.numTris));
        GC_SourceBounds(ref, mins, maxs);
        AddPointToBounds(mins, wmins, wmaxs);
        AddPointToBounds(maxs, wmins, wmaxs);
    }

    gc.originX = floor(wmins[0] / GC_CELL_SIZE) * GC_CELL_SIZE;
    gc.originY = floor(wmins[1] / GC_CELL_SIZE) * GC_CELL_SIZE;
    gc.cellsX  = (int)ceil((wmaxs[0] - gc.originX) / GC_CELL_SIZE) + 1;
    gc.cellsY  = (int)ceil((wmaxs[1] - gc.originY) / GC_CELL_SIZE) + 1;
    gc.cells   = ri.Malloc(gc.cellsX * gc.cellsY * sizeof(gcCell_t));
    Com_Memset(gc.cells, 0, gc.cellsX * gc.cellsY * sizeof(gcCell_t));

    for (x = 0; x < gc.cellsX * gc.cellsY; x++) {
        ClearBounds(gc.cells[x].bounds[0], gc.cells[x].bounds[1]);
    }

    // count, then fill
    numRefs = 0;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            int first = 0;

            gc.refs = ri.Malloc(MAX(numRefs, 1) * sizeof(int));
            for (x = 0; x < gc.cellsX * gc.cellsY; x++) {
                gc.cells[x].firstRef = first;
                first += gc.cells[x].numRefs;
                gc.cells[x].numRefs = 0;
            }
        }

        for (s = 0; s < numSources; s++) {
            int ref = s < gc.numTris ? (GC_SRC_TRI << 28 | s) : (GC_SRC_TERRAIN << 28 | (s - gc.numTris));

            GC_SourceBounds(ref, mins, maxs);
            GC_CellRange(mins, maxs, range);

            for (y = range[1]; y <= range[3]; y++) {
                for (x = range[0]; x <= range[2]; x++) {
                    gcCell_t *cell = &gc.cells[y * gc.cellsX + x];

                    if (pass == 0) {
                        numRefs++;
                    } else {
                        gc.refs[cell->firstRef + cell->numRefs] = ref;
                        AddPointToBounds(mins, cell->bounds[0], cell->bounds[1]);
                        AddPointToBounds(maxs, cell->bounds[0], cell->bounds[1]);
                    }
                    cell->numRefs++;
                }
            }
        }
    }

    // the tufts stand above the ground, and a cell holds only its own square
    for (x = 0; x < gc.cellsX * gc.cellsY; x++) {
        gcCell_t *cell = &gc.cells[x];

        if (!cell->numRefs) {
            continue;
        }
        cell->bounds[0][0] = MAX(cell->bounds[0][0], gc.originX + (x % gc.cellsX) * GC_CELL_SIZE);
        cell->bounds[0][1] = MAX(cell->bounds[0][1], gc.originY + (x / gc.cellsX) * GC_CELL_SIZE);
        cell->bounds[1][0] = MIN(cell->bounds[1][0], gc.originX + (x % gc.cellsX + 1) * GC_CELL_SIZE);
        cell->bounds[1][1] = MIN(cell->bounds[1][1], gc.originY + (x / gc.cellsX + 1) * GC_CELL_SIZE);
        cell->bounds[1][2] += 48;
    }
}

// The tufts' texture's mean colour where it is drawn (its alpha), which the
// tint brings to the ground's.
static void GC_MeasureGrassAlbedo(void)
{
    const textureBundle_t *bundle = gc.shader->stages[0] ? &gc.shader->stages[0]->bundle[0] : NULL;
    byte                  *pic;
    int                    width, height, i;
    GLenum                 format;
    double                 sum[3] = { 0, 0, 0 }, weight = 0;

    VectorSet(gc.grassAlbedo, 100, 116, 56);
    if (!bundle || !bundle->image[0]) {
        return;
    }
    R_LoadImageUncompressed(bundle->image[0]->imgName, &pic, &width, &height, &format);
    if (!pic) {
        return;
    }
    if (format == GL_RGBA8) {
        for (i = 0; i < width * height; i++) {
            float a = pic[i * 4 + 3] / 255.0f;
            sum[0] += pic[i * 4 + 0] * a;
            sum[1] += pic[i * 4 + 1] * a;
            sum[2] += pic[i * 4 + 2] * a;
            weight += a;
        }
        if (weight > 0) {
            VectorSet(gc.grassAlbedo, sum[0] / weight, sum[1] / weight, sum[2] / weight);
        }
    }
    ri.Free(pic);
}

/*
================
R_GroundCoverLoadWorld

From the BSP as it is in the file, before the renderer has packed its
lightmaps into atlases.
================
*/
void R_GroundCoverLoadWorld(const byte *fileBase, dheader_t *header)
{
    const lump_t     *shaderLump  = Q_GetLumpByVersion(header, LUMP_SHADERS);
    const lump_t     *lmLump      = Q_GetLumpByVersion(header, LUMP_LIGHTMAPS);
    const lump_t     *surfLump    = Q_GetLumpByVersion(header, LUMP_SURFACES);
    const lump_t     *vertLump    = Q_GetLumpByVersion(header, LUMP_DRAWVERTS);
    const lump_t     *indexLump   = Q_GetLumpByVersion(header, LUMP_DRAWINDEXES);
    const lump_t     *terrainLump = Q_GetLumpByVersion(header, LUMP_TERRAIN);
    const dshader_t  *dshaders;
    const dsurface_t *dsurfs;
    const drawVert_t *dverts;
    const int        *dindexes;
    int               numSurfs, numVerts, numIndexes, numPatches;
    int               pass, i, j;

    R_GroundCoverFree();
    gc_dropOutside = gc_dropSlope = gc_dropMask = gc_dropCovered = gc_kept = 0;

    if (!r_groundCover->integer) {
        return;
    }

    gc.shader = R_FindShader("groundcover/grass", LIGHTMAP_NONE, qtrue);
    if (gc.shader->defaultShader) {
        ri.Printf(PRINT_WARNING, "Ground cover: no groundcover/grass shader (zzzzzzzzz-opm-groundcover.pk3)\n");
        Com_Memset(&gc, 0, sizeof(gc));
        return;
    }

    GC_MeasureGrassAlbedo();

    dshaders       = (const dshader_t *)(fileBase + shaderLump->fileofs);
    gc.numShaders  = shaderLump->filelen / sizeof(dshader_t);
    gc.shaderSlot  = ri.Malloc(MAX(gc.numShaders, 1) * sizeof(int));
    gc.shaderNames = ri.Malloc(MAX(gc.numShaders, 1) * sizeof(*gc.shaderNames));

    for (i = 0; i < gc.numShaders; i++) {
        int surfaceFlags = LittleLong(dshaders[i].surfaceFlags);
        int contents     = LittleLong(dshaders[i].contentFlags);

        gc.shaderSlot[i] = -1;
        if (!(surfaceFlags & SURF_GRASS) || (surfaceFlags & (SURF_NODRAW | SURF_SKY))) {
            continue;
        }
        // grass the player walks through is foliage, not ground
        if (!(contents & CONTENTS_SOLID) || (contents & CONTENTS_FENCE)) {
            continue;
        }
        gc.shaderSlot[i] = gc.numGrassShaders;
        Q_strncpyz(gc.shaderNames[gc.numGrassShaders], dshaders[i].shader, MAX_QPATH);
        gc.numGrassShaders++;
    }

    if (!gc.numGrassShaders) {
        R_GroundCoverFree();
        return;
    }

    gc.shaders = ri.Malloc(gc.numGrassShaders * sizeof(gcShader_t));
    Com_Memset(gc.shaders, 0, gc.numGrassShaders * sizeof(gcShader_t));

    gc.numLightmaps = lmLump->filelen / (LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
    if (gc.numLightmaps) {
        gc.lightmaps = ri.Malloc(gc.numLightmaps * LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
        Com_Memcpy(gc.lightmaps, fileBase + lmLump->fileofs, gc.numLightmaps * LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
    }

    dsurfs     = (const dsurface_t *)(fileBase + surfLump->fileofs);
    numSurfs   = surfLump->filelen / sizeof(dsurface_t);
    dverts     = (const drawVert_t *)(fileBase + vertLump->fileofs);
    numVerts   = vertLump->filelen / sizeof(drawVert_t);
    dindexes   = (const int *)(fileBase + indexLump->fileofs);
    numIndexes = indexLump->filelen / sizeof(int);

    // brush faces and triangle soups: count the upward triangles, then copy
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            gc.tris    = ri.Malloc(MAX(gc.numTris, 1) * sizeof(gcTri_t));
            gc.numTris = 0;
        }

        for (i = 0; i < numSurfs; i++) {
            const dsurface_t *ds = &dsurfs[i];
            int               type = LittleLong(ds->surfaceType);
            int               slot = GC_ShaderSlot(LittleLong(ds->shaderNum));
            int               firstVert = LittleLong(ds->firstVert);
            int               nVerts = LittleLong(ds->numVerts);
            int               firstIndex = LittleLong(ds->firstIndex);
            int               nIndexes = LittleLong(ds->numIndexes);

            if (slot < 0 || (type != MST_PLANAR && type != MST_TRIANGLE_SOUP)) {
                continue;
            }
            if (firstVert < 0 || firstVert + nVerts > numVerts || firstIndex < 0 || firstIndex + nIndexes > numIndexes) {
                continue;
            }

            for (j = 0; j + 2 < nIndexes; j += 3) {
                const drawVert_t *v[3];
                vec3_t            e1, e2, n, vn;
                int               k;

                for (k = 0; k < 3; k++) {
                    int idx = LittleLong(dindexes[firstIndex + j + k]);
                    if (idx < 0 || idx >= nVerts) {
                        break;
                    }
                    v[k] = &dverts[firstVert + idx];
                }
                if (k < 3) {
                    continue;
                }

                VectorSubtract(v[1]->xyz, v[0]->xyz, e1);
                VectorSubtract(v[2]->xyz, v[0]->xyz, e2);
                CrossProduct(e1, e2, n);
                if (VectorNormalize(n) < 0.001f) {
                    continue;
                }
                // the winding is either way; the vertex normals say which is up
                VectorAdd(v[0]->normal, v[1]->normal, vn);
                VectorAdd(vn, v[2]->normal, vn);
                if (DotProduct(n, vn) < 0) {
                    VectorNegate(n, n);
                }
                if (n[2] < GC_MIN_NORMAL_Z) {
                    continue;
                }

                if (pass == 1) {
                    gcTri_t *tri = &gc.tris[gc.numTris];

                    for (k = 0; k < 3; k++) {
                        VectorCopy(v[k]->xyz, tri->xyz[k]);
                        tri->st[k][0] = LittleFloat(v[k]->st[0]);
                        tri->st[k][1] = LittleFloat(v[k]->st[1]);
                        tri->lm[k][0] = LittleFloat(v[k]->lightmap[0]);
                        tri->lm[k][1] = LittleFloat(v[k]->lightmap[1]);
                    }
                    VectorCopy(n, tri->normal);
                    tri->lightmap = LittleLong(ds->lightmapNum);
                    tri->shader   = slot;
                }
                gc.numTris++;
            }
        }
    }

    // terrain
    numPatches = terrainLump->filelen / sizeof(cTerraPatch_t);
    gc.terrain = ri.Malloc(MAX(numPatches, 1) * sizeof(gcTerrain_t));
    for (i = 0; i < numPatches; i++) {
        const cTerraPatch_t *p = (const cTerraPatch_t *)(fileBase + terrainLump->fileofs) + i;
        int                  slot = GC_ShaderSlot(LittleShort(p->iShader));
        gcTerrain_t         *ter;

        if (slot < 0 || p->lmapScale <= 0) {
            continue;
        }

        ter         = &gc.terrain[gc.numTerrain++];
        ter->x0     = p->x * 64;
        ter->y0     = p->y * 64;
        ter->z0     = LittleShort(p->iBaseHeight);
        Com_Memcpy(ter->heightmap, p->heightmap, sizeof(ter->heightmap));
        for (j = 0; j < 4; j++) {
            ter->texCoord[j >> 1][j & 1][0] = LittleFloat(p->texCoord[j >> 1][j & 1][0]);
            ter->texCoord[j >> 1][j & 1][1] = LittleFloat(p->texCoord[j >> 1][j & 1][1]);
        }
        ter->lmS      = p->s + 0.5f;
        ter->lmT      = p->t + 0.5f;
        ter->lmSpan   = p->lmapScale * 8;
        ter->lightmap = LittleShort(p->iLightMap);
        ter->shader   = slot;
    }

    if (!gc.numTris && !gc.numTerrain) {
        R_GroundCoverFree();
        return;
    }

    GC_BinSources();

    gc.active = qtrue;
    ri.Printf(PRINT_DEVELOPER, "Ground cover: %d grass shaders, %d triangles, %d terrain patches, %dx%d cells\n",
        gc.numGrassShaders, gc.numTris, gc.numTerrain, gc.cellsX, gc.cellsY);
}

/*
=============================================================================

Making a cell's tufts

=============================================================================
*/

// The texture the ground is drawn with: the first stage's that is not the
// lightmap. The name of a shader that has no script is its texture's.
static const char *GC_GroundImageName(int slot)
{
    const shader_t *sh = R_FindShaderByName(gc.shaderNames[slot]);
    int             i;

    if (sh && sh != tr.defaultShader && !sh->defaultShader) {
        for (i = 0; i < MAX_SHADER_STAGES && sh->stages[i]; i++) {
            const textureBundle_t *bundle = &sh->stages[i]->bundle[0];
            if (!bundle->isLightmap && bundle->image[0]) {
                return bundle->image[0]->imgName;
            }
        }
    }
    return gc.shaderNames[slot];
}

// The ground texture, small: where it is green there is grass, where it is
// brown or grey (a path, a patch of dirt) there is not. Each texture is
// measured against itself, so a dry yellow field still counts as grass.
static void GC_LoadShaderMask(int slot)
{
    gcShader_t *gs = &gc.shaders[slot];
    byte       *pic;
    int         width, height;
    GLenum      format;
    float       hist[64];
    float       g[GC_MASK_SIZE * GC_MASK_SIZE];
    vec3_t      sum;
    float       median, total, acc;
    int         x, y, i;

    gs->loaded  = qtrue;
    gs->hasMask = qfalse;
    VectorSet(gs->tint, 1, 1, 1);

    R_LoadImageUncompressed(GC_GroundImageName(slot), &pic, &width, &height, &format);
    if (!pic || format != GL_RGBA8 || width <= 0 || height <= 0) {
        if (pic) {
            ri.Free(pic);
        }
        return;
    }

    VectorClear(sum);
    Com_Memset(hist, 0, sizeof(hist));

    for (y = 0; y < GC_MASK_SIZE; y++) {
        for (x = 0; x < GC_MASK_SIZE; x++) {
            // the mean of the texels this mask texel covers
            int   x0 = x * width / GC_MASK_SIZE, x1 = MAX(x0 + 1, (x + 1) * width / GC_MASK_SIZE);
            int   y0 = y * height / GC_MASK_SIZE, y1 = MAX(y0 + 1, (y + 1) * height / GC_MASK_SIZE);
            float r = 0, gr = 0, b = 0, n = 0, chroma;
            int   u, v;

            for (v = y0; v < y1; v++) {
                for (u = x0; u < x1; u++) {
                    const byte *px = pic + (v * width + u) * 4;
                    r += px[0];
                    gr += px[1];
                    b += px[2];
                    n++;
                }
            }
            r /= n;
            gr /= n;
            b /= n;
            sum[0] += r;
            sum[1] += gr;
            sum[2] += b;

            // green against the other two, out of the brightness
            chroma = (gr - 0.5f * (r + b)) / (r + gr + b + 24.0f);
            g[y * GC_MASK_SIZE + x] = chroma;
            hist[(int)Com_Clamp(0, 63, (int)((chroma + 0.25f) * 128.0f))]++;
        }
    }

    ri.Free(pic);

    total = GC_MASK_SIZE * GC_MASK_SIZE;
    for (i = 0, acc = 0; i < 64; i++) {
        acc += hist[i];
        if (acc >= total * 0.5f) {
            break;
        }
    }
    median = i / 128.0f - 0.25f;

    // the grass is the green half and what is about as green; well under
    // the middle is something else
    for (i = 0; i < GC_MASK_SIZE * GC_MASK_SIZE; i++) {
        float w = (g[i] - (median - 0.05f)) / 0.04f;
        gs->mask[i] = (byte)(Com_Clamp(0, 1, w) * 255);
    }

    // a texture with no green in it is not grass to look at, whatever its
    // surfaceparm says: leave it bare rather than guess
    if (median < -0.01f) {
        Com_Memset(gs->mask, 0, sizeof(gs->mask));
    }

    // the tufts take half the ground's colour, so they grow out of it rather
    // than stand on it: a dark field gets darker grass, a dry one drier
    VectorScale(sum, 1.0f / total, sum);
    for (i = 0; i < 3; i++) {
        gs->tint[i] = Com_Clamp(0.4f, 1.5f, 0.5f + 0.5f * sum[i] / MAX(gc.grassAlbedo[i], 1.0f));
    }

    gs->hasMask = qtrue;
}

static float GC_MaskAt(int slot, const vec2_t st)
{
    gcShader_t *gs = &gc.shaders[slot];
    float       s, t;
    int         x, y;

    if (!gs->loaded) {
        GC_LoadShaderMask(slot);
    }
    if (!gs->hasMask || !r_groundCoverMask->integer) {
        return 1.0f;
    }

    s = st[0] - floor(st[0]);
    t = st[1] - floor(st[1]);
    x = Com_Clamp(0, GC_MASK_SIZE - 1, (int)(s * GC_MASK_SIZE));
    y = Com_Clamp(0, GC_MASK_SIZE - 1, (int)(t * GC_MASK_SIZE));
    return gs->mask[y * GC_MASK_SIZE + x] * (1.0f / 255.0f);
}

// The ground's light where the tuft stands, scaled as the renderer scales a
// lightmap (R_ColorShiftLightingFloats), for rgbGen exactVertex.
static void GC_LightAt(int lightmap, float s, float t, const vec3_t xyz, vec3_t out)
{
    float scale = (1 << (r_mapOverBrightBits->integer - tr.overbrightBits)) / 255.0f;
    float maxc;
    int   i;

    if (lightmap >= 0 && lightmap < gc.numLightmaps) {
        const byte *page = gc.lightmaps + lightmap * LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3;
        int   x0, y0, x1, y1;
        float fx, fy;

        s -= 0.5f;
        t -= 0.5f;
        x0 = (int)floor(s);
        y0 = (int)floor(t);
        fx = s - x0;
        fy = t - y0;
        x1 = Com_Clamp(0, LIGHTMAP_SIZE - 1, x0 + 1);
        y1 = Com_Clamp(0, LIGHTMAP_SIZE - 1, y0 + 1);
        x0 = Com_Clamp(0, LIGHTMAP_SIZE - 1, x0);
        y0 = Com_Clamp(0, LIGHTMAP_SIZE - 1, y0);

        for (i = 0; i < 3; i++) {
            float a = page[(y0 * LIGHTMAP_SIZE + x0) * 3 + i];
            float b = page[(y0 * LIGHTMAP_SIZE + x1) * 3 + i];
            float c = page[(y1 * LIGHTMAP_SIZE + x0) * 3 + i];
            float d = page[(y1 * LIGHTMAP_SIZE + x1) * 3 + i];
            out[i] = (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
        }
    } else {
        // vertex lit: the light grid
        vec3_t ambient, directed;

        R_GetLightingGridValue(tr.world, xyz, ambient, directed);
        VectorAdd(ambient, directed, out);
    }

    VectorScale(out, scale, out);
    maxc = MAX(out[0], MAX(out[1], out[2]));
    if (maxc > 1) {
        VectorScale(out, 1.0f / maxc, out);
    }
}

typedef struct {
    vec3_t xyz;
    vec3_t light;
    vec3_t tint;
} gcSample_t;

static qboolean GC_Clear(vec3_t xyz, float below, float above)
{
    trace_t trace;
    vec3_t  start, end;

    // Down onto it from above: the first solid hit has to be this ground.
    // Something over it (a wall standing on it, a sandbag) stops the trace
    // higher, and so does other ground: a face the map hid under terrain is
    // still textured grass, and terrain is solid only from above. Terrain is
    // drawn as triangles of its heightmap, not the smooth surface sampled
    // here, so it gets a tolerance and the tuft goes where the ground is: a
    // little up, more down, as a road laid over terrain is a few units up
    // and the terrain under it unlit.
    VectorSet(start, xyz[0], xyz[1], xyz[2] + 64);
    VectorSet(end, xyz[0], xyz[1], xyz[2] - below - 4);
    ri.CM_BoxTrace(&trace, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);
    if (trace.startsolid || trace.allsolid || trace.fraction >= 1.0f || trace.endpos[2] > xyz[2] + above || trace.endpos[2] < xyz[2] - below) {
        return qfalse;
    }
    xyz[2] = trace.endpos[2];
    return qtrue;
}

static qboolean GC_SampleTri(const gcTri_t *tri, float x, float y, gcSample_t *out, unsigned int *rnd)
{
    float  d, b0, b1, b2;
    vec2_t st, lm;
    int    i;

    // where (x, y) is in the triangle, seen from above
    d = (tri->xyz[1][1] - tri->xyz[2][1]) * (tri->xyz[0][0] - tri->xyz[2][0])
      + (tri->xyz[2][0] - tri->xyz[1][0]) * (tri->xyz[0][1] - tri->xyz[2][1]);
    if (fabs(d) < 0.0001f) {
        return qfalse;
    }
    b0 = ((tri->xyz[1][1] - tri->xyz[2][1]) * (x - tri->xyz[2][0]) + (tri->xyz[2][0] - tri->xyz[1][0]) * (y - tri->xyz[2][1])) / d;
    b1 = ((tri->xyz[2][1] - tri->xyz[0][1]) * (x - tri->xyz[2][0]) + (tri->xyz[0][0] - tri->xyz[2][0]) * (y - tri->xyz[2][1])) / d;
    b2 = 1.0f - b0 - b1;
    if (b0 < 0 || b1 < 0 || b2 < 0) {
        gc_dropOutside++;
        return qfalse;
    }

    for (i = 0; i < 2; i++) {
        st[i] = b0 * tri->st[0][i] + b1 * tri->st[1][i] + b2 * tri->st[2][i];
        lm[i] = b0 * tri->lm[0][i] + b1 * tri->lm[1][i] + b2 * tri->lm[2][i];
    }
    if (GC_MaskAt(tri->shader, st) <= GC_Rand(rnd)) {
        gc_dropMask++;
        return qfalse;
    }

    out->xyz[0] = x;
    out->xyz[1] = y;
    out->xyz[2] = b0 * tri->xyz[0][2] + b1 * tri->xyz[1][2] + b2 * tri->xyz[2][2];
    GC_LightAt(tri->lightmap, lm[0] * LIGHTMAP_SIZE, lm[1] * LIGHTMAP_SIZE, out->xyz, out->light);
    VectorCopy(gc.shaders[tri->shader].tint, out->tint);
    return qtrue;
}

static float GC_TerrainHeight(const gcTerrain_t *ter, int i, int j)
{
    return ter->z0 + ter->heightmap[j * 9 + i] * 2;
}

static qboolean GC_SampleTerrain(const gcTerrain_t *ter, float x, float y, gcSample_t *out, unsigned int *rnd)
{
    float  fx = (x - ter->x0) / 64.0f, fy = (y - ter->y0) / 64.0f;
    int    i, j;
    float  ax, ay, h00, h10, h01, h11, dzdx, dzdy;
    vec2_t st;

    if (fx < 0 || fy < 0 || fx >= 8 || fy >= 8) {
        return qfalse;
    }

    i   = (int)fx;
    j   = (int)fy;
    ax  = fx - i;
    ay  = fy - j;
    h00 = GC_TerrainHeight(ter, i, j);
    h10 = GC_TerrainHeight(ter, i + 1, j);
    h01 = GC_TerrainHeight(ter, i, j + 1);
    h11 = GC_TerrainHeight(ter, i + 1, j + 1);

    // too steep
    dzdx = ((h10 - h00) * (1 - ay) + (h11 - h01) * ay) / 64.0f;
    dzdy = ((h01 - h00) * (1 - ax) + (h11 - h10) * ax) / 64.0f;
    if (1.0f / sqrt(1.0f + dzdx * dzdx + dzdy * dzdy) < GC_MIN_NORMAL_Z) {
        gc_dropSlope++;
        return qfalse;
    }

    fx /= 8.0f;
    fy /= 8.0f;
    for (i = 0; i < 2; i++) {
        st[i] = (ter->texCoord[0][0][i] * (1 - fx) + ter->texCoord[1][0][i] * fx) * (1 - fy)
              + (ter->texCoord[0][1][i] * (1 - fx) + ter->texCoord[1][1][i] * fx) * fy;
    }
    if (GC_MaskAt(ter->shader, st) <= GC_Rand(rnd)) {
        gc_dropMask++;
        return qfalse;
    }

    out->xyz[0] = x;
    out->xyz[1] = y;
    out->xyz[2] = (h00 * (1 - ax) + h10 * ax) * (1 - ay) + (h01 * (1 - ax) + h11 * ax) * ay;
    GC_LightAt(ter->lightmap, ter->lmS + fx * ter->lmSpan, ter->lmT + fy * ter->lmSpan, out->xyz, out->light);
    VectorCopy(gc.shaders[ter->shader].tint, out->tint);
    return qtrue;
}

// Smooth noise over the map, 0 to 1: grass grows in patches, thick in one
// place and thin a few steps on, not spread evenly
static float GC_LatticeValue(int x, int y)
{
    return GC_Hash((unsigned int)x * 73856093u ^ (unsigned int)y * 19349663u) * (1.0f / 4294967296.0f);
}

static float GC_ValueNoise(float x, float y)
{
    int   ix = (int)floor(x), iy = (int)floor(y);
    float fx = x - ix, fy = y - iy;
    float a, b, c, d;

    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    a  = GC_LatticeValue(ix, iy);
    b  = GC_LatticeValue(ix + 1, iy);
    c  = GC_LatticeValue(ix, iy + 1);
    d  = GC_LatticeValue(ix + 1, iy + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

static float GC_Patchiness(float x, float y)
{
    float n = 0.65f * GC_ValueNoise(x / 340.0f, y / 340.0f) + 0.35f * GC_ValueNoise(x / 97.0f + 17.3f, y / 97.0f - 5.1f);
    // thick, thin and a few bare spots, about as much grass as even
    return Com_Clamp(0, 1, (n - 0.22f) * 1.6f);
}

static int GC_CompareTufts(const void *a, const void *b)
{
    float fa = ((const gcBuildTuft_t *)a)->tuft.fadeEnd, fb = ((const gcBuildTuft_t *)b)->tuft.fadeEnd;
    return fa < fb ? 1 : (fa > fb ? -1 : 0);
}

static void GC_SetVert(srfVert_t *v, const vec3_t xyz, float s, float t, const vec3_t color)
{
    int16_t up[4] = { 0, 0, 32767, 0 };
    int16_t tangent[4] = { 32767, 0, 0, 32767 };
    int     i;

    Com_Memset(v, 0, sizeof(*v));
    VectorCopy(xyz, v->xyz);
    v->st[0] = s;
    v->st[1] = t;
    // lit from above as the ground is (the realtime lights read this)
    VectorCopy4(up, v->normal);
    VectorCopy4(tangent, v->tangent);
    for (i = 0; i < 3; i++) {
        v->color[i] = (uint16_t)(Com_Clamp(0, 1, color[i]) * 65535.0f);
    }
    v->color[3] = 65535;
}

static void GC_BuildCell(gcCell_t *cell, int cellIndex)
{
    const float   density  = Com_Clamp(0, 8, r_groundCoverDensity->value) / (32.0f * 32.0f);
    const float   scale    = Com_Clamp(0.25f, 4, r_groundCoverHeight->value);
    const float   bright   = Com_Clamp(0, 4, r_groundCoverBrightness->value);
    float         cx0      = gc.originX + (cellIndex % gc.cellsX) * GC_CELL_SIZE;
    float         cy0      = gc.originY + (cellIndex / gc.cellsX) * GC_CELL_SIZE;
    gcBuildTuft_t tufts[GC_MAX_TUFTS_CELL];
    int           numTufts = 0;
    int           r, n, attempts;

    cell->built     = qtrue;
    cell->nextBuilt = gc.builtList;
    gc.builtList    = cell;
    gc.numBuilt++;

    for (r = 0; r < cell->numRefs && numTufts < GC_MAX_TUFTS_CELL; r++) {
        int          ref  = gc.refs[cell->firstRef + r];
        int          type = ref >> 28;
        int          index = ref & 0x0fffffff;
        unsigned int rnd   = GC_Hash(cellIndex * 7919u + ref * 104729u + 1u);
        vec3_t       mins, maxs;
        float        x0, y0, x1, y1, w;

        // sample the part of the source in this cell, as seen from above
        GC_SourceBounds(ref, mins, maxs);
        x0 = MAX(mins[0], cx0);
        y0 = MAX(mins[1], cy0);
        x1 = MIN(maxs[0], cx0 + GC_CELL_SIZE);
        y1 = MIN(maxs[1], cy0 + GC_CELL_SIZE);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }

        // the patches keep about half
        w        = (x1 - x0) * (y1 - y0) * density * 2.0f;
        attempts = (int)w + (GC_Rand(&rnd) < w - (int)w ? 1 : 0);

        for (n = 0; n < attempts && numTufts < GC_MAX_TUFTS_CELL; n++) {
            gcSample_t     sample;
            gcBuildTuft_t *bt;
            gcTuft_t      *tuft;
            float      x = x0 + GC_Rand(&rnd) * (x1 - x0);
            float      y = y0 + GC_Rand(&rnd) * (y1 - y0);
            qboolean   ok;

            if (type == GC_SRC_TRI) {
                ok = GC_SampleTri(&gc.tris[index], x, y, &sample, &rnd);
            } else {
                ok = GC_SampleTerrain(&gc.terrain[index], x, y, &sample, &rnd);
            }
            if (!ok) {
                continue;
            }
            if (GC_Patchiness(x, y) <= GC_Rand(&rnd)) {
                continue;
            }
            if (!GC_Clear(sample.xyz, type == GC_SRC_TERRAIN ? 16.0f : 3.0f, type == GC_SRC_TERRAIN ? 5.0f : 3.0f)) {
                gc_dropCovered++;
                continue;
            }
            gc_kept++;

            bt   = &tufts[numTufts];
            tuft = &bt->tuft;
            VectorCopy(sample.xyz, tuft->root);
            tuft->root[2] -= 1.5f; // into the ground, so no gap shows on a slope
            // most tufts are gone by half the distance, a few go on to it
            tuft->fadeEnd = 0.25f + 0.75f * GC_Rand(&rnd) * GC_Rand(&rnd);
            tuft->phase   = GC_Rand(&rnd) * 2.0f * M_PI;
            tuft->flex    = 0.6f + 0.6f * GC_Rand(&rnd);

            VectorScale(sample.light, bright * (0.85f + 0.25f * GC_Rand(&rnd)), bt->color);
            bt->color[0] *= sample.tint[0];
            bt->color[1] *= sample.tint[1];
            bt->color[2] *= sample.tint[2];
            numTufts++;
        }
    }

    // the farthest reaching first, so a far cell draws a prefix of them
    qsort(tufts, numTufts, sizeof(gcBuildTuft_t), GC_CompareTufts);

    cell->numTufts = numTufts;
    if (!numTufts) {
        return;
    }

    cell->tufts   = ri.Malloc(numTufts * sizeof(gcTuft_t));
    cell->verts   = ri.Malloc(numTufts * GC_VERTS_TUFT * sizeof(srfVert_t));
    cell->indexes = ri.Malloc(numTufts * GC_INDEXES_TUFT * sizeof(glIndex_t));

    for (n = 0; n < numTufts; n++) {
        gcTuft_t    *tuft = &cell->tufts[n];
        unsigned int rnd  = GC_Hash((unsigned int)(tufts[n].tuft.root[0] * 13.0f) ^ GC_Hash((unsigned int)(tufts[n].tuft.root[1] * 7.0f)));
        float        h    = (8.0f + 14.0f * GC_Rand(&rnd)) * scale;
        float        yaw  = GC_Rand(&rnd) * M_PI;
        float        halfw = h * (0.55f + 0.25f * GC_Rand(&rnd));
        float        s0   = GC_Rand(&rnd) < 0.5f ? 0.0f : 0.5f;
        vec3_t       lean, base, top;
        int          q;

        *tuft = tufts[n].tuft;
        VectorSet(lean, (GC_Rand(&rnd) - 0.5f) * 0.3f * h, (GC_Rand(&rnd) - 0.5f) * 0.3f * h, 0);
        VectorScale(tufts[n].color, 0.55f, base);
        VectorCopy(tufts[n].color, top);

        for (q = 0; q < GC_QUADS_TUFT; q++) {
            float      a  = yaw + q * (M_PI / GC_QUADS_TUFT);
            vec3_t     dir = { cos(a) * halfw, sin(a) * halfw, 0 };
            srfVert_t *v   = &cell->verts[(n * GC_QUADS_TUFT + q) * 4];
            glIndex_t *ix  = &cell->indexes[(n * GC_QUADS_TUFT + q) * 6];
            glIndex_t  first = (n * GC_QUADS_TUFT + q) * 4;
            vec3_t     p;

            // 0, 1 on the ground; 2, 3 at the tip
            VectorSubtract(tuft->root, dir, p);
            GC_SetVert(&v[0], p, s0, 0.98f, base);
            VectorAdd(tuft->root, dir, p);
            GC_SetVert(&v[1], p, s0 + 0.5f, 0.98f, base);
            VectorAdd(tuft->root, dir, p);
            VectorAdd(p, lean, p);
            p[2] += h;
            GC_SetVert(&v[2], p, s0 + 0.5f, 0.02f, top);
            VectorSubtract(tuft->root, dir, p);
            VectorAdd(p, lean, p);
            p[2] += h;
            GC_SetVert(&v[3], p, s0, 0.02f, top);

            ix[0] = first + 0;
            ix[1] = first + 1;
            ix[2] = first + 2;
            ix[3] = first + 0;
            ix[4] = first + 2;
            ix[5] = first + 3;
        }
    }
}

/*
=============================================================================

Adding to the view

=============================================================================
*/

static float GC_DistanceToBounds(const vec3_t p, vec3_t bounds[2])
{
    vec3_t d;
    int    i;

    for (i = 0; i < 3; i++) {
        d[i] = p[i] < bounds[0][i] ? bounds[0][i] - p[i] : (p[i] > bounds[1][i] ? p[i] - bounds[1][i] : 0);
    }
    return VectorLength(d);
}

static void GC_EvictFarCells(const vec3_t origin, float range)
{
    gcCell_t **link = &gc.builtList;

    while (*link) {
        gcCell_t *cell = *link;

        // never one a frame still in the back end may draw
        if (cell->lastFrame < tr.frameCount - 1
            && GC_DistanceToBounds(origin, cell->bounds) > range + GC_CELL_SIZE * 2) {
            *link = cell->nextBuilt;
            GC_FreeCell(cell);
            gc.numBuilt--;
            continue;
        }
        link = &cell->nextBuilt;
    }
}

void R_AddGroundCoverSurfaces(void)
{
    const float *origin;
    float        range;
    double       buildStart;
    int          range4[4];
    int          x, y;
    vec3_t       mins, maxs;

    if (!gc.active || !r_groundCover->integer) {
        return;
    }
    if (tr.viewParms.isPortalSky
        || (tr.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP | VPF_RTBAKED | VPF_RTSTATIC | VPF_RTDYNAMIC))) {
        return;
    }

    // a new size or density: make the cells again
    if (gc.density != r_groundCoverDensity->value || gc.height != r_groundCoverHeight->value
        || gc.brightness != r_groundCoverBrightness->value || gc.mask != r_groundCoverMask->integer) {
        if (gc.builtList) {
            R_IssuePendingRenderCommands();
        }
        GC_FreeAllCells();
        gc.density = r_groundCoverDensity->value;
        gc.height  = r_groundCoverHeight->value;
        gc.brightness = r_groundCoverBrightness->value;
        gc.mask    = r_groundCoverMask->integer;
    }

    // world space; the static models added before this leave theirs set
    tr.currentEntityNum = REFENTITYNUM_WORLD;
    tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;

    if (gc.drawsFrame != tr.frameCount) {
        gc.drawsFrame = tr.frameCount;
        gc.numDraws   = 0;
    }

    origin = tr.viewParms.ori.origin;
    if (!tr.viewParms.isPortal) {
        VectorCopy(origin, gc.lastView);
        gc.lastViewSet = qtrue;
    }
    range  = Com_Clamp(128, 8192, r_groundCoverDistance->value);

    VectorSet(mins, origin[0] - range, origin[1] - range, origin[2]);
    VectorSet(maxs, origin[0] + range, origin[1] + range, origin[2]);
    GC_CellRange(mins, maxs, range4);

    buildStart = R_MicroSeconds();

    for (y = range4[1]; y <= range4[3]; y++) {
        for (x = range4[0]; x <= range4[2]; x++) {
            int               cellIndex = y * gc.cellsX + x;
            gcCell_t         *cell = &gc.cells[cellIndex];
            srfGroundCover_t *draw;
            float             dist, lo, hi;
            int               count;

            if (!cell->numRefs) {
                continue;
            }
            dist = GC_DistanceToBounds(origin, cell->bounds);
            if (dist >= range) {
                continue;
            }
            if (R_CullBox(cell->bounds) == CULL_OUT) {
                continue;
            }

            if (!cell->built) {
                // a few a frame; the rest come in over the next frames
                if (R_MicroSeconds() - buildStart > GC_BUILD_BUDGET_US) {
                    continue;
                }
                GC_BuildCell(cell, cellIndex);
            }
            cell->lastFrame = tr.frameCount;

            // the tufts still standing this far out: a prefix
            lo = 0;
            hi = cell->numTufts;
            while (lo < hi) {
                int mid = (int)(lo + hi) / 2;
                if (cell->tufts[mid].fadeEnd * range > dist) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            count = (int)lo;
            if (!count || gc.numDraws >= GC_MAX_DRAWS) {
                continue;
            }

            draw              = &gc.draws[gc.numDraws++];
            draw->surfaceType = SF_GROUNDCOVER;
            draw->cell        = cell;
            draw->numTufts    = count;
            R_AddDrawSurf(&draw->surfaceType, gc.shader, 0, 0, 0, 0);
        }
    }

    GC_EvictFarCells(origin, range);
}

/*
=============================================================================

Drawing (back end)

=============================================================================
*/

void RB_SurfaceGroundCover(void *surface)
{
    const srfGroundCover_t *draw = (const srfGroundCover_t *)surface;
    const gcCell_t         *cell = draw->cell;
    const float            *view = backEnd.viewParms.ori.origin;
    const float             range = Com_Clamp(128, 8192, r_groundCoverDistance->value);
    const float             wind = r_groundCoverWind->value;
    const vec3_t            windDir = { 0.8f, 0.6f, 0 };
    float                   time = (float)fmod(backEnd.refdef.floatTime, 3600.0);
    int                     numVerts = draw->numTufts * GC_VERTS_TUFT;
    int                     firstVert;
    int                     n, i;

    gc.backEndDraws++;
    gc.backEndTufts += draw->numTufts;
    RB_SurfaceGroundCoverVerts(numVerts, cell->verts, draw->numTufts * GC_INDEXES_TUFT, cell->indexes);
    firstVert = tess.numVertexes - numVerts;

    for (n = 0; n < draw->numTufts; n++) {
        const gcTuft_t *tuft = &cell->tufts[n];
        float          *xyz  = tess.xyz[firstVert + n * GC_VERTS_TUFT];
        float           end  = tuft->fadeEnd * range;
        float           d, sink, sway, gust;
        vec3_t          offset;

        // sinks into the ground over the last fifth of its distance
        d    = Distance(tuft->root, view);
        sink = Com_Clamp(0, 1, (end - d) / (end * 0.2f));

        gust = 0.6f + 0.4f * sin(time * 0.31f + tuft->root[0] * 0.0021f + tuft->root[1] * 0.0013f);
        sway = sin(time * 1.9f + tuft->phase + (tuft->root[0] + tuft->root[1]) * 0.011f) * 0.65f
             + sin(time * 3.7f + tuft->phase * 1.7f) * 0.35f;
        VectorScale(windDir, (sway * 2.2f + 1.2f) * gust * tuft->flex * wind * sink, offset);

        for (i = 0; i < GC_VERTS_TUFT; i++, xyz += 4) {
            if (sink < 1.0f) {
                xyz[0] = tuft->root[0] + (xyz[0] - tuft->root[0]) * sink;
                xyz[1] = tuft->root[1] + (xyz[1] - tuft->root[1]) * sink;
                xyz[2] = tuft->root[2] + (xyz[2] - tuft->root[2]) * sink;
            }
            // the tips
            if ((i & 3) >= 2) {
                VectorAdd(xyz, offset, xyz);
            }
        }
    }
}

void R_GroundCoverInfo_f(void)
{
    int built = 0, tufts = 0, bytes = 0;
    const gcCell_t *cell;

    if (!gc.active) {
        ri.Printf(PRINT_ALL, "Ground cover: off on this map\n");
        return;
    }
    for (cell = gc.builtList; cell; cell = cell->nextBuilt) {
        built++;
        tufts += cell->numTufts;
        bytes += cell->numTufts * (sizeof(gcTuft_t) + GC_VERTS_TUFT * sizeof(srfVert_t) + GC_INDEXES_TUFT * sizeof(glIndex_t));
    }
    ri.Printf(PRINT_ALL, "Ground cover: %d grass shaders, %d triangles, %d terrain patches\n",
        gc.numGrassShaders, gc.numTris, gc.numTerrain);
    ri.Printf(PRINT_ALL, "  %d of %d cells made, %d tufts, %d KB; %d cells added last frame\n",
        built, gc.cellsX * gc.cellsY, tufts, bytes / 1024, gc.numDraws);
    ri.Printf(PRINT_ALL, "  samples: %d kept; dropped %d outside, %d steep, %d bare texture, %d covered\n",
        gc_kept, gc_dropOutside, gc_dropSlope, gc_dropMask, gc_dropCovered);
    ri.Printf(PRINT_ALL, "  drawn since the last report: %d cells, %d tufts\n", gc.backEndDraws, gc.backEndTufts);
    gc.backEndDraws = gc.backEndTufts = 0;

    // the tuft nearest the view
    if (gc.lastViewSet) {
        const gcTuft_t *best = NULL;
        const srfVert_t *bestVert = NULL;
        float           bestDist = 1e30f;
        int             i;

        for (cell = gc.builtList; cell; cell = cell->nextBuilt) {
            for (i = 0; i < cell->numTufts; i++) {
                float d = Distance(cell->tufts[i].root, gc.lastView);
                if (d < bestDist) {
                    bestDist = d;
                    best     = &cell->tufts[i];
                    bestVert = &cell->verts[i * GC_VERTS_TUFT + 2];
                }
            }
        }
        ri.Printf(PRINT_ALL, "  view (%.0f %.0f %.0f)", gc.lastView[0], gc.lastView[1], gc.lastView[2]);
        if (best) {
            ri.Printf(PRINT_ALL, ", nearest tuft at (%.0f %.0f %.0f), %.0f away, reaches %.0f",
                best->root[0], best->root[1], best->root[2], bestDist,
                best->fadeEnd * Com_Clamp(128, 8192, r_groundCoverDistance->value));
            {
                trace_t tr0;
                vec3_t  s0, e0;
                VectorSet(s0, best->root[0], best->root[1], best->root[2] + 400);
                VectorSet(e0, best->root[0], best->root[1], best->root[2] - 400);
                ri.CM_BoxTrace(&tr0, s0, e0, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);
                ri.Printf(PRINT_ALL, ", ground below it at %.0f, tip colour %.2f %.2f %.2f", tr0.endpos[2],
                    bestVert->color[0] / 65535.0f, bestVert->color[1] / 65535.0f, bestVert->color[2] / 65535.0f);
            }
        }
        ri.Printf(PRINT_ALL, "\n");
        {
            int cx = (int)floor((gc.lastView[0] - gc.originX) / GC_CELL_SIZE);
            int cy = (int)floor((gc.lastView[1] - gc.originY) / GC_CELL_SIZE);
            if (cx >= 0 && cy >= 0 && cx < gc.cellsX && cy < gc.cellsY) {
                const gcCell_t *c = &gc.cells[cy * gc.cellsX + cx];
                ri.Printf(PRINT_ALL, "  view's cell %d,%d: %d sources, %s, %d tufts, bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f), last frame %d of %d\n",
                    cx, cy, c->numRefs, c->built ? "made" : "not made", c->numTufts,
                    c->bounds[0][0], c->bounds[0][1], c->bounds[0][2], c->bounds[1][0], c->bounds[1][1], c->bounds[1][2],
                    c->lastFrame, tr.frameCount);
            }
        }
    }
}
