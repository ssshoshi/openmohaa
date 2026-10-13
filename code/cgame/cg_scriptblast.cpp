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
// The blasts a map's script sets off itself. A scripted mortar (Omaha's beach
// is full of them) is an effect model, models/animate/fx_mortar_dirt.tik and
// its kind, put where it is to land and told "anim start"; the men it is to
// kill get a death animation and "radiusdamage". The client is never sent an
// explosion for it, so nothing that answers one did: the dead were not thrown,
// lost no limbs, and the water did not heave. When one of these models starts
// its blast, it is told to all of them here, as a real explosion would be.

#include "cg_local.h"
#include "cg_ragdoll.h"
#include "cg_gore.h"
#include "cg_physics.h"
#include "cg_water.h"
#include "cg_scriptblast.h"

typedef struct {
    const char *model; // the effect, from models/animate/
    int         kind;  // as a real one: 0 grenade, 1 bazooka, 2 heavy shell, 3 tank
} cgScriptBlastKind_t;

static const cgScriptBlastKind_t cg_scriptBlastKinds[] = {
    {"fx_mortar_dirt.tik",      2},
    {"fx_mortar_water.tik",     2},
    {"fx_mortar_higgins.tik",   2},
    {"fx_explosion_mine.tik",   1},
    {"fx_explosion_flak88.tik", 3},
    {"fx_explosion_tank.tik",   3},
};

// how long into its blast it can be and still be one just seen to start: a
// model put there and set off in the same server frame is first seen so
static const float CG_SCRIPT_BLAST_FRESH = 0.3f;

// each entity's animation as it was the frame before
static struct {
    int model;
    int anim;
    int time;
} cg_scriptBlastWas[MAX_GENTITIES];

static int CG_ScriptBlastKind(int modelindex)
{
    const char *name = CG_ConfigString(CS_MODELS + modelindex);
    size_t      i;

    if (Q_stricmpn(name, "models/animate/fx_", 18)) {
        return -1;
    }
    for (i = 0; i < ARRAY_LEN(cg_scriptBlastKinds); i++) {
        if (!Q_stricmp(name + 15, cg_scriptBlastKinds[i].model)) {
            return cg_scriptBlastKinds[i].kind;
        }
    }
    return -1;
}

void CG_ScriptBlastsFrame(void)
{
    int i;

    if (!cg.snap) {
        return;
    }

    for (i = 0; i < cg.snap->numEntities; i++) {
        const int            num  = cg.snap->entities[i].number;
        const entityState_t *s    = &cg_entities[num].currentState;
        const qboolean       seen = cg_scriptBlastWas[num].time && cg_scriptBlastWas[num].model == s->modelindex
                              && cg.time - cg_scriptBlastWas[num].time < 1000;
        dtiki_t             *tiki;
        const char          *anim;
        int                  kind;

        if (s->modelindex <= 0 || s->solid == SOLID_BMODEL) {
            continue;
        }

        if (seen && cg_scriptBlastWas[num].anim == s->frameInfo[0].index) {
            cg_scriptBlastWas[num].time = cg.time;
            continue;
        }

        cg_scriptBlastWas[num].model = s->modelindex;
        cg_scriptBlastWas[num].anim  = s->frameInfo[0].index;
        cg_scriptBlastWas[num].time  = cg.time;

        kind = CG_ScriptBlastKind(s->modelindex);
        if (kind < 0) {
            continue;
        }
        tiki = cgi.R_Model_GetHandle(cgs.model_draw[s->modelindex]);
        if (!tiki) {
            continue;
        }
        anim = cgi.Anim_NameForNum(tiki, s->frameInfo[0].index);
        if (!anim || Q_stricmp(anim, "start")) {
            continue;
        }
        // just started, not one already going when it came into view
        if (!seen && s->frameInfo[0].time > CG_SCRIPT_BLAST_FRESH) {
            continue;
        }

        CG_RagdollNoteExplosion(s->origin, kind);
        CG_GoreNoteExplosion(s->origin, kind);
        CG_PhysicsNoteExplosion(s->origin, kind);
        {
            cgWaterSample_t water;

            if (CG_WaterSample(s->origin, &water) && s->origin[2] - water.surface < 48.0f) {
                vec3_t at;

                VectorSet(at, s->origin[0], s->origin[1], water.surface);
                CG_WaterSplash(at, 80.0f, 900.0f);
            }
        }
    }
}
