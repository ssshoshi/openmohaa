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

// tr_water.c -- water that one sees into
//
// Added in OPM. The maps' water is a few scrolling layers of one texture,
// opaque or nearly. A shader with `surfaceparm water` is drawn instead by the
// water program (glsl/water_fp.glsl): waves in its normal, the sky it mirrors,
// the bottom seen through it and darkening with depth, surf and foam where it
// runs shallow, and blood in it where the map has some.
//
// It is drawn after everything opaque (sorted just in front of the blended
// shaders) and not in the depth prepass, so the prepass's depth, which soft
// particles copy to tr.hdrDepthImage, has the bottom under it. Before the first
// water surface of a view the view is copied (tr.waterSceneImage), and the
// program reads the bottom from that copy.
//
// How a map's water looks is in water/<map>.txt, a line a setting:
//
//     color 0.05 0.09 0.085   // the water's own colour
//     clarity 140             // how far one sees into it, in units
//     waves 0 1 420 5.5 4     // the way they come in (x y), length, period (seconds), height
//     surf 18                 // the depth the surf starts breaking at
//     shore -770              // where the waterline is, along the way the waves come in
//     blood 0.2 0.01 0.01 0.8 // the colour of blood in it, and how much
//     bloodReach 200 600      // all of it out to the first distance from the shore, none past the second
//     bloodDepth 30 120       // or with no shore line: all of it down to the first depth, none past the second
//     bloodClouds 0.6         // 0 spread evenly, 1 all in clouds
//     sky 0.5 0.55 0.6        // the sky it mirrors where there is no sky box (else the fog's colour)
//     skyBox 1                // how much of the map's sky box it mirrors, once the view has drawn it: 0 none
//     chop 1  foam 1  refraction 0.03  glints 1
//
// And where the map has a beach the tide is going out from (it needs `shore`):
//
//     beach 900 -440          // the sand is wet this far inland of the shore line, and up to this height
//     puddles 0.3 90          // water stands in its hollows: how much of it, and how big they are
//     swash 160               // how far up the sand a wave runs, at most, from the shore line
//
// The beach is drawn over the opaque view from its depth (RB_BeachDraw,
// glsl/beach_*.glsl): wet sand darker with the sky in it, the puddles, and the
// sand soaked red under each body lying on it (the cgame says where they lie).
//
// The cgame reads the same file for how floating bodies move (cg_water.cpp),
// and the first wave train is the one it pushes them with. It also says each
// frame where there are bodies in the water (RE_SetWaterBlood), and the water
// round each of them is red with its blood, in a cloud that spreads out from
// the moment it goes in.

#include "tr_local.h"

cvar_t        *r_water;
cvar_t        *r_waterRipples;
cvar_t        *r_waterWet;
static cvar_t *r_waterBlood;

typedef struct {
    vec4_t color; // rgb, and how far one sees into it
    vec4_t blood; // rgb, and how much
    vec4_t waves; // direction x y, length, period
    float  height;
    float  surf;
    float  bloodNear, bloodFar;     // in depth
    float  reachNear, reachFar;     // out from the shore line
    float  shore;
    qboolean shoreSet;
    float  bloodClouds;
    vec3_t sky;
    qboolean skySet;
    float  skyBox;
    float  chop, foam, refraction, glints;
    qboolean beachSet;
    float  beachReach, beachTop;
    float  puddles, puddleSize;
    float  swash;
    char   file[MAX_QPATH];
    qboolean fromFile;
} waterParams_t;

static waterParams_t water;

// the clouds of blood round bodies in water: x, y, radius, strength
static vec4_t waterSpots[WATER_MAX_SPOTS];
static int    numWaterSpots;

// the sand soaked under bodies on a beach: x, y, the ground's height, radius
static vec4_t sandStains[WATER_MAX_SPOTS];
static int    numSandStains;

// The ripples (glsl/ripple_fp.glsl): a grid of the water's heights round the
// view, WATER_RIPPLE_SIZE texels across WATER_RIPPLE_SPAN units, stepped
// WATER_RIPPLE_HZ times a second. Ripples go WATER_RIPPLE_SPEED units a
// second: a boat at 90 leaves a V of about 20 degrees, as a real one does.
#define WATER_RIPPLE_SPAN  3072.0f
#define WATER_RIPPLE_HZ    60.0f
#define WATER_RIPPLE_SPEED 30.0f
#define WATER_RIPPLE_STEPS 4 // at most, a frame: a slower frame has fewer

// what disturbs the water, gathered until the next step: x, y, width, dip, foam
static float waterRipples[WATER_MAX_SPOTS][5];
static int   numWaterRipples;

static struct {
    int      current;  // which of the two grids is the latest
    int      x, y;     // the grid's middle, in texels from the world's origin
    float    time;     // the frame's time when it was last stepped
    float    owed;     // time not yet stepped
    qboolean clear;
} ripples;

void R_WaterRegisterCvars(void)
{
    r_water      = ri.Cvar_Get("r_water", "1", CVAR_ARCHIVE | CVAR_LATCH);
    r_waterBlood = ri.Cvar_Get("r_waterBlood", "1", CVAR_ARCHIVE);
    r_waterRipples = ri.Cvar_Get("r_waterRipples", "1", CVAR_ARCHIVE | CVAR_LATCH);
    r_waterWet = ri.Cvar_Get("r_waterWet", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_waterWet, "What stands in the water or on a beach below the tide's mark looks wet: darker, glossy, with the sky in it");
    ri.Cvar_SetDescription(r_waterRipples, "Ripples and wakes on water from men wading, boats, bullets and things falling in (needs vid_restart)");
    ri.Cvar_SetDescription(r_water, "Draws water with the water program: waves, reflection, depth, surf (needs vid_restart)");
    ri.Cvar_SetDescription(r_waterBlood, "How much of the blood a map puts in its water (water/<map>.txt): 0 none, 1 as the map has it");
}

