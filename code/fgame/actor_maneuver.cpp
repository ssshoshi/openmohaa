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

// actor_maneuver.cpp: OPM moves of a German's own in the middle of a fight,
// on top of the turret and cover attack states:
//  - flanking (ai_flank): one or two of a squad work round the enemy's side
//    while the rest keep him busy;
//  - rushing (ai_rush): a few close in while the player reloads, or just
//    after one of their grenades went off by him;
//  - advancing under smoke (ai_smoke_advance): a squad moves up into the
//    smoke one of them threw (DecideToThrowSmoke).
//
// Each move takes the path nodes where there are some. Open ground often has
// none, so where the path search fails he runs straight there instead (as to
// an info_waypoint), if the ground on the way is clear and walkable. All of
// it stays inside the leash the map gave him.

#include "actor.h"
#include "ai_enhance.h"
#include "player.h"
#include "weapon.h"

enum {
    OPM_MANEUVER_NONE,
    OPM_MANEUVER_FLANK,
    OPM_MANEUVER_RUSH,
    OPM_MANEUVER_ADVANCE,
};

static const char *Maneuver_Name(int eManeuver)
{
    switch (eManeuver) {
    case OPM_MANEUVER_FLANK:
        return "flank";
    case OPM_MANEUVER_RUSH:
        return "rush";
    case OPM_MANEUVER_ADVANCE:
        return "advance";
    default:
        return "move";
    }
}

static const cvar_t *Maneuver_Cvar(int eManeuver)
{
    switch (eManeuver) {
    case OPM_MANEUVER_FLANK:
        return ai_flank;
    case OPM_MANEUVER_RUSH:
        return ai_rush;
    case OPM_MANEUVER_ADVANCE:
        return ai_smoke_advance;
    default:
        return NULL;
    }
}

// Where and when a German grenade last went off (AI_GrenadeWentOff).
static int    s_iOPMLastBlastTime = -1;
static Vector s_vOPMLastBlastPos;

void AI_GrenadeWentOff(Entity *owner, const float *pos)
{
    if (!owner || !owner->IsSubclassOfActor() || static_cast<Actor *>(owner)->m_Team != TEAM_GERMAN) {
        return;
    }

    s_iOPMLastBlastTime = level.inttime;
    s_vOPMLastBlastPos  = pos;
}

// Why the last decision came to nothing, for ai_debug 2 (Maneuver_Think).
static const char *s_pszOPMWhyNot;
static int         s_piOPMWhyNotTime[MAX_GENTITIES];

#define WHY_NOT(why) (s_pszOPMWhyNot = (why), false)

// What the straight run and its checks collide with: what he does, less the
// other people about, who move.
#define MASK_OPM_MANEUVER (MASK_MONSTERSOLID & ~CONTENTS_BODY)

// How high he steps up without climbing.
#define OPM_MANEUVER_STEPSIZE 18

/*
===============
Actor::Maneuver_GroundAt

The floor under vPos (searched from a little above it), if he can stand
there: not in water, nor wedged into anything.
===============
*/
bool Actor::Maneuver_GroundAt(const Vector& vPos, Vector *pvGround)
{
    static const float pfAbove[] = {64, 160, 320};
    trace_t            trace;
    int                i;

    // From a little above it, or higher if the ground rises there.
    for (i = 0; i < (int)ARRAY_LEN(pfAbove); i++) {
        trace = G_Trace(
            vPos + Vector(0, 0, pfAbove[i]),
            vec_zero,
            vec_zero,
            vPos - Vector(0, 0, 512),
            this,
            MASK_OPM_MANEUVER | MASK_WATER,
            qfalse,
            "Actor::Maneuver_GroundAt"
        );
        if (!trace.startsolid && !trace.allsolid) {
            break;
        }
    }
    if (i == ARRAY_LEN(pfAbove) || trace.fraction >= 1 || (trace.contents & MASK_WATER)) {
        return false;
    }
    if (trace.plane.normal[2] < 0.7f) {
        // Too steep to stand on.
        return false;
    }

    *pvGround = trace.endpos;

    // Room for him there, standing on a slope: his box is checked a step up.
    trace = G_Trace(
        *pvGround + Vector(0, 0, OPM_MANEUVER_STEPSIZE),
        mins,
        maxs,
        *pvGround + Vector(0, 0, OPM_MANEUVER_STEPSIZE),
        this,
        MASK_OPM_MANEUVER,
        qfalse,
        "Actor::Maneuver_GroundAt"
    );
    return !trace.startsolid && !trace.allsolid;
}

