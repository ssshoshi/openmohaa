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
// Not only grass: each tuft is one kind of what grows (gcKind_t), a cell of
// the one texture (groundcover_cover.tga, tools/groundcover/grass.py), so the
// kinds cost nothing more to draw. Weeds and flowers grow in patches among
// the grass, reeds at a water's edge (the tops of the map's water brushes),
// dead grass through snow, dry scrub on stony and sandy ground: each ground
// as the map's shaders name it (surfaceparm snow, gravel, sand), terrain
// only, for those grounds are also roofs and beams. How much of each, and on
// which ground, is the map's atmosphere file's (`cover`, tr_atmos.c).
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
// A cell's tufts go to the card once, when it is made (GC_UploadCell): each
// vertex carries where its tuft stands, its phase and how far it reaches, and
// the vertex program sways it and sinks it (lightall's USE_GROUNDCOVER). A
// frame then copies nothing, however much grass is in view, and a cell is a
// draw from its buffer in each pass. Without buffer storage, or with
// r_groundCoverGPU 0, they go through tess as before, swayed and sunk on the
// CPU in each pass (RB_SurfaceGroundCover); the vertex cache keys on index
// arrays that a cell frees and another reuses, so never through that.

#include "tr_local.h"

#define GC_CELL_SIZE        256
#define GC_MAX_TUFTS_CELL   ((SHADER_MAX_VERTEXES - 8) / GC_VERTS_TUFT)
#define GC_QUADS_TUFT       2
#define GC_VERTS_TUFT       (GC_QUADS_TUFT * 4)
#define GC_INDEXES_TUFT     (GC_QUADS_TUFT * 6)
#define GC_MAX_DRAWS        4096
#define GC_MASK_SIZE        64
#define GC_MIN_NORMAL_Z     0.7f
#define GC_MIN_NORMAL_Z_PATCH 0.5f // the bocage's hills are steeper than its fields
#define GC_BUILD_BUDGET_US  2000.0

// on the card: a cell's tufts in pages, the pages in chunks of a buffer each
#define GC_PAGE_TUFTS       16
#define GC_PAGE_VERTS       (GC_PAGE_TUFTS * GC_VERTS_TUFT)
#define GC_CHUNK_PAGES      4096
#define GC_MAX_CHUNKS       32
#define GC_MAX_BATCH        256 // cells drawn in one batch

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
    int    patch;  // of a curved patch: not solid, the player walks on clip under it
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
    int      ground;   // gcGround_t
    qboolean solid;    // its brushes are solid (a patch need not be)
    qboolean split;    // groundcover/shaders.txt's `grow`: only the greener part of its texture
    int      kept, dropMask, dropCovered; // samples on it so far (groundcoverinfo)
} gcShader_t;

typedef struct {
    vec3_t root;
    float  fadeEnd;  // the distance it is gone at
    float  phase;
    float  flex;     // how far its tip sways
} gcTuft_t;

// a tuft's vertex on the card
typedef struct {
    vec3_t  xyz;
    vec2_t  st;
    byte    color[4];
    int16_t normal[4];
    vec4_t  root;  // where the tuft stands; w: how far this vertex sways (0 on the ground)
    vec2_t  sway;  // its phase, and the fraction of the draw distance it reaches
} gcGpuVert_t;

typedef struct {
    vao_t vao;
    byte  used[GC_CHUNK_PAGES];
    int   numUsed;
} gcChunk_t;

// a tuft while its cell is made
typedef struct {
    gcTuft_t tuft;
    vec3_t   color;
    int      kind;
} gcBuildTuft_t;

// a water surface's top, seen from above: reeds grow at its edge
typedef struct {
    float x0, y0, x1, y1, z;
} gcWater_t;

#define GC_MAX_WATER 256

// how each kind stands: its height (units, from h0 to h0 + h1), its half
// width of its height, and how far it sways (of a grass tuft's)
static const struct {
    float h0, h1, w0, w1, flex;
} gcKindShape[GC_NUM_KINDS] = {
    { 8, 14, 0.55f, 0.25f, 1.0f },  // grass
    { 8, 14, 0.55f, 0.25f, 1.0f },  // grass, another clump
    { 6, 7, 0.75f, 0.20f, 0.5f },   // weeds: low and broad
    { 9, 12, 0.55f, 0.25f, 1.0f },  // grass in flower
    { 5, 8, 0.60f, 0.25f, 0.8f },   // dead grass, half buried
    { 9, 11, 0.80f, 0.20f, 0.35f }, // dry scrub: stiff twigs
    { 30, 24, 0.38f, 0.12f, 1.8f }, // reeds: tall and narrow
    { 8, 14, 0.55f, 0.25f, 1.0f },  // dry grass
};

typedef struct gcCell_s {
    int    firstRef;
    int    numRefs;
    vec3_t bounds[2];

    // made when it comes in range
    qboolean          built;
    int               numTufts;
    gcTuft_t         *tufts;    // by fadeEnd, farthest first
    srfVert_t        *verts;    // for tess; none once on the card
    glIndex_t        *indexes;
    int               chunk;    // on the card: numPages from firstPage of gc.chunks[chunk]
    int               firstPage;
    int               numPages;
    int               lastFrame;
    struct gcCell_s  *nextBuilt;
} gcCell_t;

// one cell drawn in one view
typedef struct {
    surfaceType_t surfaceType;
    gcCell_t     *cell;
    int           numTufts;
    float         dist;
} srfGroundCover_t;

static struct {
    qboolean    active;
    shader_t   *shader;

    int         numLightmaps;
    byte       *lightmaps;  // the map's, as in the file: LIGHTMAP_SIZE^2 RGB each

    int         numShaders;
    int        *shaderSlot; // file shader -> slot in shaders, or -1
    vec3_t      grassAlbedo; // the mean colour of the tufts' texture
    qboolean    atlas;       // the texture has every kind (groundcover/cover), not two clumps of grass
    float       groundDensity[GC_NUM_GROUNDS];
    float       share[GC_NUM_GROUNDS][GC_NUM_KINDS];
    gcWater_t   water[GC_MAX_WATER];
    int         numWater;
    int         kindCount[GC_NUM_KINDS]; // tufts of each kind made so far (groundcoverinfo)
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
    qboolean    gpu;

    qboolean    gpuShader; // the shader is one the vertex program can sway (GC_CheckGpuShader)
    const char *gpuWhyNot; // and why not, when it isn't
    GLuint      indexBuffer; // the tufts' indexes, the same in every cell
    gcChunk_t  *chunks[GC_MAX_CHUNKS];
    int         numChunks;

    // the back end's batch of cells on the card, all in one chunk
    int         batchChunk;
    int         batchSize;
    GLsizei     batchCounts[GC_MAX_BATCH];
    GLint       batchBases[GC_MAX_BATCH];
    qboolean    lastViewSet;
    vec3_t      lastView;

    int         backEndDraws, backEndTufts; // drawn, since the last report

    srfGroundCover_t draws[GC_MAX_DRAWS];
    int              numDraws;
    int              drawsFrame;
} gc;

// the grass's material maps, while the card draws it without them (GC_UseCard)
static struct {
    qboolean        saved;
    textureBundle_t normal, specular, deluxe;
    int             index;
    vec4_t          normalScale, specularScale;
} gcMaterial;

static void GC_StripMaterialMaps(void);
static void GC_RestoreMaterialMaps(void);
static void GC_UseCard(qboolean card);
static void GC_CheckGpuShader(void);

// why the samples of the cells made so far were dropped (groundcoverinfo)
static int gc_dropOutside, gc_dropSlope, gc_dropMask, gc_dropCovered, gc_kept;

static cvar_t *r_groundCover;
static cvar_t *r_groundCoverDensity;
static cvar_t *r_groundCoverDistance;
static cvar_t *r_groundCoverHeight;
static cvar_t *r_groundCoverWind;
static cvar_t *r_groundCoverPush;
static cvar_t *r_groundCoverBrightness;
static cvar_t *r_groundCoverMask;
static cvar_t *r_groundCoverGPU;
static cvar_t *r_groundCoverKinds;

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
    r_groundCoverPush = ri.Cvar_Get("r_groundCoverPush", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_groundCoverPush, "The grass gives way round the feet of whoever stands in it, the player and the soldiers near: 0 off, 1 as wide as a man, up to 3");
    // upright blades catch less of the light than the ground they stand on
    r_groundCoverBrightness = ri.Cvar_Get("r_groundCoverBrightness", "0.85", CVAR_ARCHIVE);
    // 1: no grass where the ground's texture is not green (paths, dirt)
    r_groundCoverMask = ri.Cvar_Get("r_groundCoverMask", "1", CVAR_ARCHIVE);
    // 1: the tufts are kept on the card and swayed by the vertex program;
    // 0: copied and swayed on the CPU in each pass, as before
    r_groundCoverGPU = ri.Cvar_Get("r_groundCoverGPU", "1", CVAR_ARCHIVE);
    r_groundCoverKinds = ri.Cvar_Get("r_groundCoverKinds", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_groundCoverKinds, "What grows besides grass: weeds and flowers in the grass, reeds by water, dead grass through snow, scrub on the desert's stony ground, as each map's atmosphere file has it (cover). 0: grass on grass ground only, as before. Takes a map load");
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
    if (cell->numPages) {
        gcChunk_t *chunk = gc.chunks[cell->chunk];

        Com_Memset(chunk->used + cell->firstPage, 0, cell->numPages);
        chunk->numUsed -= cell->numPages;
        cell->numPages = 0;
    }
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

static void GC_AbandonBuild(void);

static void GC_FreeAllCells(void)
{
    gcCell_t *cell;

    // a cell part sampled is of the cells about to go
    GC_AbandonBuild();

    for (cell = gc.builtList; cell; cell = cell->nextBuilt) {
        GC_FreeCell(cell);
    }
    gc.builtList = NULL;
    gc.numBuilt  = 0;
}

static void GC_FreeChunks(void)
{
    int i;

    for (i = 0; i < gc.numChunks; i++) {
        vao_t *vao = &gc.chunks[i]->vao;

        if (glState.currentVao == vao) {
            R_BindNullVao();
        }
        qglDeleteVertexArrays(1, &vao->vao);
        qglDeleteBuffers(1, &vao->vertexesVBO);
        ri.Free(gc.chunks[i]);
        gc.chunks[i] = NULL;
    }
    gc.numChunks = 0;

    if (gc.indexBuffer) {
        qglDeleteBuffers(1, &gc.indexBuffer);
        gc.indexBuffer = 0;
    }
}

