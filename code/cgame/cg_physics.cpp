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
// Client-side rigid body physics, on Jolt.
//
// One Jolt world for the client. Everything inside it is in metres (Jolt's
// tuning assumes metre scale objects); game units are converted at the edge.

#include "cg_physics_local.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

cvar_t *cg_physics;
cvar_t *cg_physics_log;
cvar_t *cg_physics_debug;
cvar_t *cg_physics_props;
cvar_t *cg_physics_clipped;

// The simulation's clock: a fixed step, as the ragdolls use, with a few steps
// of catching up at most.
#define PHYS_STEP_HZ    60
#define PHYS_MAX_STEPS  4

static int   phys_lastTime;
static float phys_accum;

// For cg_physics_log: what the simulation costs, reported every few seconds.
static double phys_stepUs, phys_worstStepUs;
static int    phys_steps, phys_reportTime;

//=============================================================
// The world
//=============================================================

#define PHYS_MAX_BODIES 16384

static physWorld_t          phys_world;
static JPH::TempAllocatorImpl       *phys_temp;
static JPH::JobSystemSingleThreaded *phys_jobs;
JPH::PhysicsSystem                  *phys_system;

static void CG_PhysicsTrace(const char *fmt, ...)
{
    char    text[1024];
    va_list ap;

    va_start(ap, fmt);
    Q_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    cgi.DPrintf("physics: %s\n", text);
}

void CG_InitPhysics(void)
{
    cg_physics     = cgi.Cvar_Get("cg_physics", "1", CVAR_ARCHIVE);
    cg_physics_log   = cgi.Cvar_Get("cg_physics_log", "0", CVAR_ARCHIVE);
    cg_physics_debug = cgi.Cvar_Get("cg_physics_debug", "0", 0);
    cg_physics_props = cgi.Cvar_Get("cg_physics_props", "1", CVAR_ARCHIVE | CVAR_LATCH);
    // Props the map wraps in invisible clip brushes (most furniture), on a
    // server elsewhere: 0 keeps them fixed, 1 moves them anyway, leaving the
    // clip brush where the prop was for the server to go on colliding players
    // and bullets with. In a local single player game the clip brushes are
    // removed instead and they always move; see CG_PhysicsCanRemoveStandIns.
    cg_physics_clipped = cgi.Cvar_Get("cg_physics_clipped", "0", CVAR_ARCHIVE | CVAR_LATCH);
    cg_physics_furniture = cgi.Cvar_Get("cg_physics_furniture", "1", CVAR_ARCHIVE | CVAR_LATCH);
    CG_PhysicsEditInit();
    CG_PhysicsSoundsInit();
    phys_lastTime    = 0;
    phys_accum       = 0.0f;

    Phys_RegisterJolt(CG_PhysicsTrace);
    Phys_CreateWorld(&phys_world, PHYS_MAX_BODIES);
    phys_system = phys_world.system;
    CG_PhysicsListenForContacts();
    phys_temp   = phys_world.temp;
    phys_jobs   = phys_world.jobs;

    if (cg_physics_log->integer) {
        cgi.Printf(
            "physics: Jolt %d.%d.%d ready (%d bodies at most)\n",
            JPH_VERSION_MAJOR,
            JPH_VERSION_MINOR,
            JPH_VERSION_PATCH,
            PHYS_MAX_BODIES
        );
    }
}

// Whether the clip brushes that stand in for a prop can be taken away once it
// moves: only when the server runs in this process, so shares the collision
// model with the client, and nobody else is playing on it. On any other server
// they would be gone on this side only, and the client would predict its
// player walking through a chair the server still stops him at.
qboolean CG_PhysicsCanRemoveStandIns(void)
{
    return (cgi.apiversion >= 5 && cgi.CM_DisableBrush && cgs.localServer && cgs.gametype == GT_SINGLE_PLAYER)
             ? qtrue
             : qfalse;
}

// Whether the map's props and furniture move at all: in single player only. In
// multiplayer each client would knock them about its own way, and the ragdolls
// are the only physics there; the props stay, fixed, for them to lie on.
qboolean CG_PhysicsPropsMove(void)
{
    return cgs.gametype == GT_SINGLE_PLAYER ? qtrue : qfalse;
}