/*
===============
Actor::Maneuver_DirectRouteClear

Whether he can run straight from vFrom to vTo: a walkable floor all the way,
with no climb or drop he could not step, and nothing in the way.
===============
*/
bool Actor::Maneuver_DirectRouteClear(const Vector& vFrom, const Vector& vTo)
{
    static const float fStep     = 48;
    static const float fMaxRise  = 40;
    Vector             vDelta;
    Vector             vPrev;
    Vector             vGround;
    trace_t            trace;
    float              fDist;
    int                i, iSteps;

    vDelta   = vTo - vFrom;
    vDelta.z = 0;
    fDist    = vDelta.length();
    if (fDist < 1) {
        return true;
    }

    iSteps = (int)ceil(fDist / fStep);
    vPrev  = vFrom;

    for (i = 1; i <= iSteps; i++) {
        Vector vPos = vFrom + vDelta * ((float)i / iSteps);

        vPos.z = vPrev.z;
        if (!Maneuver_GroundAt(vPos, &vGround)) {
            return false;
        }
        if (fabs(vGround.z - vPrev.z) > fMaxRise) {
            return false;
        }

        trace = G_Trace(
            vPrev + Vector(0, 0, OPM_MANEUVER_STEPSIZE),
            mins,
            maxs,
            vGround + Vector(0, 0, OPM_MANEUVER_STEPSIZE),
            this,
            MASK_OPM_MANEUVER,
            qfalse,
            "Actor::Maneuver_DirectRouteClear"
        );
        if (trace.startsolid || trace.allsolid || trace.fraction < 1) {
            return false;
        }

        vPrev = vGround;
    }

    return fabs(vPrev.z - vTo.z) <= fMaxRise;
}

/*
===============
Actor::Maneuver_InLeash
===============
*/
bool Actor::Maneuver_InLeash(const Vector& vPos) const
{
    return (vPos - m_vHome).lengthXYSquared() <= m_fLeashSquared * 0.9f;
}

/*
===============
Actor::Maneuver_Route

Finds his way to vDest: by the path nodes if they lead there without a long
way round, else straight there if the way is clear.
===============
*/
bool Actor::Maneuver_Route(const Vector& vDest, bool *pbDirect)
{
    float fStraight;

    fStraight = (vDest - origin).lengthXY();

    ClearPath();
    SetPath(vDest, NULL, 0, m_vHome, m_fLeashSquared);
    if (PathExists() && !PathComplete() && PathDist() <= fStraight * 2 + 256) {
        *pbDirect = false;
        return true;
    }
    ClearPath();

    if (Maneuver_DirectRouteClear(origin, vDest)) {
        *pbDirect = true;
        return true;
    }

    return false;
}

/*
===============
Actor::Maneuver_Start
===============
*/
bool Actor::Maneuver_Start(int eManeuver, const Vector& vDest, int iMaxTime)
{
    bool bDirect;

    if (!Maneuver_Route(vDest, &bDirect)) {
        return false;
    }

    if (m_pCoverNode) {
        m_pCoverNode->Relinquish();
        m_pCoverNode = NULL;
    }

    m_eOPMManeuver             = eManeuver;
    m_vOPMManeuverDest         = vDest;
    m_bOPMManeuverDirect       = bDirect;
    m_iOPMManeuverStart        = level.inttime;
    m_iOPMManeuverEnd          = level.inttime + iMaxTime;
    m_vOPMManeuverLastPos      = origin;
    m_iOPMManeuverProgressTime = level.inttime;

    AI_Debug(
        "%s starts to %s, %.0f away to (%.0f %.0f %.0f), %s",
        TargetName().c_str(),
        Maneuver_Name(eManeuver),
        (vDest - origin).lengthXY(),
        vDest.x,
        vDest.y,
        vDest.z,
        bDirect ? "running straight there" : "by the path nodes"
    );
    return true;
}

/*
===============
Actor::Maneuver_End

Back to the attack state he came from, to fight from where he got to.
===============
*/
void Actor::Maneuver_End(const char *reason)
{
    AI_Debug(
        "%s stops his %s (%s), %.0f from where he was going",
        TargetName().c_str(),
        Maneuver_Name(m_eOPMManeuver),
        reason,
        (m_vOPMManeuverDest - origin).lengthXY()
    );

    m_eOPMManeuver = OPM_MANEUVER_NONE;
    ClearPath();

    if (CurrentThink() == THINK_TURRET) {
        TransitionState(ACTOR_STATE_TURRET_COMBAT, 0);
    } else if (CurrentThink() == THINK_COVER) {
        TransitionState(ACTOR_STATE_COVER_FIND_COVER, 0);
    }
}