static void R_WaterDefaults(void)
{
    memset(&water, 0, sizeof(water));

    VectorSet4(water.color, 0.05f, 0.09f, 0.085f, 140.0f);
    VectorSet4(water.blood, 0.20f, 0.012f, 0.01f, 0.0f);
    VectorSet4(water.waves, 1.0f, 0.3f, 420.0f, 5.5f);
    water.height      = 4.0f;
    water.surf        = 18.0f;
    water.bloodNear   = 30.0f;
    water.bloodFar    = 120.0f;
    water.reachNear   = 200.0f;
    water.reachFar    = 600.0f;
    water.bloodClouds = 0.6f;
    VectorSet(water.sky, 0.5f, 0.55f, 0.6f);
    water.skyBox     = 1.0f;
    water.chop       = 1.0f;
    water.foam       = 1.0f;
    water.refraction = 0.03f;
    water.glints     = 1.0f;
}

// Up to n numbers that follow on the line
static int R_WaterNumbers(char **text, float *out, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        const char *token = COM_ParseExt(text, qfalse);

        if (!token[0]) {
            break;
        }
        out[i] = atof(token);
    }
    return i;
}

/*
===============
RE_SetWaterBlood

From the cgame, each frame: where the bodies in the water are, and how far and
how thick their blood has spread round them (x, y, radius, strength). They
hold until it says otherwise.
===============
*/
void RE_SetWaterBlood(const float *spots, int numSpots, const float *stains, int numStains)
{
    numWaterSpots = Com_Clamp(0, WATER_MAX_SPOTS, numSpots);
    if (numWaterSpots) {
        Com_Memcpy(waterSpots, spots, numWaterSpots * sizeof(vec4_t));
    }
    numSandStains = Com_Clamp(0, WATER_MAX_SPOTS, numStains);
    if (numSandStains) {
        Com_Memcpy(sandStains, stains, numSandStains * sizeof(vec4_t));
    }
}

void RE_AddWaterRipples(const float *in, int num)
{
    int i;

    for (i = 0; i < num && numWaterRipples < WATER_MAX_SPOTS; i++) {
        Com_Memcpy(waterRipples[numWaterRipples++], in + i * 5, sizeof(waterRipples[0]));
    }
}

static void R_WaterReadFile(const char *mapName)
{
    union {
        char *c;
        void *v;
    } buffer;
    char *text;

    R_WaterDefaults();
    numWaterSpots   = 0;
    numSandStains   = 0;
    numWaterRipples = 0;
    ripples.clear   = qtrue;
    ripples.time    = -1.0f;

    Com_sprintf(water.file, sizeof(water.file), "water/%s.txt", mapName);
    if (ri.FS_ReadFile(water.file, &buffer.v) <= 0) {
        return;
    }
    water.fromFile = qtrue;

    text = buffer.c;
    for (;;) {
        const char *token = COM_ParseExt(&text, qtrue);
        char        key[64];
        float       v[5];

        if (!token[0]) {
            break;
        }
        Q_strncpyz(key, token, sizeof(key));

        if (!Q_stricmp(key, "color")) {
            R_WaterNumbers(&text, water.color, 3);
        } else if (!Q_stricmp(key, "clarity")) {
            R_WaterNumbers(&text, &water.color[3], 1);
        } else if (!Q_stricmp(key, "waves")) {
            if (R_WaterNumbers(&text, v, 5) == 5) {
                VectorSet4(water.waves, v[0], v[1], Q_max(v[2], 16.0f), Q_max(v[3], 0.5f));
                water.height = v[4];
            }
        } else if (!Q_stricmp(key, "surf")) {
            R_WaterNumbers(&text, &water.surf, 1);
        } else if (!Q_stricmp(key, "blood")) {
            R_WaterNumbers(&text, water.blood, 4);
        } else if (!Q_stricmp(key, "bloodDepth")) {
            if (R_WaterNumbers(&text, v, 2) == 2) {
                water.bloodNear = v[0];
                water.bloodFar  = Q_max(v[1], v[0] + 1.0f);
            }
        } else if (!Q_stricmp(key, "bloodReach")) {
            if (R_WaterNumbers(&text, v, 2) == 2) {
                water.reachNear = v[0];
                water.reachFar  = Q_max(v[1], v[0] + 1.0f);
            }
        } else if (!Q_stricmp(key, "shore")) {
            water.shoreSet = R_WaterNumbers(&text, &water.shore, 1) == 1;
        } else if (!Q_stricmp(key, "bloodClouds")) {
            R_WaterNumbers(&text, &water.bloodClouds, 1);
        } else if (!Q_stricmp(key, "sky")) {
            water.skySet = R_WaterNumbers(&text, water.sky, 3) == 3;
        } else if (!Q_stricmp(key, "skyBox")) {
            R_WaterNumbers(&text, &water.skyBox, 1);
        } else if (!Q_stricmp(key, "chop")) {
            R_WaterNumbers(&text, &water.chop, 1);
        } else if (!Q_stricmp(key, "foam")) {
            R_WaterNumbers(&text, &water.foam, 1);
        } else if (!Q_stricmp(key, "refraction")) {
            R_WaterNumbers(&text, &water.refraction, 1);
        } else if (!Q_stricmp(key, "glints")) {
            R_WaterNumbers(&text, &water.glints, 1);
        } else if (!Q_stricmp(key, "beach")) {
            if (R_WaterNumbers(&text, v, 2) == 2) {
                water.beachSet   = qtrue;
                water.beachReach = Q_max(v[0], 1.0f);
                water.beachTop   = v[1];
            }
        } else if (!Q_stricmp(key, "swash")) {
            R_WaterNumbers(&text, &water.swash, 1);
        } else if (!Q_stricmp(key, "puddles")) {
            if (R_WaterNumbers(&text, v, 2) == 2) {
                water.puddles    = Com_Clamp(0.0f, 1.0f, v[0]);
                water.puddleSize = Q_max(v[1], 8.0f);
            }
        }
        // anything else is the cgame's, or unknown: the rest of its line goes
        SkipRestOfLine(&text);
    }

    ri.FS_FreeFile(buffer.v);
}