void R_GroundCoverFree(void)
{
    // the back end may still be drawing last frame's cells
    if (gc.builtList || gc.numChunks) {
        R_IssuePendingRenderCommands();
    }

    GC_FreeAllCells();
    GC_FreeChunks();

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
        // of the grass only: the first two cells of the kinds' texture
        const int w = gc.atlas ? width / 2 : width, h = gc.atlas ? height / 2 : height;
        int       x, y;

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                i = y * width + x;
                float a = pic[i * 4 + 3] / 255.0f;
                sum[0] += pic[i * 4 + 0] * a;
                sum[1] += pic[i * 4 + 1] * a;
                sum[2] += pic[i * 4 + 2] * a;
                weight += a;
            }
        }
        if (weight > 0) {
            VectorSet(gc.grassAlbedo, sum[0] / weight, sum[1] / weight, sum[2] / weight);
        }
    }
    ri.Free(pic);
}

// What grows on each ground: the renderer's own mix, and the map's
// atmosphere file over it (R_AtmosCover).
static void GC_SetupGrounds(void)
{
    static const float density[GC_NUM_GROUNDS] = { 1.0f, 0.5f, 0.0f, 0.0f, 1.0f };
    int                g, k;

    Com_Memset(gc.share, 0, sizeof(gc.share));
    gc.share[GC_GROUND_GRASS][GC_KIND_WEEDS]   = 0.06f;
    gc.share[GC_GROUND_GRASS][GC_KIND_FLOWERS] = 0.03f;
    gc.share[GC_GROUND_GRASS][GC_KIND_REEDS]   = 0.8f;
    gc.share[GC_GROUND_SNOW][GC_KIND_DEAD]     = 0.85f;
    gc.share[GC_GROUND_SNOW][GC_KIND_DRY]      = 0.15f;
    gc.share[GC_GROUND_GRAVEL][GC_KIND_SCRUB]  = 0.45f;
    gc.share[GC_GROUND_GRAVEL][GC_KIND_DRY]    = 0.55f;
    gc.share[GC_GROUND_SAND][GC_KIND_SCRUB]    = 0.3f;
    gc.share[GC_GROUND_SAND][GC_KIND_DRY]      = 0.7f;
    gc.share[GC_GROUND_MUD][GC_KIND_REEDS]     = 0.8f;

    for (g = 0; g < GC_NUM_GROUNDS; g++) {
        const atmosCover_t *c = R_AtmosCover(g);

        gc.groundDensity[g] = density[g];
        if (!c->set) {
            continue;
        }
        gc.groundDensity[g] = c->density;
        for (k = 0; k < GC_NUM_KINDS; k++) {
            if (c->share[k] >= 0.0f) {
                gc.share[g][k] = c->share[k];
            }
        }
    }

    // the old texture is grass and nothing else; mud is reeds at water,
    // and a map with no water has none
    if (!gc.numWater) {
        gc.groundDensity[GC_GROUND_MUD] = 0.0f;
    }
    if (!gc.atlas || !r_groundCoverKinds->integer) {
        for (g = GC_GROUND_GRASS + 1; g < GC_NUM_GROUNDS; g++) {
            gc.groundDensity[g] = 0.0f;
        }
    }
}

// The ground a shader is, or -1: grass (brushes and terrain) and, of the
// others, only those the map grows something on.
static int GC_ShaderGround(int surfaceFlags)
{
    static const int flags[GC_NUM_GROUNDS] = { SURF_GRASS, SURF_SNOW, SURF_GRAVEL, SURF_SAND, SURF_MUD | SURF_DIRT };
    int              g;

    for (g = 0; g < GC_NUM_GROUNDS; g++) {
        if ((surfaceFlags & flags[g]) && gc.groundDensity[g] > 0.0f) {
            return g;
        }
    }
    return -1;
}

static int GC_GroundFlag(int ground)
{
    static const int flags[GC_NUM_GROUNDS] = { SURF_GRASS, SURF_SNOW, SURF_GRAVEL, SURF_SAND, SURF_MUD | SURF_DIRT };
    return flags[ground];
}

// The tops of the map's water: faces of water brushes that look up.
static void GC_GatherWater(const dshader_t *dshaders, const dsurface_t *dsurfs, int numSurfs, const drawVert_t *dverts, int numVerts)
{
    int i, k;

    gc.numWater = 0;
    for (i = 0; i < numSurfs && gc.numWater < GC_MAX_WATER; i++) {
        const dsurface_t *ds        = &dsurfs[i];
        int               shaderNum = LittleLong(ds->shaderNum);
        int               firstVert = LittleLong(ds->firstVert);
        int               nVerts    = LittleLong(ds->numVerts);
        gcWater_t        *w;
        float             zsum = 0;

        if (LittleLong(ds->surfaceType) != MST_PLANAR || shaderNum < 0 || shaderNum >= gc.numShaders) {
            continue;
        }
        if (!(LittleLong(dshaders[shaderNum].contentFlags) & CONTENTS_WATER)) {
            continue;
        }
        if (nVerts < 3 || firstVert < 0 || firstVert + nVerts > numVerts || LittleFloat(dverts[firstVert].normal[2]) < 0.9f) {
            continue;
        }
        w     = &gc.water[gc.numWater];
        w->x0 = w->y0 = 1e9f;
        w->x1 = w->y1 = -1e9f;
        for (k = 0; k < nVerts; k++) {
            const float *xyz = dverts[firstVert + k].xyz;
            w->x0 = MIN(w->x0, LittleFloat(xyz[0]));
            w->y0 = MIN(w->y0, LittleFloat(xyz[1]));
            w->x1 = MAX(w->x1, LittleFloat(xyz[0]));
            w->y1 = MAX(w->y1, LittleFloat(xyz[1]));
            zsum += LittleFloat(xyz[2]);
        }
        w->z = zsum / nVerts;
        gc.numWater++;
    }
}

// How much at a water's edge a place is, 0 to 1: from a little under the
// water's level to `above` over it, at or near one of the map's water tops.
static float GC_WaterEdge(const vec3_t at, float above)
{
    float best = 0;
    int   i;

    for (i = 0; i < gc.numWater; i++) {
        const gcWater_t *w = &gc.water[i];
        float            dz, f;

        if (at[0] < w->x0 - 48 || at[0] > w->x1 + 48 || at[1] < w->y0 - 48 || at[1] > w->y1 + 48) {
            continue;
        }
        dz = at[2] - w->z;
        f  = Com_Clamp(0, 1, (above - dz) / 10.0f) * Com_Clamp(0, 1, (dz + 22.0f) / 8.0f);
        best = MAX(best, f);
    }
    return best;
}

// groundcover/shaders.txt (the ground cover's pak): the shaders that are not
// what their surfaceparm says, a line each:
//     bare <shader>   never grows anything (the top of a hedge flagged grass)
//     grow <shader>   is grass ground whatever its flags; only the greener of
//                     the two parts of its texture grows (a rock and grass blend)
enum { GC_RULE_NONE, GC_RULE_BARE, GC_RULE_GROW };
#define GC_MAX_RULES 256

static struct {
    char name[MAX_QPATH];
    int  rule;
} gcRules[GC_MAX_RULES];
static int gcNumRules;

static void GC_LoadRules(void)
{
    union {
        char *c;
        void *v;
    } buffer;
    char *text;

    gcNumRules = 0;
    if (ri.FS_ReadFile("groundcover/shaders.txt", &buffer.v) <= 0) {
        return;
    }
    text = buffer.c;
    for (;;) {
        char        kind[16];
        const char *token = COM_ParseExt(&text, qtrue);

        if (!token[0]) {
            break;
        }
        Q_strncpyz(kind, token, sizeof(kind));
        token = COM_ParseExt(&text, qfalse);
        if (!token[0] || gcNumRules >= GC_MAX_RULES) {
            continue;
        }
        if (!Q_stricmp(kind, "bare") || !Q_stricmp(kind, "grow")) {
            Q_strncpyz(gcRules[gcNumRules].name, token, MAX_QPATH);
            gcRules[gcNumRules].rule = !Q_stricmp(kind, "bare") ? GC_RULE_BARE : GC_RULE_GROW;
            gcNumRules++;
        } else {
            ri.Printf(PRINT_WARNING, "groundcover/shaders.txt: '%s' is not bare or grow\n", kind);
        }
    }
    ri.FS_FreeFile(buffer.v);
}

static int GC_Rule(const char *shader)
{
    int i;

    for (i = 0; i < gcNumRules; i++) {
        if (!Q_stricmp(gcRules[i].name, shader)) {
            return gcRules[i].rule;
        }
    }
    return GC_RULE_NONE;
}

// One triangle of grass ground, if it looks up enough: counted in the first
// pass, copied in the second.
static void GC_AddTri(int pass, const drawVert_t *v[3], int lightmap, int slot, qboolean patch)
{
    vec3_t e1, e2, n, vn;
    int    k;

    VectorSubtract(v[1]->xyz, v[0]->xyz, e1);
    VectorSubtract(v[2]->xyz, v[0]->xyz, e2);
    CrossProduct(e1, e2, n);
    if (VectorNormalize(n) < 0.001f) {
        return;
    }
    // the winding is either way; the vertex normals say which is up
    VectorAdd(v[0]->normal, v[1]->normal, vn);
    VectorAdd(vn, v[2]->normal, vn);
    if (DotProduct(n, vn) < 0) {
        VectorNegate(n, n);
    }
    if (n[2] < (patch ? GC_MIN_NORMAL_Z_PATCH : GC_MIN_NORMAL_Z)) {
        return;
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
        tri->lightmap = lightmap;
        tri->shader   = slot;
        tri->patch    = patch;
    }
    gc.numTris++;
}

#define GC_PATCH_STEPS 6 // triangles' rows across each 3x3 piece of a patch

