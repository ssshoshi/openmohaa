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

// actor_gaze.cpp: Added in OPM
//
// An actor at ease (idle, patrolling, playing a scripted animation) glances
// at whoever of its own side is near: the player most of all, more so when
// the player is looking at it, and other soldiers, more so when they are
// already looking back. A glance lasts a few seconds and is followed by a
// look away, so nobody stares. The head turn and the eyes come from the
// desired look direction (UpdateBoneControllers, and the skeletor's eye
// morphs for what the head turn leaves): the eyes lead and the head follows.
// Script look targets, enemies and combat states win over all of it, since
// they never get here (only IdleLook calls it).

#include "actor.h"
#include "player.h"

static const float GAZE_PLAYER_RANGE = 400.f;
static const float GAZE_ACTOR_RANGE  = 256.f;
static const float GAZE_MIN_RANGE    = 24.f;
// how far round from where the body faces: head 60 + torso 30 + eyes
static const float GAZE_MAX_YAW        = 105.f;
static const float GAZE_MAX_YAW_MOVING = 70.f;
static const float GAZE_MAX_PITCH      = 50.f;

static inline float GazeRand(float lo, float hi)
{
    return lo + (hi - lo) * G_Random();
}

static bool GazePlayerLooksAt(Player *player, const Vector& eye)
{
    Vector fwd;
    Vector delta = eye - player->EyePosition();

    if (delta.normalize() < 1) {
        return false;
    }

    player->GetVAngles().AngleVectorsLeft(&fwd, NULL, NULL);
    // about 15 degrees either side of the crosshair
    return DotProduct(fwd, delta) > 0.965f;
}

void Actor::ClearGaze(void)
{
    m_pGazeTarget = NULL;
}

/*
===============
Actor::GazeCandidate

Whether another can be glanced at from here, and how much it draws the eye.
===============
*/
bool Actor::GazeCandidate(Sentient *pOther, bool bMoving, float *pScore)
{
    Vector myEye, delta, dir;
    float  dist, range, yaw, pitch;
    float  score;
    bool   isPlayer;

    if (!pOther || pOther == this || pOther->IsDead() || pOther->hidden() || !pOther->edict) {
        return false;
    }

    isPlayer = pOther->IsSubclassOfPlayer();
    range    = isPlayer ? GAZE_PLAYER_RANGE : GAZE_ACTOR_RANGE;

    myEye = EyePosition();
    delta = pOther->EyePosition() - myEye;
    dist  = delta.length();

    if (dist > range || dist < GAZE_MIN_RANGE) {
        return false;
    }

    dir   = delta.toAngles();
    yaw   = fabs(AngleNormalize180(dir[YAW] - angles[YAW]));
    pitch = fabs(AngleNormalize180(dir[PITCH]));

    if (yaw > (bMoving ? GAZE_MAX_YAW_MOVING : GAZE_MAX_YAW) || pitch > GAZE_MAX_PITCH) {
        return false;
    }

    if (!pScore) {
        // only checking a current target: keep it a little past the range
        // it was picked at so it does not flicker at the edge
        return G_SightTrace(
            myEye, vec_zero, vec_zero, pOther->EyePosition(), this, pOther, MASK_CANSEE_NOENTS, qfalse, "Actor::Gaze"
        );
    }

    // nearer and more in front draw the eye more
    score = 1.f - dist / range;
    score *= 1.f - yaw / 240.f;

    if (isPlayer) {
        score = score * 1.5f + 0.15f;
        if (GazePlayerLooksAt(static_cast<Player *>(pOther), myEye)) {
            // being looked at is noticed
            score += 1.2f;
        }
    } else if (pOther->IsSubclassOfActor() && static_cast<Actor *>(pOther)->m_pGazeTarget == this) {
        // look back at who is looking at you
        score += 0.8f;
    }

    if (pOther->velocity.lengthXY() > 60) {
        // movement catches the eye
        score += 0.25f;
    }

    if (score <= 0) {
        return false;
    }

    if (!G_SightTrace(
            myEye, vec_zero, vec_zero, pOther->EyePosition(), this, pOther, MASK_CANSEE_NOENTS, qfalse, "Actor::Gaze"
        )) {
        return false;
    }

    *pScore = score;
    return true;
}

/*
===============
Actor::AmbientLook

Look at someone near, if anyone; false leaves the look to the caller.
===============
*/
bool Actor::AmbientLook(bool bMoving, bool bSubtle)
{
    if (!g_actorGaze->integer || IsDead() || m_Enemy) {
        ClearGaze();
        return false;
    }

    Sentient *target = m_pGazeTarget;

    // keep checking the one being looked at, a few times a second
    if (target && level.time >= m_fGazeNextCheck) {
        m_fGazeNextCheck = level.time + GazeRand(0.25f, 0.45f);

        if (!GazeCandidate(target, bMoving, NULL)) {
            ClearGaze();
            target          = NULL;
            m_fGazeNextPick = level.time + GazeRand(0.8f, 2.5f);
        }
    }

    if (target && level.time >= m_fGazeUntil) {
        bool held = false;

        if (target->IsSubclassOfPlayer() && GazePlayerLooksAt(static_cast<Player *>(target), EyePosition())) {
            // eye contact holds, for a while
            held = G_Random() < 0.55f;
        } else if (target->IsSubclassOfActor() && static_cast<Actor *>(target)->m_pGazeTarget == this) {
            held = G_Random() < 0.3f;
        }

        if (held) {
            m_fGazeUntil = level.time + GazeRand(1.0f, 2.5f);
        } else {
            // look away for a bit
            ClearGaze();
            target          = NULL;
            m_fGazeNextPick = level.time + GazeRand(1.5f, 5.0f);
        }
    }

    if (!target && level.time >= m_fGazeNextPick) {
        Sentient *candidates[16];
        float     scores[16];
        int       numCandidates = 0;
        float     total;
        float     none = 0.7f;

        if (m_Team >= 0 && m_Team < MAX_HEAD_SENTIENTS) {
            for (Sentient *pSent = level.m_HeadSentient[m_Team]; pSent && numCandidates < 16;
                 pSent           = pSent->m_NextSentient) {
                float score;

                if (!GazeCandidate(pSent, bMoving, &score)) {
                    continue;
                }

                if (pSent->IsSubclassOfPlayer() && score > 1.2f) {
                    // the player looking at you is hard to ignore
                    none = 0.15f;
                }

                candidates[numCandidates] = pSent;
                scores[numCandidates]     = score;
                numCandidates++;
            }
        }

        total = none;
        for (int i = 0; i < numCandidates; i++) {
            total += scores[i];
        }

        float pick = G_Random() * total;
        for (int i = 0; i < numCandidates; i++) {
            pick -= scores[i];
            if (pick < 0) {
                target = candidates[i];
                break;
            }
        }

        if (target) {
            m_pGazeTarget    = target;
            m_fGazeNextCheck = level.time + GazeRand(0.25f, 0.45f);
            m_fGazeUntil     = level.time
                         + (target->IsSubclassOfPlayer() ? GazeRand(2.5f, 6.0f) : GazeRand(1.5f, 4.5f));
        } else {
            // nobody, or not now: a moment before looking around again
            m_fGazeNextPick = level.time + GazeRand(0.6f, 1.6f);
        }
    }

    if (!target) {
        return false;
    }

    SetDesiredLookDir(target->EyePosition() - EyePosition());
    m_bGazeSubtle = bSubtle || bMoving;
    m_bGazeActive = true;
    return true;
}