/*
===============
R_WaterBuildSky

The sky box water mirrors: the six sides of the world's sky shader resampled
into one cubemap (tr.waterSkyImage) that a direction in the world looks up
as it is, each texel taken from the side and the spot of it that the box
(tr_sky.c, MakeSkyVec) draws in that direction. Built when the map loads, if
the map has water or a beach to mirror it, and used once a view has drawn
the box (RB_WaterSawSky): a map whose far plane hides its sky mirrors the
fog's colour as it shows it.
===============
*/
#define WATER_SKY_SIZE 256

static const shader_t *waterSkyShader; // the sky the cubemap holds
static qboolean        waterSkySeen;   // and a view of this map drew it

// The same as tr_sky.c's: for each of the box's sides, which of the world's
// axes its s, t and out are (1 x, 2 y, 3 z, negative the other way), and
// which image of the shader that side shows
static const int waterSkyStToVec[6][3] = {
    {3, -1, 2}, {-3, 1, 2}, {1, 3, 2}, {-1, -3, 2}, {-2, -1, 3}, {2, -1, -3}
};
static const int waterSkyTexOrder[6] = {0, 2, 1, 3, 4, 5};

// One side of the box: an image's pixels, bilinearly
static void R_WaterSkySample(const byte *pic, int width, int height, float u, float v, byte *out)
{
    float x = u * width - 0.5f, y = v * height - 0.5f;
    int   x0, y0, x1, y1, c;
    float fx, fy;

    x  = Com_Clamp(0.0f, width - 1.0f, x);
    y  = Com_Clamp(0.0f, height - 1.0f, y);
    x0 = (int)x;
    y0 = (int)y;
    x1 = Q_min(x0 + 1, width - 1);
    y1 = Q_min(y0 + 1, height - 1);
    fx = x - x0;
    fy = y - y0;

    for (c = 0; c < 3; c++) {
        const float top    = pic[(y0 * width + x0) * 4 + c] * (1.0f - fx) + pic[(y0 * width + x1) * 4 + c] * fx;
        const float bottom = pic[(y1 * width + x0) * 4 + c] * (1.0f - fx) + pic[(y1 * width + x1) * 4 + c] * fx;

        out[c] = (byte)(top * (1.0f - fy) + bottom * fy + 0.5f);
    }
    out[3] = 255;
}

static const shader_t *R_WaterWorldSky(qboolean *hasWater)
{
    const shader_t *sky = NULL;
    int             i;

    *hasWater = qfalse;
    for (i = 0; i < tr.world->numsurfaces; i++) {
        const shader_t *sh = tr.world->surfaces[i].shader;

        if (!sh) {
            continue;
        }
        if (sh->waterImage) {
            *hasWater = qtrue;
        }
        if (!sky && sh->isSky && sh->sky.outerbox[0] && sh->sky.outerbox[0] != tr.defaultImage) {
            sky = sh;
        }
    }
    return sky;
}

