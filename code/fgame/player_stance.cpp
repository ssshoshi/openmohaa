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

// player_stance.cpp: lying prone, diving and sprinting, in single player
//
// Added in OPM. The player state machine (global/mike_legs.st) knows standing
// and crouching only, so prone lives beside it: while lying down the heights and
// position flags its states set are held at prone (ModifyHeight,
// SetMovePosFlags), the legs play the prone animations in place of what the
// states ask for (ProneLegAnim) and getting up puts the legs back in STAND or
// CROUCH_IDLE.
//
//   prone      toggles lying down. Moving at a run it is a dive, farther from
//              a sprint; jump gets up, crouch goes to a crouch.
//   +sprint    runs faster while moving forward, weapon lowered; firing or
//              aiming stops it.
//

#include "player.h"
#include "earthquake.h"
#include "g_phys.h"

bool Player::IsProne() const
{
    return m_bProne;
}

void Player::EventProne(Event *ev)
{
    m_bProneRequest = true;
}

bool Player::StanceAllowed() const
{
    if (g_gametype->integer != GT_SINGLE_PLAYER) {
        return false;
    }

    if (IsDead() || m_pTurret || m_pVehicle || camera || m_pGlueMaster) {
        return false;
    }

    if (movetype != MOVETYPE_WALK || level.playerfrozen || m_bFrozen || (flags & (FL_IMMOBILE | FL_PARTIAL_IMMOBILE))) {
        return false;
    }

    if (movecontrol != MOVECONTROL_USER && movecontrol != MOVECONTROL_LEGS && movecontrol != MOVECONTROL_USER_MOVEANIM) {
        // ladders, use animations and scripted moves
        return false;
    }

    return waterlevel < 2;
}

bool Player::HasRoomFor(float height)
{
    Vector  newmaxs;
    trace_t trace;

    if (maxs[2] >= height) {
        return true;
    }

    newmaxs    = maxs;
    newmaxs[2] = height;

    trace = G_Trace(origin, mins, newmaxs, origin, edict, MASK_PLAYERSOLID, true, "Player::HasRoomFor");
    return !trace.startsolid;
}

void Player::SetProneHeight()
{
    viewheight = PRONE_VIEWHEIGHT;
    maxs.z     = PRONE_MAXS_Z;
}

void Player::StartProne(usercmd_t *ucmd)
{
    Vector hvel(velocity.x, velocity.y, 0);
    float  speed = hvel.length();
    float  run   = sv_runspeed->value;

    m_bProne      = true;
    m_bHoldUpmove = ucmd->upmove != 0;

    if (speed > run * 0.6f && (ucmd->buttons & BUTTON_RUN)) {
        // a dive: about twice as far from a sprint as from a run
        bool sprint = m_bSprinting || speed > run * 1.15f;

        hvel *= (sprint ? g_dive_sprint->value : g_dive_run->value) / speed;

        velocity   = hvel;
        // Pmove keeps the player on the ground at 150 or less (PM_GroundTrace)
        velocity.z = Q_max(sprint ? g_dive_sprint_up->value : g_dive_run_up->value, 155.0f);

        // make sure the player leaves the ground (as Player::Jump)
        client->ps.walking = qfalse;

        m_bDiving   = true;
        m_fDiveTime = level.time;

        // the body goes down through the air, flat on landing
        viewheight = CROUCH_VIEWHEIGHT;
        maxs.z     = CROUCH_MAXS_Z;
    } else {
        SetProneHeight();
    }

    m_bSprinting    = false;
    m_iMovePosFlags = MPF_POSITION_PRONE;

    if (currentState_Legs) {
        str legsAnim = currentState_Legs->getLegAnim(*this, &legs_conditionals);
        if (legsAnim.length() && legsAnim != "none") {
            SetPartAnim(legsAnim.c_str(), legs);
        }
    }
}

bool Player::LeaveProne(bool crouch, bool force)
{
    State *state;

    if (!force) {
        if (!crouch && !HasRoomFor(MAXS_Z)) {
            crouch = true;
        }

        if (crouch && !HasRoomFor(CROUCH_MAXS_Z)) {
            // no room to get up
            return false;
        }
    }

    m_bProne  = false;
    m_bDiving = false;

    if (IsDead()) {
        // the dead hull takes over
        return true;
    }

    if (crouch) {
        viewheight      = CROUCH_VIEWHEIGHT;
        maxs.z          = CROUCH_MAXS_Z;
        m_iMovePosFlags = MPF_POSITION_CROUCHING;
    } else {
        viewheight      = DEFAULT_VIEWHEIGHT;
        maxs.z          = MAXS_Z;
        m_iMovePosFlags = MPF_POSITION_STANDING;
    }

    if (!statemap_Legs || movecontrol != MOVECONTROL_LEGS) {
        return true;
    }

    // put the legs back in the state machine's own idle, which sets the
    // height the way the state files want it
    state = statemap_Legs->FindState(crouch ? "CROUCH_IDLE" : "STAND");
    if (state) {
        str legsAnim;

        if (currentState_Legs) {
            currentState_Legs->ProcessExitCommands(this);
        }

        currentState_Legs = state;
        state->ProcessEntryCommands(this);

        legsAnim = state->getLegAnim(*this, &legs_conditionals);
        if (legsAnim.length() && legsAnim != "none") {
            SetPartAnim(legsAnim.c_str(), legs);
        }
    }

    return true;
}

