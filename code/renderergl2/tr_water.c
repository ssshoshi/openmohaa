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
//     sky 0.5 0.55 0.6        // the sky it mirrors (else the fog's colour)
//     chop 1  foam 1  refraction 0.03  glints 1
//
// And where the map has a beach the tide is going out from (it needs `shore`):
//
//     beach 900 -440          // the sand is wet this far inland of the shore line, and up to this height
//     puddles 0.3 90          // water stands in its hollows: how much of it, and how big they are
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
    float  chop, foam, refraction, glints;
    qboolean beachSet;
    float  beachReach, beachTop;
    float  puddles, puddleSize;
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

void R_WaterRegisterCvars(void)
{
    r_water      = ri.Cvar_Get("r_water", "1", CVAR_ARCHIVE | CVAR_LATCH);
    r_waterBlood = ri.Cvar_Get("r_waterBlood", "1", CVAR_ARCHIVE);
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

void R_WaterLoadWorld(const char *mapName)
{
    union {
        char *c;
        void *v;
    } buffer;
    char *text;

    R_WaterDefaults();
    numWaterSpots = 0;
    numSandStains = 0;

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

    VectorSet4(v, water.foam, water.refraction, water.glints, water.bloodClouds);
    GLSL_SetUniformVec4(sp, UNIFORM_WATERLOOK, v);

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
bodies), the second adds the sky it mirrors.
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

    if (!water.beachSet || !water.shoreSet || !tr.beachShader[1].program || !backEnd.softDepth || !tr.hdrDepthImage
        || backEnd.projection2D || (backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))
        || (backEnd.refdef.rdflags & RDF_NOWORLDMODEL)) {
        return;
    }

    box[0] = backEnd.viewParms.viewportX / (float)glConfig.vidWidth;
    box[1] = backEnd.viewParms.viewportY / (float)glConfig.vidHeight;
    box[2] = box[0] + backEnd.viewParms.viewportWidth / (float)glConfig.vidWidth;
    box[3] = box[1] + backEnd.viewParms.viewportHeight / (float)glConfig.vidHeight;

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
        GLSL_SetUniformVec4(sp, UNIFORM_WATERBLOOD, water.blood);
        VectorSet4(v, water.beachReach, water.beachTop, water.puddles, water.puddleSize);
        GLSL_SetUniformVec4(sp, UNIFORM_BEACHWET, v);

        {
            const int stains = r_waterBlood->value > 0.0f ? numSandStains : 0;

            VectorSet4(v, 0.0f, 0.0f, 0.0f, stains);
            GLSL_SetUniformVec4(sp, UNIFORM_WATERBLOODZONE, v);
            if (stains) {
                GLSL_SetUniformWaterSpots(sp, UNIFORM_SANDSTAINS, sandStains, stains);
            }
        }

        GL_State(GLS_DEPTHTEST_DISABLE | (pass ? (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE) : (GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO)));
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
    if (ri.Cmd_Argc() > 1) {
        int i;

        for (i = 0; i < numSandStains; i++) {
            ri.Printf(PRINT_ALL, "    on the sand at %.0f %.0f %.0f, %.0f across\n", sandStains[i][0], sandStains[i][1],
                sandStains[i][2], sandStains[i][3]);
        }
    }
    if (water.beachSet) {
        ri.Printf(PRINT_ALL, "  beach %g %g  puddles %g %g%s\n", water.beachReach, water.beachTop, water.puddles, water.puddleSize,
            water.shoreSet ? "" : "  (no shore line: not drawn)");
    }
    ri.Printf(PRINT_ALL, "  sky %s  chop %g  foam %g  refraction %g  glints %g\n", water.skySet ? va("%g %g %g", water.sky[0], water.sky[1], water.sky[2]) : "(the fog's)",
        water.chop, water.foam, water.refraction, water.glints);
}
