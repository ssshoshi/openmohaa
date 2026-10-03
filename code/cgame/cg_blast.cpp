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

// cg_blast.cpp -- what a nearby explosion does to the player's senses
//
// Added in OPM. An explosion effect (models/fx/*, the opm-explosions data)
// calls "opm_blast <size>" from its TIKI, size 1 for a grenade up to 3 for an
// airstrike. Near enough, the view shakes (cg_blastshake), and close up the
// ears ring (cg_blastring): a high tone, with the rest of the game's sound
// pushed down and brought back up over a few seconds, as in Half-Life 2. The
// push back on the body is the server's (RadiusDamage, g_blastpush).
//
// The volume is lowered through s_volume, which is archived: the player's own
// setting is kept in cg_blastvolume until the ringing is over, and put back
// from there if the game was left while the ears were still ringing.

#include "cg_local.h"

static cvar_t *cg_blastshake;
static cvar_t *cg_blastring;
static cvar_t *cg_blastvolume;

// The shake, in degrees, as it dies away.
static float cg_shake;

// The ringing: from ringStart to ringEnd, deepest at ringDepth (0 to 1).
static int   cg_ringStart;
static int   cg_ringEnd;
static float cg_ringDepth;

#define BLAST_REACH      700.0f // how far a size 1 blast is felt
#define BLAST_SHAKE      5.0f   // degrees of shake from a size 1 blast at the centre
#define BLAST_SHAKE_MAX  14.0f
#define BLAST_RING_NEAR  0.4f   // the closeness past which the ears ring
#define BLAST_RING_SOUND "sound/opm/ear_ring.wav"

extern "C" void CG_BlastInit(void)
{
    cg_blastshake  = cgi.Cvar_Get("cg_blastshake", "1", CVAR_ARCHIVE);
    cg_blastring   = cgi.Cvar_Get("cg_blastring", "1", CVAR_ARCHIVE);
    cg_blastvolume = cgi.Cvar_Get("cg_blastvolume", "", 0);

    // Left while ringing: the volume was still down.
    if (cg_blastvolume->string[0]) {
        cgi.Cvar_Set("s_volume", cg_blastvolume->string);
        cgi.Cvar_Set("cg_blastvolume", "");
    }

    cg_shake     = 0;
    cg_ringStart = cg_ringEnd = 0;
}

static void CG_BlastRingStop(void)
{
    if (cg_blastvolume && cg_blastvolume->string[0]) {
        cgi.Cvar_Set("s_volume", cg_blastvolume->string);
        cgi.Cvar_Set("cg_blastvolume", "");
    }
    cg_ringStart = cg_ringEnd = 0;
}

extern "C" void CG_BlastShutdown(void)
{
    CG_BlastRingStop();
}

extern "C" void CG_BlastFeel(const vec3_t origin, float size)
{
    float reach, dist, near;

    if (!cg_blastshake || size <= 0) {
        return;
    }
    if (cg.snap && cg.snap->ps.stats[STAT_HEALTH] <= 0) {
        return;
    }

    reach = BLAST_REACH * size;
    dist  = Distance(origin, cg.refdef.vieworg);
    if (dist >= reach) {
        return;
    }

    near = 1.0f - dist / reach;
    near *= near;

    if (cg_blastshake->value > 0) {
        cg_shake = Q_max(cg_shake, Q_min(BLAST_SHAKE_MAX, BLAST_SHAKE * size * near * cg_blastshake->value));
    }

    if (cg_blastring->integer && near > BLAST_RING_NEAR) {
        float depth = (near - BLAST_RING_NEAR) / (1.0f - BLAST_RING_NEAR);
        int   length = 1500 + (int)(3500 * depth * Q_min(size, 2.0f) / 2.0f);

        if (!cg_ringEnd) {
            cgi.Cvar_Set("cg_blastvolume", cgi.Cvar_Get("s_volume", "0.9", CVAR_ARCHIVE)->string);
            cg_ringDepth = 0;
        }
        if (cg.time + length > cg_ringEnd) {
            cg_ringStart = cg.time;
            cg_ringEnd   = cg.time + length;
        }
        cg_ringDepth = Q_max(cg_ringDepth, depth);

        cgi.S_StartLocalSound(BLAST_RING_SOUND, qtrue);
    }
}

// Once a frame, with the view's angles.
extern "C" void CG_BlastUpdate(vec3_t angles)
{
    if (cg_shake > 0) {
        angles[PITCH] += crandom() * cg_shake;
        angles[YAW] += crandom() * cg_shake;
        angles[ROLL] += crandom() * cg_shake * 0.5f;

        // most of it gone in half a second
        cg_shake *= exp(-6.0f * cg.frametime / 1000.0f);
        if (cg_shake < 0.02f) {
            cg_shake = 0;
        }
    }

    if (cg_ringEnd) {
        float saved, frac, muffle;

        if (cg.time >= cg_ringEnd || cg.time < cg_ringStart || !cg_blastvolume->string[0]) {
            CG_BlastRingStop();
            return;
        }

        // down at once, then back up over the last two thirds
        frac   = (float)(cg.time - cg_ringStart) / (float)(cg_ringEnd - cg_ringStart);
        muffle = frac < 0.33f ? 1.0f : 1.0f - (frac - 0.33f) / 0.67f;
        saved  = atof(cg_blastvolume->string);
        cgi.Cvar_Set("s_volume", va("%.3f", saved * (1.0f - 0.75f * cg_ringDepth * muffle)));
    }
}