void Player::UpdateStance(usercmd_t *ucmd)
{
    bool allowed = StanceAllowed();
    bool wantSprint;

    if (m_bProne && (!allowed || !g_prone->integer)) {
        LeaveProne(false, true);
    }

    if (m_bProneRequest) {
        m_bProneRequest = false;

        if (m_bProne) {
            if (!m_bDiving) {
                LeaveProne(false, false);
            }
        } else if (allowed && g_prone->integer && groundentity) {
            StartProne(ucmd);
        }
    }

    // jump or crouch held through getting up does nothing until let go,
    // or the state machine would jump or crouch at once
    if (!ucmd->upmove) {
        m_bHoldUpmove = false;
    }

    if (m_bProne) {
        if (m_bDiving) {
            if (groundentity && level.time - m_fDiveTime > 0.1f) {
                m_bDiving = false;
                SetProneHeight();

                LandingSound(1.0f, 1);
                new ViewJitter(origin, 128, 0.05f, Vector(2.5f, 1.0f, 2.0f), 0, vec_zero, 0);
            }
        } else if (ucmd->upmove && !m_bHoldUpmove) {
            m_bHoldUpmove = true;
            LeaveProne(ucmd->upmove < 0, false);
        }
    }

    if (m_bProne || m_bHoldUpmove) {
        ucmd->upmove = 0;
    }

    if (m_bDiving) {
        // the dive goes where it was launched, steering would only add to it
        ucmd->forwardmove = 0;
        ucmd->rightmove   = 0;
    }

    if (m_bProne) {
        ucmd->buttons &= ~(BUTTON_LEAN_LEFT | BUTTON_LEAN_RIGHT | BUTTON_SPRINT);
    }

    UpdateSprintStamina(ucmd);

    wantSprint = allowed && g_sprint->integer && !m_bProne && !m_bSprintExhausted && (ucmd->buttons & BUTTON_SPRINT)
              && (ucmd->buttons & BUTTON_RUN) && !(ucmd->buttons & (BUTTON_ATTACKLEFT | BUTTON_ATTACKRIGHT | BUTTON_AIM))
              && ucmd->forwardmove > 0 && !(m_iMovePosFlags & MPF_POSITION_CROUCHING) && !IsZoomed();

    // a sprint starts on the ground and carries on through a jump
    m_bSprinting = wantSprint && (groundentity || m_bSprinting);

    if (m_bSprinting) {
        client->ps.pm_flags |= PMF_SPRINTING;
    } else {
        client->ps.pm_flags &= ~PMF_SPRINTING;
    }
}

/*
====================
UpdateSprintStamina

How long the player can sprint depends on his health: g_sprint_time seconds
at full health, less as he is hurt, none at all below g_sprint_minhealth
percent. Run out and he cannot sprint again until half of it has come back
(g_sprint_recover seconds of sprint a second, while not sprinting) and the
sprint key has been let go.
====================
*/
void Player::UpdateSprintStamina(usercmd_t *ucmd)
{
    float dt, healthFrac, endurance;

    dt = Q_clamp_float((ucmd->serverTime - client->ps.commandTime) / 1000.0f, 0.0f, 0.2f);

    healthFrac = max_health > 0 ? health / max_health : 1.0f;
    endurance  = g_sprint_time->value > 0 ? g_sprint_time->value * Q_min(1.0f, healthFrac) : 0;

    if (m_bSprinting) {
        m_fSprintUsed += dt;
    } else {
        m_fSprintUsed = Q_max(0.0f, m_fSprintUsed - dt * g_sprint_recover->value);
    }

    if (healthFrac * 100.0f < g_sprint_minhealth->value) {
        // too badly hurt to sprint at all
        m_bSprintExhausted = true;
    } else if (g_sprint_time->value <= 0) {
        // no limit
        m_bSprintExhausted = false;
        m_fSprintUsed      = 0;
    } else if (m_fSprintUsed >= endurance) {
        m_bSprintExhausted = true;
    } else if (m_bSprintExhausted && m_fSprintUsed <= endurance * 0.5f && !(ucmd->buttons & BUTTON_SPRINT)) {
        // back once half has come back and the key has been let go, so that
        // holding it does not stutter in and out of a sprint
        m_bSprintExhausted = false;
    }
}

float Player::StanceSpeed(float speed) const
{
    if (!groundentity) {
        return speed;
    }

    if (m_bProne) {
        // crawling, slower again when aiming or walking
        speed = sv_runspeed->value * g_prone_speed->value;
        if (!(last_ucmd.buttons & BUTTON_RUN)) {
            speed *= 0.6f;
        }
        return speed;
    }

    if (m_bSprinting) {
        return speed * g_sprint_speed->value;
    }

    return speed;
}

const char *Player::ProneLegAnim(const char *anim) const
{
    const char *name;

    if (!m_bProne || m_bDiving || !anim || !*anim) {
        return anim;
    }

    // the states' anims name their direction (rifle_stand_run_back and so on)
    if (Q_stristr(anim, "back")) {
        name = "opm_prone_back";
    } else if (Q_stristr(anim, "left")) {
        name = "opm_prone_left";
    } else if (Q_stristr(anim, "right")) {
        name = "opm_prone_right";
    } else if (Q_stristr(anim, "run") || Q_stristr(anim, "walk") || Q_stristr(anim, "fwd")) {
        name = "opm_prone_forward";
    } else {
        name = "opm_prone_idle";
    }

    // the prone animations come with the opm-prone pak
    if (gi.Anim_NumForName(edict->tiki, name) < 0) {
        return anim;
    }

    return name;
}