// A curved patch (the bocage's hills): each 3x3 piece of its control grid
// evaluated as the renderer does, a quadratic Bezier each way, into small
// triangles.
static void GC_AddPatch(int pass, const drawVert_t *cp, int width, int height, int numVerts, int lightmap, int slot)
{
    drawVert_t        grid[GC_PATCH_STEPS + 1][GC_PATCH_STEPS + 1];
    int               px, py, a, b, i, j;

    if (width < 3 || height < 3 || !(width & 1) || !(height & 1) || width * height > numVerts) {
        return;
    }
    for (py = 0; py + 2 < height; py += 2) {
        for (px = 0; px + 2 < width; px += 2) {
            for (b = 0; b <= GC_PATCH_STEPS; b++) {
                const float t = b / (float)GC_PATCH_STEPS;
                const float wt[3] = { (1 - t) * (1 - t), 2 * t * (1 - t), t * t };

                for (a = 0; a <= GC_PATCH_STEPS; a++) {
                    const float s = a / (float)GC_PATCH_STEPS;
                    const float ws[3] = { (1 - s) * (1 - s), 2 * s * (1 - s), s * s };
                    drawVert_t *out = &grid[b][a];

                    Com_Memset(out, 0, sizeof(*out));
                    for (j = 0; j < 3; j++) {
                        for (i = 0; i < 3; i++) {
                            const drawVert_t *c = &cp[(py + j) * width + px + i];
                            const float       w = ws[i] * wt[j];

                            VectorMA(out->xyz, w, c->xyz, out->xyz);
                            VectorMA(out->normal, w, c->normal, out->normal);
                            out->st[0] += w * LittleFloat(c->st[0]);
                            out->st[1] += w * LittleFloat(c->st[1]);
                            out->lightmap[0] += w * LittleFloat(c->lightmap[0]);
                            out->lightmap[1] += w * LittleFloat(c->lightmap[1]);
                        }
                    }
                }
            }
            for (b = 0; b < GC_PATCH_STEPS; b++) {
                for (a = 0; a < GC_PATCH_STEPS; a++) {
                    const drawVert_t *t0[3] = { &grid[b][a], &grid[b][a + 1], &grid[b + 1][a + 1] };
                    const drawVert_t *t1[3] = { &grid[b][a], &grid[b + 1][a + 1], &grid[b + 1][a] };

                    GC_AddTri(pass, t0, lightmap, slot, qtrue);
                    GC_AddTri(pass, t1, lightmap, slot, qtrue);
                }
            }
        }
    }
}

/*
================
R_GroundCoverLoadWorld

From the BSP as it is in the file, before the renderer has packed its
lightmaps into atlases.
================
*/
static void GC_LoadShaderMask(int slot);

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
    int              *grounds;

    R_GroundCoverFree();
    gc_dropOutside = gc_dropSlope = gc_dropMask = gc_dropCovered = gc_kept = 0;

    if (!r_groundCover->integer) {
        return;
    }

    // every kind in one texture; a pak from before the kinds has only grass
    gc.atlas  = qtrue;
    gc.shader = R_FindShader("groundcover/cover", LIGHTMAP_NONE, qtrue);
    if (gc.shader->defaultShader) {
        gc.atlas  = qfalse;
        gc.shader = R_FindShader("groundcover/grass", LIGHTMAP_NONE, qtrue);
    }
    if (gc.shader->defaultShader) {
        ri.Printf(PRINT_WARNING, "Ground cover: no groundcover/grass shader (zzzzzzzzz-opm-groundcover.pk3)\n");
        Com_Memset(&gc, 0, sizeof(gc));
        return;
    }

    GC_MeasureGrassAlbedo();
    // whether the card could draw it without its material maps; it keeps them
    // until it does (GC_UseCard)
    Com_Memset(&gcMaterial, 0, sizeof(gcMaterial));
    GC_StripMaterialMaps();
    GC_CheckGpuShader();
    GC_RestoreMaterialMaps();

    dshaders       = (const dshader_t *)(fileBase + shaderLump->fileofs);
    gc.numShaders  = shaderLump->filelen / sizeof(dshader_t);
    gc.shaderSlot  = ri.Malloc(MAX(gc.numShaders, 1) * sizeof(int));
    gc.shaderNames = ri.Malloc(MAX(gc.numShaders, 1) * sizeof(*gc.shaderNames));

    dsurfs   = (const dsurface_t *)(fileBase + surfLump->fileofs);
    numSurfs = surfLump->filelen / sizeof(dsurface_t);
    dverts   = (const drawVert_t *)(fileBase + vertLump->fileofs);
    numVerts = vertLump->filelen / sizeof(drawVert_t);

    // the water's tops, for reeds: before the grounds, for a map with no
    // water grows nothing on its mud
    if (gc.atlas && r_groundCoverKinds->integer) {
        GC_GatherWater(dshaders, dsurfs, numSurfs, dverts, numVerts);
    }
    GC_SetupGrounds();

    GC_LoadRules();
    grounds = ri.Malloc(MAX(gc.numShaders, 1) * sizeof(int));
    for (i = 0; i < gc.numShaders; i++) {
        int surfaceFlags = LittleLong(dshaders[i].surfaceFlags);
        int contents     = LittleLong(dshaders[i].contentFlags);
        int ground       = GC_ShaderGround(surfaceFlags);
        int rule         = GC_Rule(dshaders[i].shader);

        gc.shaderSlot[i] = -1;
        if (rule == GC_RULE_BARE) {
            continue;
        }
        if (rule == GC_RULE_GROW && !(surfaceFlags & (SURF_NODRAW | SURF_SKY))) {
            ground = GC_GROUND_GRASS;
        }
        if (ground < 0 || (surfaceFlags & (SURF_NODRAW | SURF_SKY))) {
            continue;
        }
        // grass the player walks through is foliage, not ground; one not
        // solid at all is taken for its curved patches only (the bocage's
        // hills: the player walks on clip brushes under them)
        if (contents & CONTENTS_FENCE) {
            continue;
        }
        gc.shaderSlot[i] = gc.numGrassShaders;
        grounds[gc.numGrassShaders] = ((contents & CONTENTS_SOLID) ? ground : ground | 0x100) | (rule == GC_RULE_GROW ? 0x200 : 0);
        Q_strncpyz(gc.shaderNames[gc.numGrassShaders], dshaders[i].shader, MAX_QPATH);
        gc.numGrassShaders++;
    }

    if (!gc.numGrassShaders) {
        ri.Free(grounds);
        R_GroundCoverFree();
        return;
    }

    gc.shaders = ri.Malloc(gc.numGrassShaders * sizeof(gcShader_t));
    Com_Memset(gc.shaders, 0, gc.numGrassShaders * sizeof(gcShader_t));
    for (i = 0; i < gc.numGrassShaders; i++) {
        gc.shaders[i].ground = grounds[i] & 0xff;
        gc.shaders[i].solid  = !(grounds[i] & 0x100);
        gc.shaders[i].split  = (grounds[i] & 0x200) != 0;
    }
    ri.Free(grounds);

    gc.numLightmaps = lmLump->filelen / (LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
    if (gc.numLightmaps) {
        gc.lightmaps = ri.Malloc(gc.numLightmaps * LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
        Com_Memcpy(gc.lightmaps, fileBase + lmLump->fileofs, gc.numLightmaps * LIGHTMAP_SIZE * LIGHTMAP_SIZE * 3);
    }

    dindexes   = (const int *)(fileBase + indexLump->fileofs);
    numIndexes = indexLump->filelen / sizeof(int);

    // brush faces, triangle soups and curved patches: count the upward
    // triangles, then copy
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
            int               lightmap = LittleLong(ds->lightmapNum);

            if (slot < 0 || (type != MST_PLANAR && type != MST_TRIANGLE_SOUP && type != MST_PATCH)) {
                continue;
            }
            // snow, gravel and sand brushes are as often roofs, beams and
            // walls' tops as ground: of those, terrain only (mud grows only
            // at water, so its brushes too)
            if (gc.shaders[slot].ground != GC_GROUND_GRASS && gc.shaders[slot].ground != GC_GROUND_MUD) {
                continue;
            }
            if (firstVert < 0 || firstVert + nVerts > numVerts) {
                continue;
            }

            if (type == MST_PATCH) {
                GC_AddPatch(pass, &dverts[firstVert], LittleLong(ds->patchWidth), LittleLong(ds->patchHeight), nVerts,
                            lightmap, slot);
                continue;
            }
            if (!gc.shaders[slot].solid || firstIndex < 0 || firstIndex + nIndexes > numIndexes) {
                continue;
            }
            for (j = 0; j + 2 < nIndexes; j += 3) {
                const drawVert_t *v[3];
                int               k;

                for (k = 0; k < 3; k++) {
                    int idx = LittleLong(dindexes[firstIndex + j + k]);
                    if (idx < 0 || idx >= nVerts) {
                        break;
                    }
                    v[k] = &dverts[firstVert + idx];
                }
                if (k == 3) {
                    GC_AddTri(pass, v, lightmap, slot, qfalse);
                }
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

    // Added in OPM
    //  Each grass shader's mask: made from its ground texture decoded on the
    //  CPU, a tenth of a second for a 2048 one. Left to the first cell that
    //  needed it, it was a hitch each time new ground came into view, so the
    //  masks of every shader this map grows grass on are made with the map.
    if (r_groundCoverMask->integer) {
        byte *used = ri.Malloc(gc.numGrassShaders);

        Com_Memset(used, 0, gc.numGrassShaders);
        for (i = 0; i < gc.numTris; i++) {
            used[gc.tris[i].shader] = 1;
        }
        for (i = 0; i < gc.numTerrain; i++) {
            used[gc.terrain[i].shader] = 1;
        }
        for (i = 0; i < gc.numGrassShaders; i++) {
            if (used[i] && !gc.shaders[i].loaded) {
                GC_LoadShaderMask(i);
            }
        }
        ri.Free(used);
    }

    gc.active = qtrue;
    ri.Printf(PRINT_DEVELOPER, "Ground cover: %d ground shaders, %d triangles, %d terrain patches, %dx%d cells, %d water tops\n",
        gc.numGrassShaders, gc.numTris, gc.numTerrain, gc.cellsX, gc.cellsY, gc.numWater);
}

/*
=============================================================================

On the card

=============================================================================
*/

// The tufts drawn from the card take no material maps: their tangent and
// lightmap arrays carry where each stands and how it sways (GC_SetAttrib).
// matgen's pak gives every texture GL2 loads a normal and a specular map
// (zzzzzzzzzz-opm-materials-2.pk3), this one's too, and with them the card
// couldn't draw the tufts (GC_CheckGpuShader): every tuft was swayed on the
// CPU and copied, each frame, 5 ms of a frame in the tech demo's field. So
// the grass's are let go while the card draws it, and given back to the CPU's
// tufts (r_groundCoverGPU 0), whose turned over faces they keep lit.
static void GC_StripMaterialMaps(void)
{
    shaderStage_t *stage = gc.shader->stages[0];

    if (!stage || stage->glslShaderGroup != tr.lightallShader) {
        return;
    }
    if (!gcMaterial.saved) {
        gcMaterial.saved    = qtrue;
        gcMaterial.normal   = stage->bundle[TB_NORMALMAP];
        gcMaterial.specular = stage->bundle[TB_SPECULARMAP];
        gcMaterial.deluxe   = stage->bundle[TB_DELUXEMAP];
        gcMaterial.index    = stage->glslShaderIndex;
        VectorCopy4(stage->normalScale, gcMaterial.normalScale);
        VectorCopy4(stage->specularScale, gcMaterial.specularScale);
    }
    stage->bundle[TB_NORMALMAP].image[0]   = NULL;
    stage->bundle[TB_SPECULARMAP].image[0] = NULL;
    stage->bundle[TB_DELUXEMAP].image[0]   = NULL;
    stage->glslShaderIndex &= ~(LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_SPECULARMAP | LIGHTDEF_USE_DELUXEMAP | LIGHTDEF_USE_PARALLAXMAP);
    // and the scales as they are without them (ParseStage): the specular map's
    // left white specular, all of the diffuse given to it, and black grass
    VectorSet4(stage->normalScale, 0.0f, 0.0f, 0.0f, 0.0f);
    if (r_pbr->integer) {
        stage->specularScale[0] = r_baseGloss->value;
    } else {
        stage->specularScale[0] = stage->specularScale[1] = stage->specularScale[2] = r_baseSpecular->value;
        stage->specularScale[3] = r_baseGloss->value;
    }
}

// The grass's own material maps, for the CPU's tufts
static void GC_RestoreMaterialMaps(void)
{
    shaderStage_t *stage = gc.shader->stages[0];

    if (!stage || !gcMaterial.saved) {
        return;
    }
    stage->bundle[TB_NORMALMAP]   = gcMaterial.normal;
    stage->bundle[TB_SPECULARMAP] = gcMaterial.specular;
    stage->bundle[TB_DELUXEMAP]   = gcMaterial.deluxe;
    stage->glslShaderIndex        = gcMaterial.index;
    VectorCopy4(gcMaterial.normalScale, stage->normalScale);
    VectorCopy4(gcMaterial.specularScale, stage->specularScale);
}

// Drawn from the card or not: the grass's material maps let go or given back
static void GC_UseCard(qboolean card)
{
    if (card) {
        GC_StripMaterialMaps();
    } else {
        GC_RestoreMaterialMaps();
    }
}

// Whether the vertex program that sways the tufts can draw the shader: one
// lightall stage with nothing worked out per vertex on the CPU, and none of
// the arrays the tufts use for where they stand (tangents, lightmap
// coordinates). r_realtimeLighting 2 makes the grass's "rgbGen exactVertex"
// lit by its vertexes, and that kept the card from drawing it, every tuft
// swayed on the CPU with all of the light live.
static void GC_CheckGpuShader(void)
{
    const shaderStage_t *stage = gc.shader->stages[0];
    int                  index, lightType;

    gc.gpuShader = qfalse;
    gc.gpuWhyNot = "not one lightall stage";
    if (!stage || gc.shader->stages[1] || stage->glslShaderGroup != tr.lightallShader) {
        return;
    }
    gc.gpuWhyNot = "deforms or colours worked out on the CPU";
    if (gc.shader->numDeforms || gc.shader->needsCPUVertexAlpha || gc.shader->needsLGrid || gc.shader->needsLSpherical) {
        return;
    }
    index     = stage->glslShaderIndex;
    lightType = index & LIGHTDEF_LIGHTTYPE_MASK;
    // (lit by its vertexes it may be: with all of the light live its colour
    // gives way to the ambient, and the tufts bring no light direction, as on
    // the CPU, where theirs is none; lightall_vp.glsl)
    gc.gpuWhyNot = "lit by a lightmap";
    if (lightType == LIGHTDEF_USE_LIGHTMAP) {
        return;
    }
    gc.gpuWhyNot = "material maps";
    if (index & (LIGHTDEF_USE_NORMALMAP | LIGHTDEF_USE_SPECULARMAP | LIGHTDEF_USE_DELUXEMAP | LIGHTDEF_USE_PARALLAXMAP)) {
        return;
    }
    gc.gpuWhyNot  = NULL;
    gc.gpuShader = qtrue;
}

static qboolean GC_GpuAvailable(void)
{
    return gc.gpuShader && r_groundCoverGPU->integer && glRefConfig.bufferStorage && glRefConfig.vertexArrayObject;
}

static void GC_SetAttrib(vao_t *vao, int attribIndex, int count, GLenum type, GLboolean normalized, int offset)
{
    vaoAttrib_t *vAtb = &vao->attribs[attribIndex];

    vAtb->enabled    = 1;
    vAtb->count      = count;
    vAtb->type       = type;
    vAtb->normalized = normalized;
    vAtb->stride     = sizeof(gcGpuVert_t);
    vAtb->offset     = offset;
}

static gcChunk_t *GC_NewChunk(void)
{
    gcChunk_t *chunk;
    vao_t     *vao;
    GLint      prevArray = 0;

    if (gc.numChunks >= GC_MAX_CHUNKS) {
        return NULL;
    }

    chunk = ri.Malloc(sizeof(*chunk));
    Com_Memset(chunk, 0, sizeof(*chunk));
    vao = &chunk->vao;
    Q_strncpyz(vao->name, "groundCover_VAO", sizeof(vao->name));

    qglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);

    qglGenVertexArrays(1, &vao->vao);
    qglBindVertexArray(vao->vao);
    glState.currentVao = vao;

    // every cell's tufts are indexed the same, from the cell's first vertex
    if (!gc.indexBuffer) {
        glIndex_t *indexes = ri.Malloc(GC_MAX_TUFTS_CELL * GC_INDEXES_TUFT * sizeof(glIndex_t));
        int        q;

        for (q = 0; q < GC_MAX_TUFTS_CELL * GC_QUADS_TUFT; q++) {
            glIndex_t *ix = &indexes[q * 6];

            ix[0] = q * 4 + 0;
            ix[1] = q * 4 + 1;
            ix[2] = q * 4 + 2;
            ix[3] = q * 4 + 0;
            ix[4] = q * 4 + 2;
            ix[5] = q * 4 + 3;
        }
        qglGenBuffers(1, &gc.indexBuffer);
        qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gc.indexBuffer);
        qglBufferData(GL_ELEMENT_ARRAY_BUFFER, GC_MAX_TUFTS_CELL * GC_INDEXES_TUFT * sizeof(glIndex_t), indexes, GL_STATIC_DRAW);
        ri.Free(indexes);
    } else {
        qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gc.indexBuffer);
    }
    vao->indexesIBO  = gc.indexBuffer;
    vao->indexesSize = GC_MAX_TUFTS_CELL * GC_INDEXES_TUFT * sizeof(glIndex_t);

    vao->vertexesSize = GC_CHUNK_PAGES * GC_PAGE_VERTS * sizeof(gcGpuVert_t);
    qglGenBuffers(1, &vao->vertexesVBO);
    qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
    qglBufferData(GL_ARRAY_BUFFER, vao->vertexesSize, NULL, GL_STATIC_DRAW);

    GC_SetAttrib(vao, ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, offsetof(gcGpuVert_t, xyz));
    GC_SetAttrib(vao, ATTR_INDEX_TEXCOORD, 2, GL_FLOAT, GL_FALSE, offsetof(gcGpuVert_t, st));
    GC_SetAttrib(vao, ATTR_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(gcGpuVert_t, color));
    GC_SetAttrib(vao, ATTR_INDEX_NORMAL, 4, GL_SHORT, GL_TRUE, offsetof(gcGpuVert_t, normal));
    GC_SetAttrib(vao, ATTR_INDEX_TANGENT, 4, GL_FLOAT, GL_FALSE, offsetof(gcGpuVert_t, root));
    GC_SetAttrib(vao, ATTR_INDEX_LIGHTCOORD, 2, GL_FLOAT, GL_FALSE, offsetof(gcGpuVert_t, sway));
    Vao_SetVertexPointers(vao);

    qglBindBuffer(GL_ARRAY_BUFFER, prevArray);
    GL_CheckErrors();

    gc.chunks[gc.numChunks++] = chunk;
    return chunk;
}

