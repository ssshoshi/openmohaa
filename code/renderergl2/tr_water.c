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
//     ocean 190 12 0.38 32    // the open sea's waves in place of the chop: their scale (units; the
//                             // longest is 6.3 of it), height, drag (how they gather into crests), octaves
//
// And where the map has a beach the tide is going out from (it needs `shore`):
//
//     beach 900 -440          // the sand is wet this far inland of the shore line, and up to this height
//     puddles 0.3 90          // water stands in its hollows: how much of it, and how big they are
//     swash 160               // how far up the sand a wave runs, at most, from the shore line
//     remapShader textures/a textures/b // draw the first as the second on this map (up to 4)
//     beachSurface textures/sand  // the beach is the ground of this shader alone (up to 4 of these
//                             // lines): not the sand under the water, nor what stands on it
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
cvar_t        *r_rainWet;     // Added in OPM: set by the cgame: 1 once the map has rained (cg_lens.cpp)
cvar_t        *r_rainPuddles;
static cvar_t *r_rainPuddleShaders;
static cvar_t *r_rainFlash;
static cvar_t *r_rainRunoff;
static cvar_t *r_rainMist;
cvar_t        *r_rainBeads;
cvar_t        *r_rainDebug;
cvar_t        *r_waterReflect;
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
    vec4_t ocean; // the open sea's waves: their scale (units), height, drag, octaves; 0: the chop
    qboolean beachSet;
    float  beachReach, beachTop;
    float  puddles, puddleSize;
    float  swash;
    char   beachSurfaces[4][MAX_QPATH];
    int    numBeachSurfaces;
    char   remaps[4][2][MAX_QPATH]; // shaders drawn as others on this map
    int    numRemaps;
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
    r_rainWet     = ri.Cvar_Get("r_rainWet", "0", CVAR_TEMP);
    r_rainPuddles = ri.Cvar_Get("r_rainPuddles", "0.35", CVAR_ARCHIVE);
    r_rainBeads   = ri.Cvar_Get("r_rainBeads", "1", CVAR_ARCHIVE);
    r_rainPuddleShaders = ri.Cvar_Get("r_rainPuddleShaders", "mudpath_trans2", CVAR_ARCHIVE);
    r_rainFlash = ri.Cvar_Get("r_rainFlash", "1", CVAR_ARCHIVE);
    r_rainRunoff = ri.Cvar_Get("r_rainRunoff", "1", CVAR_ARCHIVE);
    r_rainMist = ri.Cvar_Get("r_rainMist", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_rainMist, "In the rain, the mist lying low over what is out under the sky, drifting, and the haze of the rain further up (r_volumetric 2): 0 none, 1 as it is, more for thicker");
    ri.Cvar_SetDescription(r_rainRunoff, "In the rain, water pours off the eaves of roofs and splashes where it lands: 0 none, 1 as it is, more for more of it");
    ri.Cvar_SetDescription(r_rainFlash, "In the rain, how brightly lightning lights what is out under the sky: 0 not at all");
    ri.Cvar_SetDescription(r_rainPuddleShaders, "In the rain, the textures puddles gather on: names or parts of names, separated by spaces or commas (empty: all open ground)");
    r_rainDebug   = ri.Cvar_Get("r_rainDebug", "0", CVAR_CHEAT);
    ri.Cvar_SetDescription(r_rainWet, "How wet the rain has made what is out under the sky, 0 to 1 (the game sets it on a map with rain; r_waterWet 0 turns it off)");
    ri.Cvar_SetDescription(r_rainPuddles, "In the rain, how much of the open ground holds puddles, with the rain ringing them: 0 none, 1 most of it");
    ri.Cvar_SetDescription(r_rainBeads, "In the rain, drops beaded on the hands and the weapon in view: 0 none, 1 as many as hold");
    r_waterReflect = ri.Cvar_Get("r_waterReflect", "1", CVAR_ARCHIVE);
    ri.Cvar_SetDescription(r_waterReflect, "Water mirrors the world from under its surface, a second view drawn first: 0 only what is on the screen, 1 at half the screen's size, 2 at all of it (the size needs vid_restart), 3 as 1 and shows it in a corner");
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
        } else if (!Q_stricmp(key, "ocean")) {
            if (R_WaterNumbers(&text, v, 4) == 4) {
                VectorSet4(water.ocean, Q_max(v[0], 1.0f), v[1], v[2], Com_Clamp(4.0f, 40.0f, v[3]));
            }
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
        } else if (!Q_stricmp(key, "remapShader")) {
            char from[MAX_QPATH];

            token = COM_ParseExt(&text, qfalse);
            Q_strncpyz(from, token, sizeof(from));
            token = COM_ParseExt(&text, qfalse);
            if (from[0] && token[0] && water.numRemaps < 4) {
                Q_strncpyz(water.remaps[water.numRemaps][0], from, MAX_QPATH);
                Q_strncpyz(water.remaps[water.numRemaps][1], token, MAX_QPATH);
                water.numRemaps++;
            }
        } else if (!Q_stricmp(key, "beachSurface")) {
            token = COM_ParseExt(&text, qfalse);
            if (token[0] && water.numBeachSurfaces < 4) {
                Q_strncpyz(water.beachSurfaces[water.numBeachSurfaces++], token, MAX_QPATH);
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

/*
===============
R_WaterBuildBeach

The beach's ground (beachSurface in the water file): the surfaces of those
shaders, the world's and the terrain's, drawn from above into a grid of their
height, tr.beachGroundImage. The beach pass (beach_fp.glsl) is drawn only on
that ground: not on the sand under the water, which is another shader and
has the water's own reflection over it, nor on what stands on the beach.
===============
*/
#define BEACH_GROUND_SIZE 1024
#define BEACH_GROUND_NONE -1.0e9f

static struct {
    float *height;
    int    width, rows;
    float  x0, y0, texel;
    vec2_t mins, maxs;
    qboolean on;
} beachGround;

static qboolean R_WaterIsBeachShader(const shader_t *sh)
{
    int i;

    for (i = 0; sh && i < water.numBeachSurfaces; i++) {
        if (!Q_stricmp(sh->name, water.beachSurfaces[i])) {
            return qtrue;
        }
    }
    return qfalse;
}

// A triangle of the ground: its bounds, or its height at each texel it covers
static void R_WaterBeachTri(const vec3_t a, const vec3_t b, const vec3_t c, qboolean draw)
{
    float area, minx, maxx, miny, maxy;
    int   x0, x1, y0, y1, x, y;

    minx = Q_min(a[0], Q_min(b[0], c[0]));
    maxx = Q_max(a[0], Q_max(b[0], c[0]));
    miny = Q_min(a[1], Q_min(b[1], c[1]));
    maxy = Q_max(a[1], Q_max(b[1], c[1]));
    if (!draw) {
        beachGround.mins[0] = Q_min(beachGround.mins[0], minx);
        beachGround.mins[1] = Q_min(beachGround.mins[1], miny);
        beachGround.maxs[0] = Q_max(beachGround.maxs[0], maxx);
        beachGround.maxs[1] = Q_max(beachGround.maxs[1], maxy);
        return;
    }

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (fabs(area) < 0.001f) {
        return; // edge on from above
    }
    x0 = Q_max(0, (int)floor((minx - beachGround.x0) / beachGround.texel));
    x1 = Q_min(beachGround.width - 1, (int)floor((maxx - beachGround.x0) / beachGround.texel));
    y0 = Q_max(0, (int)floor((miny - beachGround.y0) / beachGround.texel));
    y1 = Q_min(beachGround.rows - 1, (int)floor((maxy - beachGround.y0) / beachGround.texel));

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            const float px = beachGround.x0 + (x + 0.5f) * beachGround.texel;
            const float py = beachGround.y0 + (y + 0.5f) * beachGround.texel;
            const float wa = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / area;
            const float wb = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / area;
            const float wc = 1.0f - wa - wb;
            float      *h  = &beachGround.height[y * beachGround.width + x];

            if (wa < -0.001f || wb < -0.001f || wc < -0.001f) {
                continue;
            }
            *h = Q_max(*h, wa * a[2] + wb * b[2] + wc * c[2]);
        }
    }
}