/*
===============
Actor::Maneuver_BlockedDirect

Called when something stops his straight run (GetMoveInfo): rather than the
fail-safe, which would slide him through it, he gives up the move.
===============
*/
bool Actor::Maneuver_BlockedDirect(void)
{
    if (m_eOPMManeuver == OPM_MANEUVER_NONE || !m_bOPMManeuverDirect) {
        return false;
    }

    // Maneuver_Think ends it on his next think.
    m_iOPMManeuverEnd = level.inttime;
    return true;
}

/*
===============
Actor::Maneuver_CanStart

Whether he is free to start a move: fighting, not in the middle of something
else, and with room to move on his leash.
===============
*/
bool Actor::Maneuver_CanStart(void)
{
    if (m_Team != TEAM_GERMAN || !m_Enemy || m_Enemy->IsDead()) {
        return WHY_NOT("no enemy");
    }
    if (m_ThinkState != THINKSTATE_ATTACK || m_bLockThinkState || m_bInReload) {
        return WHY_NOT("not attacking, locked or reloading");
    }
    if (m_iOPMHoldGrenadeTime && level.inttime < m_iOPMHoldGrenadeTime + 3700) {
        // Winding up to throw (HoldGrenade).
        return WHY_NOT("throwing");
    }
    if (m_fLeash < 128) {
        return WHY_NOT("leash under 128");
    }

    switch (CurrentThink()) {
    case THINK_TURRET:
        switch (m_State) {
        case ACTOR_STATE_TURRET_GRENADE:
        case ACTOR_STATE_TURRET_INTRO_AIM:
        case ACTOR_STATE_TURRET_RUN_HOME:
        case ACTOR_STATE_TURRET_RUN_AWAY:
        case ACTOR_STATE_TURRET_FAKE_ENEMY:
        case ACTOR_STATE_TURRET_COVER_INSTEAD:
        case ACTOR_STATE_TURRET_BECOME_COVER:
            return WHY_NOT("turret state busy");
        }
        break;
    case THINK_COVER:
        switch (m_State) {
        case ACTOR_STATE_COVER_GRENADE:
        case ACTOR_STATE_COVER_SPECIAL_ATTACK:
        case ACTOR_STATE_COVER_FINISH_RELOADING:
        case ACTOR_STATE_COVER_LOOP:
        case ACTOR_STATE_COVER_FAKE_ENEMY:
            return WHY_NOT("cover state busy");
        }
        break;
    default:
        return WHY_NOT("not turret or cover");
    }

    if ((origin - m_vHome).lengthXYSquared() > m_fLeashSquared) {
        return WHY_NOT("off his leash");
    }
    return true;
}