// The first of a run of free pages, or -1
static int GC_FindPages(const gcChunk_t *chunk, int count)
{
    int i, run = 0;

    if (GC_CHUNK_PAGES - chunk->numUsed < count) {
        return -1;
    }
    for (i = 0; i < GC_CHUNK_PAGES; i++) {
        run = chunk->used[i] ? 0 : run + 1;
        if (run == count) {
            return i - count + 1;
        }
    }
    return -1;
}

/*
================
GC_UploadCell

A cell's tufts to the card, and its copy for tess freed: false if there is no
room, and then it is drawn through tess.
================
*/
static qboolean GC_UploadCell(gcCell_t *cell)
{
    static gcGpuVert_t out[GC_MAX_TUFTS_CELL * GC_VERTS_TUFT];
    const int          numVerts = cell->numTufts * GC_VERTS_TUFT;
    const int          pages = (cell->numTufts + GC_PAGE_TUFTS - 1) / GC_PAGE_TUFTS;
    gcChunk_t         *chunk = NULL;
    GLint              prevArray = 0;
    int                c, first = -1, n, i;

    for (c = 0; c < gc.numChunks; c++) {
        first = GC_FindPages(gc.chunks[c], pages);
        if (first >= 0) {
            chunk = gc.chunks[c];
            break;
        }
    }
    if (!chunk) {
        chunk = GC_NewChunk();
        if (!chunk) {
            return qfalse;
        }
        c     = gc.numChunks - 1;
        first = 0;
    }

    Com_Memset(chunk->used + first, 1, pages);
    chunk->numUsed += pages;
    cell->chunk     = c;
    cell->firstPage = first;
    cell->numPages  = pages;

    for (n = 0; n < cell->numTufts; n++) {
        const gcTuft_t *tuft = &cell->tufts[n];

        for (i = 0; i < GC_VERTS_TUFT; i++) {
            const srfVert_t *v = &cell->verts[n * GC_VERTS_TUFT + i];
            gcGpuVert_t     *o = &out[n * GC_VERTS_TUFT + i];

            VectorCopy(v->xyz, o->xyz);
            o->st[0]    = v->st[0];
            o->st[1]    = v->st[1];
            o->color[0] = v->color[0] >> 8;
            o->color[1] = v->color[1] >> 8;
            o->color[2] = v->color[2] >> 8;
            o->color[3] = v->color[3] >> 8;
            VectorCopy4(v->normal, o->normal);
            VectorCopy(tuft->root, o->root);
            // 0, 1 on the ground; 2, 3 at the tip
            o->root[3] = (i & 3) >= 2 ? tuft->flex : 0.0f;
            o->sway[0] = tuft->phase;
            o->sway[1] = tuft->fadeEnd;
        }
    }

    // the back end binds its vertex array object when it draws from it
    qglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
    qglBindBuffer(GL_ARRAY_BUFFER, chunk->vao.vertexesVBO);
    qglBufferSubData(GL_ARRAY_BUFFER, (GLintptr)first * GC_PAGE_VERTS * sizeof(gcGpuVert_t), numVerts * sizeof(gcGpuVert_t), out);
    qglBindBuffer(GL_ARRAY_BUFFER, prevArray);

    ri.Free(cell->verts);
    ri.Free(cell->indexes);
    cell->verts   = NULL;
    cell->indexes = NULL;
    return qtrue;
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

    // snow, gravel and sand: what grows there is its own colour, and grows
    // anywhere on it (as thinly as the map says)
    if (gs->ground != GC_GROUND_GRASS) {
        return;
    }

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

    // Unless the texture is two things, a path through a field: then the
    // middle may be the path's, and the cut goes between the two. Two
    // things are two groups of green further apart than they are spread
    // (a field's own light and dark grass are about 1.3 apart; a path and its
    // verges 2 and more), each a good part of the texture.
    {
        const int n = GC_MASK_SIZE * GC_MASK_SIZE;
        float     bestScore = 0, bestCut = 0, bestSep = 0;
        int       b;

        for (b = 1; b < 64; b++) {
            const float cut = b / 128.0f - 0.25f;
            float       sum0 = 0, sum1 = 0, sq0 = 0, sq1 = 0, n0 = 0, n1 = 0, m0, m1, score;

            for (i = 0; i < n; i++) {
                if (g[i] < cut) {
                    sum0 += g[i];
                    sq0 += g[i] * g[i];
                    n0++;
                } else {
                    sum1 += g[i];
                    sq1 += g[i] * g[i];
                    n1++;
                }
            }
            if (n0 < n * 0.08f || n1 < n * 0.08f) {
                continue;
            }
            m0    = sum0 / n0;
            m1    = sum1 / n1;
            score = n0 * n1 * (m1 - m0) * (m1 - m0);
            if (score > bestScore) {
                const float sd0 = sqrt(Q_max(0.0f, sq0 / n0 - m0 * m0)), sd1 = sqrt(Q_max(0.0f, sq1 / n1 - m1 * m1));

                bestScore = score;
                bestCut   = cut;
                bestSep   = (m1 - m0) / Q_max(sd0 + sd1, 1e-4f);
            }
        }
        if (bestSep > 1.8f || (gs->split && bestSep > 0.0f)) {
            for (i = 0; i < n; i++) {
                gs->mask[i] = (byte)(Com_Clamp(0, 1, (g[i] - bestCut) / 0.02f + 0.5f) * 255);
            }
        }
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
    float  normalZ; // how level the ground is there
} gcSample_t;

static qboolean GC_Clear(vec3_t xyz, float below, float above, int groundFlag)
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
    // and the terrain under it unlit. What it lands on has to be grass too: a
    // floor laid just over the grass (a house built on the field) is not.
    VectorSet(start, xyz[0], xyz[1], xyz[2] + 64);
    VectorSet(end, xyz[0], xyz[1], xyz[2] - below - 4);
    ri.CM_BoxTrace(&trace, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);
    if (trace.startsolid || trace.allsolid || trace.fraction >= 1.0f || trace.endpos[2] > xyz[2] + above || trace.endpos[2] < xyz[2] - below
        || !(trace.surfaceFlags & groundFlag)) {
        return qfalse;
    }
    xyz[2] = trace.endpos[2];
    return qtrue;
}