// Every triangle of the beach's ground, for its bounds and then its height
static void R_WaterBeachSurfaces(qboolean draw)
{
    int i, k, x, y;

    for (i = 0; i < tr.world->numsurfaces; i++) {
        const msurface_t      *surf = &tr.world->surfaces[i];
        const srfBspSurface_t *bsp;

        if (!R_WaterIsBeachShader(surf->shader) || !surf->data) {
            continue;
        }
        if (*surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES) {
            continue;
        }
        bsp = (const srfBspSurface_t *)surf->data;
        for (k = 0; k + 2 < bsp->numIndexes; k += 3) {
            R_WaterBeachTri(bsp->verts[bsp->indexes[k]].xyz, bsp->verts[bsp->indexes[k + 1]].xyz,
                bsp->verts[bsp->indexes[k + 2]].xyz, draw);
        }
    }

    // the terrain: each patch 512 units square, its heights 9 by 9
    for (i = 0; i < tr.world->numTerraPatches; i++) {
        const cTerraPatchUnpacked_t *patch = &tr.world->terraPatches[i];

        if (!R_WaterIsBeachShader(patch->shader)) {
            continue;
        }
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                vec3_t p[4];

                for (k = 0; k < 4; k++) {
                    const int xi = x + (k & 1), yi = y + (k >> 1);

                    VectorSet(p[k], patch->x0 + xi * 64.0f, patch->y0 + yi * 64.0f,
                        patch->heightmap[yi * 9 + xi] * 2.0f + patch->z0);
                }
                R_WaterBeachTri(p[0], p[1], p[3], draw);
                R_WaterBeachTri(p[0], p[3], p[2], draw);
            }
        }
    }
}

static void R_WaterBuildBeach(void)
{
    float extent;
    int   i;

    if (beachGround.height) {
        ri.Free(beachGround.height);
    }
    memset(&beachGround, 0, sizeof(beachGround));
    // the last map's grid is one of its images, which the next map frees
    tr.beachGroundImage = NULL;

    if (!tr.world || !water.beachSet || !water.numBeachSurfaces || !tr.beachShader[0].program) {
        return;
    }

    VectorSet2(beachGround.mins, 1e9f, 1e9f);
    VectorSet2(beachGround.maxs, -1e9f, -1e9f);
    R_WaterBeachSurfaces(qfalse);
    if (beachGround.maxs[0] <= beachGround.mins[0] || beachGround.maxs[1] <= beachGround.mins[1]) {
        ri.Printf(PRINT_WARNING, "WARNING: %s: no surface of its beachSurface shaders, the beach is everywhere\n", water.file);
        return;
    }

    extent            = Q_max(beachGround.maxs[0] - beachGround.mins[0], beachGround.maxs[1] - beachGround.mins[1]);
    beachGround.texel = Q_max(extent / BEACH_GROUND_SIZE, 4.0f);
    beachGround.x0    = beachGround.mins[0] - beachGround.texel;
    beachGround.y0    = beachGround.mins[1] - beachGround.texel;
    beachGround.width = Q_min(BEACH_GROUND_SIZE, (int)ceil((beachGround.maxs[0] - beachGround.mins[0]) / beachGround.texel) + 2);
    beachGround.rows  = Q_min(BEACH_GROUND_SIZE, (int)ceil((beachGround.maxs[1] - beachGround.mins[1]) / beachGround.texel) + 2);

    beachGround.height = ri.Malloc(beachGround.width * beachGround.rows * sizeof(float));
    for (i = 0; i < beachGround.width * beachGround.rows; i++) {
        beachGround.height[i] = BEACH_GROUND_NONE;
    }
    R_WaterBeachSurfaces(qtrue);

    tr.beachGroundImage = R_CreateImage("*beachGround", NULL, beachGround.width, beachGround.rows, IMGTYPE_COLORALPHA,
        IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_R32F);
    qglTextureSubImage2DEXT(tr.beachGroundImage->texnum, GL_TEXTURE_2D, 0, 0, 0, beachGround.width, beachGround.rows,
        GL_RED, GL_FLOAT, beachGround.height);
    // a height or none: never one between them
    qglTextureParameteriEXT(tr.beachGroundImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    qglTextureParameteriEXT(tr.beachGroundImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    beachGround.on = qtrue;
}

/*
===============
R_RainBuildCover

Added in OPM
  What keeps the rain off: the world's surfaces and its terrain, all but the
  sky, what is not drawn and what is see-through, drawn from above into a
  grid of the highest of them at each place (tr.rainCoverImage). Out in the
  rain is at or above it (lightall_fp.glsl's RainExposed); a wall reads it a
  little way out from its face, where the rain falls past it.
  Built the first frame it rains on a map, from the front end.
===============
*/
#define RAIN_COVER_SIZE 2048
#define RAIN_COVER_NONE -1.0e9f

static struct {
    float   *height;
    float   *puddle;      // the highest of what puddles gather on, out in the rain (R_RainBuildPuddles)
    int      puddleCount; // r_rainPuddleShaders as it was built for
    float    lastHeight;  // the height the puddles' mirror last took (R_WaterReflectView)
    float    flash;       // a lightning flash, fading (R_RainFrame)
    float    flashTime;
    struct rainEave_s *eaves; // where water pours off a roof (R_RainFindEaves)
    int      numEaves;
    qhandle_t streamShader, curtainShader, crownShader, dropShader, mistShader;
    qboolean shadersTried;
    qboolean hasLast;
    int      width, rows;
    float    x0, y0, texel;
    vec2_t   mins, maxs;
    qboolean on, tried;
} rainCover;

static qboolean R_RainPuddleShader(const shader_t *shader);
static float    R_RainAmount(void);
static void     R_WaterSky(vec4_t out);
static void     R_RainFindEaves(void);

// the grid R_RainTri draws into: the cover's, or the puddles'
static float *rainGrid;

static qboolean R_RainCovers(const shader_t *sh)
{
    return sh && !sh->isSky && !(sh->surfaceFlags & (SURF_SKY | SURF_NODRAW)) && !(sh->contentFlags & CONTENTS_WATER)
             && sh->sort < SS_BLEND0
         ? qtrue
         : qfalse;
}

static void R_RainTri(const vec3_t a, const vec3_t b, const vec3_t c, qboolean draw)
{
    float area, minx, maxx, miny, maxy;
    int   x0, x1, y0, y1, x, y;

    minx = Q_min(a[0], Q_min(b[0], c[0]));
    maxx = Q_max(a[0], Q_max(b[0], c[0]));
    miny = Q_min(a[1], Q_min(b[1], c[1]));
    maxy = Q_max(a[1], Q_max(b[1], c[1]));
    if (!draw) {
        rainCover.mins[0] = Q_min(rainCover.mins[0], minx);
        rainCover.mins[1] = Q_min(rainCover.mins[1], miny);
        rainCover.maxs[0] = Q_max(rainCover.maxs[0], maxx);
        rainCover.maxs[1] = Q_max(rainCover.maxs[1], maxy);
        return;
    }

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    x0   = Q_max(0, (int)floor((minx - rainCover.x0) / rainCover.texel));
    x1   = Q_min(rainCover.width - 1, (int)floor((maxx - rainCover.x0) / rainCover.texel));
    y0   = Q_max(0, (int)floor((miny - rainCover.y0) / rainCover.texel));
    y1   = Q_min(rainCover.rows - 1, (int)floor((maxy - rainCover.y0) / rainCover.texel));

    if (fabs(area) < 0.001f) {
        // no puddle lies on a wall
        if (rainGrid != rainCover.height) {
            return;
        }
        // edge on from above (a wall): its top along the texels it crosses,
        // as a thin wall would be lost between their middles. The top where
        // the texel is along it, not its highest anywhere: a broken wall's
        // tall piece roofed the low top beside it (dry, its edges wet)
        const float *tv[3] = {a, b, c};

        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                const float px = rainCover.x0 + (x + 0.5f) * rainCover.texel;
                const float py = rainCover.y0 + (y + 0.5f) * rainCover.texel;
                vec2_t      d  = {maxx - minx, maxy - miny};
                const float len = sqrt(d[0] * d[0] + d[1] * d[1]);
                float       off;

                if (len < 0.001f) {
                    continue;
                }
                // how far the texel's middle is off the line from a to the far end
                {
                    const float *p0 = a, *p1 = (Distance(a, b) > Distance(a, c)) ? b : c;
                    const float  ex = p1[0] - p0[0], ey = p1[1] - p0[1];
                    const float  l  = sqrt(ex * ex + ey * ey);

                    if (l < 0.001f) {
                        continue;
                    }
                    off = fabs((px - p0[0]) * ey - (py - p0[1]) * ex) / l;
                }
                if (off <= rainCover.texel * 0.71f) {
                    const float *p0 = a, *p1 = (Distance(a, b) > Distance(a, c)) ? b : c;
                    const float  ex = p1[0] - p0[0], ey = p1[1] - p0[1];
                    const float  l2 = ex * ex + ey * ey;
                    float        sv[3], st, top = RAIN_COVER_NONE, smin = 1e9f, smax = -1e9f;
                    float       *h = &rainGrid[y * rainCover.width + x];
                    int          e;

                    // along the wall: each corner's place, and the texel's,
                    // kept within the wall's ends
                    for (e = 0; e < 3; e++) {
                        sv[e] = ((tv[e][0] - p0[0]) * ex + (tv[e][1] - p0[1]) * ey) / l2;
                        smin  = Q_min(smin, sv[e]);
                        smax  = Q_max(smax, sv[e]);
                    }
                    st = ((px - p0[0]) * ex + (py - p0[1]) * ey) / l2;
                    st = Q_max(smin, Q_min(smax, st));
                    // the highest of the edges over that place
                    for (e = 0; e < 3; e++) {
                        const int   f  = (e + 1) % 3;
                        const float s0 = sv[e], s1 = sv[f];

                        if (fabs(s1 - s0) < 1e-5f) {
                            if (fabs(st - s0) < 1e-4f) {
                                top = Q_max(top, Q_max(tv[e][2], tv[f][2]));
                            }
                        } else if ((st - s0) * (st - s1) <= 1e-6f) {
                            const float k = (st - s0) / (s1 - s0);
                            top = Q_max(top, tv[e][2] + (tv[f][2] - tv[e][2]) * k);
                        }
                    }
                    if (top <= RAIN_COVER_NONE * 0.5f) {
                        top = Q_max(a[2], Q_max(b[2], c[2]));
                    }
                    *h = Q_max(*h, top);
                }
            }
        }
        return;
    }

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            const float px = rainCover.x0 + (x + 0.5f) * rainCover.texel;
            const float py = rainCover.y0 + (y + 0.5f) * rainCover.texel;
            const float wa = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / area;
            const float wb = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / area;
            const float wc = 1.0f - wa - wb;
            float      *h  = &rainGrid[y * rainCover.width + x];

            if (wa < -0.001f || wb < -0.001f || wc < -0.001f) {
                continue;
            }
            *h = Q_max(*h, wa * a[2] + wb * b[2] + wc * c[2]);
        }
    }
}