static void R_WaterBuildSky(void)
{
    const shader_t *sh;
    qboolean        hasWater;
    byte           *pics[6];
    int             widths[6], heights[6];
    byte           *faces;
    int             average[3] = {0, 0, 0}, loaded = 0;
    int             face, side, i;

    waterSkyShader = NULL;
    waterSkySeen   = qfalse;

    if (!tr.world || !glRefConfig.framebufferObject || !tr.waterShader.program) {
        return;
    }
    sh = R_WaterWorldSky(&hasWater);
    if (!sh || (!hasWater && !water.beachSet)) {
        return;
    }

    for (side = 0; side < 6; side++) {
        GLenum format;

        pics[side] = NULL;
        if (sh->sky.outerbox[side] && sh->sky.outerbox[side] != tr.defaultImage) {
            R_LoadImageUncompressed(sh->sky.outerbox[side]->imgName, &pics[side], &widths[side], &heights[side], &format);
            if (pics[side] && (format != GL_RGBA8 || widths[side] <= 0 || heights[side] <= 0)) {
                ri.Free(pics[side]);
                pics[side] = NULL;
            }
        }
        if (!pics[side]) {
            continue;
        }

        // as the sky's own images are uploaded (Upload32)
        R_LightScaleTexture(pics[side], widths[side], heights[side], qfalse);
        for (i = 0; i < widths[side] * heights[side]; i += 61) {
            average[0] += pics[side][i * 4 + 0];
            average[1] += pics[side][i * 4 + 1];
            average[2] += pics[side][i * 4 + 2];
            loaded++;
        }
    }
    if (!loaded) {
        return;
    }

    faces = ri.Hunk_AllocateTempMemory(WATER_SKY_SIZE * WATER_SKY_SIZE * 4 * 6);

    // GL's cube sides, in its order (+x -x +y -y +z -z), each texel the
    // direction GL looks it up in
    for (face = 0; face < 6; face++) {
        byte *out = faces + face * WATER_SKY_SIZE * WATER_SKY_SIZE * 4;
        int   x, y;

        for (y = 0; y < WATER_SKY_SIZE; y++) {
            for (x = 0; x < WATER_SKY_SIZE; x++, out += 4) {
                const float sc = 2.0f * (x + 0.5f) / WATER_SKY_SIZE - 1.0f;
                const float tc = 2.0f * (y + 0.5f) / WATER_SKY_SIZE - 1.0f;
                vec3_t      dir, b;
                int         axis, image, j;

                switch (face) {
                case 0: VectorSet(dir, 1.0f, -tc, -sc); break;
                case 1: VectorSet(dir, -1.0f, -tc, sc); break;
                case 2: VectorSet(dir, sc, 1.0f, tc); break;
                case 3: VectorSet(dir, sc, -1.0f, -tc); break;
                case 4: VectorSet(dir, sc, -tc, 1.0f); break;
                default: VectorSet(dir, -sc, -tc, -1.0f); break;
                }

                // the box's side that way (AddSkyPolygon), and where on it
                if (fabs(dir[0]) > fabs(dir[1]) && fabs(dir[0]) > fabs(dir[2])) {
                    axis = dir[0] < 0 ? 1 : 0;
                } else if (fabs(dir[1]) > fabs(dir[2]) && fabs(dir[1]) > fabs(dir[0])) {
                    axis = dir[1] < 0 ? 3 : 2;
                } else {
                    axis = dir[2] < 0 ? 5 : 4;
                }
                for (j = 0; j < 3; j++) {
                    const int k = waterSkyStToVec[axis][j];

                    b[abs(k) - 1] = k < 0 ? -dir[j] : dir[j];
                }

                image = waterSkyTexOrder[axis];
                if (pics[image] && b[2] > 0.0f) {
                    const float s = b[0] / b[2], t = b[1] / b[2];

                    R_WaterSkySample(pics[image], widths[image], heights[image], (s + 1.0f) * 0.5f,
                        1.0f - (t + 1.0f) * 0.5f, out);
                } else {
                    // a side the sky has no image for: the rest of it, evened out
                    out[0] = average[0] / loaded;
                    out[1] = average[1] / loaded;
                    out[2] = average[2] / loaded;
                    out[3] = 255;
                }
            }
        }
    }

    for (side = 0; side < 6; side++) {
        if (pics[side]) {
            ri.Free(pics[side]);
        }
    }

    if (!tr.waterSkyImage) {
        tr.waterSkyImage = R_CreateImage("*waterSky", NULL, WATER_SKY_SIZE, WATER_SKY_SIZE, IMGTYPE_COLORALPHA,
            IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_MIPMAP | IMGFLAG_CUBEMAP, GL_RGBA8);
        // made once and filled again for each map: not one of this map's
        // images, which the next map frees (R_FreeUnusedImages)
        tr.waterSkyImage->r_sequence = -1;
    }
    for (face = 0; face < 6; face++) {
        qglTextureSubImage2DEXT(tr.waterSkyImage->texnum, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0,
            WATER_SKY_SIZE, WATER_SKY_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, faces + face * WATER_SKY_SIZE * WATER_SKY_SIZE * 4);
    }
    qglGenerateTextureMipmapEXT(tr.waterSkyImage->texnum, GL_TEXTURE_CUBE_MAP);

    ri.Hunk_FreeTempMemory(faces);

    waterSkyShader = sh;
}

// The view drew this sky's box (RB_StageIteratorSky): water may mirror it
void RB_WaterSawSky(const shader_t *sh)
{
    if (sh && sh == waterSkyShader) {
        waterSkySeen = qtrue;
    }
}

// How much of the sky box to mirror, and the cubemap bound for it
static float RB_WaterBindSky(void)
{
    if (!waterSkySeen || !tr.waterSkyImage || water.skyBox <= 0.0f || r_fastsky->integer) {
        return 0.0f;
    }
    GL_BindToTMU(tr.waterSkyImage, TB_CUBEMAP);
    return Q_min(water.skyBox, 1.0f);
}

void R_WaterLoadWorld(const char *mapName)
{
    R_WaterReadFile(mapName);
    R_WaterBuildSky();
}

/*
===============
R_WaterFinishShader

A shader with `surfaceparm water` is drawn by the water program, if it is
there. Its stages stay, for a view the program cannot draw (RB_WaterDraw).
Its deforms go: the waves are the program's, and without them the surface is
drawn from the world's buffers instead of being built again each frame.
===============
*/
void R_WaterFinishShader(shader_t *sh, shaderStage_t *firstStage)
{
    if (!r_water->integer || !tr.waterShader.program) {
        return;
    }
    if (!(sh->contentFlags & CONTENTS_WATER) || (sh->surfaceFlags & (SURF_NODRAW | SURF_SKY)) || sh->isSky) {
        return;
    }
    if (!firstStage->active || !firstStage->bundle[0].image[0]) {
        return;
    }

    sh->isWater    = qtrue;
    sh->waterImage = firstStage->bundle[0].image[0];
    sh->sort       = SS_UNDERWATER + 0.5f;
    sh->numDeforms = 0;
    sh->vertexAttribs |= ATTR_POSITION | ATTR_NORMAL;
}

// The sky water mirrors: the map's, else what the fog is, else a grey one
static void R_WaterSky(vec4_t out)
{
    if (water.skySet) {
        VectorCopy(water.sky, out);
    } else if (backEnd.globalFogEnabled) {
        VectorCopy(backEnd.globalFogColor, out);
    } else {
        VectorCopy(water.sky, out);
    }
    out[3] = 0.0f;
}