// Whether props wrapped in clip brushes move at all.
qboolean CG_PhysicsClippedPropsMove(void)
{
    return (CG_PhysicsPropsMove() && (CG_PhysicsCanRemoveStandIns() || cg_physics_clipped->integer)) ? qtrue : qfalse;
}

void CG_ShutdownPhysics(void)
{
    CG_PhysicsGrabRelease();
    CG_JoltRagdollsUnload();
    CG_PhysicsUnloadMovers();
    CG_PhysicsUnloadProps();
    CG_PhysicsUnloadWorld();
    Phys_DestroyWorld(&phys_world);
    phys_system = NULL;
    phys_jobs   = NULL;
    phys_temp   = NULL;
}

void CG_PhysicsFrame(void)
{
    if (!phys_system || !cg_physics->integer || !cg.snap) {
        // The particle solver's corpses are heard without the physics.
        CG_PhysicsPlayImpacts();
        return;
    }

    CG_PhysicsLoadWorld();
    CG_PhysicsLoadProps();

    // The same gravity the player falls by, as the ragdolls use.
    {
        const float g = cg.snap->ps.gravity ? (float)cg.snap->ps.gravity : 800.0f;
        phys_system->SetGravity(JPH::Vec3(0.0f, 0.0f, -g * PHYS_UNITS_TO_METRES));
    }

    {
        const float dt    = 1.0f / PHYS_STEP_HZ;
        int         steps = 0;

        if (!phys_lastTime || cg.time < phys_lastTime || cg.time - phys_lastTime > 1000) {
            phys_lastTime = cg.time;
            phys_accum    = 0.0f;
            // A game restored, or a long stall: what lies about settles again.
            CG_PhysicsSoundsQuiet();
        }

        phys_accum += (cg.time - phys_lastTime) * 0.001f;
        phys_lastTime = cg.time;
        phys_accum    = Q_min(phys_accum, dt * PHYS_MAX_STEPS);

        // The people in the world, and the player's pushing.
        CG_PhysicsFollowMovers();
        CG_PhysicsPlayerPushes();

        const int total = Q_min((int)(phys_accum / dt), PHYS_MAX_STEPS);
        if (!total) {
            CG_PhysicsMoversHeld();
        }

        while (steps < total) {
            const auto start = std::chrono::steady_clock::now();

            CG_PhysicsMoveMovers((float)(steps + 1) / total, dt);
            CG_PhysicsGrabStep(dt);
            CG_JoltRagdollsStep(dt);
            phys_system->Update(dt, 1, phys_temp, phys_jobs);
            CG_PhysicsPropsStepped();
            CG_PhysicsFurnitureStepped();

            {
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                phys_stepUs += us;
                phys_worstStepUs = us > phys_worstStepUs ? us : phys_worstStepUs;
                phys_steps++;
            }

            phys_accum -= dt;
            steps++;
        }

        if (cg_physics_log->integer && cg.time - phys_reportTime >= 5000) {
            if (phys_steps) {
                cgi.Printf(
                    "physics: %d steps in the last %.1f s, %.0f us each, worst %.0f us; %d bodies awake\n",
                    phys_steps,
                    (cg.time - phys_reportTime) * 0.001f,
                    phys_stepUs / phys_steps,
                    phys_worstStepUs,
                    (int)phys_system->GetNumActiveBodies(JPH::EBodyType::RigidBody)
                );
            }
            phys_reportTime   = cg.time;
            phys_stepUs       = 0.0;
            phys_worstStepUs  = 0.0;
            phys_steps        = 0;
        }

        CG_PhysicsSendNudges();
        CG_PhysicsPlayImpacts();
        CG_PhysicsPlayScrapes();
        CG_PhysicsDrawProps(phys_accum / dt);
        CG_PhysicsDrawFurniture(phys_accum / dt);
    }

    if (cg_physics_debug->integer == 1) {
        CG_PhysicsDrawWorld();
    }

    CG_PhysicsEditFrame();
}

