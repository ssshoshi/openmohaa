/*
===========================================================================
Copyright (C) 2015 the OpenMoHAA team

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

// windows.cpp: Window class
//

#include "g_phys.h"
#include "windows.h"
#include "game.h"
#include "g_glass.h"

Event EV_Window_Setup
(
    "_Windowsetup",
    EV_CODEONLY,
    NULL,
    NULL,
    "Does the post spawn setup of the Window"
);
Event EV_Window_SetDebris
(
    "debristype",
    EV_DEFAULT,
    "i",
    "type",
    "Sets the debris type of the Window"
);

CLASS_DECLARATION(Entity, WindowObject, "func_window") {
    {&EV_Window_Setup,     &WindowObject::WindowSetup     },
    {&EV_Damage,           &WindowObject::WindowDamaged   },
    {&EV_Killed,           &WindowObject::WindowKilled    },
    {&EV_Window_SetDebris, &WindowObject::WindowDebrisType},
    {NULL,                 NULL                           }
};

WindowObject::WindowObject()
{
    if (LoadingSavegame) {
        return;
    }

    edict->s.eType = ET_GENERAL;
    health         = 250;
    max_health     = health;
    deadflag       = 0;
    takedamage     = DAMAGE_YES;

    m_iDebrisType = WINDOW_GLASS;

    PostEvent(EV_Window_Setup, EV_POSTSPAWN);
}

void WindowObject::Archive(Archiver& arc)
{
    Entity::Archive(arc);

    arc.ArchiveInteger(&m_iDebrisType);
    arc.ArchiveString(&m_sBrokenModel);
}

void WindowObject::WindowSetup(Event *ev)
{
    Entity *pEnt;

    setMoveType(MOVETYPE_NONE);
    setSolidType(SOLID_BSP);
    setContents(CONTENTS_TRIGGER);

    // Added in OPM
    //  Glass that breaks a piece at a time (bg_glass.h, cgame cg_glass.cpp):
    //  its hits are kept in its surfaces[], which a brush model has no use for.
    if (m_iDebrisType == WINDOW_GLASS) {
        edict->s.eFlags |= EF_GLASS;
        memset(edict->s.surfaces, 0, sizeof(edict->s.surfaces));
        edict->s.surfaces[GLASS_S_PANE] = GLASS_PANE_SELF;
    }

    if (Target().length()) {
        // If a target is defined,
        // find it and set the model from it
        pEnt = (Entity *)G_FindTarget(NULL, Target());

        if (pEnt) {
            m_sBrokenModel = pEnt->model;

            // Don't keep the original entity
            pEnt->PostEvent(EV_Remove, 0);
        }
    }
}

void WindowObject::WindowDebrisType(Event *ev)
{
    m_iDebrisType = ev->GetInteger(1);
}

void WindowObject::WindowDamaged(Event *ev)
{
    int    iMeansOfDeath;
    int    iDamage;
    Vector vRight;
    Vector vForward;
    Vector vDir;
    Event *event;

    if (!takedamage) {
        return;
    }

    iDamage       = ev->GetInteger(2);
    iMeansOfDeath = ev->GetInteger(9);

    // Added in OPM
    //  Glass that breaks a piece at a time: a round knocks a hole where it went
    //  through, a blast or enough rounds bring it all down (g_glass.cpp).
    if (edict->s.eFlags & EF_GLASS) {
        WindowGlassDamaged(ev, iDamage, iMeansOfDeath);
        return;
    }

    if (iMeansOfDeath == MOD_BASH) {
        // double damage for bash
        iDamage *= 2;
    } else if (iMeansOfDeath == MOD_EXPLOSION) {
        // instant break
        iDamage *= 10;
    }

    health -= iDamage;

    if (health <= 0.0f) {
        event = new Event(EV_Killed);

        event->AddEntity(ev->GetEntity(1));
        event->AddInteger(ev->GetInteger(2));
        event->AddEntity(ev->GetEntity(3));

        ProcessEvent(event);
    }
}

// Added in OPM
void WindowObject::WindowGlassDamaged(Event *ev, int iDamage, int iMeansOfDeath)
{
    glassFrame_t frame;
    Vector       pos = ev->GetVector(4);
    Vector       dir = ev->GetVector(5);
    vec3_t       local, d, ldir;
    int          k, result;

    BG_GlassFrameFromBounds(mins, maxs, &frame);
    VectorSubtract(pos, origin, d);
    for (k = 0; k < 3; k++) {
        local[k] = DotProduct(d, orientation[k]);
        ldir[k]  = DotProduct(dir, orientation[k]);
    }
    const qboolean out = DotProduct(ldir, frame.n) > 0;

    switch (iMeansOfDeath) {
    case MOD_EXPLOSION:
    case MOD_GRENADE:
    case MOD_ROCKET:
    case MOD_AAGUN:
    case MOD_LANDMINE:
    case MOD_EXPLODEWALL:
        result = G_GlassPaneShatter(this, out, qtrue, pos) ? 2 : 0;
        break;
    case MOD_BASH:
        result = G_GlassPaneHit(this, &frame, local, 200, out, pos);
        break;
    default:
        result = G_GlassPaneHit(this, &frame, local, iDamage, out, pos);
        break;
    }

    if (result == 2) {
        // all its glass down: the frame stays (cgame draws it), shot through
        takedamage = DAMAGE_NO;
        deadflag   = DEAD_DEAD;
    }
}

void WindowObject::WindowKilled(Event *ev)
{
    Vector vCenter;

    setSolidType(SOLID_NOT);

    hideModel();
    takedamage = DAMAGE_NO;

    vCenter = (mins + maxs) * 0.5;
    vCenter += origin;

    gi.SetBroadcastVisible(vCenter, vCenter);
    gi.MSG_StartCGM(BG_MapCGMToProtocol(g_protocol, CGM_MAKE_WINDOW_DEBRIS));
    gi.MSG_WriteCoord(vCenter[0]);
    gi.MSG_WriteCoord(vCenter[1]);
    gi.MSG_WriteCoord(vCenter[2]);
    gi.MSG_WriteByte(m_iDebrisType);
    gi.MSG_EndCGM();

    if (m_sBrokenModel.length()) {
        Entity *ent = new Entity;

        ent->takedamage = DAMAGE_NO;
        ent->setModel(m_sBrokenModel);

        ent->setMoveType(MOVETYPE_NONE);
        ent->setSolidType(SOLID_BSP);

        setModel(m_sBrokenModel);

        if (spawnflags & WINDOW_BROKEN_BLOCK) {
            ent->setContents(CONTENTS_MONSTERCLIP | CONTENTS_PLAYERCLIP);
        } else {
            ent->setContents(CONTENTS_SHOOTONLY);
        }

        ent->setOrigin(origin);
        ent->showModel();
    }

    deadflag = DEAD_DEAD;

    // remove the window
    PostEvent(EV_Remove, 0);
}