// The fog, as an opaque stage takes it
static void R_WaterFog(shaderProgram_t *sp)
{
    vec4_t color;
    vec2_t params = {0.0f, 0.0f};

    VectorClear4(color);
    if (backEnd.globalFogEnabled && !tess.no_global_fog) {
        VectorCopy4(backEnd.globalFogColor, color);
        params[0] = backEnd.globalFogStart;
        params[1] = backEnd.globalFogInvRange;
    }
    GLSL_SetUniformVec4(sp, UNIFORM_GLOBALFOGCOLOR, color);
    GLSL_SetUniformVec2(sp, UNIFORM_GLOBALFOGPARAMS, params);
}

static qboolean RB_WaterCopyView(void);

// How the latest water was drawn, for waterinfo: with the depth, with the view
// before it, and if not, which buffer the view was being drawn to
static qboolean waterLastDepth, waterLastView;
static int      waterLastFbo;

// The view's box on the screen, a share of it across and up: where it starts,
// and where it ends
static void RB_WaterViewBox(vec4_t box)
{
    box[0] = backEnd.viewParms.viewportX / (float)glConfig.vidWidth;
    box[1] = backEnd.viewParms.viewportY / (float)glConfig.vidHeight;
    box[2] = box[0] + backEnd.viewParms.viewportWidth / (float)glConfig.vidWidth;
    box[3] = box[1] + backEnd.viewParms.viewportHeight / (float)glConfig.vidHeight;
}

// The view as it is before its first water, for the program to see through
static qboolean RB_WaterCopyView(void)
{
    FBO_t *fbo = glState.currentFBO;

    if (backEnd.waterScene) {
        return qtrue;
    }
    // only the view the scene is drawn to: not a cubemap's face
    if (!tr.waterSceneFbo || !glRefConfig.framebufferBlit || fbo != tr.renderFbo) {
        return qfalse;
    }

    FBO_FastBlit(fbo, NULL, tr.waterSceneFbo, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    FBO_Bind(fbo);
    qglViewport(backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
        backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight);
    qglScissor(backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
        backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight);

    backEnd.waterScene = qtrue;
    return qtrue;
}

/*
===============
RB_WaterRipples

Steps the ripples on to now, once a frame, after the opaque surfaces of the
first view of the world that has them (RB_BeachDraw): between batches, as it
draws with tess.
===============
*/
static void RB_WaterRipples(void)
{
    const float now   = backEnd.refdef.floatTime;
    const float texel = WATER_RIPPLE_SPAN / WATER_RIPPLE_SIZE;
    const float speed = WATER_RIPPLE_SPEED / WATER_RIPPLE_HZ / texel;
    shaderProgram_t *sp = &tr.rippleShader;
    FBO_t *oldFbo = glState.currentFBO;
    vec4_t quadVerts[4], v;
    vec2_t texCoords[4];
    vec4_t sources[WATER_MAX_SPOTS], foam[WATER_MAX_SPOTS];
    int    steps, step, x, y, i;

    if (!sp->program || !tr.waterRippleFbo[0] || !tr.waterRippleFbo[1] || now == ripples.time
        || backEnd.projection2D || (backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))
        || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) || backEnd.viewParms.isPortal
        || glState.currentFBO != tr.renderFbo) {
        return;
    }

    if (ripples.time < 0.0f || now < ripples.time || now - ripples.time > 1.0f) {
        ripples.owed = 1.0f / WATER_RIPPLE_HZ;
    } else {
        ripples.owed += now - ripples.time;
    }
    ripples.time = now;

    steps = (int)(ripples.owed * WATER_RIPPLE_HZ);
    if (!steps) {
        return;
    }
    if (steps > WATER_RIPPLE_STEPS) {
        steps        = WATER_RIPPLE_STEPS;
        ripples.owed = 0.0f;
    } else {
        ripples.owed -= steps / WATER_RIPPLE_HZ;
    }

    // the grid goes with the view, a texel at a time
    x = (int)floor(backEnd.viewParms.ori.origin[0] / texel);
    y = (int)floor(backEnd.viewParms.ori.origin[1] / texel);
    if (ripples.clear || abs(x - ripples.x) >= WATER_RIPPLE_SIZE / 2 || abs(y - ripples.y) >= WATER_RIPPLE_SIZE / 2) {
        for (i = 0; i < 2; i++) {
            FBO_Bind(tr.waterRippleFbo[i]);
            qglViewport(0, 0, WATER_RIPPLE_SIZE, WATER_RIPPLE_SIZE);
            qglScissor(0, 0, WATER_RIPPLE_SIZE, WATER_RIPPLE_SIZE);
            qglClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            qglClear(GL_COLOR_BUFFER_BIT);
        }
        ripples.x     = x;
        ripples.y     = y;
        ripples.clear = qfalse;
    }

    // what disturbs it, where it is on the grid as it will be
    for (i = 0; i < numWaterRipples; i++) {
        const float *r = waterRipples[i];

        sources[i][0] = (r[0] / texel - (x - WATER_RIPPLE_SIZE / 2)) / WATER_RIPPLE_SIZE;
        sources[i][1] = (r[1] / texel - (y - WATER_RIPPLE_SIZE / 2)) / WATER_RIPPLE_SIZE;
        sources[i][2] = r[2] / WATER_RIPPLE_SPAN;
        sources[i][3] = r[3];
        VectorSet4(foam[i], r[4], 0.0f, 0.0f, 0.0f);
    }

    VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
    VectorSet2(texCoords[0], 0.0f, 1.0f);
    VectorSet2(texCoords[1], 1.0f, 1.0f);
    VectorSet2(texCoords[2], 1.0f, 0.0f);
    VectorSet2(texCoords[3], 0.0f, 0.0f);

    GLSL_BindProgram(sp);
    GL_State(GLS_DEPTHTEST_DISABLE);
    GL_Cull(CT_TWO_SIDED);

    for (step = 0; step < steps; step++) {
        const int from = ripples.current, to = !ripples.current;
        const int disturbed = step ? 0 : numWaterRipples;

        FBO_Bind(tr.waterRippleFbo[to]);
        qglViewport(0, 0, WATER_RIPPLE_SIZE, WATER_RIPPLE_SIZE);
        qglScissor(0, 0, WATER_RIPPLE_SIZE, WATER_RIPPLE_SIZE);
        GL_BindToTMU(tr.waterRippleImage[from], TB_DIFFUSEMAP);

        // the grid moved with the view on the first step only
        VectorSet4(v, 1.0f / WATER_RIPPLE_SIZE, step ? 0.0f : x - ripples.x, step ? 0.0f : y - ripples.y, speed * speed);
        GLSL_SetUniformVec4(sp, UNIFORM_RIPPLESIM, v);
        // ripples die away over a few seconds, foam a little slower
        VectorSet4(v, 0.995f, 0.998f, 0.996f, disturbed);
        GLSL_SetUniformVec4(sp, UNIFORM_RIPPLEDECAY, v);
        if (disturbed) {
            GLSL_SetUniformWaterSpots(sp, UNIFORM_RIPPLESOURCES, sources, disturbed);
            GLSL_SetUniformWaterSpots(sp, UNIFORM_RIPPLEFOAM, foam, disturbed);
        }

        RB_InstantQuad2(quadVerts, texCoords);
        ripples.current = to;
    }
    ripples.x = x;
    ripples.y = y;
    numWaterRipples = 0;

    FBO_Bind(oldFbo);
    qglViewport(backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
        backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight);
    qglScissor(backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
        backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight);
}

