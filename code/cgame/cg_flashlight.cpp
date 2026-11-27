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
// The TL-122 flashlight a player wears clipped to the shirt. The "flashlight"
// command (player_stance.cpp) turns it on and off; the state comes in the
// player's entity flags (EF_OPM_FLASHLIGHT).
//
// While it is on the model (content/opm-flashlight, tools/flashlight/tl122.py)
// is drawn on the chest of the player's body, the way the chest faces. With
// realtime lighting 2 its light is the renderer's: lights/lamps.txt makes the
// model a lamp (kind headlight) that shines a spot light with shadows from its
// lens while its glow surface (tl122_beam) is drawn. Otherwise a dynamic light
// goes where the beam falls.

#include "cg_flashlight.h"

#define FLASHLIGHT_MODEL "models/items/opm_tl122.tik"
#define FLASHLIGHT_ICON  "textures/hud/opm_flashlight.tga"
#define FLASHLIGHT_REACH 900.0f // the dynamic light's beam, no further than this

static cvar_t *cg_flashlightTag;
static cvar_t *cg_flashlightOffset;
static cvar_t *cg_flashlightAngles;
static cvar_t *cg_flashlightScale;
static cvar_t *cg_flashlightIcon;
static cvar_t *cg_flashlightSmooth;
static cvar_t *cg_flashlightDebug;

static qhandle_t   flModel;
static qhandle_t   flIcon;
static sfxHandle_t flSoundOn, flSoundOff;
static qboolean    flRegistered;
static byte        flWasOn[MAX_GENTITIES];
static int         flSeen[MAX_GENTITIES]; // when each player was last drawn
static int         flOwnOn; // when the player's own was last drawn on

// Each player's flashlight, smoothed: its place in the body's frame and the
// beam's turn from the body's facing
typedef struct {
    int    time;
    vec3_t local;
    float  yaw;
} flSmooth_t;

static flSmooth_t flSmooth[MAX_GENTITIES];

void CG_InitFlashlight(void)
{
    // Where on the player's body it is clipped: the tag, and the flashlight's
    // place (forward, left, up) in the chest's frame (up the spine, forward
    // the way the body faces), and the beam's turn (pitch from level, yaw
    // from the torso's facing, roll)
    cg_flashlightTag    = cgi.Cvar_Get("cg_flashlightTag", "Bip01 Spine2", 0);
    cg_flashlightOffset = cgi.Cvar_Get("cg_flashlightOffset", "7 3 -2", 0);
    cg_flashlightAngles = cgi.Cvar_Get("cg_flashlightAngles", "-10 0 0", 0);
    cg_flashlightScale  = cgi.Cvar_Get("cg_flashlightScale", "1", 0);
    // How long (seconds) it takes to settle after the chest moves
    cg_flashlightSmooth = cgi.Cvar_Get("cg_flashlightSmooth", "0.12", 0);
    cg_flashlightDebug  = cgi.Cvar_Get("cg_flashlightDebug", "0", 0);
    // The HUD shows the flashlight while it is on
    cg_flashlightIcon = cgi.Cvar_Get("cg_flashlightIcon", "1", CVAR_ARCHIVE);

    flRegistered = qfalse;
    memset(flWasOn, 0, sizeof(flWasOn));
    memset(flSeen, 0, sizeof(flSeen));
    memset(flSmooth, 0, sizeof(flSmooth));
    flOwnOn = 0;
}

static void CG_FlashlightRegister(void)
{
    if (flRegistered) {
        return;
    }
    flRegistered = qtrue;
    flModel      = cgi.R_RegisterModel(FLASHLIGHT_MODEL);
    flIcon       = cgi.R_RegisterShaderNoMip(FLASHLIGHT_ICON);
    flSoundOn    = cgi.S_RegisterSound("sound/opm/flashlight_on.wav", qfalse);
    flSoundOff   = cgi.S_RegisterSound("sound/opm/flashlight_off.wav", qfalse);
}

// Whether the renderer lights it itself (lamps.txt): GL2 with realtime lighting 2
static qboolean CG_FlashlightRendererLit(void)
{
    static cvar_t *renderer, *rtl, *lamps;

    if (!renderer) {
        renderer = cgi.Cvar_Get("cl_renderer", "opengl1", 0);
        rtl      = cgi.Cvar_Get("r_realtimeLighting", "0", 0);
        lamps    = cgi.Cvar_Get("r_rtLamps", "1", 0);
    }
    return flModel && !Q_stricmp(renderer->string, "opengl2") && rtl->integer == 2 && lamps->integer;
}