static void R_RainCoverSurfaces(qboolean draw, qboolean puddles)
{
    int i, k, x, y;

    for (i = 0; i < tr.world->numsurfaces; i++) {
        const msurface_t      *surf = &tr.world->surfaces[i];
        const srfBspSurface_t *bsp;

        if (!R_RainCovers(surf->shader) || !surf->data || (puddles && !R_RainPuddleShader(surf->shader))) {
            continue;
        }
        if (*surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES) {
            continue;
        }
        bsp = (const srfBspSurface_t *)surf->data;
        for (k = 0; k + 2 < bsp->numIndexes; k += 3) {
            R_RainTri(bsp->verts[bsp->indexes[k]].xyz, bsp->verts[bsp->indexes[k + 1]].xyz,
                bsp->verts[bsp->indexes[k + 2]].xyz, draw);
        }
    }

    for (i = 0; i < tr.world->numTerraPatches; i++) {
        const cTerraPatchUnpacked_t *patch = &tr.world->terraPatches[i];

        if (!R_RainCovers(patch->shader) || (puddles && !R_RainPuddleShader(patch->shader))) {
            continue;
        }
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                vec3_t p[4];

                for (k = 0; k < 4; k++) {
                    const int xi = x + (k & 1), yi = y + (k >> 1);

                    VectorSet(p[k], patch->x0 + xi * 64.0f, patch->y0 + yi * 64.0f,
                        patch->heightmap[yi * 9 + xi] * 2.0f + patch->z0);
                }
                R_RainTri(p[0], p[1], p[3], draw);
                R_RainTri(p[0], p[3], p[2], draw);
            }
        }
    }
}

