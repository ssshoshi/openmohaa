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
// The sound of a body striking something, shared by the client (corpses,
// props, furniture) and the server (entity props). See phys_impact.cpp.

#pragma once

#include "phys_jolt.h"

#include <map>
#include <string>
#include <vector>

// What a moving thing is made of, as far as its sound goes.
typedef enum {
    PHYS_SND_DEFAULT,
    PHYS_SND_WOOD,
    PHYS_SND_METAL,
    PHYS_SND_GLASS,
    PHYS_SND_PAPER,
    PHYS_SND_STONE,
    PHYS_SND_FLESH,
    PHYS_SND_WEAPON,
    PHYS_SND_HELMET,
    PHYS_SND_COUNT
} physSoundMat_t;

const char *Phys_SoundMatName(physSoundMat_t mat);

// The surface a thing landed on, named as the retail body fall aliases name it
// (snd_bodyfall_<name>): from a trace's surface flags, or "wade"/"puddle" in
// water, as CG_BodyFallSound has them. A thing struck stands for a surface of
// what it is made of.
const char *Phys_SurfaceName(int surfaceFlags);
const char *Phys_SurfaceOfMat(physSoundMat_t mat);
// Ground that swallows a knock: earth, grass, sand, snow, carpet.
bool        Phys_SurfaceIsSoft(const char *surface);

// A contact as the listener sees it, before anything is decided about it.
typedef struct {
    JPH::uint64 data[2]; // each body's user data
    bool        dynamic[2];
    float       mass[2]; // kilograms, 0 for what does not move
    JPH::RVec3  point;   // metres
    JPH::Vec3   normal;  // from the first body to the second
    float       closing; // metres a second, along the normal
    float       approach[2]; // each one's own speed toward the other
    float       sliding;     // metres a second, across the surface
    bool        sensor;
} physContact_t;

// The approach speed of two bodies at the first point of a manifold, along its
// normal: what turns into sound. Positive while they close.
physContact_t Phys_ContactFrom(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold);

// An impact worth a sound, with the hitter (the one that moved into the other)
// first.
typedef struct {
    JPH::uint64    hitter, other; // user data
    physSoundMat_t hitterMat;
    float          mass;  // the hitter's, kilograms
    float          speed; // metres a second
    vec3_t         point; // game units
    vec3_t         dir;   // from the hitter into what it struck
    float          strength; // 0..1, how hard, from its speed and weight
} physImpact_t;

// How hard an impact is for its speed and the hitter's weight, 0..1; 0 for one
// too slow to be heard.
float Phys_ImpactStrength(float speed, float mass);

// The alias for an impact of mat on surface (a Phys_SurfaceName), and the
// volume and pitch to play it at.
std::string Phys_ImpactAlias(physSoundMat_t mat, const char *surface, float mass, float strength, float *volume, float *pitch);

//=============================================================
// Scraping
//=============================================================

// A body sliding along something, as long as it slides: a loop that follows
// it, as loud as it is fast and heavy.
typedef struct {
    JPH::uint64    source;
    JPH::uint64    hitter, other; // user data, the slider first
    float          mass;
    float          speed;  // metres a second, this frame's fastest
    vec3_t         point;  // game units
    vec3_t         dir;    // from the slider into what it slides on
    float          level;  // 0..1, eased toward how hard it scrapes now
    physSoundMat_t mat;    // the caller's, once it is known
    bool           matKnown;
    std::string    surface; // the caller's, looked again now and then
    int            surfaceTime;
} physScrape_t;

// The alias for mat sliding on surface, and its volume and pitch at level.
// Empty when it makes no such sound (glass, or anything in water).
std::string Phys_ScrapeAlias(physSoundMat_t mat, const char *surface, float level, float *volume, float *pitch);

class PhysScrapeTracker
{
public:
    PhysScrapeTracker();

    // A contact sliding fast enough, from the listener; the fastest of a
    // source's contacts over the frame is kept.
    void Note(const physContact_t& c, int slider, JPH::uint64 source, float mass);

    // Advances each scrape's level to now; out has the loudest few still to be
    // heard, ended has the sources that were and are not any more.
    void Update(int now, std::vector<physScrape_t *> *out, std::vector<JPH::uint64> *ended);

    void Clear();

    int maxHeard; // scrapes heard at once at most

private:
    struct Noted {
        physContact_t contact;
        int           slider;
        float         mass;
    };

    std::map<JPH::uint64, Noted>        noted;
    std::map<JPH::uint64, physScrape_t> scrapes;
    std::vector<JPH::uint64>            heard;
    int                                 lastTime;
};

// Keeps the sounds few: each source no more often than its interval, and no
// more than a handful a frame, the loudest kept. Its clock is the caller's, in
// milliseconds.
class PhysImpactLimiter
{
public:
    PhysImpactLimiter();

    // Nothing heard until this time: a world being made, or a game restored,
    // whose bodies drop the last fraction of a unit onto what holds them.
    void Quiet(int untilTime);

    // Offered over a step; source groups the bodies that are one thing (the
    // parts of a corpse). Within a source's interval only a knock twice as hard
    // as the last is heard: a corpse's trunk landing just after a foot.
    void Offer(const physImpact_t& impact, JPH::uint64 source);

    // What is to be heard of what was offered since the last call, loudest
    // first; the rest is forgotten. rejected says how many were left out.
    void Take(int now, std::vector<physImpact_t> *out, int *rejected);

    void Clear();

    int interval; // milliseconds between two sounds of one source
    int perFrame; // sounds a frame at most

private:
    struct Offered {
        physImpact_t impact;
        JPH::uint64  source;
    };

    std::vector<Offered>         offered;
    struct Heard {
        int   time;
        float strength;
    };

    std::map<JPH::uint64, Heard> last;
    int                          quietUntil;
};