// A patch is not solid (the player walks on clip brushes under it): it is
// enough that nothing solid is over the place.
static qboolean GC_ClearOver(const vec3_t xyz)
{
    trace_t trace;
    vec3_t  start, end;

    VectorSet(start, xyz[0], xyz[1], xyz[2] + 64);
    VectorSet(end, xyz[0], xyz[1], xyz[2] + 2);
    ri.CM_BoxTrace(&trace, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse);
    return !trace.startsolid && !trace.allsolid && trace.fraction >= 1.0f;
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
        gc.shaders[tri->shader].dropMask++;
        return qfalse;
    }

    out->xyz[0] = x;
    out->xyz[1] = y;
    out->xyz[2] = b0 * tri->xyz[0][2] + b1 * tri->xyz[1][2] + b2 * tri->xyz[2][2];
    GC_LightAt(tri->lightmap, lm[0] * LIGHTMAP_SIZE, lm[1] * LIGHTMAP_SIZE, out->xyz, out->light);
    VectorCopy(gc.shaders[tri->shader].tint, out->tint);
    out->normalZ = tri->normal[2];
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
        gc.shaders[ter->shader].dropMask++;
        return qfalse;
    }

    out->xyz[0] = x;
    out->xyz[1] = y;
    out->xyz[2] = (h00 * (1 - ax) + h10 * ax) * (1 - ay) + (h01 * (1 - ax) + h11 * ax) * ay;
    GC_LightAt(ter->lightmap, ter->lmS + fx * ter->lmSpan, ter->lmT + fy * ter->lmSpan, out->xyz, out->light);
    VectorCopy(gc.shaders[ter->shader].tint, out->tint);
    out->normalZ = 1.0f / sqrt(1.0f + dzdx * dzdx + dzdy * dzdy);
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

// Snow, gravel and sand grow in clumps with bare ground between: about a
// third of it, 0 to 1
static float GC_Clumps(float x, float y)
{
    return Com_Clamp(0, 1, (0.6f * GC_ValueNoise(x / 110.0f + 5.3f, y / 110.0f - 3.1f) + 0.4f * GC_ValueNoise(x / 37.0f - 1.7f, y / 37.0f + 8.9f) - 0.5f) * 5.0f);
}

static float GC_Patchiness(float x, float y)
{
    float n = 0.65f * GC_ValueNoise(x / 340.0f, y / 340.0f) + 0.35f * GC_ValueNoise(x / 97.0f + 17.3f, y / 97.0f - 5.1f);
    // thick, thin and a few bare spots, about as much grass as even
    return Com_Clamp(0, 1, (n - 0.22f) * 1.6f);
}

// What grows at a place on a ground, or -1 for nothing. On grass ground weeds and flowers come
// in patches of their own among the grass, and reeds at a water's edge; the
// grass is what is left. On the others the shares are of each other.
static int GC_PickKind(int ground, const vec3_t at, unsigned int *rnd)
{
    const float *share = gc.share[ground];
    float        r, total;
    int          k;

    if (!gc.atlas || !r_groundCoverKinds->integer) {
        return GC_Rand(rnd) < 0.5f ? GC_KIND_GRASS : GC_KIND_GRASS2;
    }

    r = GC_Rand(rnd);
    if (ground == GC_GROUND_MUD) {
        return r < share[GC_KIND_REEDS] * GC_WaterEdge(at, 14.0f) ? GC_KIND_REEDS : -1;
    }
    if (ground == GC_GROUND_GRASS) {
        float edge = gc.numWater && share[GC_KIND_REEDS] > 0.0f ? GC_WaterEdge(at, 30.0f) : 0.0f;
        float weeds, flowers;

        if (edge > 0.0f && r < share[GC_KIND_REEDS] * edge) {
            return GC_KIND_REEDS;
        }
        r = GC_Rand(rnd);
        // patches a few paces across: thick in some, none in most
        weeds   = share[GC_KIND_WEEDS] * 3.0f * Com_Clamp(0, 1, (GC_ValueNoise(at[0] / 150.0f + 31.7f, at[1] / 150.0f - 12.2f) - 0.3f) * 2.0f);
        flowers = share[GC_KIND_FLOWERS] * 5.0f * Com_Clamp(0, 1, (GC_ValueNoise(at[0] / 210.0f - 7.3f, at[1] / 210.0f + 3.9f) - 0.45f) * 2.5f);
        if (r < weeds) {
            return GC_KIND_WEEDS;
        }
        r -= weeds;
        if (r < flowers) {
            return GC_KIND_FLOWERS;
        }
        r -= flowers;
        for (k = GC_KIND_DEAD; k < GC_NUM_KINDS; k++) {
            if (k == GC_KIND_REEDS) {
                continue;
            }
            if (r < share[k]) {
                return k;
            }
            r -= share[k];
        }
        return GC_Rand(rnd) < 0.5f ? GC_KIND_GRASS : GC_KIND_GRASS2;
    }

    for (k = 0, total = 0; k < GC_NUM_KINDS; k++) {
        total += share[k];
    }
    if (total <= 0.0f) {
        return GC_KIND_DEAD;
    }
    r *= total;
    for (k = 0; k < GC_NUM_KINDS; k++) {
        if (r < share[k]) {
            return k == GC_KIND_GRASS && GC_Rand(rnd) < 0.5f ? GC_KIND_GRASS2 : k;
        }
        r -= share[k];
    }
    return GC_KIND_DEAD;
}