/*
===============
Actor::Maneuver_DecideToFlank

Now and then, one of a squad (two, if it is four or more) goes round to the
enemy's side, 45 to 90 degrees round from where he is, to somewhere he can
see him from. The rest stay to keep him busy.
===============
*/
bool Actor::Maneuver_DecideToFlank(void)
{
    static const float pfAngles[] = {75, 60, 90, 45};
    static const float pfRanges[] = {1.0f, 0.75f};
    Sentient          *pSquadMate;
    Vector             vEnemy, vDelta, vPos, vGround, vEye;
    float              fDist, fYaw, fMinDist, fRange;
    int                iMates, iFlanking, iCooldown, iTries;
    int                iSide, iFirstSide, i, j;
    int                iNoGround, iNoSight, iNoRoute;
    static char        szWhy[64];

    if (!AI_Enhanced(ai_flank) || level.inttime < m_iOPMNextFlankCheck) {
        return false;
    }
    m_iOPMNextFlankCheck = level.inttime + 2500 + (rand() % 2000);

    iCooldown = (int)(ai_flank_cooldown->value * 1000);
    if (m_iOPMLastFlankTime && level.inttime < m_iOPMLastFlankTime + iCooldown) {
        return WHY_NOT("flank cooldown");
    }

    vEnemy = m_vLastEnemyPos;
    vDelta = origin - vEnemy;
    fDist  = vDelta.lengthXY();
    if (fDist < 384 || fDist > 1800) {
        return WHY_NOT(fDist < 384 ? "enemy too close to flank" : "enemy too far to flank");
    }

    iMates    = 0;
    iFlanking = 0;
    for (pSquadMate = m_pNextSquadMate; pSquadMate != this; pSquadMate = pSquadMate->m_pNextSquadMate) {
        Actor *pActor;

        if (!pSquadMate->IsSubclassOfActor() || pSquadMate->IsDead()) {
            continue;
        }

        pActor = static_cast<Actor *>(pSquadMate);
        if (pActor->m_Enemy != m_Enemy) {
            continue;
        }

        iMates++;
        if (pActor->m_eOPMManeuver == OPM_MANEUVER_FLANK) {
            iFlanking++;
        }
        if (pActor->m_iOPMLastFlankTime && level.inttime < pActor->m_iOPMLastFlankTime + iCooldown / 2) {
            // One at a time: someone has only just gone.
            return WHY_NOT("a squadmate just flanked");
        }
    }

    // Someone must stay to keep the enemy's head down.
    if (!iMates || iFlanking >= (iMates >= 3 ? 2 : 1)) {
        return WHY_NOT(iMates ? "enough flanking" : "no squadmates on his enemy");
    }

    if (random() >= 0.4f) {
        return WHY_NOT("chance");
    }

    fYaw     = atan2(vDelta.y, vDelta.x);
    fMinDist = Q_max(320.0f, sqrt(m_fMinDistanceSquared) + 32);
    vEye     = m_Enemy->EyePosition();

    iFirstSide = (rand() & 1) ? 1 : -1;
    iTries     = 0;
    iNoGround  = 0;
    iNoSight   = 0;
    iNoRoute   = 0;

    for (iSide = 0; iSide < 2; iSide++) {
        float fSide = iSide ? -iFirstSide : iFirstSide;

        for (i = 0; i < (int)ARRAY_LEN(pfAngles); i++) {
            for (j = 0; j < (int)ARRAY_LEN(pfRanges); j++) {
                float fAngle = fYaw + DEG2RAD(pfAngles[i]) * fSide;

                fRange = Q_min(Q_max(fDist * pfRanges[j], fMinDist), 1600.0f);
                vPos   = vEnemy + Vector(cos(fAngle), sin(fAngle), 0) * fRange;
                vPos.z = Q_max(vEnemy.z, origin.z);

                if (!Maneuver_InLeash(vPos) || (vPos - origin).lengthXY() < 192) {
                    continue;
                }

                if (++iTries > 6) {
                    Com_sprintf(szWhy, sizeof(szWhy), "no flank spot: ground %d, sight %d, route %d", iNoGround, iNoSight, iNoRoute);
                    return WHY_NOT(szWhy);
                }

                if (!Maneuver_GroundAt(vPos, &vGround)) {
                    iNoGround++;
                    continue;
                }

                // He must be able to see the enemy from there.
                if (!G_SightTrace(
                        vGround + Vector(0, 0, 56),
                        vec_zero,
                        vec_zero,
                        vEye,
                        this,
                        m_Enemy,
                        MASK_CANSEE_NOENTS,
                        qfalse,
                        "Actor::Maneuver_DecideToFlank"
                    )) {
                    iNoSight++;
                    continue;
                }

                if (Maneuver_Start(OPM_MANEUVER_FLANK, vGround, 3000 + (int)((vGround - origin).lengthXY() * 10))) {
                    m_iOPMLastFlankTime = level.inttime;
                    AI_Debug(
                        "%s flanks %.0f degrees round, %.0f from the enemy, with %d squadmates on him",
                        TargetName().c_str(),
                        pfAngles[i],
                        fRange,
                        iMates
                    );
                    return true;
                }
                iNoRoute++;
            }
        }
    }

    if (!iTries) {
        return WHY_NOT("no flank spot in his leash");
    }
    Com_sprintf(szWhy, sizeof(szWhy), "no flank spot: ground %d, sight %d, route %d", iNoGround, iNoSight, iNoRoute);
    return WHY_NOT(szWhy);
}