// The click of its switch, when it goes on or off
static void CG_FlashlightClick(const centity_t *cent, qboolean on)
{
    const int num = cent->currentState.number;
    qboolean  seen;

    if (num < 0 || num >= MAX_GENTITIES) {
        return;
    }
    // not when a player comes into view with it already on
    seen        = flSeen[num] && cg.time >= flSeen[num] && cg.time - flSeen[num] < 500;
    flSeen[num] = cg.time;
    if (flWasOn[num] == (byte)on) {
        return;
    }
    flWasOn[num] = (byte)on;
    if (seen) {
        cgi.S_StartSound(cent->lerpOrigin, num, CHAN_AUTO, on ? flSoundOn : flSoundOff, 0.8f, 48.0f, 1.0f, 600.0f, 0);
    }
}

void CG_Flashlight(centity_t *cent, refEntity_t *body, int renderfx)
{
    const entityState_t *s = &cent->currentState;
    refEntity_t          fl;
    vec3_t               offset, angles, lens, end;
    trace_t              trace;
    int                  tag, k;
    qboolean             on;

    if (s->eType != ET_PLAYER || s->number < 0 || s->number >= MAX_GENTITIES) {
        return;
    }
    on = (s->eFlags & EF_OPM_FLASHLIGHT) ? qtrue : qfalse;
    CG_FlashlightRegister();
    CG_FlashlightClick(cent, on);
    if (!on || !body->tiki) {
        return;
    }
    if (s->number == cg.snap->ps.clientNum) {
        flOwnOn = cg.time;
    }

    tag = cgi.Tag_NumForName(body->tiki, cg_flashlightTag->string);
    if (tag < 0) {
        return;
    }

    memset(&fl, 0, sizeof(fl));
    fl.reType       = RT_MODEL;
    fl.hModel       = flModel;
    fl.tiki         = flModel ? cgi.R_Model_GetHandle(flModel) : NULL;
    fl.entityNumber = ENTITYNUM_NONE; // the player's number has the body's poses
    fl.scale        = cg_flashlightScale->value > 0 ? cg_flashlightScale->value : 1.0f;
    for (k = 0; k < 4; k++) {
        fl.shaderRGBA[k] = 255;
    }
    VectorClear(offset);
    VectorClear(angles);
    sscanf(cg_flashlightOffset->string, "%f %f %f", &offset[0], &offset[1], &offset[2]);
    sscanf(cg_flashlightAngles->string, "%f %f %f", &angles[0], &angles[1], &angles[2]);

    // The chest's frame: up along the spine (the biped's spine bones run
    // along their x), forward the way the body faces with the spine's lean
    // taken out of it. The model sits on the chest in this frame; the beam
    // only takes the torso's turn from it and keeps its pitch from level, so
    // the spine's rocking (strafing, kneeling) does not swing it about
    {
        orientation_t tagOr = cgi.TIKI_Orientation(body, tag & TAG_MASK);
        vec3_t        tagAxis[3], chest[3], turn[3], at, local;
        float         yaw, a;
        int           dt;
        flSmooth_t   *sm = &flSmooth[s->number];

        MatrixMultiply(tagOr.axis, body->axis, tagAxis);
        VectorNormalize2(tagAxis[0], chest[2]);
        VectorMA(body->axis[0], -DotProduct(body->axis[0], chest[2]), chest[2], chest[0]);
        if (VectorNormalize(chest[0]) < 0.01f) {
            return;
        }
        CrossProduct(chest[2], chest[0], chest[1]);

        VectorCopy(body->origin, at);
        for (k = 0; k < 3; k++) {
            VectorMA(at, tagOr.origin[k], body->axis[k], at);
        }
        for (k = 0; k < 3; k++) {
            VectorMA(at, offset[k], chest[k], at);
        }
        // in the body's frame, so walking does not lag it behind
        VectorSubtract(at, body->origin, at);
        for (k = 0; k < 3; k++) {
            local[k] = DotProduct(at, body->axis[k]);
        }
        yaw = RAD2DEG(atan2(DotProduct(chest[0], body->axis[1]), DotProduct(chest[0], body->axis[0])));

        if (cg_flashlightDebug->integer && s->number == cg.snap->ps.clientNum) {
            cgi.Printf(
                "flashlight: chest pitch %.1f yaw %.1f, at %.1f %.1f %.1f\n",
                -RAD2DEG(asin(Q_clamp_float(DotProduct(chest[0], body->axis[2]), -1.0f, 1.0f))),
                yaw,
                local[0],
                local[1],
                local[2]
            );
        }

        // smoothed: the animations' bob, not the body's moves
        dt = cg.time - sm->time;
        if (!sm->time || dt < 0 || dt > 250) {
            a = 1.0f;
        } else {
            a = cg_flashlightSmooth->value > 0 ? 1.0f - expf(-dt / (1000.0f * cg_flashlightSmooth->value)) : 1.0f;
        }
        sm->time = cg.time;
        for (k = 0; k < 3; k++) {
            sm->local[k] += (local[k] - sm->local[k]) * a;
        }
        sm->yaw += AngleNormalize180(yaw - sm->yaw) * a;
        sm->yaw = AngleNormalize180(sm->yaw);

        VectorCopy(body->origin, fl.origin);
        for (k = 0; k < 3; k++) {
            VectorMA(fl.origin, sm->local[k], body->axis[k], fl.origin);
        }
        angles[1] += sm->yaw;
        AnglesToAxis(angles, turn);
        MatrixMultiply(turn, body->axis, fl.axis);
        VectorCopy(fl.origin, fl.oldorigin);
        fl.scale *= body->scale > 0 ? body->scale : 1.0f;
    }
    fl.renderfx = body->renderfx & ~(RF_FLAGS_NOT_INHERITED | RF_LIGHTING_ORIGIN);
    fl.renderfx &= ~(RF_FIRST_PERSON | RF_THIRD_PERSON | RF_FIRST_PERSON_BODY | RF_DEPTHHACK);
    fl.renderfx |= renderfx | RF_LIGHTING_ORIGIN;
    VectorCopy(fl.origin, fl.lightingOrigin);
    fl.frameInfo[0].weight = 1.0f;
    fl.actionWeight        = 1.0f;

    if (fl.tiki) {
        cgi.R_AddRefEntityToScene(&fl, ENTITYNUM_NONE);
    }
    if (CG_FlashlightRendererLit()) {
        return;
    }

    // Without the renderer's lamp: a dynamic light where the beam falls,
    // dimmer the further it goes, pulled back off the wall it lights
    VectorCopy(fl.origin, lens);
    VectorMA(lens, 3.0f * fl.scale, fl.axis[0], lens);
    VectorMA(lens, FLASHLIGHT_REACH, fl.axis[0], end);
    CG_Trace(&trace, lens, vec3_origin, vec3_origin, end, s->number, MASK_SHOT, qfalse, qtrue, "CG_Flashlight");
    {
        const float along = trace.fraction * FLASHLIGHT_REACH;
        const float fade  = 1.0f - 0.65f * trace.fraction;
        vec3_t      at;

        VectorMA(lens, Q_max(8.0f, along - 24.0f), fl.axis[0], at);
        cgi.R_AddLightToScene(at, (90.0f + along * 0.18f) * fade, 1.0f, 0.88f, 0.66f, 0);
    }
}

void CG_DrawFlashlight(void)
{
    const float size = 40.0f;
    vec4_t      color;

    if (!cg.snap || !cg_flashlightIcon->integer || cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        return;
    }
    // (the player's own entity state, which CG_Flashlight saw this frame)
    if (!flOwnOn || cg.time < flOwnOn || cg.time - flOwnOn > 100) {
        return;
    }
    CG_FlashlightRegister();
    if (!flIcon) {
        return;
    }
    // bottom left, above the compass's corner, a little dimmed like the HUD
    VectorSet4(color, 1.0f, 1.0f, 1.0f, 0.85f);
    cgi.R_SetColor(color);
    cgi.R_DrawStretchPic(
        8.0f, cgs.glconfig.vidHeight - size - 112.0f, size, size, 0, 0, 1, 1, flIcon
    );
    cgi.R_SetColor(NULL);
}