static int GC_SourceShader(int type, int index)
{
    return type == GC_SRC_TRI ? gc.tris[index].shader : gc.terrain[index].shader;
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

// Added in OPM
//  The cell being sampled. A cell is hundreds of tufts, each tried with a
//  trace through the world, and on a heavy map one cell took a tenth of a
//  second and more: the per frame budget was looked at only between cells.
//  So a cell is sampled a slice at a time, its place in the sampling kept
//  here between frames, and finished when all of its ground has been
//  tried. The random numbers come in the same order however it is sliced,
//  so the cell is the same as if it had been built at once.
static struct {
    gcCell_t      *cell;      // NULL: none under way
    int            cellIndex;
    int            r, n, attempts;
    int            type, index;
    unsigned int   rnd, pick;
    float          x0, y0, x1, y1;
    int            numTufts, numAccepted;
    gcBuildTuft_t *tufts;     // GC_MAX_TUFTS_CELL
} gcBuild;

static void GC_AbandonBuild(void)
{
    gcBuild.cell = NULL;
    if (gcBuild.tufts) {
        ri.Free(gcBuild.tufts);
        gcBuild.tufts = NULL;
    }
}

// Sample the cell under way until the budget, from buildStart, runs out.
// qtrue when it is done and built.
static qboolean GC_BuildCell(gcCell_t *cell, int cellIndex, double buildStart)
{
    const float   density  = Com_Clamp(0, 8, r_groundCoverDensity->value) / (32.0f * 32.0f);
    const float   scale    = Com_Clamp(0.25f, 4, r_groundCoverHeight->value);
    const float   bright   = Com_Clamp(0, 4, r_groundCoverBrightness->value);
    float         cx0      = gc.originX + (cellIndex % gc.cellsX) * GC_CELL_SIZE;
    float         cy0      = gc.originY + (cellIndex / gc.cellsX) * GC_CELL_SIZE;
    gcBuildTuft_t *tufts;
    int           numTufts, n;

    // a new cell: from the start
    if (gcBuild.cell != cell) {
        if (!gcBuild.tufts) {
            gcBuild.tufts = ri.Malloc(GC_MAX_TUFTS_CELL * sizeof(gcBuildTuft_t));
        }
        gcBuild.cell        = cell;
        gcBuild.cellIndex   = cellIndex;
        gcBuild.r           = -1;
        gcBuild.n           = 0;
        gcBuild.attempts    = 0;
        gcBuild.numTufts    = 0;
        gcBuild.numAccepted = 0;
        gcBuild.pick        = GC_Hash(cellIndex * 2654435761u + 7u);
    }
    tufts = gcBuild.tufts;

    for (;;) {
        // the next source in the cell
        if (gcBuild.n >= gcBuild.attempts) {
            int    ref;
            vec3_t mins, maxs;
            float  w;

            if (++gcBuild.r >= cell->numRefs) {
                break;
            }
            ref           = gc.refs[cell->firstRef + gcBuild.r];
            gcBuild.type  = ref >> 28;
            gcBuild.index = ref & 0x0fffffff;
            gcBuild.rnd   = GC_Hash(cellIndex * 7919u + ref * 104729u + 1u);
            gcBuild.n     = 0;
            gcBuild.attempts = 0;

            // sample the part of the source in this cell, as seen from above
            GC_SourceBounds(ref, mins, maxs);
            gcBuild.x0 = MAX(mins[0], cx0);
            gcBuild.y0 = MAX(mins[1], cy0);
            gcBuild.x1 = MIN(maxs[0], cx0 + GC_CELL_SIZE);
            gcBuild.y1 = MIN(maxs[1], cy0 + GC_CELL_SIZE);
            if (gcBuild.x1 <= gcBuild.x0 || gcBuild.y1 <= gcBuild.y0) {
                continue;
            }

            // the patches keep about half; other grounds than grass grow
            // as thinly as the map has it
            w                = (gcBuild.x1 - gcBuild.x0) * (gcBuild.y1 - gcBuild.y0) * density * 2.0f
                             * gc.groundDensity[gc.shaders[GC_SourceShader(gcBuild.type, gcBuild.index)].ground];
            gcBuild.attempts = (int)w + (GC_Rand(&gcBuild.rnd) < w - (int)w ? 1 : 0);
            continue;
        }

        // out of time: on from here next frame
        if (!(gcBuild.n & 15) && R_MicroSeconds() - buildStart > GC_BUILD_BUDGET_US) {
            return qfalse;
        }

        {
            const int      type = gcBuild.type, index = gcBuild.index;
            unsigned int  *rnd  = &gcBuild.rnd;
            gcSample_t     sample;
            gcBuildTuft_t *bt;
            gcTuft_t      *tuft;
            int            ground, kind;
            float          x = gcBuild.x0 + GC_Rand(rnd) * (gcBuild.x1 - gcBuild.x0);
            float          y = gcBuild.y0 + GC_Rand(rnd) * (gcBuild.y1 - gcBuild.y0);
            qboolean       ok;

            gcBuild.n++;

            if (type == GC_SRC_TRI) {
                ok = GC_SampleTri(&gc.tris[index], x, y, &sample, rnd);
            } else {
                ok = GC_SampleTerrain(&gc.terrain[index], x, y, &sample, rnd);
            }
            if (!ok) {
                continue;
            }
            ground = gc.shaders[GC_SourceShader(type, index)].ground;
            if (GC_Patchiness(x, y) <= GC_Rand(rnd)) {
                continue;
            }
            if (ground != GC_GROUND_GRASS && ground != GC_GROUND_MUD && GC_Clumps(x, y) <= GC_Rand(rnd)) {
                continue;
            }
            // mud grows only at water: no trace for the rest of it
            if (ground == GC_GROUND_MUD && GC_WaterEdge(sample.xyz, 14.0f) <= 0.0f) {
                continue;
            }
            if (type == GC_SRC_TRI && gc.tris[index].patch ? !GC_ClearOver(sample.xyz)
                : !GC_Clear(sample.xyz, type == GC_SRC_TERRAIN ? 16.0f : 3.0f, type == GC_SRC_TERRAIN ? 5.0f : 3.0f,
                            GC_GroundFlag(ground))) {
                gc_dropCovered++;
                gc.shaders[GC_SourceShader(type, index)].dropCovered++;
                continue;
            }
            kind = GC_PickKind(ground, sample.xyz, rnd);
            if (kind < 0) {
                continue;
            }
            gc_kept++;
            gc.shaders[GC_SourceShader(type, index)].kept++;

            // A cell holds GC_MAX_TUFTS_CELL. Past that, each tuft that
            // stands replaces a kept one by chance, so every source keeps
            // its share however many come before it.
            gcBuild.numAccepted++;
            if (gcBuild.numTufts < GC_MAX_TUFTS_CELL) {
                bt = &tufts[gcBuild.numTufts++];
            } else {
                int slot = (int)(GC_Rand(&gcBuild.pick) * gcBuild.numAccepted);
                if (slot >= GC_MAX_TUFTS_CELL) {
                    continue;
                }
                bt = &tufts[slot];
            }
            tuft = &bt->tuft;
            VectorCopy(sample.xyz, tuft->root);
            // into the ground, so no gap shows under its downhill side: the
            // steeper, the deeper
            tuft->root[2] -= 1.5f + 3.0f * sqrt(Q_max(0.0f, 1.0f - sample.normalZ * sample.normalZ)) / Q_max(sample.normalZ, 0.3f);
            // most tufts are gone by half the distance, a few go on to it
            tuft->fadeEnd = 0.25f + 0.75f * GC_Rand(rnd) * GC_Rand(rnd);
            tuft->phase   = GC_Rand(rnd) * 2.0f * M_PI;
            tuft->flex    = (0.6f + 0.6f * GC_Rand(rnd)) * gcKindShape[kind].flex;
            bt->kind      = kind;

            VectorScale(sample.light, bright * (0.85f + 0.25f * GC_Rand(rnd)), bt->color);
            bt->color[0] *= sample.tint[0];
            bt->color[1] *= sample.tint[1];
            bt->color[2] *= sample.tint[2];
        }
    }

    // all of it tried: the cell is built
    numTufts     = gcBuild.numTufts;
    gcBuild.cell = NULL;

    cell->built     = qtrue;
    cell->nextBuilt = gc.builtList;
    gc.builtList    = cell;
    gc.numBuilt++;

    // the farthest reaching first, so a far cell draws a prefix of them
    qsort(tufts, numTufts, sizeof(gcBuildTuft_t), GC_CompareTufts);

    cell->numTufts = numTufts;
    if (!numTufts) {
        return qtrue;
    }

    cell->tufts   = ri.Malloc(numTufts * sizeof(gcTuft_t));
    cell->verts   = ri.Malloc(numTufts * GC_VERTS_TUFT * sizeof(srfVert_t));
    cell->indexes = ri.Malloc(numTufts * GC_INDEXES_TUFT * sizeof(glIndex_t));

    for (n = 0; n < numTufts; n++) {
        gcTuft_t    *tuft = &cell->tufts[n];
        unsigned int rnd  = GC_Hash((unsigned int)(tufts[n].tuft.root[0] * 13.0f) ^ GC_Hash((unsigned int)(tufts[n].tuft.root[1] * 7.0f)));
        const int    kind = tufts[n].kind;
        float        h    = (gcKindShape[kind].h0 + gcKindShape[kind].h1 * GC_Rand(&rnd)) * scale;
        float        yaw  = GC_Rand(&rnd) * M_PI;
        float        halfw = h * (gcKindShape[kind].w0 + gcKindShape[kind].w1 * GC_Rand(&rnd));
        float        s0, s1, t0, t1;
        vec3_t       lean, base, top;
        int          q;

        // its cell of the texture: of the kinds' four by two, or of the old
        // texture's two clumps side by side
        if (gc.atlas) {
            s0 = (kind & 3) * 0.25f;
            s1 = s0 + 0.25f;
            t0 = (kind >> 2) * 0.5f + 0.49f; // the foot
            t1 = (kind >> 2) * 0.5f + 0.01f; // the tip
        } else {
            s0 = kind == GC_KIND_GRASS2 ? 0.5f : 0.0f;
            s1 = s0 + 0.5f;
            t0 = 0.98f;
            t1 = 0.02f;
        }
        gc.kindCount[kind]++;

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
            GC_SetVert(&v[0], p, s0, t0, base);
            VectorAdd(tuft->root, dir, p);
            GC_SetVert(&v[1], p, s1, t0, base);
            VectorAdd(tuft->root, dir, p);
            VectorAdd(p, lean, p);
            p[2] += h;
            GC_SetVert(&v[2], p, s1, t1, top);
            VectorSubtract(tuft->root, dir, p);
            VectorAdd(p, lean, p);
            p[2] += h;
            GC_SetVert(&v[3], p, s0, t1, top);

            ix[0] = first + 0;
            ix[1] = first + 1;
            ix[2] = first + 2;
            ix[3] = first + 0;
            ix[4] = first + 2;
            ix[5] = first + 3;
        }
    }

    if (gc.gpu) {
        GC_UploadCell(cell);
    }
    return qtrue;
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

// By the buffer they are in, so the back end draws a chunk's cells as one
// batch; then near to far, so the depth the near ones leave hides what is
// behind them.
static int GC_CompareDraws(const void *a, const void *b)
{
    const srfGroundCover_t *da = (const srfGroundCover_t *)a, *db = (const srfGroundCover_t *)b;
    int ca = da->cell->numPages ? da->cell->chunk : -1, cb = db->cell->numPages ? db->cell->chunk : -1;

    if (ca != cb) {
        return ca < cb ? -1 : 1;
    }
    return da->dist < db->dist ? -1 : (da->dist > db->dist ? 1 : 0);
}

void R_AddGroundCoverSurfaces(void)
{
    const float *origin;
    float        range;
    double       buildStart;
    int          range4[4];
    int          x, y, i, firstDraw;
    vec3_t       mins, maxs;
    uint64_t     rtCandidates;

    if (!gc.active || !r_groundCover->integer) {
        return;
    }
    if (tr.viewParms.isPortalSky
        || (tr.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP | VPF_RTBAKED | VPF_RTSTATIC | VPF_RTDYNAMIC))) {
        return;
    }

    // a new size or density: make the cells again
    if (gc.density != r_groundCoverDensity->value || gc.height != r_groundCoverHeight->value
        || gc.brightness != r_groundCoverBrightness->value || gc.mask != r_groundCoverMask->integer
        || gc.gpu != GC_GpuAvailable()) {
        if (gc.builtList) {
            R_IssuePendingRenderCommands();
        }
        GC_FreeAllCells();
        gc.density = r_groundCoverDensity->value;
        gc.height  = r_groundCoverHeight->value;
        gc.brightness = r_groundCoverBrightness->value;
        gc.mask    = r_groundCoverMask->integer;
        gc.gpu     = GC_GpuAvailable();
        GC_UseCard(gc.gpu);
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

    buildStart   = R_MicroSeconds();
    rtCandidates = R_RtViewMask();
    firstDraw    = gc.numDraws;

    // the cell under way first, wherever it is now: not in range any more,
    // it is dropped and taken up again from the start when it is
    if (gcBuild.cell) {
        gcCell_t *cell = gcBuild.cell;

        if (GC_DistanceToBounds(origin, cell->bounds) >= range) {
            gcBuild.cell = NULL;
        } else {
            GC_BuildCell(cell, gcBuild.cellIndex, buildStart);
        }
    }

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
                // a slice of a cell at a time, one cell at a time (gcBuild):
                // the rest come in over the next frames
                if ((gcBuild.cell && gcBuild.cell != cell) || R_MicroSeconds() - buildStart > GC_BUILD_BUDGET_US) {
                    continue;
                }
                if (!GC_BuildCell(cell, cellIndex, buildStart)) {
                    continue;
                }
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
            draw->dist        = dist;
        }
    }

    // a surface list keeps the order of a shader's surfaces
    qsort(gc.draws + firstDraw, gc.numDraws - firstDraw, sizeof(gc.draws[0]), GC_CompareDraws);
    for (i = firstDraw; i < gc.numDraws; i++) {
        gcCell_t *cell = gc.draws[i].cell;

        // only the realtime lights that reach the cell
        tr.rtDrawCulled = ~R_RtBoxMask(cell->bounds[0], cell->bounds[1], rtCandidates);
        R_AddDrawSurf(&gc.draws[i].surfaceType, gc.shader, 0, 0, 0, 0);
    }
    tr.rtDrawCulled = 0;

    GC_EvictFarCells(origin, range);
}