/*
===============
Actor::Maneuver_DecideToRush

When the player reloads, or one of their grenades has just gone off by him,
a German or two close enough to see (or hear) it rush him.
===============
*/
bool Actor::Maneuver_DecideToRush(void)
{
    Sentient *pSquadMate;
    Vector    vDir, vPos, vGround;
    float     fDist, fStop, fTravel;
    bool      bReloading, bBlast;
    int       iRushing;

    if (!AI_Enhanced(ai_rush) || level.inttime < m_iOPMNextRushTime) {
        return false;
    }

    bReloading = false;
    if (m_Enemy->IsSubclassOfPlayer()) {
        Weapon *pWeapon = static_cast<Player *>(m_Enemy.Pointer())->GetActiveWeapon(WEAPON_MAIN);
        bReloading      = pWeapon && pWeapon->GetState() == WEAPON_RELOADING;
    }

    bBlast = s_iOPMLastBlastTime >= 0 && level.inttime >= s_iOPMLastBlastTime
          && level.inttime < s_iOPMLastBlastTime + 1500
          && (s_vOPMLastBlastPos - m_Enemy->origin).lengthSquared() < Square(384);

    if (!bReloading && !bBlast) {
        return false;
    }

    // One chance for each reload or blast.
    m_iOPMNextRushTime = level.inttime + 5000;

    vDir  = m_Enemy->origin - origin;
    fDist = vDir.lengthXY();
    if (fDist < 200 || fDist > 1000) {
        return false;
    }

    // He sees the reload or is close enough to hear it; after a blast he
    // must be nearer, and see the enemy unless he is close.
    if (!(bReloading ? CanSeeEnemy(500) || fDist < 500 : fDist < 700 && (CanSeeEnemy(500) || fDist < 400))) {
        return false;
    }

    iRushing = 0;
    for (pSquadMate = m_pNextSquadMate; pSquadMate != this; pSquadMate = pSquadMate->m_pNextSquadMate) {
        if (pSquadMate->IsSubclassOfActor() && static_cast<Actor *>(pSquadMate)->m_eOPMManeuver == OPM_MANEUVER_RUSH) {
            iRushing++;
        }
    }
    if (iRushing >= 2) {
        return false;
    }

    if (random() * 100 >= ai_rush_chance->value) {
        return false;
    }

    fStop   = Q_max(160.0f, sqrt(m_fMinDistanceSquared));
    fTravel = Q_min(fDist - fStop, 450.0f);

    vDir.z = 0;
    vDir.normalize();

    for (; fTravel >= 96; fTravel -= 64) {
        vPos = origin + vDir * fTravel;
        if (Maneuver_InLeash(vPos)) {
            break;
        }
    }
    if (fTravel < 96 || !Maneuver_GroundAt(vPos, &vGround)) {
        return false;
    }

    if (!Maneuver_Start(OPM_MANEUVER_RUSH, vGround, 1500 + (int)(fTravel * 5))) {
        return false;
    }

    AI_Debug(
        "%s rushes the enemy %.0f away while %s",
        TargetName().c_str(),
        fDist,
        bReloading ? "he reloads" : "he reels from a grenade"
    );
    return true;
}

/*
===============
Actor::Maneuver_SquadSmokeThrown

He has thrown smoke at vSmoke: his squad moves up into it once it is thick.
===============
*/
void Actor::Maneuver_SquadSmokeThrown(const Vector& vSmoke)
{
    Sentient *pSquadMate;

    if (!AI_Enhanced(ai_smoke_advance)) {
        return;
    }

    pSquadMate = this;
    do {
        if (pSquadMate->IsSubclassOfActor() && !pSquadMate->IsDead()) {
            Actor *pActor = static_cast<Actor *>(pSquadMate);

            if (pActor->m_Enemy && (pActor->origin - vSmoke).lengthSquared() < Square(1500)) {
                pActor->m_iOPMSmokeAdvanceTime = level.inttime + 3000 + (rand() % 1500);
                pActor->m_vOPMSmokeAdvancePos  = vSmoke;
            }
        }

        pSquadMate = pSquadMate->m_pNextSquadMate;
    } while (pSquadMate != this);
}