static void R_RainBuildCover(void)
{
    float extent;
    int   i, start = ri.Milliseconds();

    rainCover.tried = qtrue;
    if (!tr.world) {
        return;
    }

    VectorSet2(rainCover.mins, 1e9f, 1e9f);
    VectorSet2(rainCover.maxs, -1e9f, -1e9f);
    R_RainCoverSurfaces(qfalse, qfalse);
    if (rainCover.maxs[0] <= rainCover.mins[0] || rainCover.maxs[1] <= rainCover.mins[1]) {
        return;
    }

    extent          = Q_max(rainCover.maxs[0] - rainCover.mins[0], rainCover.maxs[1] - rainCover.mins[1]);
    rainCover.texel = Q_max(extent / RAIN_COVER_SIZE, 6.0f);
    rainCover.x0    = rainCover.mins[0] - rainCover.texel;
    rainCover.y0    = rainCover.mins[1] - rainCover.texel;
    rainCover.width = Q_min(RAIN_COVER_SIZE, (int)ceil((rainCover.maxs[0] - rainCover.mins[0]) / rainCover.texel) + 2);
    rainCover.rows  = Q_min(RAIN_COVER_SIZE, (int)ceil((rainCover.maxs[1] - rainCover.mins[1]) / rainCover.texel) + 2);

    rainCover.height = ri.Malloc(rainCover.width * rainCover.rows * sizeof(float));
    for (i = 0; i < rainCover.width * rainCover.rows; i++) {
        rainCover.height[i] = RAIN_COVER_NONE;
    }
    rainGrid = rainCover.height;
    R_RainCoverSurfaces(qtrue, qfalse);
    R_RainFindEaves();

    tr.rainCoverImage = R_CreateImage("*rainCover", NULL, rainCover.width, rainCover.rows, IMGTYPE_COLORALPHA,
        IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_R32F);
    qglTextureSubImage2DEXT(tr.rainCoverImage->texnum, GL_TEXTURE_2D, 0, 0, 0, rainCover.width, rainCover.rows, GL_RED,
        GL_FLOAT, rainCover.height);
    qglTextureParameteriEXT(tr.rainCoverImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    qglTextureParameteriEXT(tr.rainCoverImage->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    rainCover.on = qtrue;
    ri.Printf(PRINT_DEVELOPER, "rain: cover %dx%d, %.1f units a texel, in %d ms\n", rainCover.width, rainCover.rows,
        rainCover.texel, ri.Milliseconds() - start);
}

/*
===============
R_RainBuildPuddles

Added in OPM
  Where puddles gather (r_rainPuddleShaders), out in the rain: the highest
  of those surfaces at each place of the cover's grid, for the height of the
  puddles' mirror (R_WaterReflectView). Again when the list changes.
===============
*/
static void R_RainBuildPuddles(void)
{
    int i, n = rainCover.width * rainCover.rows;

    if (!rainCover.puddle) {
        rainCover.puddle = ri.Malloc(n * sizeof(float));
    }
    for (i = 0; i < n; i++) {
        rainCover.puddle[i] = RAIN_COVER_NONE;
    }
    rainGrid = rainCover.puddle;
    R_RainCoverSurfaces(qtrue, qtrue);
    rainGrid = rainCover.height;

    // under something, no rain falls in it
    for (i = 0; i < n; i++) {
        if (rainCover.puddle[i] < rainCover.height[i] - 2.0f) {
            rainCover.puddle[i] = RAIN_COVER_NONE;
        }
    }
    rainCover.puddleCount = r_rainPuddleShaders->modificationCount;
}

/*
===============
R_RainPuddleHeight

Added in OPM
  The height of the puddles the eye looks at: the first place along its
  look, flat over the ground, from under it out to 1024 units, where they
  gather below it.
===============
*/
static qboolean R_RainPuddleHeight(const vec3_t eye, const vec3_t forward, float *height)
{
    vec2_t dir;
    float  len, d;

    if (!rainCover.puddle) {
        return qfalse;
    }
    VectorSet2(dir, forward[0], forward[1]);
    len = sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    if (len < 0.01f) {
        VectorSet2(dir, 0.0f, 0.0f);
    } else {
        dir[0] /= len;
        dir[1] /= len;
    }
    for (d = 0.0f; d <= 1024.0f; d += rainCover.texel) {
        const int x = (int)floor((eye[0] + dir[0] * d - rainCover.x0) / rainCover.texel);
        const int y = (int)floor((eye[1] + dir[1] * d - rainCover.y0) / rainCover.texel);
        float     h;

        if (x < 0 || y < 0 || x >= rainCover.width || y >= rainCover.rows) {
            break;
        }
        h = rainCover.puddle[y * rainCover.width + x];
        if (h > RAIN_COVER_NONE * 0.5f && h < eye[2] - 8.0f) {
            *height = h;
            return qtrue;
        }
    }
    return qfalse;
}

/*
===============
R_RainFindEaves

Added in OPM
  Where water pours off a roof: the lower edge of a slope in the cover's
  grid, the roof rising behind it, over a drop of six feet and more to what
  is under it, where it lands. A flat top (a wall's) has no lower edge, and
  pours nothing. One every 18 units or so along an edge.
===============
*/
typedef struct rainEave_s {
    vec3_t top;    // the edge, where it leaves the roof
    float  bottom; // where it lands
    float  seed;
    vec2_t out;    // the way off the roof, flat
    vec3_t ground; // which way what it lands on faces
} rainEave_t;

#define RAIN_MAX_EAVES 8192
#define RAIN_EAVE_GAP  18.0f

static void R_RainFindEaves(void)
{
    static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const int        W = rainCover.width, H = rainCover.rows;
    const float     *g = rainCover.height;
    const float      t = rainCover.texel;
    int              ow, oh, x, y, d;
    byte            *taken;

    rainCover.eaves    = ri.Malloc(RAIN_MAX_EAVES * sizeof(rainEave_t));
    rainCover.numEaves = 0;
    ow    = (int)(W * t / RAIN_EAVE_GAP) + 1;
    oh    = (int)(H * t / RAIN_EAVE_GAP) + 1;
    taken = ri.Malloc(ow * oh);
    Com_Memset(taken, 0, ow * oh);

    for (y = 2; y < H - 2; y++) {
        for (x = 2; x < W - 2; x++) {
            const float h = g[y * W + x];

            if (h <= RAIN_COVER_NONE * 0.5f) {
                continue;
            }
            for (d = 0; d < 8; d++) {
                const int   dx = dirs[d][0], dy = dirs[d][1];
                const float n  = g[(y + dy) * W + x + dx];
                const float run = t * (dx && dy ? 1.41f : 1.0f);
                float       px, py, len;
                int         cx, cy;
                rainEave_t *e;

                // a drop over the edge, onto something, of an eave's height
                // (a bank's ledge is lower)
                if (n <= RAIN_COVER_NONE * 0.5f || n > h - 72.0f) {
                    continue;
                }
                // the roof rising behind it, steadily, not a step up, and on
                // for 30 units and more (a wall's sloped cap is narrower)
                {
                    float    prev = h;
                    int      k;
                    qboolean roof = qtrue;

                    for (k = 1; k <= 5 && roof; k++) {
                        const int   rx = x - k * dx, ry = y - k * dy;
                        const float r  = (rx >= 0 && rx < W && ry >= 0 && ry < H) ? g[ry * W + rx] : RAIN_COVER_NONE;

                        roof = r >= prev + run * 0.15f && r <= prev + run * 1.6f;
                        prev = r;
                    }
                    if (!roof) {
                        continue;
                    }
                }
                len = sqrt((float)(dx * dx + dy * dy));
                px  = rainCover.x0 + (x + 0.5f + 0.5f * dx / len) * t;
                py  = rainCover.y0 + (y + 0.5f + 0.5f * dy / len) * t;
                cx  = (int)((px - rainCover.x0) / RAIN_EAVE_GAP);
                cy  = (int)((py - rainCover.y0) / RAIN_EAVE_GAP);
                if (cx < 0 || cy < 0 || cx >= ow || cy >= oh || taken[cy * ow + cx]) {
                    break;
                }
                taken[cy * ow + cx] = 1;
                if (rainCover.numEaves >= RAIN_MAX_EAVES) {
                    break;
                }
                e = &rainCover.eaves[rainCover.numEaves++];
                VectorSet(e->top, px, py, h - 1.0f);
                e->bottom = n;
                VectorSet2(e->out, dx / len, dy / len);
                // the slope of what it lands on, from the grid about the
                // place: a neighbour far off its height (a wall, a step) is
                // not the same surface, and stands in as level
                {
                    const int   lx = x + dx, ly = y + dy;
                    const float hl = g[ly * W + lx - 1], hr = g[ly * W + lx + 1];
                    const float hd = g[(ly - 1) * W + lx], hu = g[(ly + 1) * W + lx];
                    const float gx = (fabs(hr - n) < 24.0f ? hr : n) - (fabs(hl - n) < 24.0f ? hl : n);
                    const float gy = (fabs(hu - n) < 24.0f ? hu : n) - (fabs(hd - n) < 24.0f ? hd : n);

                    VectorSet(e->ground, -gx / (2.0f * t), -gy / (2.0f * t), 1.0f);
                    VectorNormalize(e->ground);
                }
                e->seed   = (float)((x * 73856093u) ^ (y * 19349663u)) / 4294967296.0f;
                if (e->seed < 0.0f) {
                    e->seed = -e->seed;
                }
                e->seed = e->seed - floor(e->seed);
                break;
            }
        }
    }
    ri.Free(taken);
    ri.Printf(PRINT_DEVELOPER, "rain: %d eaves\n", rainCover.numEaves);
}

/*
===============
R_RainRunoff

Added in OPM
  The water pouring off the eaves near the eye (R_RainFindEaves), as rain
  runs off a roof: a veil all along the edge (opm/rain_curtain), leaving it
  whole and thinning to drops as it falls, leaning out as water thrown off
  an edge does; here and there a heavier stream in it (opm/rain_stream); and
  a splash where it lands (R_RainSplash). Added to the scene as polygons,
  before it is begun (RE_RenderScene).
===============
*/
#define RAIN_RUNOFF_RANGE 1600.0f
#define RAIN_RUNOFF_MAX   384

// a quad, its top two corners first: the colour, and the alpha at the top
// and at the bottom
static void R_RainQuad(qhandle_t shader, const vec3_t tl, const vec3_t trc, const vec3_t br, const vec3_t bl,
    const float st[4][2], byte grey, float alphaTop, float alphaBottom)
{
    polyVert_t v[4];
    int        k;

    VectorCopy(tl, v[0].xyz);
    VectorCopy(trc, v[1].xyz);
    VectorCopy(br, v[2].xyz);
    VectorCopy(bl, v[3].xyz);
    for (k = 0; k < 4; k++) {
        v[k].st[0]       = st[k][0];
        v[k].st[1]       = st[k][1];
        v[k].modulate[0] = v[k].modulate[1] = v[k].modulate[2] = grey;
        v[k].modulate[3] = (byte)Com_Clamp(0.0f, 255.0f, (k < 2 ? alphaTop : alphaBottom) * 255.0f);
    }
    RE_AddPolyToScene(shader, 4, v, 1);
}

/*
===============
R_RainSplash

Added in OPM
  Where the water off an eave lands, a splash with some body to it, of our
  own textures (tools/content/rain_textures.py): the crown of spray thrown
  up and out (opm/rain_crown), two of them out of step along the edge; drops
  flying off it, each on its own arc and drawn out along its flight
  (opm/rain_drop); and the fine mist rising off it in two puffs that grow
  and thin (opm/rain_mist). Turned to the eye about the upright, but at
  different places and heights, so it has depth as the eye goes round it.
  All of it stands on what it lands on, at its slope, its foot a little
  into it so its edge is not seen; the drops leave the slope and fall.
===============
*/
#define RAIN_SPLASH_RANGE 900.0f
#define RAIN_SPLASH_DROPS 6

static float R_RainHash(float x)
{
    const float h = sin(x * 12.9898f) * 43758.5453f;

    return h - floor(h);
}

static void R_RainSplash(const vec3_t base, const vec3_t ground, const vec3_t edge, const vec3_t eye, float half,
    float seed, float flow, float a, byte grey, float time)
{
    float  st[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    vec3_t c, tl, trc, br, bl, up, along, side;
    int    k;

    // stood on what it lands on: its face's way out is up, and the edge
    // and the way across to the eye lie in it
    VectorCopy(ground, up);
    VectorMA(edge, -DotProduct(edge, up), up, along);
    if (VectorNormalize(along) < 0.01f) {
        VectorCopy(edge, along);
    }
    VectorMA(eye, -DotProduct(eye, up), up, side);
    if (VectorNormalize(side) < 0.01f) {
        CrossProduct(up, along, side);
        VectorNormalize(side);
    }

    // the crowns
    if (rainCover.crownShader) {
        for (k = 0; k < 2; k++) {
            const float phase = time * (3.2f + 2.0f * seed + 0.7f * k) + seed * 17.0f + k * 0.5f;
            const float f     = phase - floor(phase);
            const float w     = (5.0f + 7.0f * sqrt(f)) * (0.75f + 0.5f * R_RainHash(floor(phase) + seed)) * (0.6f + 0.5f * flow);
            const float h     = w * (0.9f + 0.5f * (1.0f - f));
            const int   flip  = ((int)floor(phase) + k) & 1;

            VectorMA(base, (k ? -0.3f : 0.3f) * half + (seed - 0.5f) * 4.0f, along, c);
            // its foot a little into the ground, so its edge is not seen
            VectorMA(c, -(1.5f + 0.22f * h), up, c);
            VectorMA(c, -w, side, bl);
            VectorMA(c, w, side, br);
            VectorMA(bl, h, up, tl);
            VectorMA(br, h, up, trc);
            st[0][0] = st[3][0] = flip ? 1.0f : 0.0f;
            st[1][0] = st[2][0] = flip ? 0.0f : 1.0f;
            R_RainQuad(rainCover.crownShader, tl, trc, br, bl, st, grey, a * (1.0f - f) * 0.8f, a * (1.0f - f));
        }
    }

    // the drops thrown off it
    if (rainCover.dropShader) {
        st[0][0] = st[3][0] = 0.0f;
        st[1][0] = st[2][0] = 1.0f;
        for (k = 0; k < RAIN_SPLASH_DROPS; k++) {
            const float id    = seed * 97.0f + k * 7.31f;
            const float rate  = 1.6f + 1.4f * R_RainHash(id);
            const float phase = time * rate + R_RainHash(id + 1.0f);
            const float n     = floor(phase) + id;
            const float f     = phase - floor(phase);
            // its throw: up, and out any way, this time round
            const float vz    = 50.0f + 70.0f * R_RainHash(n + 2.0f);
            const float vh    = 15.0f + 45.0f * R_RainHash(n + 3.0f);
            const float ang   = R_RainHash(n + 4.0f) * 2.0f * M_PI;
            // back down to the slope it leaves, under gravity
            const float life  = vz / (250.0f * Q_max(0.3f, up[2]));
            const float t     = f * life;
            vec3_t      dir, vel, pos, ax, sd, toEye;
            float       len, w;

            VectorScale(along, cos(ang), dir);
            VectorMA(dir, sin(ang), side, dir);
            VectorMA(base, (R_RainHash(n + 5.0f) - 0.5f) * 2.0f * half, along, pos);
            VectorMA(pos, vh * t, dir, pos);
            VectorMA(pos, vz * t, up, pos);
            pos[2] -= 250.0f * t * t;

            VectorScale(dir, vh, vel);
            VectorMA(vel, vz, up, vel);
            vel[2] -= 500.0f * t;
            len    = VectorNormalize2(vel, ax);
            // drawn out along its flight, across it to the eye
            CrossProduct(side, up, toEye);
            CrossProduct(ax, toEye, sd);
            if (VectorNormalize(sd) < 0.01f) {
                VectorCopy(side, sd);
            }
            w   = 0.45f + 0.35f * R_RainHash(n + 6.0f);
            len = w + len * 0.018f;

            VectorMA(pos, len, ax, c);
            VectorMA(c, -w, sd, tl);
            VectorMA(c, w, sd, trc);
            VectorMA(pos, -len, ax, c);
            VectorMA(c, w, sd, br);
            VectorMA(c, -w, sd, bl);
            R_RainQuad(rainCover.dropShader, tl, trc, br, bl, st, grey, a * (1.0f - f * f), a * (1.0f - f * f));
        }
    }

    // the mist
    if (rainCover.mistShader) {
        for (k = 0; k < 2; k++) {
            const float phase = time * (0.45f + 0.2f * k) + seed * 5.0f + k * 0.5f;
            const float f     = phase - floor(phase);
            const float size  = (8.0f + 16.0f * f) * (0.8f + 0.4f * seed);
            const float ma    = a * 0.4f * sin(f * M_PI) * (0.5f + 0.5f * flow);

            VectorMA(base, (k ? 0.45f : -0.45f) * half, along, c);
            // a third of it in the ground: its lower edge is never seen
            VectorMA(c, size * 0.35f + 6.0f * f, up, c);
            VectorMA(c, -size, side, tl);
            VectorMA(tl, size, up, tl);
            VectorMA(c, size, side, trc);
            VectorMA(trc, size, up, trc);
            VectorMA(c, size, side, br);
            VectorMA(br, -size, up, br);
            VectorMA(c, -size, side, bl);
            VectorMA(bl, -size, up, bl);
            st[0][0] = st[3][0] = (k ^ ((int)floor(phase) & 1)) ? 1.0f : 0.0f;
            st[1][0] = st[2][0] = 1.0f - st[0][0];
            R_RainQuad(rainCover.mistShader, tl, trc, br, bl, st, grey, ma, ma);
        }
    }
}

static qhandle_t R_RainRunoffShader(const char *name)
{
    const qhandle_t h  = RE_RegisterShader(name);
    shader_t       *sh = R_GetShaderByHandle(h);

    // without the pak that has it, none: the default shader is a white square
    return sh && !sh->defaultShader ? h : 0;
}

static void R_RainRunoff(const refdef_t *fd)
{
    const float amount = r_rainRunoff->value * R_RainAmount();
    const float time   = fd->time * 0.001f;
    vec4_t      sky;
    float       lum;
    byte        grey;
    int         i, drawn = 0;

    if (amount <= 0.0f || !rainCover.numEaves) {
        return;
    }
    if (!rainCover.shadersTried) {
        rainCover.shadersTried  = qtrue;
        rainCover.curtainShader = R_RainRunoffShader("opm/rain_curtain");
        rainCover.streamShader  = R_RainRunoffShader("opm/rain_stream");
        rainCover.crownShader   = R_RainRunoffShader("opm/rain_crown");
        rainCover.dropShader    = R_RainRunoffShader("opm/rain_drop");
        rainCover.mistShader    = R_RainRunoffShader("opm/rain_mist");
    }
    if (!rainCover.curtainShader) {
        return;
    }

    // as bright as the sky over it
    R_WaterSky(sky);
    lum  = Com_Clamp(0.25f, 1.0f, 0.2126f * sky[0] + 0.7152f * sky[1] + 0.0722f * sky[2] + 0.15f);
    grey = (byte)Com_Clamp(0.0f, 255.0f, lum * 235.0f);

    for (i = 0; i < rainCover.numEaves && drawn < RAIN_RUNOFF_MAX; i++) {
        const rainEave_t *e = &rainCover.eaves[i];
        vec3_t            to, side, along, out, bot, mid, tl, trc, ml, mr, bl, br;
        float             dist, fade, flow, a, fall, lean, half, s0, s1;
        float             st[4][2];

        VectorSubtract(fd->vieworg, e->top, to);
        to[2] = 0.0f;
        dist  = VectorLength(to);
        if (dist > RAIN_RUNOFF_RANGE || dist < 1.0f) {
            continue;
        }
        // behind the eye
        if (DotProduct(to, fd->viewaxis[0]) > 64.0f) {
            continue;
        }
        // less of the edge pours when it rains less
        if (e->seed > 0.2f + 0.8f * Com_Clamp(0.0f, 1.0f, amount)) {
            continue;
        }

        fade = 1.0f - Com_Clamp(0.0f, 1.0f, (dist - RAIN_RUNOFF_RANGE * 0.6f) / (RAIN_RUNOFF_RANGE * 0.4f));
        // heavier here, lighter there along the edge, and as gusts come
        flow = 0.55f + 0.3f * sin(e->seed * 53.0f + time * 0.9f) + 0.15f * sin(time * 5.0f + e->seed * 91.0f);
        a    = fade * Com_Clamp(0.0f, 1.0f, flow) * Com_Clamp(0.0f, 1.5f, amount);
        if (a < 0.02f) {
            continue;
        }

        VectorSet(out, e->out[0], e->out[1], 0.0f);
        VectorSet(along, -out[1], out[0], 0.0f);
        fall = e->top[2] - e->bottom;
        // thrown off the edge: out as the square root of the fall
        lean = (4.0f + 0.05f * fall) * (0.7f + 0.6f * e->seed);
        half = RAIN_EAVE_GAP * 0.5f + 3.0f;

        VectorSet(mid, e->top[0], e->top[1], e->top[2] - fall * 0.35f);
        VectorMA(mid, lean * 0.59f, out, mid);
        VectorSet(bot, e->top[0], e->top[1], e->bottom);
        VectorMA(bot, lean, out, bot);
        // on a slope, the ground as far out as it lands
        bot[2] -= lean * (e->ground[0] * out[0] + e->ground[1] * out[1]) / Q_max(0.3f, e->ground[2]);

        VectorMA(e->top, -half, along, tl);
        VectorMA(e->top, half, along, trc);
        VectorMA(mid, -half, along, ml);
        VectorMA(mid, half, along, mr);
        VectorMA(bot, -half, along, bl);
        VectorMA(bot, half, along, br);

        // the texture held to the world along the edge, so the pieces of
        // one edge run on into each other
        s0 = (DotProduct(e->top, along) - half) / 64.0f;
        s1 = (DotProduct(e->top, along) + half) / 64.0f;

        // the veil: whole off the edge, then breaking up
        st[0][0] = st[3][0] = s0;
        st[1][0] = st[2][0] = s1;
        st[0][1] = st[1][1] = -e->top[2] / 96.0f;
        st[2][1] = st[3][1] = -mid[2] / 96.0f;
        R_RainQuad(rainCover.curtainShader, tl, trc, mr, ml, st, grey, a * 0.9f, a * 0.7f);
        st[0][1] = st[1][1] = -mid[2] / 96.0f;
        st[2][1] = st[3][1] = -bot[2] / 96.0f;
        R_RainQuad(rainCover.curtainShader, ml, mr, br, bl, st, grey, a * 0.7f, a * 0.25f);

        // the face of the eye's to turn the rest to
        VectorNormalize(to);
        VectorSet(side, -to[1], to[0], 0.0f);

        // a heavier stream, here and there
        if (rainCover.streamShader && e->seed < 0.14f) {
            const float w = 2.0f + 2.5f * (e->seed / 0.14f);

            VectorMA(e->top, -w, side, tl);
            VectorMA(e->top, w, side, trc);
            VectorMA(bot, w, side, br);
            VectorMA(bot, -w, side, bl);
            st[0][0] = st[3][0] = floor(e->seed * 100.0f) * 0.25f;
            st[1][0] = st[2][0] = st[0][0] + 0.75f;
            st[0][1] = st[1][1] = -e->top[2] / 160.0f;
            st[2][1] = st[3][1] = -e->bottom / 160.0f;
            R_RainQuad(rainCover.streamShader, tl, trc, br, bl, st, grey, a, a * 0.8f);
        }

        // the splash where it lands, near enough to see (R_RainSplash)
        if (dist < RAIN_SPLASH_RANGE) {
            R_RainSplash(bot, e->ground, along, side, half, e->seed, flow, a, grey, time);
        }
        drawn++;
    }

    // r_rainDebug 3: what it drew, now and then
    if (r_rainDebug->integer == 3) {
        static int last;

        if (fd->time - last > 2000 || fd->time < last) {
            float best = 1e9f;
            int   bi = -1;

            last = fd->time;
            for (i = 0; i < rainCover.numEaves; i++) {
                const float dd = Distance(fd->vieworg, rainCover.eaves[i].top);

                if (dd < best) {
                    best = dd;
                    bi   = i;
                }
            }
            ri.Printf(PRINT_ALL, "rain runoff: %d drawn of %d eaves, amount %.2f; nearest %.0f away at (%.0f %.0f %.0f) to %.0f\n", drawn,
                rainCover.numEaves, amount, best, bi >= 0 ? rainCover.eaves[bi].top[0] : 0.0f,
                bi >= 0 ? rainCover.eaves[bi].top[1] : 0.0f, bi >= 0 ? rainCover.eaves[bi].top[2] : 0.0f,
                bi >= 0 ? rainCover.eaves[bi].bottom : 0.0f);
        }
    }
}

static void R_RainFreeCover(void)
{
    if (rainCover.eaves) {
        ri.Free(rainCover.eaves);
    }
    if (rainCover.height) {
        ri.Free(rainCover.height);
    }
    if (rainCover.puddle) {
        ri.Free(rainCover.puddle);
    }
    memset(&rainCover, 0, sizeof(rainCover));
    // the last map's grid is one of its images, which the next map frees
    tr.rainCoverImage = NULL;
}

// Once a scene, in the front end: the cover, the first time it rains
void R_RainFrame(const refdef_t *fd)
{
    if (!rainCover.tried && r_rainWet->value > 0.0f && r_waterWet->integer && tr.world) {
        R_RainBuildCover();
    }
    if (rainCover.on && (!rainCover.puddle || rainCover.puddleCount != r_rainPuddleShaders->modificationCount)) {
        R_RainBuildPuddles();
    }

    // lightning: global/weather.scr flashes by setting r_fastsky to 1 (the
    // sky cleared to the white fog) for a tenth or a fifth of a second; the
    // light of it on the world goes out over a moment after
    {
        const float now = fd->time * 0.001f;
        const float dt  = Com_Clamp(0.0f, 0.25f, now - rainCover.flashTime);

        rainCover.flashTime = now;
        if (rainCover.on && r_rainFlash->value > 0.0f && r_fastsky->value >= 0.99f) {
            rainCover.flash = 1.0f;
        } else {
            rainCover.flash *= exp(-dt * 9.0f);
            if (rainCover.flash < 0.01f) {
                rainCover.flash = 0.0f;
            }
        }
    }

    R_RainRunoff(fd);
}

static float R_RainAmount(void)
{
    return rainCover.on ? Com_Clamp(0.0f, 1.0f, r_rainWet->value) : 0.0f;
}

void R_WaterLoadWorld(const char *mapName)
{
    int i;

    R_RainFreeCover();

    R_WaterReadFile(mapName);
    // a shore's own texture swapped for the beach's (m3l1a's painted
    // shoreline strip, a straight bright line once the beach was wet)
    for (i = 0; i < water.numRemaps; i++) {
        R_RemapShader(water.remaps[i][0], water.remaps[i][1], "0");
    }
    R_WaterBuildSky();
    R_WaterBuildBeach();
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
    // Added in OPM: the light in the air between it and the eye
    R_FroxelSetFog(sp, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);

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

    if (water.ocean[0] > 0.0f) {
        VectorSet4(v, 1.0f / water.ocean[0], water.ocean[1], water.ocean[2], water.ocean[3]);
    } else {
        VectorClear4(v);
    }
    GLSL_SetUniformVec4(sp, UNIFORM_WATEROCEAN, v);

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

    // the view under its surface, if it was drawn (R_WaterReflectView)
    if (backEnd.viewParms.waterReflect && tr.waterReflectImage) {
        VectorSet4(v, 1.0f, 0.0f, backEnd.viewParms.waterReflectShare[0], backEnd.viewParms.waterReflectShare[1]);
        GLSL_SetUniformVec4(sp, UNIFORM_WATERREFLECTPLANE, backEnd.viewParms.waterReflectPlane);
        GLSL_SetUniformMat4(sp, UNIFORM_WATERREFLECTMVP, backEnd.viewParms.waterReflectMvp);
        GL_BindToTMU(tr.waterReflectImage, TB_SHADOWMAP);
    } else {
        VectorClear4(v);
    }
    GLSL_SetUniformVec4(sp, UNIFORM_WATERREFLECT, v);

    GL_BindToTMU(input->shader->waterImage, TB_DIFFUSEMAP);
    GL_BindToTMU(hasView ? tr.waterSceneImage : tr.whiteImage, TB_LIGHTMAP);
    GL_BindToTMU(hasDepth ? tr.hdrDepthImage : tr.whiteImage, TMU_SOFTDEPTH);

    GL_State(GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK_TRUE);

    R_DrawElements(input->numIndexes, input->firstIndex);

    // r_waterReflect 3: the view under the water as it was drawn, in a corner
    if (r_waterReflect->integer == 3 && backEnd.viewParms.waterReflect && glState.currentFBO) {
        ivec4_t box;

        box[0] = 0;
        box[1] = 0;
        box[2] = glState.currentFBO->width / 3;
        box[3] = glState.currentFBO->height / 3;
        FBO_FastBlit(tr.waterReflectFbo, NULL, glState.currentFBO, box, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    return qtrue;
}

/*
===============
R_WaterReflectView

What the view's water mirrors, of the world above it: the view drawn again
first, from under the surface, into tr.waterReflectImage, which the water
looks itself up in (water_fp.glsl) where it lies at that height. On the
screen alone (RB_WaterDraw's ray through the view's depth) it can only show
what the view also sees, so looking down at it a tree went from it.

It is a mirror portal (oblique near plane at the surface, the faces turned)
that draws what the main view does: entities and sprites as it has them
(R_AddEntitySurfaces), no water, no viewmodel; no depth prepass, so no sun
shadow mask either. One height a view: the water nearest under the eye.

With no water in view, in the rain, the puddles' (r_rainPuddleShaders):
level at the height of those the eye looks at (R_RainPuddleHeight), cut a
little over it, as the bumps of the ground around a puddle are not seen in
it. Their lightall looks itself up in it (R_WaterSetWet).
===============
*/
// what the last view under the water was, for waterinfo
static struct {
    float height;
    int   surfs;
    int   frame;
    int   numPlanes;          // the water planes in view, for waterinfo
    vec4_t planes[8];         // normal z, height, surfaces, under the eye
} waterReflectInfo;

void R_WaterReflectView(drawSurf_t *drawSurfs, int numDrawSurfs)
{
    viewParms_t parms, oldParms;
    cplane_t    mirror;
    float       above = 0.0f, d;
    qboolean    found = qfalse, puddles = qfalse, puddleInView = qfalse;
    const qboolean rain = R_RainAmount() > 0.0f && r_rainPuddles->value > 0.0f && rainCover.puddle;
    mat4_t      mvp;
    int         i;

    tr.viewParms.waterReflect = qfalse;
    tr.viewParms.rainReflect  = qfalse;

    if (!r_waterReflect->integer || !tr.waterReflectFbo || !tr.waterShader.program || !tr.world
        || tr.viewParms.isPortal || tr.viewParms.isPortalSky || tr.viewParms.targetFbo
        || (tr.viewParms.flags & (VPF_SHADOWMAP | VPF_DEPTHSHADOW | VPF_ORTHOGRAPHIC | VPF_RTSTATIC | VPF_RTDYNAMIC | VPF_RTBAKED | VPF_RTSKY))
        || (tr.refdef.rdflags & (RDF_NOWORLDMODEL | RDF_HUD)) || r_noportals->integer) {
        return;
    }

    waterReflectInfo.numPlanes = 0;
    for (i = 0; i < numDrawSurfs; i++) {
        shader_t *shader;
        int       entityNum, fogNum, dlighted, pshadowed;
        qboolean  bStaticModel;
        cplane_t  plane;
        float     h;

        R_DecomposeSort(drawSurfs[i].sort, &entityNum, &shader, &fogNum, &dlighted, &pshadowed, &bStaticModel);
        if (rain && !puddleInView && entityNum == REFENTITYNUM_WORLD && !bStaticModel && R_RainPuddleShader(shader)) {
            puddleInView = qtrue;
        }
        if (!shader->waterImage || entityNum != REFENTITYNUM_WORLD) {
            continue;
        }
        R_PlaneForSurface(drawSurfs[i].surface, &plane);
        if (plane.normal[2] < 0.99f) {
            continue;
        }
        // its height under the eye, for waterinfo
        h = (plane.dist - plane.normal[0] * tr.viewParms.ori.origin[0] - plane.normal[1] * tr.viewParms.ori.origin[1])
          / plane.normal[2];
        {
            int k;

            for (k = 0; k < waterReflectInfo.numPlanes; k++) {
                if (fabs(waterReflectInfo.planes[k][1] - h) < 0.5f && fabs(waterReflectInfo.planes[k][0] - plane.normal[2]) < 0.0005f) {
                    waterReflectInfo.planes[k][2] += 1.0f;
                    break;
                }
            }
            if (k == waterReflectInfo.numPlanes && k < 8) {
                VectorSet4(waterReflectInfo.planes[k], plane.normal[2], h, 1.0f, tr.viewParms.ori.origin[2] > h);
                waterReflectInfo.numPlanes++;
            }
        }
        // how far over it the eye is: a map's water need not lie level
        // (m3l1a's sea leans by two degrees, thirty units in a thousand)
        d = DotProduct(tr.viewParms.ori.origin, plane.normal) - plane.dist;
        if (d <= 0.0f) {
            continue; // under it
        }
        if (!found || d < above) {
            found  = qtrue;
            mirror = plane;
            above  = d;
        }
    }
    if (!found && puddleInView) {
        float h;

        // none along the look: the height it last took, so the mirror is not
        // dropped and taken up again as the eye turns
        qboolean have = R_RainPuddleHeight(tr.viewParms.ori.origin, tr.viewParms.ori.axis[0], &h);

        if (have) {
            rainCover.lastHeight = h;
            rainCover.hasLast    = qtrue;
        } else if (rainCover.hasLast && rainCover.lastHeight < tr.viewParms.ori.origin[2] - 8.0f) {
            h    = rainCover.lastHeight;
            have = qtrue;
        }
        if (have) {
            VectorSet(mirror.normal, 0.0f, 0.0f, 1.0f);
            mirror.dist = h;
            above       = tr.viewParms.ori.origin[2] - h;
            found = puddles = qtrue;
        }
    }
    if (!found) {
        return;
    }

    oldParms = tr.viewParms;
    parms    = tr.viewParms;

    // the eye and its axes turned over in the surface's plane; it sees what
    // the eye does above it, so the eye's PVS
    VectorMA(oldParms.ori.origin, -2.0f * above, mirror.normal, parms.ori.origin);
    for (i = 0; i < 3; i++) {
        VectorMA(oldParms.ori.axis[i], -2.0f * DotProduct(oldParms.ori.axis[i], mirror.normal), mirror.normal,
            parms.ori.axis[i]);
    }
    parms.isPortal = qtrue;
    parms.isMirror = qtrue;
    VectorCopy(mirror.normal, parms.portalPlane.normal);
    parms.portalPlane.dist = mirror.dist + (puddles ? 8.0f : -1.0f);
    parms.zFar             = 0.0f;
    parms.flags            = (parms.flags | VPF_NOVIEWMODEL | VPF_WATERREFLECT) & ~VPF_USESUNLIGHT;

    // the same share of the image as the view has of the screen
    parms.targetFbo      = tr.waterReflectFbo;
    parms.viewportX      = 0;
    parms.viewportY      = 0;
    parms.viewportWidth  = Q_max(1, oldParms.viewportWidth * tr.waterReflectFbo->width / glConfig.vidWidth);
    parms.viewportHeight = Q_max(1, oldParms.viewportHeight * tr.waterReflectFbo->height / glConfig.vidHeight);

    {
        const int before = tr.refdef.numDrawSurfs;

        R_RenderView(&parms);
        waterReflectInfo.surfs = tr.refdef.numDrawSurfs - before;
    }
    // the water's height under the eye, for waterinfo
    waterReflectInfo.height = oldParms.ori.origin[2] - above / mirror.normal[2];
    waterReflectInfo.frame  = tr.frameCount;

    Mat4Multiply(tr.viewParms.projectionMatrix, tr.viewParms.world.modelMatrix, mvp);

    tr.viewParms                    = oldParms;
    tr.viewParms.waterReflect       = !puddles;
    tr.viewParms.rainReflect        = puddles;
    VectorCopy(mirror.normal, tr.viewParms.waterReflectPlane);
    tr.viewParms.waterReflectPlane[3] = mirror.dist;
    tr.viewParms.waterReflectShare[0] = parms.viewportWidth / (float)tr.waterReflectFbo->width;
    tr.viewParms.waterReflectShare[1] = parms.viewportHeight / (float)tr.waterReflectFbo->height;
    Mat4Copy(mvp, tr.viewParms.waterReflectMvp);
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

        // the beach's ground alone (R_WaterBuildBeach)
        if (beachGround.on && tr.beachGroundImage) {
            VectorSet4(v, beachGround.x0, beachGround.y0, 1.0f / (beachGround.width * beachGround.texel),
                1.0f / (beachGround.rows * beachGround.texel));
            GL_BindToTMU(tr.beachGroundImage, TB_SPECULARMAP);
        } else {
            VectorClear4(v);
        }
        GLSL_SetUniformVec4(sp, UNIFORM_BEACHGROUND, v);

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
    if (water.ocean[0] > 0.0f) {
        ri.Printf(PRINT_ALL, "  ocean %g %g %g %g\n", water.ocean[0], water.ocean[1], water.ocean[2], water.ocean[3]);
    }
    if (waterReflectInfo.frame && tr.frameCount - waterReflectInfo.frame < 10) {
        int k;

        ri.Printf(PRINT_ALL, "  mirrors the world from under the water at %g: %d surfaces\n", waterReflectInfo.height,
            waterReflectInfo.surfs);
        for (k = 0; k < waterReflectInfo.numPlanes; k++) {
            ri.Printf(PRINT_ALL, "    water in view at %g (normal z %.5f): %d surfaces%s\n", waterReflectInfo.planes[k][1],
                waterReflectInfo.planes[k][0], (int)waterReflectInfo.planes[k][2],
                waterReflectInfo.planes[k][3] > 0.5f ? "" : ", over the eye");
        }
    } else {
        ri.Printf(PRINT_ALL, "  mirrors only what is on the screen (r_waterReflect %d)\n", r_waterReflect->integer);
    }
    if (water.numBeachSurfaces) {
        ri.Printf(PRINT_ALL, "  beach on %s%s: %s\n", water.beachSurfaces[0], water.numBeachSurfaces > 1 ? " (and more)" : "",
            beachGround.on ? va("%d x %d texels of %g units", beachGround.width, beachGround.rows, beachGround.texel)
                           : "none of it found, the beach is everywhere");
    }
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

/*
===============
R_RainPuddleShader

Added in OPM
  Whether puddles gather on what is drawn with this shader: its name holds
  one of r_rainPuddleShaders' words (or the list is empty). Kept for the last
  shader asked about, as the draws come in runs of one.
===============
*/
static qboolean R_RainPuddleShader(const shader_t *shader)
{
    static const shader_t *lastShader;
    static int             lastCount = -1;
    static qboolean        last;
    char                   list[MAX_CVAR_VALUE_STRING];
    char                  *word, *next;

    if (!shader) {
        return qfalse;
    }
    if (shader == lastShader && r_rainPuddleShaders->modificationCount == lastCount) {
        return last;
    }
    lastShader = shader;
    lastCount  = r_rainPuddleShaders->modificationCount;

    Q_strncpyz(list, r_rainPuddleShaders->string, sizeof(list));
    last = qtrue;
    for (word = list; *word; word = next) {
        while (*word == ' ' || *word == ',') {
            word++;
        }
        if (!*word) {
            break;
        }
        last = qfalse;
        for (next = word; *next && *next != ' ' && *next != ','; next++) {
        }
        if (*next) {
            *next++ = 0;
        }
        if (Q_stristr(shader->name, word)) {
            return last = qtrue;
        }
    }
    return last;
}

void R_WaterSetWet(shaderProgram_t *sp)
{
    vec4_t      v;
    qboolean    beach;
    float       rain;

    if (!r_waterWet->integer || !tr.world || (backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))) {
        R_WaterSetDry(sp);
        return;
    }
    beach = water.beachSet && water.shoreSet;
    rain  = R_RainAmount();
    if (!beach && rain <= 0.0f) {
        R_WaterSetDry(sp);
        return;
    }

    // the beach: how wet, its mark; the rain: how wet, and what is drawn (1
    // the world's own surfaces, which hold puddles; 2 the hands and the
    // weapon in view, which hold beads of it)
    VectorSet4(v, beach ? 1.0f : 0.0f, beach ? water.beachTop + 24.0f : 0.0f, rain, 0.0f);
    if (rain > 0.0f) {
        const trRefEntity_t *ent = backEnd.currentEntity;

        if (ent == &tr.worldEntity && !backEnd.currentStaticModel) {
            v[3] = 1.0f;
        } else if (ent && ent != &tr.worldEntity && (ent->e.renderfx & RF_FIRST_PERSON)) {
            v[3] = 2.0f;
        }
        // shown as how wet and the puddles, in place of the colour
        if (r_rainDebug->integer == 1 || r_rainDebug->integer == 2) {
            v[3] += r_rainDebug->integer == 2 ? 20.0f : 10.0f;
        }
    }
    GLSL_SetUniformVec4(sp, UNIFORM_WET, v);

    if (rain > 0.0f) {
        VectorSet4(v, rainCover.x0, rainCover.y0, 1.0f / (rainCover.width * rainCover.texel),
            1.0f / (rainCover.rows * rainCover.texel));
        GLSL_SetUniformVec4(sp, UNIFORM_RAINCOVER, v);
        VectorSet4(v, (float)fmod(backEnd.refdef.floatTime, 1000.0), rainCover.texel,
            R_RainPuddleShader(tess.shader) ? Com_Clamp(0.0f, 1.0f, r_rainPuddles->value) : 0.0f,
            Com_Clamp(0.0f, 1.0f, r_rainBeads->value));
        GLSL_SetUniformVec4(sp, UNIFORM_RAINPARAMS, v);
        GL_BindToTMU(tr.rainCoverImage, TMU_RAINCOVER);

        // the view the puddles mirror, if it was drawn (R_WaterReflectView)
        if (backEnd.viewParms.rainReflect && tr.waterReflectImage) {
            VectorSet4(v, 1.0f, backEnd.viewParms.waterReflectPlane[3], backEnd.viewParms.waterReflectShare[0],
                backEnd.viewParms.waterReflectShare[1]);
            GLSL_SetUniformMat4(sp, UNIFORM_RAINREFLECTMVP, backEnd.viewParms.waterReflectMvp);
            GL_BindToTMU(tr.waterReflectImage, TMU_RAINREFLECT);
        } else {
            VectorClear4(v);
        }
        GLSL_SetUniformVec4(sp, UNIFORM_RAINREFLECT, v);

        // a lightning flash (R_RainFrame), cold white
        VectorSet4(v, 0.8f, 0.87f, 1.0f, rainCover.flash * r_rainFlash->value * 2.5f);
        GLSL_SetUniformVec4(sp, UNIFORM_RAINFLASH, v);
    }

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

/*
===============
R_RainMistParams

Added in OPM
  The rain's mist for the light in the air (tr_froxel.c, froxel_fp.glsl):
  mist the density of the lowest of it, how high it thins to a third of
  that, the drift's time, the rain's haze further up; cover the cover's
  grid (as UNIFORM_RAINCOVER), which says what is out under the sky. False
  with no rain, or none of it wanted.
===============
*/
qboolean R_RainMistParams(vec4_t mist, vec4_t cover, float densityScale)
{
    const float amount = R_RainAmount() * Q_max(0.0f, r_rainMist->value);

    if (amount <= 0.0f || !tr.rainCoverImage) {
        return qfalse;
    }
    VectorSet4(mist, 5.0f * amount * densityScale, 50.0f, backEnd.refdef.floatTime, 0.12f * amount * densityScale);
    VectorSet4(cover, rainCover.x0, rainCover.y0, 1.0f / (rainCover.width * rainCover.texel),
        1.0f / (rainCover.rows * rainCover.texel));
    return qtrue;
}