// phys_selftest [rays]: casts random rays from the eye through Jolt's copy of
// the map and through the engine's own collision, and says how often they
// disagree. The two are built from the same brushes, patches and terrain, so
// they should agree to within the rounding of the hulls' corners; player clip
// is in neither.
void CG_PhysicsSelftest_f(void)
{
    const int   rays   = cgi.Argc() > 1 ? atoi(cgi.Argv(1)) : 2000;
    const float length = 512.0f;
    int         bothHit = 0, bothMiss = 0, joltOnly = 0, traceOnly = 0, far = 0, fence = 0;
    float       sumDiff = 0.0f, worst = 0.0f;
    int         i;

    if (!phys_system) {
        cgi.Printf("phys_selftest: physics is not running\n");
        return;
    }

    CG_PhysicsLoadWorld();

    for (i = 0; i < rays; i++) {
        vec3_t         dir, start, end;
        trace_t        tr;
        JPH::RayCastResult hit;
        qboolean       joltHit;

        do {
            dir[0] = crandom();
            dir[1] = crandom();
            dir[2] = crandom();
        } while (VectorNormalize(dir) < 0.1f);

        VectorCopy(cg.refdef.vieworg, start);
        VectorMA(start, length, dir, end);

        CG_Trace(&tr, start, vec3_origin, vec3_origin, end, ENTITYNUM_NONE, CONTENTS_SOLID | CONTENTS_FENCE, qfalse, qfalse, "phys_selftest");

        {
            JPH::RRayCast ray(JPH::RVec3(PhysToJolt(start)), PhysToJolt(end) - PhysToJolt(start));
            joltHit = phys_system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, PhysNoKinematicObjects()) ? qtrue : qfalse;
        }

        if (tr.startsolid) {
            continue;
        }

        // A grate: see CG_PhysicsLoadWorld.
        if (joltHit && phys_system->GetBodyInterface().GetUserData(hit.mBodyID) == PHYS_USERDATA_FENCE
            && (tr.fraction >= 1.0f || tr.fraction > hit.mFraction)) {
            fence++;
            continue;
        }

        if (cgi.Argc() > 2 && (joltHit != (tr.fraction < 1.0f) || (joltHit && fabsf(hit.mFraction - tr.fraction) * length > 4.0f))
            && joltOnly + traceOnly + far < atoi(cgi.Argv(2))) {
            cgi.Printf(
                "  ray %d from %.0f %.0f %.0f dir %.2f %.2f %.2f: engine %.1f (contents 0x%x surf 0x%x normal %.2f %.2f %.2f at %.0f %.0f %.0f), jolt %s %.1f\n",
                i, start[0], start[1], start[2], dir[0], dir[1], dir[2],
                tr.fraction * length, tr.contents, tr.surfaceFlags, tr.plane.normal[0], tr.plane.normal[1], tr.plane.normal[2],
                tr.endpos[0], tr.endpos[1], tr.endpos[2], joltHit ? "hit" : "miss", joltHit ? hit.mFraction * length : 0.0f
            );
        }

        if (joltHit && tr.fraction < 1.0f) {
            const float diff = fabsf(hit.mFraction - tr.fraction) * length;

            bothHit++;
            sumDiff += diff;
            if (diff > worst) {
                worst = diff;
            }
            if (diff > 4.0f) {
                far++;
            }
        } else if (!joltHit && tr.fraction >= 1.0f) {
            bothMiss++;
        } else if (joltHit) {
            joltOnly++;
        } else {
            traceOnly++;
        }
    }

    cgi.Printf(
        "phys_selftest: %d rays: %d both hit (%d more than 4 units apart, mean %.2f, worst %.1f), %d both miss, "
        "%d only Jolt hit, %d only the engine hit, %d through a grate the engine sees through: %.1f%% disagree\n",
        rays,
        bothHit,
        far,
        bothHit ? sumDiff / bothHit : 0.0f,
        worst,
        bothMiss,
        joltOnly,
        traceOnly,
        fence,
        rays ? 100.0f * (joltOnly + traceOnly + far) / rays : 0.0f
    );
}