/*
=============================================================================

Drawing (back end)

=============================================================================
*/

// Added in OPM: the wind on a tuft, as glsl/lightall_vp.glsl works it out for
// the tufts on the card: which way its tips go and how far (for a flex of
// one), in gusts. The same noise, the same numbers.
static void GC_Hash2(float x, float y, float *out)
{
    float p[3] = { x * 0.1031f, y * 0.1030f, x * 0.0973f };
    float d;
    int   i;

    for (i = 0; i < 3; i++) {
        p[i] -= floor(p[i]);
    }
    d = p[0] * (p[1] + 33.33f) + p[1] * (p[2] + 33.33f) + p[2] * (p[0] + 33.33f);
    for (i = 0; i < 3; i++) {
        p[i] += d;
    }
    out[0] = (p[0] + p[1]) * p[2];
    out[1] = (p[0] + p[2]) * p[1];
    out[0] -= floor(out[0]);
    out[1] -= floor(out[1]);
}

static void GC_Noise2(float x, float y, float *out)
{
    const float ix = floor(x), iy = floor(y);
    const float fx = x - ix, fy = y - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    float       a[2], b[2], c[2], d[2];
    int         i;

    GC_Hash2(ix, iy, a);
    GC_Hash2(ix + 1.0f, iy, b);
    GC_Hash2(ix, iy + 1.0f, c);
    GC_Hash2(ix + 1.0f, iy + 1.0f, d);
    for (i = 0; i < 2; i++) {
        const float top = a[i] + (b[i] - a[i]) * ux, bottom = c[i] + (d[i] - c[i]) * ux;

        out[i] = top + (bottom - top) * uy;
    }
}

static float GC_Smoothstep(float a, float b, float x)
{
    x = Com_Clamp(0.0f, 1.0f, (x - a) / (b - a));
    return x * x * (3.0f - 2.0f * x);
}

static void GC_Wind(const float *root, float phase, float time, float yaw, float gusty, vec2_t dir, float *lean)
{
    const float wx = cos(yaw), wy = sin(yaw);
    const float gust = 0.6f + 0.4f * sin(time * 0.31f + root[0] * 0.0021f + root[1] * 0.0013f);
    const float sway = sin(time * 1.9f + phase + (root[0] + root[1]) * 0.011f) * 0.65f + sin(time * 3.7f + phase * 1.7f) * 0.35f;
    const float burst = GC_Smoothstep(0.2f, 0.75f, 0.5f + 0.5f * sin(time * 0.21f + 2.0f * sin(time * 0.083f)));
    const float flut = sin(time * 7.3f + phase * 2.3f + root[0] * 0.03f);
    float       n[2], g, to;

    GC_Noise2((root[0] * wx + root[1] * wy - time * 220.0f) / 520.0f, (root[1] * wx - root[0] * wy) / 900.0f + time * 0.013f, n);
    g  = gusty * GC_Smoothstep(0.4f, 0.8f, n[0]) * (0.35f + 0.65f * burst);
    to = yaw + gusty * ((n[1] - 0.5f) * 1.5f + sin(time * 0.047f) * 0.21f + sin(time * 0.113f + 1.3f) * 0.14f);

    dir[0] = cos(to);
    dir[1] = sin(to);
    *lean  = (sway * 2.2f + 1.2f) * gust * (1.0f - 0.45f * Q_min(gusty, 1.0f)) + g * (13.0f + sway * 3.0f + flut * 2.0f);
}

/*
================
GC_Pushers

Added in OPM: who stands in the grass, which gives way round his feet
(r_groundCoverPush): the nearest of the view's whole models that are a
man's size, and the eye's own feet. Where, and how far round it (0: none).
================
*/
#define GC_PUSHERS 8

void RB_GroundCoverPushers(vec4_t out[GC_PUSHERS])
{
    static vec4_t kept[GC_PUSHERS];
    static float  keptTime = -1.0f;
    static vec3_t keptView;
    const float  *view = backEnd.refdef.vieworg;
    const float   reach = Com_Clamp(0.0f, 96.0f, r_groundCoverPush->value * 30.0f);
    float         dists[GC_PUSHERS];
    int           count = 0, i, k;

    if (keptTime == backEnd.refdef.floatTime && VectorCompare(keptView, view)) {
        Com_Memcpy(out, kept, sizeof(kept));
        return;
    }
    Com_Memset(out, 0, sizeof(vec4_t) * GC_PUSHERS);
    if (reach > 0.0f) {
        // the eye's own feet (drawn or not: the first person has no body
        // in every view)
        if (!(backEnd.refdef.rdflags & RDF_NOWORLDMODEL)) {
            VectorSet4(out[0], view[0], view[1], view[2] - 60.0f, reach);
            dists[0] = 0.0f;
            count    = 1;
        }
        for (i = 0; i < backEnd.refdef.num_entities; i++) {
            const refEntity_t *e = &backEnd.refdef.entities[i].e;
            float              d;

            // a model of its own, a man's size: not what he carries, nor
            // the hands and the weapon in view, nor a helmet on the ground
            if (e->reType != RT_MODEL || !e->tiki || e->parentEntity != ENTITYNUM_NONE
                || (e->renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK)) || e->radius < 40.0f) {
                continue;
            }
            d = DistanceSquared(e->origin, view);
            if (d > 900.0f * 900.0f) {
                continue;
            }
            // (the player's own body, where the eye's feet already are)
            if (count && (e->origin[0] - out[0][0]) * (e->origin[0] - out[0][0])
                + (e->origin[1] - out[0][1]) * (e->origin[1] - out[0][1]) < 24.0f * 24.0f
                && fabs(e->origin[2] - out[0][2]) < 64.0f) {
                continue;
            }
            // a place of its own, or the place of the farthest kept if it is nearer
            if (count < GC_PUSHERS) {
                k = count++;
            } else {
                int farthest = 1;

                for (k = 2; k < GC_PUSHERS; k++) {
                    if (dists[k] > dists[farthest]) {
                        farthest = k;
                    }
                }
                if (dists[farthest] <= d) {
                    continue;
                }
                k = farthest;
            }
            VectorSet4(out[k], e->origin[0], e->origin[1], e->origin[2], reach);
            dists[k] = d;
        }
    }
    // Added in OPM: and where something blew up, it lies flat (the two
    // newest blasts, in the last places: the farthest of who stands in it)
    if (reach > 0.0f) {
        vec4_t flat[2];
        int    n = R_BlastPushers(flat, 2);

        for (i = 0; i < n; i++) {
            Vector4Copy(flat[i], out[GC_PUSHERS - 1 - i]);
        }
    }
    Com_Memcpy(kept, out, sizeof(kept));
    keptTime = backEnd.refdef.floatTime;
    VectorCopy(view, keptView);
}