/*
===============
RB_WaterDraw

Draws tess with the water program. qfalse if it cannot, and the shader's own
stages are to draw it.
===============
*/
qboolean RB_WaterDraw(shaderCommands_t *input)
{
    shaderProgram_t *sp = &tr.waterShader;
    vec4_t           v;
    qboolean         hasDepth, hasView;
    float            zNear;

    if (!sp->program || !input->shader->waterImage || backEnd.projection2D
        || (backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))) {
        return qfalse;
    }

    hasDepth = backEnd.softDepth && tr.hdrDepthImage;
    hasView  = RB_WaterCopyView();

    waterLastDepth = hasDepth;
    waterLastView  = hasView;
    waterLastFbo   = glState.currentFBO == tr.renderFbo ? 0 : glState.currentFBO ? glState.currentFBO->frameBuffer : -1;
    zNear    = backEnd.viewParms.zNear > 0 ? backEnd.viewParms.zNear : r_znear->value;

    GLSL_BindProgram(sp);

    GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
    GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.ori.transformMatrix);
    GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.ori.origin);
    GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);

    VectorSet4(v, zNear, backEnd.viewParms.zFar, hasDepth ? 1.0f : 0.0f, hasView ? 1.0f : 0.0f);
    GLSL_SetUniformVec4(sp, UNIFORM_VIEWINFO, v);

    VectorCopy(backEnd.refdef.sunDir, v);
    v[3] = 0.0f;
    GLSL_SetUniformVec4(sp, UNIFORM_PRIMARYLIGHTORIGIN, v);
    // the sun's colour, at most one: the scene's light is overbright, and a
    // glint of that on the waves is a white blot
    {
        const float most = Q_max(Q_max(backEnd.refdef.sunCol[0], backEnd.refdef.sunCol[1]), backEnd.refdef.sunCol[2]);
        vec3_t      sun;

        VectorScale(backEnd.refdef.sunCol, most > 1.0f ? 1.0f / most : 1.0f, sun);
        GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTCOLOR, sun);
    }

    R_WaterFog(sp);

    // to follow what it mirrors through the view
    {
        mat4_t worldMvp;

        Mat4Multiply(backEnd.viewParms.projectionMatrix, backEnd.viewParms.world.modelMatrix, worldMvp);
        GLSL_SetUniformMat4(sp, UNIFORM_WORLDVIEWPROJECTION, worldMvp);
        RB_WaterViewBox(v);
        GLSL_SetUniformVec4(sp, UNIFORM_WATERVIEW, v);
    }

    GLSL_SetUniformVec4(sp, UNIFORM_WATERCOLOR, water.color);

    VectorCopy(water.blood, v);
    v[3] = water.blood[3] * Q_max(r_waterBlood->value, 0.0f);
    GLSL_SetUniformVec4(sp, UNIFORM_WATERBLOOD, v);

    GLSL_SetUniformVec4(sp, UNIFORM_WATERWAVES, water.waves);

    VectorSet4(v, water.height, water.surf, water.shoreSet ? 1.0f : 0.0f, water.shore);
    GLSL_SetUniformVec4(sp, UNIFORM_WATERSHORE, v);

    // the bodies' blood counts for as much however much the map has
    {
        const int spots = r_waterBlood->value > 0.0f ? numWaterSpots : 0;

        if (water.shoreSet) {
            VectorSet4(v, water.reachNear, water.reachFar, spots, 0.0f);
        } else {
            VectorSet4(v, water.bloodNear, water.bloodFar, spots, 0.0f);
        }
        GLSL_SetUniformVec4(sp, UNIFORM_WATERBLOODZONE, v);
        if (spots) {
            GLSL_SetUniformWaterSpots(sp, UNIFORM_WATERSPOTS, waterSpots, spots);
        }
    }

    R_WaterSky(v);
    v[3] = water.chop;
    GLSL_SetUniformVec4(sp, UNIFORM_WATERSKY, v);
    VectorSet4(v, RB_WaterBindSky(), 0.0f, 0.0f, 0.0f);
    GLSL_SetUniformVec4(sp, UNIFORM_CUBEMAPINFO, v);

    VectorSet4(v, water.foam, water.refraction, water.glints, water.bloodClouds);
    GLSL_SetUniformVec4(sp, UNIFORM_WATERLOOK, v);

    // where the ripples' grid lies
    if (tr.rippleShader.program && tr.waterRippleImage[0] && ripples.time >= 0.0f) {
        const float texel = WATER_RIPPLE_SPAN / WATER_RIPPLE_SIZE;

        VectorSet4(v, (ripples.x - WATER_RIPPLE_SIZE / 2) * texel, (ripples.y - WATER_RIPPLE_SIZE / 2) * texel,
            1.0f / WATER_RIPPLE_SPAN, 1.0f);
        GL_BindToTMU(tr.waterRippleImage[ripples.current], TB_SPECULARMAP);
    } else {
        VectorClear4(v);
        GL_BindToTMU(tr.whiteImage, TB_SPECULARMAP); // not read: no grid
    }
    GLSL_SetUniformVec4(sp, UNIFORM_WATERRIPPLES, v);

    GL_BindToTMU(input->shader->waterImage, TB_DIFFUSEMAP);
    GL_BindToTMU(hasView ? tr.waterSceneImage : tr.whiteImage, TB_LIGHTMAP);
    GL_BindToTMU(hasDepth ? tr.hdrDepthImage : tr.whiteImage, TMU_SOFTDEPTH);

    GL_State(GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK_TRUE);

    R_DrawElements(input->numIndexes, input->firstIndex);

    return qtrue;
}

