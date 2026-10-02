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

// cg_mg42.cpp -- a hot MG42 barrel glows
//
// Added in OPM. The server keeps an MG42's barrel heat (TurretGun::ShotFired)
// in its entity's shader_data[0], as a fraction of the heat at which it overheats:
// the world model's, and the view model's its gunner sees instead. The model is
// drawn once more over itself (RF_CUSTOMSHADERPASS) in added light the colour
// of the heat: dull red, then orange, then yellow-white as it nears overheating,
// fading as it cools. The MG42 is one mesh on one texture: the shader's mask
// (tools/mg42/heatmask.py) keeps the light to the barrel.

#include "cg_local.h"

static cvar_t *cg_mg42glow;

// The colour of steel at a heat: nothing when cool, then dull red, orange and
// yellow-white as it nears overheating.
static void CG_MG42HeatColour(float heat, float *rgb)
{
    float t = (heat - 0.2f) / 0.8f;

    if (t <= 0) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    if (t > 1.25f) {
        t = 1.25f;
    }

    rgb[0] = Q_min(1.f, 0.35f + 0.9f * t) * t;
    rgb[1] = Q_max(0.f, t - 0.35f) * 0.9f * t;
    rgb[2] = Q_max(0.f, t - 0.75f) * 1.2f * t;
}

extern "C" void CG_MG42BarrelGlow(refEntity_t *model, const entityState_t *s1)
{
    float rgb[3];
    float flicker;
    int   k;

    if (!cg_mg42glow) {
        cg_mg42glow = cgi.Cvar_Get("cg_mg42glow", "1", CVAR_ARCHIVE);
    }

    if (!cg_mg42glow->integer || !model->tiki || model->customShader || !Q_stristr(model->tiki->name, "mg42")) {
        return;
    }

    if (cg_mg42glow->integer == 2) {
        // debug: the glow at its brightest, hot or not
        rgb[0] = rgb[1] = rgb[2] = 1.f;
        flicker = 1.f;
    } else if (s1->shader_data[0] < 0.2f) {
        return;
    } else {
        flicker = 0.92f + 0.08f * sin(cg.time * 0.013f + s1->number);
        CG_MG42HeatColour(s1->shader_data[0], rgb);
    }

    model->customShader = cgi.R_RegisterShader("opm_mg42_glow");
    model->renderfx |= RF_CUSTOMSHADERPASS;
    for (k = 0; k < 3; k++) {
        model->shaderRGBA[k] = (byte)(Q_min(1.f, rgb[k] * flicker) * 255);
    }
    model->shaderRGBA[3] = 255;
}