void RB_SurfaceGroundCover(void *surface)
{
    const srfGroundCover_t *draw = (const srfGroundCover_t *)surface;
    const gcCell_t         *cell = draw->cell;
    const float            *view = backEnd.viewParms.ori.origin;
    const float             range = Com_Clamp(128, 8192, r_groundCoverDistance->value);
    // Added in OPM: the map's wind (tr_atmos.c), where there was one wind for all
    float                   wind = r_groundCoverWind->value;
    vec3_t                  windDir = { 0.8f, 0.6f, 0 };
    float                   time = (float)fmod(backEnd.refdef.floatTime, 3600.0);
    int                     numVerts = draw->numTufts * GC_VERTS_TUFT;
    int                     firstVert;
    int                     n, i;

    float                   yaw, gusty;
    vec4_t                  pushers[GC_PUSHERS];

    {
        float strength;

        R_AtmosWind(windDir, &strength);
        wind *= strength;
        // where it blows to, from 0 up to a turn, and how gusty in tenths
        yaw = atan2(windDir[1], windDir[0]);
        if (yaw < 0.0f) {
            yaw += 2.0f * M_PI;
        }
        gusty = floor(Com_Clamp(0.0f, 2.0f, R_AtmosGusts()) * 10.0f + 0.5f) * 0.1f;
    }

    RB_GroundCoverPushers(pushers);

    gc.backEndDraws++;
    gc.backEndTufts += draw->numTufts;

    // On the card: drawn from there with the other cells of its chunk, as one
    // batch that nothing is copied into (RB_GroundCoverBindVao), and the
    // vertex program sways and sinks it. tess counts what the batch draws.
    if (cell->numPages) {
        // tufts from tess, or cells of another chunk, are a batch of their own
        if (tess.numIndexes && (!tess.groundCover || gc.batchChunk != cell->chunk || gc.batchSize == GC_MAX_BATCH)) {
            RB_EndSurface();
            RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);
        }
        if (!tess.groundCover) {
            tess.groundCover = qtrue;
            // (the gustiness rides over the yaw, which is under 7: the
            // vertex program takes the two apart)
            VectorSet4(tess.groundCoverParams, time, range, wind, yaw + 7.0f * floor(gusty * 10.0f + 0.5f));
            gc.batchChunk = cell->chunk;
            gc.batchSize  = 0;
        }
        // every cell is indexed from its first vertex, the tufts it draws a prefix
        gc.batchBases[gc.batchSize]  = cell->firstPage * GC_PAGE_VERTS;
        gc.batchCounts[gc.batchSize] = draw->numTufts * GC_INDEXES_TUFT;
        gc.batchSize++;
        tess.numVertexes += numVerts;
        tess.numIndexes  += draw->numTufts * GC_INDEXES_TUFT;
        return;
    }

    if (tess.groundCover) {
        RB_EndSurface();
        RB_BeginSurface(tess.shader, tess.fogNum, tess.cubemapIndex);
    }
    RB_SurfaceGroundCoverVerts(numVerts, cell->verts, draw->numTufts * GC_INDEXES_TUFT, cell->indexes);
    firstVert = tess.numVertexes - numVerts;

    for (n = 0; n < draw->numTufts; n++) {
        const gcTuft_t *tuft = &cell->tufts[n];
        float          *xyz  = tess.xyz[firstVert + n * GC_VERTS_TUFT];
        float           end  = tuft->fadeEnd * range;
        float           d, sink, lean;
        vec2_t          to;
        vec3_t          offset;

        // sinks into the ground over the last fifth of its distance
        d    = Distance(tuft->root, view);
        sink = Com_Clamp(0, 1, (end - d) / (end * 0.2f));

        float           press = 0.0f;
        vec2_t          away = { 0.0f, 0.0f };

        GC_Wind(tuft->root, tuft->phase, time, yaw, gusty, to, &lean);
        lean *= tuft->flex * wind * sink;
        VectorSet(offset, to[0] * lean, to[1] * lean, 0.0f);

        // it gives way round whoever stands in it (as lightall_vp.glsl)
        for (i = 0; i < GC_PUSHERS; i++) {
            const float dx = tuft->root[0] - pushers[i][0], dy = tuft->root[1] - pushers[i][1];
            const float r2 = dx * dx + dy * dy;

            if (r2 < pushers[i][3] * pushers[i][3] && fabs(tuft->root[2] - pushers[i][2]) < 64.0f) {
                const float dist = sqrt(r2);
                float       f = 1.0f - dist / pushers[i][3];

                f = f * f * (3.0f - 2.0f * f);
                away[0] += (dist > 0.5f ? dx / dist : 1.0f) * f;
                away[1] += (dist > 0.5f ? dy / dist : 0.0f) * f;
                press = Q_max(press, f);
            }
        }

        for (i = 0; i < GC_VERTS_TUFT; i++, xyz += 4) {
            if (sink < 1.0f) {
                xyz[0] = tuft->root[0] + (xyz[0] - tuft->root[0]) * sink;
                xyz[1] = tuft->root[1] + (xyz[1] - tuft->root[1]) * sink;
                xyz[2] = tuft->root[2] + (xyz[2] - tuft->root[2]) * sink;
            }
            // the tips, which come down as they go over
            if ((i & 3) >= 2) {
                const float height = xyz[2] - tuft->root[2];

                VectorAdd(xyz, offset, xyz);
                xyz[2] -= Q_min(lean * lean / (2.0f * Q_max(height, 6.0f)), 0.5f * Q_max(height, 0.0f));
                {
                    const float stands = Q_max(xyz[2] - tuft->root[2], 0.0f);

                    xyz[0] += away[0] * stands * 0.75f * sink;
                    xyz[1] += away[1] * stands * 0.75f * sink;
                    xyz[2] -= press * stands * 0.6f * sink;
                }
            }
        }
    }
}

/*
================
RB_GroundCoverBindVao

The batch RB_SurfaceGroundCover made of cells on the card: their chunk's
vertex array object bound, which RB_GroundCoverDrawElements draws them from.
================
*/
void RB_GroundCoverBindVao(void)
{
    R_BindVao(&gc.chunks[gc.batchChunk]->vao);
}

// R_DrawElements for that batch: a draw for each cell, nothing set between them
void RB_GroundCoverDrawElements(void)
{
    int i;

    R_BindVao(&gc.chunks[gc.batchChunk]->vao);
    for (i = 0; i < gc.batchSize; i++) {
        qglDrawElementsBaseVertex(GL_TRIANGLES, gc.batchCounts[i], GL_INDEX_TYPE, BUFFER_OFFSET(0), gc.batchBases[i]);
    }
    backEnd.pc.c_drawCalls += gc.batchSize - 1;
}

void R_GroundCoverInfo_f(void)
{
    int built = 0, tufts = 0, bytes = 0, onCard = 0, pages = 0, ch;
    const gcCell_t *cell;

    if (!gc.active) {
        ri.Printf(PRINT_ALL, "Ground cover: off on this map\n");
        return;
    }
    for (cell = gc.builtList; cell; cell = cell->nextBuilt) {
        built++;
        tufts += cell->numTufts;
        bytes += cell->numTufts * sizeof(gcTuft_t);
        if (cell->verts) {
            bytes += cell->numTufts * (GC_VERTS_TUFT * sizeof(srfVert_t) + GC_INDEXES_TUFT * sizeof(glIndex_t));
        }
        if (cell->numPages) {
            onCard++;
        }
    }
    for (ch = 0; ch < gc.numChunks; ch++) {
        pages += gc.chunks[ch]->numUsed;
    }
    ri.Printf(PRINT_ALL, "Ground cover: %d ground shaders, %d triangles, %d terrain patches\n",
        gc.numGrassShaders, gc.numTris, gc.numTerrain);
    ri.Printf(PRINT_ALL, "  %d of %d cells made, %d tufts, %d KB; %d cells added last frame\n",
        built, gc.cellsX * gc.cellsY, tufts, bytes / 1024, gc.numDraws);
    ri.Printf(PRINT_ALL, "  on the card: %s, %d cells; %d buffers of %d MB, %d%% used\n",
        gc.gpu ? "yes" : !r_groundCoverGPU->integer ? "no (r_groundCoverGPU 0)"
            : !gc.gpuShader ? va("no (the grass shader: %s)", gc.gpuWhyNot ? gc.gpuWhyNot : "-") : "no (needs buffer storage and vertex arrays)",
        onCard, gc.numChunks, (int)(GC_CHUNK_PAGES * GC_PAGE_VERTS * sizeof(gcGpuVert_t) / (1024 * 1024)),
        gc.numChunks ? pages * 100 / (gc.numChunks * GC_CHUNK_PAGES) : 0);
    ri.Printf(PRINT_ALL, "  samples: %d kept; dropped %d outside, %d steep, %d bare texture, %d covered\n",
        gc_kept, gc_dropOutside, gc_dropSlope, gc_dropMask, gc_dropCovered);
    {
        char line[512];
        int  k, g;

        line[0] = 0;
        for (k = 0; k < GC_NUM_KINDS; k++) {
            if (gc.kindCount[k]) {
                Q_strcat(line, sizeof(line), va(" %s %d", gcKindNames[k], gc.kindCount[k]));
            }
        }
        ri.Printf(PRINT_ALL, "  kinds (%s):%s\n", !gc.atlas ? "grass only: no groundcover/cover shader"
            : !r_groundCoverKinds->integer ? "grass only: r_groundCoverKinds 0" : "all", line[0] ? line : " none made");
        line[0] = 0;
        for (g = 0; g < GC_NUM_GROUNDS; g++) {
            Q_strcat(line, sizeof(line), va(" %s %g", gcGroundNames[g], gc.groundDensity[g]));
        }
        ri.Printf(PRINT_ALL, "  grounds:%s; %d water tops\n", line, gc.numWater);
        // what each ground shader has grown so far, and why not
        for (k = 0; k < gc.numGrassShaders; k++) {
            const gcShader_t *gs = &gc.shaders[k];

            if (!gs->kept && !gs->dropMask && !gs->dropCovered) {
                continue;
            }
            ri.Printf(PRINT_ALL, "    %-48s %-6s kept %6d, bare texture %6d, covered %6d%s\n", gc.shaderNames[k],
                gcGroundNames[gs->ground], gs->kept, gs->dropMask, gs->dropCovered, gs->hasMask ? "" : " (no mask)");
        }
    }
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
                    bestVert = cell->verts ? &cell->verts[i * GC_VERTS_TUFT + 2] : NULL;
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
                ri.Printf(PRINT_ALL, ", ground below it at %.0f", tr0.endpos[2]);
                if (bestVert) {
                    ri.Printf(PRINT_ALL, ", tip colour %.2f %.2f %.2f",
                        bestVert->color[0] / 65535.0f, bestVert->color[1] / 65535.0f, bestVert->color[2] / 65535.0f);
                }
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