/*
===============
Actor::Maneuver_DecideToAdvance

Moves up into his squad's smoke, spread out across it on his side, as far as
his leash lets him.
===============
*/
bool Actor::Maneuver_DecideToAdvance(void)
{
    Vector vSmoke, vToEnemy, vSide, vPos, vGround, vDir;
    float  fDist;

    if (!m_iOPMSmokeAdvanceTime || level.inttime < m_iOPMSmokeAdvanceTime) {
        return false;
    }
    m_iOPMSmokeAdvanceTime = 0;

    if (!AI_Enhanced(ai_smoke_advance)) {
        return false;
    }

    vSmoke     = m_vOPMSmokeAdvancePos;
    vToEnemy   = m_vLastEnemyPos - vSmoke;
    vToEnemy.z = 0;
    if (vToEnemy.normalize() < 1) {
        return false;
    }
    vSide = Vector(-vToEnemy.y, vToEnemy.x, 0);

    vPos   = vSmoke - vToEnemy * (32 + random() * 96) + vSide * (crandom() * 160);
    vPos.z = Q_max(vSmoke.z, origin.z);

    // As far toward it as the leash lets him.
    vDir   = vPos - origin;
    vDir.z = 0;
    fDist  = vDir.normalize();
    while (fDist >= 128 && !Maneuver_InLeash(origin + vDir * fDist)) {
        fDist -= 64;
    }
    if (fDist < 128) {
        return false;
    }

    vPos = origin + vDir * fDist;
    vPos.z = Q_max(vSmoke.z, origin.z);
    if (!Maneuver_GroundAt(vPos, &vGround)) {
        return false;
    }

    return Maneuver_Start(OPM_MANEUVER_ADVANCE, vGround, 3000 + (int)(fDist * 10));
}

/*
===============
Actor::Maneuver_Think

Runs his move, if he is on one, or decides on one. Returns whether it took
over this think from the attack state.
===============
*/
bool Actor::Maneuver_Think(void)
{
    const char *reason = NULL;

    if (m_eOPMManeuver == OPM_MANEUVER_NONE) {
        s_pszOPMWhyNot = NULL;
        if (!Maneuver_CanStart() || (!Maneuver_DecideToRush() && !Maneuver_DecideToAdvance() && !Maneuver_DecideToFlank())) {
            if (ai_debug && ai_debug->integer >= 2 && s_pszOPMWhyNot && level.inttime >= s_piOPMWhyNotTime[entnum] + 3000) {
                s_piOPMWhyNotTime[entnum] = level.inttime;
                AI_Debug("%s: no move (%s), leash %.0f", TargetName().c_str(), s_pszOPMWhyNot, m_fLeash);
            }
            return false;
        }
    }

    if (!AI_Enhanced(Maneuver_Cvar(m_eOPMManeuver))) {
        reason = "switched off";
    } else if (!m_Enemy || m_Enemy->IsDead()) {
        reason = "no enemy";
    } else if (m_bLockThinkState || m_ThinkState != THINKSTATE_ATTACK) {
        reason = "busy";
    } else if (level.inttime >= m_iOPMManeuverEnd) {
        reason = "out of time or blocked";
    } else if (m_eOPMManeuver == OPM_MANEUVER_RUSH && (m_Enemy->origin - origin).lengthXYSquared() < Square(192)) {
        reason = "close enough";
    } else if (m_bOPMManeuverDirect) {
        if ((m_vOPMManeuverDest - origin).lengthXYSquared() < Square(24)) {
            reason = "there";
        }
    } else {
        // The path may have been taken over meanwhile (dodging a grenade).
        SetPath(m_vOPMManeuverDest, NULL, 0, m_vHome, m_fLeashSquared);
        if (!PathExists()) {
            reason = "lost the path";
        } else if (PathComplete()) {
            reason = "there";
        }
    }

    if (!reason && level.inttime >= m_iOPMManeuverProgressTime + 1000) {
        if (level.inttime > m_iOPMManeuverStart + 1500 && (origin - m_vOPMManeuverLastPos).lengthXYSquared() < Square(12)) {
            reason = "stuck";
        }
        m_vOPMManeuverLastPos      = origin;
        m_iOPMManeuverProgressTime = level.inttime;
    }

    if (reason) {
        Maneuver_End(reason);
        return false;
    }

    const_str csAnim =
        m_eOPMManeuver == OPM_MANEUVER_RUSH ? STRING_ANIM_RUNTO_INOPEN_SCR : STRING_ANIM_RUNTO_DANGER_SCR;

    if (m_bOPMManeuverDirect) {
        SetDest(m_vOPMManeuverDest);
        DesiredAnimation(ANIM_MODE_DEST, csAnim);
    } else {
        DesiredAnimation(ANIM_MODE_PATH_GOAL, csAnim);
    }
    FaceMotion();

    m_pszDebugState = Maneuver_Name(m_eOPMManeuver);
    return true;
}