// Reads the map's water file again: for working on one
void R_WaterReload_f(void)
{
    if (!tr.world) {
        ri.Printf(PRINT_ALL, "No map loaded\n");
        return;
    }
    R_WaterLoadWorld(tr.world->baseName);
    R_WaterInfo_f();
}

/*
===============
RB_BeachDraw

The beach the tide is leaving, over the opaque view: called once a view,
between the opaque world and anything blended (RB_RenderDrawSurfList). Two
passes over the view, each pixel placed in the world from the prepass's depth:
the first darkens the sand (wet, under water standing in it, soaked red under
bodies), the second adds the sky it mirrors. The water's ripples are stepped
on here first.
===============
*/
void RB_BeachDraw(void)
{
    vec4_t quadVerts[4];
    vec2_t texCoords[4];
    vec4_t box, v;
    vec3_t viewVector;
    mat4_t worldMvp;
    float  zmax, xmax, ymax;
    qboolean hasView;
    int    pass;

    RB_WaterRipples();

    if (!water.beachSet || !water.shoreSet || !tr.beachShader[1].program || !backEnd.softDepth || !tr.hdrDepthImage
        || backEnd.projection2D || (backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))
        || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)) {
        return;
    }

    RB_WaterViewBox(box);

    texCoords[0][0] = box[0]; texCoords[0][1] = box[3];
    texCoords[1][0] = box[2]; texCoords[1][1] = box[3];
    texCoords[2][0] = box[2]; texCoords[2][1] = box[1];
    texCoords[3][0] = box[0]; texCoords[3][1] = box[1];

    VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
    VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);

    zmax = backEnd.viewParms.zFar;
    ymax = zmax * tan(backEnd.viewParms.fovY * M_PI / 360.0f);
    xmax = zmax * tan(backEnd.viewParms.fovX * M_PI / 360.0f);

    // what the wet sand mirrors: the view as it is now
    hasView = RB_WaterCopyView();
    Mat4Multiply(backEnd.viewParms.projectionMatrix, backEnd.viewParms.world.modelMatrix, worldMvp);

    for (pass = 0; pass < 2; pass++) {
        shaderProgram_t *sp = &tr.beachShader[pass];

        GLSL_BindProgram(sp);
        GL_BindToTMU(tr.hdrDepthImage, TB_COLORMAP);
        GL_BindToTMU(hasView ? tr.waterSceneImage : tr.whiteImage, TB_LIGHTMAP);
        GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, worldMvp);

        GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.refdef.vieworg);
        VectorScale(backEnd.refdef.viewaxis[0], zmax, viewVector);
        GLSL_SetUniformVec3(sp, UNIFORM_VIEWFORWARD, viewVector);
        VectorScale(backEnd.refdef.viewaxis[1], xmax, viewVector);
        GLSL_SetUniformVec3(sp, UNIFORM_VIEWLEFT, viewVector);
        VectorScale(backEnd.refdef.viewaxis[2], ymax, viewVector);
        GLSL_SetUniformVec3(sp, UNIFORM_VIEWUP, viewVector);

        VectorSet4(v, backEnd.viewParms.zNear > 0 ? backEnd.viewParms.zNear : r_znear->value, zmax,
            box[2] - box[0], box[3] - box[1]);
        GLSL_SetUniformVec4(sp, UNIFORM_VIEWINFO, v);
        GLSL_SetUniformFloat(sp, UNIFORM_TIME, backEnd.refdef.floatTime);
        R_WaterFog(sp);

        GLSL_SetUniformVec4(sp, UNIFORM_WATERWAVES, water.waves);
        VectorSet4(v, water.height, water.surf, 1.0f, water.shore);
        GLSL_SetUniformVec4(sp, UNIFORM_WATERSHORE, v);
        R_WaterSky(v);
        GLSL_SetUniformVec4(sp, UNIFORM_WATERSKY, v);
        VectorSet4(v, RB_WaterBindSky(), 0.0f, 0.0f, 0.0f);
        GLSL_SetUniformVec4(sp, UNIFORM_CUBEMAPINFO, v);
        GLSL_SetUniformVec4(sp, UNIFORM_WATERBLOOD, water.blood);
        VectorSet4(v, water.beachReach, water.beachTop, water.puddles, water.puddleSize);
        GLSL_SetUniformVec4(sp, UNIFORM_BEACHWET, v);
        VectorSet4(v, water.swash, 0.0f, 0.0f, 0.0f);
        GLSL_SetUniformVec4(sp, UNIFORM_BEACHSWASH, v);

        {
            const int stains = r_waterBlood->value > 0.0f ? numSandStains : 0;

            VectorSet4(v, 0.0f, 0.0f, 0.0f, stains);
            GLSL_SetUniformVec4(sp, UNIFORM_WATERBLOODZONE, v);
            if (stains) {
                GLSL_SetUniformWaterSpots(sp, UNIFORM_SANDSTAINS, sandStains, stains);
            }
        }

        // depth tested, not written: each pixel is at the prepass's depth
        // (beach_fp.glsl), so where the main pass drew something nearer that
        // the prepass did not have, the beach is not drawn over it
        GL_State(pass ? (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE) : (GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO));
        GL_Cull(CT_TWO_SIDED);

        RB_InstantQuad2(quadVerts, texCoords);
    }

    // the water is to see the beach as it is now
    backEnd.waterScene = qfalse;
}

