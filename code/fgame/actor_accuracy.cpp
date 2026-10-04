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

// actor_accuracy.cpp: OPM aim error against the player (ai_accuracy_model).
//
// The stock scatter depends on range and on how well the shooter sees. On top
// of it, the scatter grows when:
//  - he has only just seen the player (ai_accuracy_acquire times as wide,
//    easing to normal over 2 s);
//  - the player moves (up to 1.6 times at a run);
//  - he moves himself (1.4 times);
//  - he is under fire: one of the player's bullets passed close to him in the
//    last 1.5 s (1.5 times).
// Against a player standing still who has been in sight for 2 s, it is the
// stock aim.

#include "actor.h"
#include "ai_enhance.h"
#include "player.h"

// How close a bullet passes for him to count as under fire.
#define OPM_NEAR_MISS_DIST 64

// How long he stays put off by it.
#define OPM_UNDER_FIRE_TIME 1500

/*
===============
AI_BulletPassed

One of the player's bullets went from start to end: the Germans it passed
close to are under fire.
===============
*/
void AI_BulletPassed(Entity *owner, const float *start, const float *end)
{
    Vector vStart(start), vEnd(end), vSeg;
    float  fLenSquared;
    int    i;

    if (g_gametype->integer != GT_SINGLE_PLAYER || !owner || !owner->IsSubclassOfPlayer()) {
        return;
    }
    if (!AI_Enhanced(ai_accuracy_model) && !AI_Enhanced(ai_cover)) {
        return;
    }

    vSeg        = vEnd - vStart;
    fLenSquared = vSeg.lengthSquared();
    if (fLenSquared < 1) {
        return;
    }

    for (i = 1; i <= SentientList.NumObjects(); i++) {
        Sentient *pSent = SentientList.ObjectAt(i);
        Actor    *pActor;
        Vector    vClosest;
        float     t;

        if (!pSent->IsSubclassOfActor() || pSent->IsDead() || pSent->m_Team == TEAM_AMERICAN) {
            continue;
        }

        pActor = static_cast<Actor *>(pSent);
        t      = DotProduct(pActor->centroid - vStart, vSeg) / fLenSquared;
        t      = Q_clamp_float(t, 0, 1);

        vClosest = vStart + vSeg * t;
        if ((pActor->centroid - vClosest).lengthSquared() > Square(OPM_NEAR_MISS_DIST)) {
            continue;
        }

        if (level.inttime >= pActor->m_iOPMUnderFireTime + OPM_UNDER_FIRE_TIME && ai_debug && ai_debug->integer >= 2) {
            AI_Debug("%s is under fire", pActor->TargetName().c_str());
        }
        pActor->m_iOPMUnderFireTime = level.inttime;
    }
}

/*
===============
Actor::UnderFire
===============
*/
bool Actor::UnderFire(void) const
{
    return m_iOPMUnderFireTime && level.inttime >= m_iOPMUnderFireTime
        && level.inttime < m_iOPMUnderFireTime + OPM_UNDER_FIRE_TIME;
}

/*
===============
Actor::Accuracy_ScatterMult

How much wider than stock he scatters his shots at the player.
===============
*/
float Actor::Accuracy_ScatterMult(Player *player)
{
    float fAcquire, fSpeed, fMoving, fFire;
    float fSpeedFrac;
    int   iSeenFor;

    if (!AI_Enhanced(ai_accuracy_model)) {
        return 1;
    }

    // Only just seen him (or seen him again).
    fAcquire = 1;
    if (m_bEnemyVisible && m_Enemy == player) {
        iSeenFor = level.inttime - Q_max(m_iEnemyChangeTime, m_iEnemyVisibleChangeTime);
        if (iSeenFor < 2000) {
            fAcquire = 1 + (ai_accuracy_acquire->value - 1) * (1 - iSeenFor / 2000.0f);
        }
    }

    // A moving target: up to 1.6 at a run.
    fSpeedFrac = player->velocity.lengthXY() / 250.0f;
    fSpeed     = 1 + 0.6f * Q_min(fSpeedFrac, 1.0f);

    // A moving shooter.
    fMoving = velocity.lengthXY() > 30 ? 1.4f : 1;

    fFire = UnderFire() ? 1.5f : 1;

    if (ai_debug && ai_debug->integer >= 2 && fAcquire * fSpeed * fMoving * fFire > 1.01f) {
        AI_Debug(
            "%s aims %.1fx wider (acquire %.1f, target moving %.1f, moving %.1f, under fire %.1f)",
            TargetName().c_str(),
            fAcquire * fSpeed * fMoving * fFire,
            fAcquire,
            fSpeed,
            fMoving,
            fFire
        );
    }

    return fAcquire * fSpeed * fMoving * fFire;
}