void R_WaterInfo_f(void)
{
    ri.Printf(PRINT_ALL, "Water: %s, %s\n", r_water->integer && tr.waterShader.program ? "the water program" : "the shaders' stages",
        water.fromFile ? water.file : "no water file for this map (the defaults)");
    ri.Printf(PRINT_ALL, "  color %g %g %g  clarity %g\n", water.color[0], water.color[1], water.color[2], water.color[3]);
    ri.Printf(PRINT_ALL, "  waves %g %g %g %g %g  surf %g\n", water.waves[0], water.waves[1], water.waves[2], water.waves[3],
        water.height, water.surf);
    ri.Printf(PRINT_ALL, "  blood %g %g %g %g (x r_waterBlood %g)  %s  bloodClouds %g\n", water.blood[0], water.blood[1],
        water.blood[2], water.blood[3], r_waterBlood->value,
        water.shoreSet ? va("shore %g  bloodReach %g %g", water.shore, water.reachNear, water.reachFar)
                       : va("bloodDepth %g %g", water.bloodNear, water.bloodFar),
        water.bloodClouds);
    ri.Printf(PRINT_ALL, "  %d bodies' blood in the water, %d on the sand\n", numWaterSpots, numSandStains);
    ri.Printf(PRINT_ALL, "  last drawn: depth %s, view %s%s\n", waterLastDepth ? "yes" : "no", waterLastView ? "yes" : "no",
        waterLastFbo ? va(" (drawn to buffer %d, not the scene's)", waterLastFbo) : "");
    if (ri.Cmd_Argc() > 1) {
        int i;

        for (i = 0; i < numSandStains; i++) {
            ri.Printf(PRINT_ALL, "    on the sand at %.0f %.0f %.0f, %.0f across\n", sandStains[i][0], sandStains[i][1],
                sandStains[i][2], sandStains[i][3]);
        }
    }
    if (water.beachSet) {
        ri.Printf(PRINT_ALL, "  beach %g %g  puddles %g %g  swash %g%s\n", water.beachReach, water.beachTop, water.puddles,
            water.puddleSize, water.swash,
            water.shoreSet ? "" : "  (no shore line: not drawn)");
    }
    ri.Printf(PRINT_ALL, "  sky %s  chop %g  foam %g  refraction %g  glints %g\n", water.skySet ? va("%g %g %g", water.sky[0], water.sky[1], water.sky[2]) : "(the fog's)",
        water.chop, water.foam, water.refraction, water.glints);
    ri.Printf(PRINT_ALL, "  skyBox %g  %s\n", water.skyBox,
        !waterSkyShader ? "(no sky box to mirror)" : waterSkySeen ? va("mirrors %s", waterSkyShader->name) : va("%s, not drawn yet", waterSkyShader->name));
}

/*
===============
R_WaterSetWet

Added in OPM
  The wet layer (glsl/lightall_fp.glsl) for a draw: on a map with a beach
  the tide is leaving, what stands below the mark it reached (the wet sand's
  top) and a little above it, where the spray goes, is wet. None elsewhere.
  Every draw with the lightall program sets it, wet or dry: a uniform left
  as the last draw had it wets whatever comes next (the sky box, below the
  mark, came out a grey wall round the map).
===============
*/
void R_WaterSetDry(shaderProgram_t *sp)
{
    vec4_t v;

    VectorClear4(v);
    GLSL_SetUniformVec4(sp, UNIFORM_WET, v);
}

void R_WaterSetWet(shaderProgram_t *sp)
{
    vec4_t v;

    if (!r_waterWet->integer || !water.beachSet || !water.shoreSet || !tr.world
        || (backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))) {
        R_WaterSetDry(sp);
        return;
    }

    VectorSet4(v, 1.0f, water.beachTop + 24.0f, 0.0f, 0.0f);
    GLSL_SetUniformVec4(sp, UNIFORM_WET, v);

    R_WaterSky(v);
    GLSL_SetUniformVec4(sp, UNIFORM_WETSKY, v);

    // the sun, as on the water: its light at most one, as a glint of the
    // overbright scene light is a white blot
    {
        const float most = Q_max(Q_max(backEnd.refdef.sunCol[0], backEnd.refdef.sunCol[1]), backEnd.refdef.sunCol[2]);

        VectorCopy(backEnd.refdef.sunDir, v);
        VectorNormalize(v);
        v[3] = Com_Clamp(0.0f, 1.0f, most) * 0.6f;
        GLSL_SetUniformVec4(sp, UNIFORM_WETSUN, v);
    }
}
