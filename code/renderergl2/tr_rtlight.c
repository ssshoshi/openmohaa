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

// tr_rtlight.c -- the map's lights, live
//
// Added in OPM.
//
// MOH:AA lights a map once, when it is compiled, into its lightmaps and its
// static models' vertex colours. Nothing that moves afterwards takes part:
// the props the client and server now simulate keep the light they had where
// the map put them, and cast no shadow where they are.
//
// With r_realtimeLighting, the lights the BSP keeps -- its sphere lights, the
// ones the game lights models with, and the worldspawn sun -- light every
// surface per pixel as it is drawn (RtLight in the shaders, tr_glsl.c), with
// shadows:
//
//   1  the lights' direct light is live, and the map's own lighting keeps
//      the rest;
//   2  all of it is live: the map's lighting gives way to its ambient light
//      and the sky's.
//
// A point light's shadow is a cube: six 90 degree faces, each a tile of an
// atlas. What stands still (the world, terrain, and static models where the
// map put them) is drawn into one atlas once and kept; what moves (entities,
// and the static models the client moved) into another every frame, only in
// the faces it is in. The sun keeps its cascades, through the screen shadow the
// depth prepass leaves.

#include "tr_local.h"

cvar_t *r_realtimeLighting;
cvar_t *r_rtScale;
cvar_t *r_rtShadows;
cvar_t *r_rtShadowTile;
cvar_t *r_rtFacesPerFrame;
cvar_t *r_rtMaxLights;
cvar_t *r_rtAmbient;
cvar_t *r_rtDynamicLights;
cvar_t *r_rtShadowCharacters;
cvar_t *r_rtDarkest;
cvar_t *r_rtDlights;
cvar_t *r_rtLightsAtFixtures;
cvar_t *r_rtCapsules;
cvar_t *r_rtParticles;
static cvar_t *r_rtParticleCascade;
cvar_t *r_rtDebug;
cvar_t *r_rtLightLists;
cvar_t *r_rtBatchSplit;
cvar_t *r_rtCutoff;
cvar_t *r_rtSky;
cvar_t *r_rtSpecular;
cvar_t *r_rtShadowSoft;
cvar_t *r_rtSkyLightmaps;
cvar_t *r_rtSkyTile;
cvar_t *r_rtShadowCull;
cvar_t *r_rtMovingLights;
cvar_t *r_rtCasterMinRadius;
cvar_t *r_rtSelfShadowLights;
cvar_t *r_rtLamps;
cvar_t *r_rtWindowLights;
static cvar_t *r_rtBounce;
static cvar_t *r_rtBakedLamps;
static cvar_t *r_rtBakedDebug;
static qboolean rt_bounceCompiled; // r_rtBounce was on when the shaders were built

qboolean R_RtBounceCompiled(void)
{
	return rt_bounceCompiled;
}
static cvar_t *r_rtBounceRays;
static cvar_t *r_rtBounceMsec;

// the lights a surface may take are a bit each of 64 (drawSurf_t.rtMask),
// sent four indexes a vec4 (u_RtList)
typedef char rt_maxLightsFit[(RT_MAX_LIGHTS <= 64 && RT_MAX_LIGHTS % 4 == 0) ? 1 : -1];

// six faces a light: in the atlas of what stands still, whole tiles for the
// first RT_SLOTS_BIG, quarter tiles for the rest; in that of what moves,
// whole tiles
#define RT_SLOTS_SMALL   (((RT_ATLAS_TILES * RT_ATLAS_TILES - RT_SMALL_FIRST) * 4) / 6)
#define RT_SLOTS         (RT_SLOTS_BIG + RT_SLOTS_SMALL)
#define RT_DYNAMIC_SLOTS ((RT_ATLAS_TILES * RT_ATLAS_TILES) / 6)
#define RT_SLOTS_SPARE   8  // whole tiles kept for a light come near (all but these for the nearest)
#define RT_SLOTS_UPGRADE 12 // one in quarter tiles among this many nearest goes to whole tiles
typedef char rt_smallRowFits[(RT_SMALL_FIRST % RT_ATLAS_TILES == 0 && RT_SLOTS >= RT_MAX_LIGHTS) ? 1 : -1];

// A light adds less than this to a texel, in the BSP's raw lightmap units:
// it is out of range.
#define RT_CUTOFF 1.0f

#define RT_SQRT1_2 0.70710678f

// Leaves, two sided and cut out by their alpha: how much of the light on one
// side comes through to the other
#define RT_FOLIAGE_THROUGH 0.6f

// how long a light takes to come in or go out
#define RT_EASE_SECONDS 0.5f

// a lamp's light moved less than this with its lamp stays where it is
#define RT_LIGHT_MOVE_EPSILON 0.25f

// frames a moved light's faces out of the view wait before they are drawn
// again, so that one still moving does not spend the budget every frame on
// faces it will move away from before they are seen
#define RT_LIGHT_SETTLE_FRAMES 3

typedef struct {
	vec3_t   origin;
	vec3_t   color;        // raw lightmap units: intensity * 7500 * colour
	float    range;
	qboolean spot;
	vec3_t   spotDir;
	float    radiusByDist;

	int      staticSlot;   // in the atlas of what stands still, -1 none
	int      staticFaces;  // the faces up to date in it, a bit each
	qboolean staticReady;  // all six drawn since it took the slot: the stale
	                       // ones are still better than none while redrawn
	int      bakedFaces;   // r_realtimeLighting 1: the world as compiled, in the
	                       // same tiles of its own atlas; drawn once
	float    weight;       // eases in and out, so that a light never pops
	int      chosen;       // the frame it was last among the lights chosen
	int      lastUsed;     // frame
	int      source;       // its sphere light (tr.sLights)
	int      mergedInto;   // while loading: the light at its fixture it went into, -1 none
	int      visFrame;     // the frame a leaf the view may see listed it
	int      fixture;      // the static model it is the light of (R_RtLightsAtFixtures), -1 none
	vec3_t   fixtureLocal; // where it is in that model's frame
	vec3_t   spotDirLocal; // a spot light's direction in that model's frame
	int      movedFrame;   // the frame it last moved with its fixture
	qboolean lamp;         // at a lamp's bulbs (R_RtLightsAtLamps): seen by its cluster, not the map's light lists
	qboolean noShadow;     // the map's fill light no lamp could take: soft, and casting no shadow
	vec3_t   pool;         // where it lights most: what its cone, or straight down, meets (R_RtFindPools)
	qboolean seenByView;   // the view sees its bulbs or its pool, as last looked (R_RtViewSees)
	int      seenTested;   // the frame that was looked at, 0 never
	int      entity;       // an entity lamp's (R_RtEntityLamps): its entity, -1 none
	int      entityFrame;  // the frame it was last in the scene
	int      inViewFrame;  // the frame its light was last in the view (R_RtInView)
	int      enteredView;  // the frame it came into the view after being out of it
	qboolean baked;        // its light on the lightmaps is baked (R_RtBakeLamps)
	vec3_t   bakedOrigin;  // where it was baked from
	int      bakedReady;   // the frame its faces of the world alone were all drawn by
	qboolean live;         // its baked light is taken back out, all of it lit live (R_RtLiveIn)
	qboolean liveShadowed; // with the shadow it was baked with
	float    area;         // a lit surface's (a window's): how far across it is, its light
	                       // going out the way spotDir points (R_RtAreaLights)
	int      cluster;      // where it is, for a lamp's; -1 seen from anywhere
	float    flicker;      // a flame's: how much it dips
	float    seed;
} rtLight_t;

static struct {
	qboolean   loaded;
	rtLight_t *lights;
	int        numLights;
	int        numMapLights; // those before the entity lamps' (R_RtEntityLamps)
	int        waiting, easing; // chosen lights this frame whose shadows are not whole, or not yet at full (r_rtDebug)
	qboolean   dlightTaken[MAX_DLIGHTS]; // the game's lights an entity lamp's light stands in for, this frame
	int       *fromSphere; // each sphere light's, -1 none (dark, for models only)
	qboolean   leafLights; // the map lists the lights reaching each leaf
	int        seenCluster;  // the view's cluster the list below is for, -1 none
	int       *seenLights;   // the lights listed for the leaves it can see
	int       *candidateOf;  // each light's place among this frame's candidates, -1 none
	int        numSeen;
	int        staticOwner[RT_SLOTS]; // the light in each slot, -1 none

	vec3_t     ambient;  // raw
	qboolean   sun;
	vec3_t     sunDir;   // towards the sun
	vec3_t     sunColor; // raw

	vec3_t     skyColor;      // raw: what an open floor takes from all of the sky
	vec3_t     skyDirs[RT_SKY_DIRS]; // towards the sky
	int        skyDrawn;      // the directions drawn so far
	image_t   *skyImage;      // what they were drawn into
	float      skyWeight;     // eases in once they all are

	int        frame;
	int        active[RT_MAX_LIGHTS];
	int        numActive;
	int        lastTime;   // msec, for the lights' easing
	vec4_t     packed[RT_MAX_CAPSULES * 2]; // the lights' runs of bodies (R_RtLightCapsules)
	int        particleDraws; // since the last r_rtDebug report: dust and smoke drawn lit
	int        particleVerts;
	int        numPacked;
	int        facesDrawn; // this frame, for r_rtDebug
	int        staticDrawn;
	int        invalidated;
	int        movingDrawn;  // this frame, for r_rtDebug: faces drawn for lights that moved
	int        moverPairs;   // this frame, for r_rtDebug: movers in a dynamic light's range
	int        moverCulled;  // of those, out of its reach or its light lists
} rt;

// Something that casts a shadow, followed from frame to frame: once it has
// stood still a while it is drawn with what stands still, and kept, until it
// moves or goes.
#define RT_SETTLE_MSEC   1000 // still this long: it joins what stands still
#define RT_HANDOVER_MSEC 1500 // and is drawn with what moves this much longer,
                              // while the faces it joined are drawn again

typedef struct {
	qboolean used;
	float    pose[10]; // origin, two axes, and its animation
	vec3_t   centre;
	float    radius;
	int      lastMoved;
	int      joined;   // when it joined what stands still, 0 if it has not
	int      seen;     // frame
} rtTracked_t;

static rtTracked_t rt_entities[MAX_GENTITIES];
static rtTracked_t rt_clientModels[MAX_MOD_KNOWN]; // the client's own, by model
static rtTracked_t *rt_staticModels;
static int          rt_numStaticModels;

// The size of the faces of the world alone (the third atlas): as the
// others with the map's lighting kept, small with all of it live, where they
// are only for the baked lamps (R_RtBakeLamps)
int R_RtBakedTile(void)
{
	return r_realtimeLighting->integer == 2 ? RT_BAKED_TILE : r_rtShadowTile->integer;
}

qboolean R_RtActive(void)
{
	return (r_realtimeLighting && r_realtimeLighting->integer && rt.loaded && tr.rtShadowFbo[0]) ? qtrue : qfalse;
}

// All of the light live (r_realtimeLighting 2): the models too, and the sun.
qboolean R_RtLitLive(void)
{
	return (R_RtActive() && r_realtimeLighting->integer == 2) ? qtrue : qfalse;
}

void R_RtRegister(void)
{
	r_realtimeLighting = ri.Cvar_Get("r_realtimeLighting", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_realtimeLighting, "Lights the map live, per pixel, with shadows: 1 the lights' direct light (the map's lighting keeps the rest), 2 all of it. Needs vid_restart.");
	r_rtScale = ri.Cvar_Get("r_rtScale", "0.7", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtScale, "How bright the realtime lights are against the map's own lighting");
	r_rtShadows = ri.Cvar_Get("r_rtShadows", "1", CVAR_ARCHIVE);
	r_rtShadowTile = ri.Cvar_Get("r_rtShadowTile", "256", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_CheckRange(r_rtShadowTile, 64, 512, qtrue);
	r_rtFacesPerFrame = ri.Cvar_Get("r_rtFacesPerFrame", "12", CVAR_ARCHIVE);
	r_rtMaxLights = ri.Cvar_Get("r_rtMaxLights", "24", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtMaxLights, 0, RT_MAX_LIGHTS, qtrue);
	ri.Cvar_SetDescription(r_rtMaxLights, "How many realtime lights are lit live, with their shadows: the nearest and those the view sees. With r_rtBakedLamps the rest still light the world, baked; these add sharp shadows, shine and the light on people and props");
	r_rtAmbient = ri.Cvar_Get("r_rtAmbient", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtAmbient, "With r_realtimeLighting 2, how much of the map's ambient light stands in for its lightmaps");
	r_rtDynamicLights = ri.Cvar_Get("r_rtDynamicLights", "8", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtDynamicLights, "How many of the nearest realtime lights draw the shadows of what moves");
	r_rtShadowCharacters = ri.Cvar_Get("r_rtShadowCharacters", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtShadowCharacters, "People and bodies cast the nearest realtime lights' shadows (r_rtDynamicLights) as they are, drawn into the shadow faces they are in; the lights past those use capsules (r_rtCapsules)");
	r_rtDarkest = ri.Cvar_Get("r_rtDarkest", "0.35", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtDarkest, "With r_realtimeLighting 1, the most a new shadow may darken the map's own lighting, as a part of it");
	r_rtDlights = ri.Cvar_Get("r_rtDlights", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtDlights, "How bright muzzle flashes, blasts and the game's other passing lights are among the realtime lights (0: as GL2 draws them otherwise)");
	r_rtLightsAtFixtures = ri.Cvar_Get("r_rtLightsAtFixtures", "1", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_rtLightsAtFixtures, "The map's point lights around a lamp model become one, at the lamp");
	r_rtCapsules = ri.Cvar_Get("r_rtCapsules", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtCapsules, "People and bodies cast the realtime lights' shadows as capsules along their bones, for the lights that do not draw them whole (r_rtShadowCharacters, r_rtDynamicLights)");
	r_rtParticles = ri.Cvar_Get("r_rtParticles", "1", 0);
	r_rtParticleCascade = ri.Cvar_Get("r_rtParticleCascade", "1", 0);
	ri.Cvar_SetDescription(r_rtParticleCascade, "Dust, smoke and spray take the sun's shadow from its widest shadow cascade where they are (0: from the screen behind them). Not archived: an A/B switch");
	ri.Cvar_SetDescription(r_rtParticles, "Dust and smoke sprites take the realtime lights at their corners, not per pixel (0: per pixel, which piles of them make very slow). Not archived: an A/B switch");
	r_rtDebug = ri.Cvar_Get("r_rtDebug", "0", 0);
	r_rtCutoff = ri.Cvar_Get("r_rtCutoff", "0.0005", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtCutoff, 0, 0.05, qfalse);
	ri.Cvar_SetDescription(r_rtCutoff, "A realtime light that would add less than this to a pixel (1 is full brightness), shadow or not, is passed over there without its shadow being looked up (0: none passed over)");
	r_rtSelfShadowLights = ri.Cvar_Get("r_rtSelfShadowLights", "4", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtSelfShadowLights, "How many of the nearest realtime lights draw the player's own shadow as it moves (cg_firstPersonShadow)");
	r_rtCasterMinRadius = ri.Cvar_Get("r_rtCasterMinRadius", "6", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtCasterMinRadius, "Models smaller than this (a radius in units) cast no realtime light shadows: spent cases, chips");
	r_rtMovingLights = ri.Cvar_Get("r_rtMovingLights", "4", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtMovingLights, 0, RT_MAX_LIGHTS, qtrue);
	ri.Cvar_SetDescription(r_rtMovingLights, "How many of the nearest lamps moving with their models (a lantern knocked over, a lamp swinging) draw their shadows again every frame as they move; the rest catch up once they stop");
	r_rtShadowCull = ri.Cvar_Get("r_rtShadowCull", "1", 0);
	ri.Cvar_SetDescription(r_rtShadowCull, "What moves is drawn into a realtime light's shadow only within the light's reach (r_rtCutoff) and where the map lists the light as reaching (0: anywhere in its range). Not archived: an A/B switch");
	r_rtLightLists = ri.Cvar_Get("r_rtLightLists", "1", 0);
	ri.Cvar_SetDescription(r_rtLightLists, "Each draw takes only the realtime lights that reach its surfaces' bounds (0: every draw takes them all). Not archived: an A/B switch");
	r_rtBatchSplit = ri.Cvar_Get("r_rtBatchSplit", "1", 0);
	ri.Cvar_SetDescription(r_rtBatchSplit, "A batch of world surfaces is drawn in pieces where the next surface would bring lights the batch does not have, so that a wall is not shaded with the lights of the whole room (0: one batch, all of their lights). Not archived: an A/B switch");
	r_rtSky = ri.Cvar_Get("r_rtSky", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtSky, "With r_realtimeLighting 2, how strong the sky's light is, as a part of what the map was compiled with (0: none, the shade takes the ambient light alone)");
	r_rtSpecular = ri.Cvar_Get("r_rtSpecular", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtSpecular, "With r_realtimeLighting 2, how much the realtime lights and the sun shine on surfaces with specular (0: none)");
	r_rtShadowSoft = ri.Cvar_Get("r_rtShadowSoft", "2", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtShadowSoft, 1, 4, qfalse);
	ri.Cvar_SetDescription(r_rtShadowSoft, "How soft the edges of the realtime lights' shadows are, in half texels of their shadow faces between its taps (1: the hardest)");
	r_rtSkyLightmaps = ri.Cvar_Get("r_rtSkyLightmaps", "1", 0);
	ri.Cvar_SetDescription(r_rtSkyLightmaps, "Lightmapped surfaces read the sky they see from lightmaps of it, worked out once a map (0: worked out at each pixel). Not archived: an A/B switch");
	r_rtLamps = ri.Cvar_Get("r_rtLamps", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtLamps, "With r_realtimeLighting 2, the light comes from the lamps' bulbs (lights/lamps.txt): the map's lights in a room go to a lamp there, a lamp with none gets its own, and those no lamp sees cast no shadow. Applies when a map is loaded");
	r_rtBounce = ri.Cvar_Get("r_rtBounce", "3", CVAR_ARCHIVE);
	rt_bounceCompiled = r_rtBounce->value > 0;
	ri.Cvar_SetDescription(r_rtBounce, "With r_realtimeLighting 2, how strong the light the sun, the sky and the lamps put on what they reach is where it bounces onto the rest, worked out once a map (rtbounce/<map>.bnc): 1 as it would be, more for the eye that grows used to a room; where the sky is seen its fill stands in for it (0: none, and from 0 or to it needs vid_restart: the shaders are built with it or without)");
	r_rtBounceRays = ri.Cvar_Get("r_rtBounceRays", "32", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtBounceRays, 4, 128, qtrue);
	ri.Cvar_SetDescription(r_rtBounceRays, "The rays each texel of the bounce gathers its light by: more is smoother and slower to work out. Applies when a map is loaded");
	r_rtBounceMsec = ri.Cvar_Get("r_rtBounceMsec", "6", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtBounceMsec, 1, 1000, qtrue);
	ri.Cvar_SetDescription(r_rtBounceMsec, "The milliseconds a frame spent working out the bounce, until it is done");
	r_rtBakedLamps = ri.Cvar_Get("r_rtBakedLamps", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtBakedLamps, "With r_realtimeLighting 2, every lamp's light on the lightmapped world is baked once a map, with its shadow of the world, so that all of them show however few are lit live (r_rtMaxLights): those add their sharp shadows and shine. 0: only the lamps lit live give light");
	r_rtBakedDebug = ri.Cvar_Get("r_rtBakedDebug", "0", 0);
	ri.Cvar_SetDescription(r_rtBakedDebug, "1: the baked lamps' light without the normal maps' shading of it; 2: each lamp baked onto every surface, not those in its reach alone; 100 + n: light n baked alone (both on the next bake); 3: of the bounce, the lamps' alone; 4: the daylight's alone. Not archived: a test switch");
	r_rtWindowLights = ri.Cvar_Get("r_rtWindowLights", "0", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_rtWindowLights, "With r_realtimeLighting 2, keep the map's spot lights that stand in for daylight through its windows (0: the sun and the sky come in by themselves, and they go). Applies when a map is loaded");
	r_rtSkyTile = ri.Cvar_Get("r_rtSkyTile", "1024", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_CheckRange(r_rtSkyTile, 256, 2048, qtrue);
	ri.Cvar_SetDescription(r_rtSkyTile, "The size of the sky's depth from each of its directions, which says what sees the sky: the whole map in it");
}

/*
=============================================================================

LOADING

=============================================================================
*/

static void R_RtParseAmbient(void)
{
	const char *ents = ri.CM_EntityString();
	char       *p    = (char *)ents;
	qboolean    inWorld = qfalse;

	VectorClear(rt.ambient);
	VectorClear(rt.skyColor);

	while (p) {
		const char *token = COM_Parse(&p);
		char        key[MAX_TOKEN_CHARS];

		if (!token[0]) {
			break;
		}
		if (!strcmp(token, "{")) {
			inWorld = qfalse;
			continue;
		}
		if (!strcmp(token, "}")) {
			if (inWorld) {
				break;
			}
			continue;
		}

		Q_strncpyz(key, token, sizeof(key));
		token = COM_Parse(&p);
		if (!Q_stricmp(key, "classname")) {
			inWorld = !Q_stricmp(token, "worldspawn") ? qtrue : qfalse;
		} else if (!Q_stricmp(key, "ambientlight")) {
			sscanf(token, "%f %f %f", &rt.ambient[0], &rt.ambient[1], &rt.ambient[2]);
		} else if (!Q_stricmp(key, "sundiffusecolor")) {
			sscanf(token, "%f %f %f", &rt.skyColor[0], &rt.skyColor[1], &rt.skyColor[2]);
		}
	}
}

// A static model that is a light fixture: a lamp, a bulb, a lantern.
static qboolean R_RtIsFixture(const cStaticModelUnpacked_t *sm)
{
	return (Q_stristr(sm->model, "light") || Q_stristr(sm->model, "lamp") || Q_stristr(sm->model, "lantern")
			|| Q_stristr(sm->model, "bulb"))
			 ? qtrue
			 : qfalse;
}

/*
The compiler's lights around a lamp are often several, a little way off it,
placed to light what is near the way the level designer wanted. Lit live,
they show as light coming from beside the lamp, and they drop out one at a
time as the view goes away. The point lights at a fixture become one, at it,
with all their light; and the fixture, which is round the light now, casts no
shadow of it.
*/
#define RT_FIXTURE_REACH 40.0f
#define RT_FIXTURE_TALL  80.0f  // a fixture longer than this keeps its light where the lights are
#define RT_MERGE_REACH   48.0f // point lights this close are one lamp

/*
=============================================================================

THE LAMPS

A lamp model is one surface with one texture, and the map's lights are seldom
at it: the level designer lit each room with a few point lights where they
lit it best, up near the ceiling, a hand or two from the lamps, and put lamps
on the walls the compiler gave no light at all. Lit live, the light came from
the air beside the lamps.

lights/lamps.txt (data/opm-lamps, tools/lamps) says where each lamp model's
bulbs are. With r_realtimeLighting 2 the light comes from them: each of the
map's point lights goes to the nearest lamp it can see, all of their light
together in one light at its bulbs; a lamp no light went to gets one of its
own, as the table gives it; and a light that sees no lamp stays where it is,
a fill the room was lit with, casting no shadow (there is nothing for it to
cast one from). The map's spot lights stay as they are.

r_realtimeLighting 1 keeps the map's lighting and takes only the change each
light makes to it: there the lights stay where the map compiled them.
=============================================================================
*/

#define RT_LAMP_DEFS   128
#define RT_LAMP_BULBS  12
#define RT_LAMP_REACH  512.0f // a light further than this from every lamp is a fill
#define RT_LAMP_SEEN   16.0f  // a trace that stops this near a lamp got to it
#define RT_LAMP_MOST   1.5f   // a lamp takes up to this much of what its kind gives
#define RT_LAMP_NEAR   48.0f  // a lamp's light stops growing this near it: its shade glows, it is no point
#define RT_LAMP_SPOT_BACK 128.0f // a spot light this far down its cone from a lamp is the lamp's
#define RT_LAMP_SPOT_SIDE 24.0f  // and no further than this off its middle
#define RT_LAMP_SPOT_MOST 2.0f   // its light raised no more than this for the way it went up

enum {
	RT_LAMP_BULB,
	RT_LAMP_SHADED,
	RT_LAMP_CANDLE,
	RT_LAMP_LANTERN,
	RT_LAMP_OFF,
	RT_LAMP_SEARCHLIGHT // an entity's, aimed by the game (R_RtEntityLamps)
};

typedef struct {
	char     model[MAX_QPATH]; // without "models/", lowercase
	int      kind;
	vec3_t   color;
	float    intensity;
	vec3_t   bulbs[RT_LAMP_BULBS];
	int      numBulbs;
	qboolean spot;
	vec3_t   spotDir;
	float    spotAngle;
	vec3_t   shade; // the way its shade closes it off, as long as the part of its light it stops there
} rtLampDef_t;

static rtLampDef_t rt_lampDefs[RT_LAMP_DEFS];
static int         rt_numLampDefs;

// a model's name as the table keys it: lowercase, no "models/", no doubled slashes
static void R_RtLampKey(const char *model, char *out, int size)
{
	int o = 0;

	if (!Q_stricmpn(model, "models/", 7)) {
		model += 7;
	}
	for (; *model && o < size - 1; model++) {
		const char c = *model == '\\' ? '/' : tolower((unsigned char)*model);

		if (c == '/' && o > 0 && out[o - 1] == '/') {
			continue;
		}
		out[o++] = c;
	}
	out[o] = 0;
}

static void R_RtLoadLampDefs(void)
{
	union {
		char *c;
		void *v;
	} buffer;
	char        *text;
	rtLampDef_t *def = NULL;

	rt_numLampDefs = 0;
	if (ri.FS_ReadFile("lights/lamps.txt", &buffer.v) <= 0) {
		return;
	}

	text = buffer.c;
	while (1) {
		const char *token = COM_ParseExt(&text, qtrue);

		if (!token[0]) {
			break;
		}
		if (!strcmp(token, "{")) {
			continue;
		}
		if (!strcmp(token, "}")) {
			def = NULL;
			continue;
		}
		if (!def) {
			if (rt_numLampDefs >= RT_LAMP_DEFS) {
				ri.Printf(PRINT_WARNING, "lights/lamps.txt: more than %d lamps\n", RT_LAMP_DEFS);
				break;
			}
			def = &rt_lampDefs[rt_numLampDefs++];
			Com_Memset(def, 0, sizeof(*def));
			R_RtLampKey(token, def->model, sizeof(def->model));
			def->kind = RT_LAMP_SHADED;
			VectorSet(def->color, 1.0f, 0.86f, 0.68f);
			def->intensity = 100;
			continue;
		}
		if (!Q_stricmp(token, "kind")) {
			token = COM_ParseExt(&text, qfalse);
			def->kind = !Q_stricmp(token, "bulb")      ? RT_LAMP_BULB
					  : !Q_stricmp(token, "candle")  ? RT_LAMP_CANDLE
					  : !Q_stricmp(token, "lantern") ? RT_LAMP_LANTERN
					  : !Q_stricmp(token, "off")     ? RT_LAMP_OFF
					  : !Q_stricmp(token, "searchlight") ? RT_LAMP_SEARCHLIGHT
													 : RT_LAMP_SHADED;
			if (def->kind == RT_LAMP_CANDLE || def->kind == RT_LAMP_LANTERN) {
				VectorSet(def->color, 1.0f, 0.68f, 0.38f);
			}
		} else if (!Q_stricmp(token, "color")) {
			int k;

			for (k = 0; k < 3; k++) {
				def->color[k] = atof(COM_ParseExt(&text, qfalse));
			}
		} else if (!Q_stricmp(token, "intensity")) {
			def->intensity = atof(COM_ParseExt(&text, qfalse));
		} else if (!Q_stricmp(token, "bulb")) {
			int k;

			for (k = 0; k < 3; k++) {
				const float v = atof(COM_ParseExt(&text, qfalse));

				if (def->numBulbs < RT_LAMP_BULBS) {
					def->bulbs[def->numBulbs][k] = v;
				}
			}
			def->numBulbs = Q_min(def->numBulbs + 1, RT_LAMP_BULBS);
		} else if (!Q_stricmp(token, "shade")) {
			int   k;
			float stops;

			for (k = 0; k < 3; k++) {
				def->shade[k] = atof(COM_ParseExt(&text, qfalse));
			}
			stops = atof(COM_ParseExt(&text, qfalse));
			VectorNormalize(def->shade);
			// a ring: a lampshade open at both ends, holding back what leaves
			// by its sides; sent as longer than a whole (RtLight)
			token = COM_ParseExt(&text, qfalse);
			VectorScale(def->shade, Com_Clamp(0.0f, 1.0f, stops) + (!Q_stricmp(token, "ring") ? 1.0f : 0.0f), def->shade);
		} else if (!Q_stricmp(token, "spot")) {
			int k;

			for (k = 0; k < 3; k++) {
				def->spotDir[k] = atof(COM_ParseExt(&text, qfalse));
			}
			def->spotAngle = atof(COM_ParseExt(&text, qfalse));
			def->spot      = VectorNormalize(def->spotDir) > 0 && def->spotAngle > 0;
		}
		SkipRestOfLine(&text);
	}
	ri.FS_FreeFile(buffer.v);
}

static const rtLampDef_t *R_RtLampDef(const char *model)
{
	char key[MAX_QPATH];
	int  i;

	R_RtLampKey(model, key, sizeof(key));
	for (i = 0; i < rt_numLampDefs; i++) {
		if (!strcmp(rt_lampDefs[i].model, key)) {
			return &rt_lampDefs[i];
		}
	}
	return NULL;
}

static int R_RtClusterAt(const vec3_t p)
{
	const mnode_t *leaf = R_PointInLeaf(p);

	return leaf && leaf->cluster >= 0 && leaf->cluster < tr.world->numClusters ? leaf->cluster : -1;
}

static float R_RtRangeOf(const vec3_t color)
{
	const float brightest = Q_max(color[0], Q_max(color[1], color[2]));

	return Q_min(4096.0f, sqrt(brightest / RT_CUTOFF));
}

/*
=============================================================================

THE LAMPS THE GAME MOVES

A searchlight is an entity the level's script turns (global/spotlight.scr):
its light was a pair of the game's lights (dlights) on the ground where its
beam falls, moved there each frame, so that the light came up from the
ground and not out of the lamp. With r_realtimeLighting 2 a model lamps.txt
gives "kind searchlight" lights from its lens, the way its beam points, a
spot light with its shadows; the game's lights in its beam are its own, and
go. The lamp's models cast no shadow of anything (it is in its light).
=============================================================================
*/

#define RT_ENTITY_LAMPS      8
#define RT_SEARCHLIGHT_SLACK 48.0f // a game's light this far off its beam is in it

static const rtLampDef_t *rt_modelDefs[MAX_MOD_KNOWN];
static byte               rt_modelDefLooked[MAX_MOD_KNOWN];

// The lamp a model is, by its handle, looked up once a map
static const rtLampDef_t *R_RtModelLampDef(qhandle_t hModel)
{
	const model_t *model;

	if (hModel <= 0 || hModel >= MAX_MOD_KNOWN || !rt_numLampDefs) {
		return NULL;
	}
	if (!rt_modelDefLooked[hModel]) {
		rt_modelDefLooked[hModel] = 1;
		model                     = R_GetModelByHandle(hModel);
		rt_modelDefs[hModel]      = model && model->type == MOD_TIKI ? R_RtLampDef(model->name) : NULL;
	}
	return rt_modelDefs[hModel];
}

static void R_RtEntityLamps(void)
{
	int i, j, k;

	Com_Memset(rt.dlightTaken, 0, sizeof(rt.dlightTaken));
	if (!rt_numLampDefs || r_realtimeLighting->integer != 2 || !r_rtLamps->integer) {
		return;
	}

	for (i = 0; i < tr.refdef.num_entities; i++) {
		const refEntity_t *e = &tr.refdef.entities[i].e;
		const rtLampDef_t *def;
		rtLight_t         *l = NULL;
		vec3_t             lens, dir, axis[3];
		float              cone, scale, nearest = 0;

		if (e->reType != RT_MODEL || !(def = R_RtModelLampDef(e->hModel)) || def->kind != RT_LAMP_SEARCHLIGHT || !def->spot) {
			continue;
		}
		if (e->entityNumber < 0 || e->entityNumber >= ENTITYNUM_WORLD) {
			continue;
		}

		// its light: the one it had, else one no entity has had for a while
		for (j = rt.numMapLights; j < rt.numLights; j++) {
			if (rt.lights[j].entity == e->entityNumber) {
				l = &rt.lights[j];
				break;
			}
		}
		for (j = rt.numMapLights; j < rt.numLights && !l; j++) {
			if (rt.lights[j].entity < 0 || (rt.frame - rt.lights[j].entityFrame > 30 && rt.lights[j].weight <= 0)) {
				l = &rt.lights[j];
			}
		}
		if (!l) {
			continue;
		}

		scale = e->scale > 0 ? e->scale : 1.0f;
		for (k = 0; k < 3; k++) {
			VectorCopy(e->axis[k], axis[k]);
			VectorNormalize(axis[k]);
		}
		VectorCopy(e->origin, lens);
		VectorClear(dir);
		for (k = 0; k < 3; k++) {
			VectorMA(lens, def->bulbs[0][k] * scale, axis[k], lens);
			VectorMA(dir, def->spotDir[k], axis[k], dir);
		}
		VectorNormalize(dir);
		cone = tan(DEG2RAD(def->spotAngle * 0.5f));

		// the game's lights in its beam: where it falls, its own
		for (j = 0; j < tr.refdef.num_dlights && j < MAX_DLIGHTS; j++) {
			const dlight_t *dl = &tr.refdef.dlights[j];
			vec3_t          v;
			float           along, off;

			VectorSubtract(dl->origin, lens, v);
			along = DotProduct(v, dir);
			if (along <= 0) {
				continue;
			}
			off = sqrt(Q_max(0.0f, DotProduct(v, v) - Square(along)));
			if (off > Q_max(dl->radius * 0.5f, along * cone) + RT_SEARCHLIGHT_SLACK) {
				continue;
			}
			rt.dlightTaken[j] = qtrue;
			// aimed at the nearest of them, where the game puts it
			if (!nearest || along < nearest) {
				nearest = along;
				VectorNormalize2(v, dir);
			}
		}

		if (l->entity != e->entityNumber) {
			// a new one: its shadows from where it is
			if (l->staticSlot >= 0) {
				rt.staticOwner[l->staticSlot] = -1;
			}
			l->staticSlot  = -1;
			l->staticFaces = 0;
			l->staticReady = qfalse;
			l->bakedFaces  = 0;
			l->weight      = 0;
			l->entity      = e->entityNumber;
			l->cluster     = R_RtClusterAt(e->origin);
			VectorCopy(e->origin, l->origin);
		}
		// the lens turns round its pivot: its shadows are drawn again only
		// when it has gone a few units, from the pivot, which stands still
		if (DistanceSquared(e->origin, l->origin) > Square(RT_LIGHT_MOVE_EPSILON)) {
			VectorCopy(e->origin, l->origin);
			l->movedFrame  = rt.frame;
			l->staticFaces = 0;
			l->cluster     = R_RtClusterAt(l->origin);
		}
		l->entityFrame  = rt.frame;
		l->spot         = qtrue;
		l->radiusByDist = cone;
		VectorCopy(dir, l->spotDir);
		VectorScale(def->color, def->intensity * 7500.0f, l->color);
		l->range = R_RtRangeOf(l->color);
		VectorMA(l->origin, Q_min(l->range, nearest > 0 ? nearest : 512.0f), dir, l->pool);
	}

	// those gone from the scene go dark (eased out as lights not chosen)
	for (j = rt.numMapLights; j < rt.numLights; j++) {
		rtLight_t *l = &rt.lights[j];

		if (l->entity >= 0 && l->entityFrame != rt.frame && l->weight <= 0) {
			l->range = 0;
		}
	}
}

// Whether a light at from sees a lamp's bulbs at to: no wall between them.
// Traced from the lamp, which hangs in the room, out to the light, which the
// designer put anywhere, often a hand from a ceiling or a wall: what stops
// the trace that near the light is beside it. Bulbs a ceiling or a wall cuts
// through (a lamp's middle can be in one) see from as far as they reach.
static qboolean R_RtSeesLamp(const vec3_t from, const vec3_t to, float size)
{
	trace_t trace;
	vec3_t  zero = {0, 0, 0};

	ri.CM_BoxTrace(&trace, to, from, zero, zero, 0, CONTENTS_SOLID, qfalse);
	if (trace.startsolid) {
		ri.CM_BoxTrace(&trace, from, to, zero, zero, 0, CONTENTS_SOLID, qfalse);
		return (trace.fraction >= 0.999f || Distance(trace.endpos, to) < Q_max(RT_LAMP_SEEN, size)) ? qtrue : qfalse;
	}
	return (trace.fraction >= 0.999f || Distance(trace.endpos, from) < RT_LAMP_SEEN) ? qtrue : qfalse;
}

/*
Each of the map's point lights to the lamp nearest it that it sees, the lamps
lit, and the lamps the table has marked in isLamp, for R_RtLightsAtFixtures
to leave alone. rt.lights has room for a light at every static model.
*/
static void R_RtLightsAtLamps(byte *isLamp)
{
	typedef struct {
		int    model;
		vec3_t centre;
		int    light; // its light in rt.lights
		int    given; // the map's lights that went to it
		const rtLampDef_t *def;
	} lamp_t;
	lamp_t *lamps;
	int     numLamps = 0, taken = 0, fills = 0, added = 0, spots = 0;
	int     numMapLights = rt.numLights;
	int     i, j, k;

	R_RtLoadLampDefs();
	if (!rt_numLampDefs) {
		ri.Printf(PRINT_DEVELOPER, "realtime lighting: no lights/lamps.txt\n");
		return;
	}

	lamps = ri.Hunk_AllocateTempMemory(sizeof(lamp_t) * Q_max(1, tr.world->numStaticModels));

	for (i = 0; i < tr.world->numStaticModels; i++) {
		cStaticModelUnpacked_t *sm = &tr.world->staticModels[i];
		const rtLampDef_t      *def;
		lamp_t                 *lamp;

		if (!sm->tiki || !(def = R_RtLampDef(sm->model)) || !def->numBulbs || def->kind == RT_LAMP_SEARCHLIGHT) {
			continue;
		}
		isLamp[i]       = 1;
		sm->rtFixture   = qtrue;
		if (def->kind == RT_LAMP_OFF) {
			continue;
		}

		lamp        = &lamps[numLamps++];
		lamp->model = i;
		lamp->given = 0;
		lamp->def   = def;
		// the middle of its bulbs, in the world
		VectorCopy(sm->origin, lamp->centre);
		for (j = 0; j < def->numBulbs; j++) {
			for (k = 0; k < 3; k++) {
				VectorMA(lamp->centre, def->bulbs[j][k] * sm->scale / def->numBulbs, sm->axis[k], lamp->centre);
			}
		}
	}

	// a light at each lamp, as the table has it, until the map's lights come to it
	for (i = 0; i < numLamps; i++) {
		lamp_t                       *lamp = &lamps[i];
		const cStaticModelUnpacked_t *sm   = &tr.world->staticModels[lamp->model];
		rtLight_t                    *l    = &rt.lights[rt.numLights];

		lamp->light = rt.numLights++;
		Com_Memset(l, 0, sizeof(*l));
		l->source     = -1;
		l->mergedInto = -1;
		l->staticSlot = -1;
		l->entity     = -1;
		l->fixture    = lamp->model;
		l->lamp       = qtrue;
		l->cluster    = R_RtClusterAt(lamp->centre);
		l->seed       = (float)(lamp->model % 97) * 0.37f;
		l->flicker    = lamp->def->kind == RT_LAMP_CANDLE ? 0.14f : lamp->def->kind == RT_LAMP_LANTERN ? 0.08f : 0.0f;
		VectorCopy(lamp->centre, l->origin);
		if (lamp->def->spot) {
			l->spot = qtrue;
			for (k = 0; k < 3; k++) {
				VectorMA(l->spotDir, lamp->def->spotDir[k], sm->axis[k], l->spotDir);
			}
			VectorNormalize(l->spotDir);
			l->radiusByDist = tan(DEG2RAD(lamp->def->spotAngle * 0.5f));
		} else {
			// its shade, where a spot light keeps its cone's way (RtLight)
			for (k = 0; k < 3; k++) {
				VectorMA(l->spotDir, lamp->def->shade[k], sm->axis[k], l->spotDir);
			}
		}
	}

	// The map's point lights, each to the nearest lamp it sees. A lamp takes
	// no more than RT_LAMP_MOST of what its kind gives: a light the designer
	// lit a whole room with, put five units from a wall in a sconce, burnt a
	// white hole in the wall. What it does not take stays where it was, a
	// fill with no shadow.
	for (j = 0; j < numMapLights; j++) {
		rtLight_t *l    = &rt.lights[j];
		float      best = RT_LAMP_REACH;
		int        to   = -1;

		if (l->range <= 0 || l->spot || l->area > 0) {
			continue;
		}
		for (i = 0; i < numLamps; i++) {
			const float d = Distance(l->origin, lamps[i].centre);

			if (d < best && R_RtSeesLamp(l->origin, lamps[i].centre, tr.world->staticModels[lamps[i].model].cull_radius)) {
				best = d;
				to   = i;
			}
		}
		if (r_rtDebug->integer) {
			ri.Printf(PRINT_ALL, "  light %d at (%.0f %.0f %.0f) intensity %.0f: %s\n", l->source, l->origin[0], l->origin[1], l->origin[2],
				Q_max(l->color[0], Q_max(l->color[1], l->color[2])) / 7500.0f,
				to < 0 ? "a fill" : va("to %s at (%.0f %.0f %.0f), %.0f away", tr.world->staticModels[lamps[to].model].model, lamps[to].centre[0], lamps[to].centre[1], lamps[to].centre[2], best));
		}
		if (to < 0) {
			l->noShadow = qtrue;
			fills++;
			continue;
		}
		{
			rtLight_t  *into = &rt.lights[lamps[to].light];
			const float most = lamps[to].def->intensity * 7500.0f * RT_LAMP_MOST;
			const float has  = Q_max(into->color[0], Q_max(into->color[1], into->color[2]));
			const float its  = Q_max(l->color[0], Q_max(l->color[1], l->color[2]));
			const float give = its > 0 ? Q_min(1.0f, Q_max(0.0f, most - has) / its) : 0;

			VectorMA(into->color, give, l->color, into->color);
			if (give >= 0.999f) {
				l->range      = 0;
				l->mergedInto = lamps[to].light;
			} else {
				VectorScale(l->color, 1.0f - give, l->color);
				l->range    = R_RtRangeOf(l->color);
				l->noShadow = qtrue;
				fills++;
			}
			lamps[to].given++;
		}
		taken++;
	}

	// A spot light at a lamp (its cone down onto what the lamp lights) is the
	// lamp's light: from its bulbs, and turning with it. The designer put it
	// a little way under the lamp, or beside it, often a hand or two: its
	// apex goes up to the bulbs, its cone narrowed and its light raised so
	// that what it lit (where its middle meets the world) is lit as before.
	// A lamp it went to takes no light of its own from the table.
	for (j = 0; j < numMapLights; j++) {
		rtLight_t *l    = &rt.lights[j];
		float      best = 1e9f;
		int        to   = -1;

		if (!l->spot || l->range <= 0 || l->fixture >= 0) {
			continue;
		}
		for (i = 0; i < numLamps; i++) {
			vec3_t      d;
			float       dist, back, side;

			VectorSubtract(lamps[i].centre, l->origin, d);
			dist = VectorLength(d);
			back = -DotProduct(d, l->spotDir); // up its cone's middle, towards the lamp
			side = sqrt(Q_max(0.0f, Square(dist) - Square(back)));
			if (dist >= best || (dist > RT_FIXTURE_REACH && (back <= 0 || back > RT_LAMP_SPOT_BACK || side > RT_LAMP_SPOT_SIDE))) {
				continue;
			}
			if (!R_RtSeesLamp(l->origin, lamps[i].centre, tr.world->staticModels[lamps[i].model].cull_radius)) {
				continue;
			}
			best = dist;
			to   = i;
		}
		if (to < 0) {
			continue;
		}
		{
			trace_t trace;
			vec3_t  zero = {0, 0, 0}, end, d;
			float   lit, back;

			// where its middle met the world, as far as it reaches
			VectorMA(l->origin, l->range, l->spotDir, end);
			ri.CM_BoxTrace(&trace, l->origin, end, zero, zero, 0, CONTENTS_SOLID, qfalse);
			lit = trace.startsolid ? 0 : trace.fraction * l->range;

			VectorSubtract(lamps[to].centre, l->origin, d);
			back = Q_max(0.0f, -DotProduct(d, l->spotDir));
			if (lit > 16.0f && back > 0) {
				const float keep = lit / (lit + back);

				l->radiusByDist *= keep;
				VectorScale(l->color, Q_min(RT_LAMP_SPOT_MOST, 1.0f / Square(keep)), l->color);
				l->range = R_RtRangeOf(l->color);
			}
			if (r_rtDebug->integer) {
				ri.Printf(PRINT_ALL, "  spot light %d at (%.0f %.0f %.0f): to %s at (%.0f %.0f %.0f), %.0f up its cone, lighting %.0f away\n", l->source,
					l->origin[0], l->origin[1], l->origin[2], tr.world->staticModels[lamps[to].model].model, lamps[to].centre[0], lamps[to].centre[1],
					lamps[to].centre[2], back, lit);
			}
			VectorCopy(lamps[to].centre, l->origin);
			l->fixture = lamps[to].model;
			l->cluster = R_RtClusterAt(l->origin);
			lamps[to].given++;
			spots++;
		}
	}

	// the lamps none came to light as the table has them
	for (i = 0; i < numLamps; i++) {
		rtLight_t *l = &rt.lights[lamps[i].light];

		if (!lamps[i].given) {
			VectorScale(lamps[i].def->color, lamps[i].def->intensity * 7500.0f, l->color);
			added++;
		}
		l->range = R_RtRangeOf(l->color);
	}

	ri.Hunk_FreeTempMemory(lamps);
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d lamps, %d of the map's lights went to them (%d spot lights), %d lit their own, %d fills\n",
		numLamps, taken + spots, spots, added, fills);
}

/*
=============================================================================

LIT SURFACES

A lit window, a lamp's glass in the brushwork: a shader with
q3map_surfacelight. The compiler lit the map with each as a light the size of
the surface, which it keeps as a spot light whose cone is less than nothing
(the stock sphere lighting never lights a model with one), at the surface's
middle, pointing the way it faces, as wide as its radius by distance. Lit
live (r_realtimeLighting 2), where the lightmaps give way, each is a light
just in front of its surface: a half of a sphere the way it faces (a lamp's
shade closing off what is behind), as near to it as the surface is wide
(RtLight's "near": a big window lights what is close evenly, as an area does).
A window cut in pieces is one light.
=============================================================================
*/

#define RT_AREA_MERGE 1.1f // pieces of one surface whose radii together reach this far apart are one
#define RT_AREA_OUT   8.0f // the light this far in front of its surface, at most
#define RT_AREA_SCALE 0.35f // of its light: the compiler's lightmaps have a lit window light
                            // little round it, its sphere light (for models) a lot

// A lit surface's light, from its sphere light (still in the spot's terms)
static void R_RtAreaLight(rtLight_t *l)
{
	VectorScale(l->color, RT_AREA_SCALE, l->color);
	l->area         = Q_max(8.0f, -l->radiusByDist);
	l->spot         = qfalse;
	l->radiusByDist = 0;
}

static void R_RtMergeAreaLights(void)
{
	int      i, j, merged = 0, count = 0;
	qboolean again = qtrue;

	while (again) {
		again = qfalse;
		for (i = 0; i < rt.numLights; i++) {
			rtLight_t *l = &rt.lights[i];

			if (l->area <= 0 || l->range <= 0) {
				continue;
			}
			for (j = i + 1; j < rt.numLights; j++) {
				rtLight_t  *o = &rt.lights[j];
				vec3_t      d;
				float       wl, wo;

				if (o->area <= 0 || o->range <= 0 || DotProduct(o->spotDir, l->spotDir) < 0.95f) {
					continue;
				}
				VectorSubtract(o->origin, l->origin, d);
				if (fabs(DotProduct(d, l->spotDir)) > 8.0f || VectorLength(d) > (l->area + o->area) * RT_AREA_MERGE) {
					continue;
				}
				wl = l->color[0] + l->color[1] + l->color[2];
				wo = o->color[0] + o->color[1] + o->color[2];
				VectorScale(l->origin, wl, l->origin);
				VectorMA(l->origin, wo, o->origin, l->origin);
				VectorScale(l->origin, 1.0f / Q_max(wl + wo, 1e-6f), l->origin);
				VectorAdd(l->color, o->color, l->color);
				l->area       = sqrt(Square(l->area) + Square(o->area));
				l->range      = R_RtRangeOf(l->color);
				o->range      = 0;
				o->mergedInto = i;
				merged++;
				again = qtrue;
			}
		}
	}

	for (i = 0; i < rt.numLights; i++) {
		rtLight_t *l = &rt.lights[i];

		if (l->area <= 0 || l->range <= 0) {
			continue;
		}
		// in front of it, clear of the wall it is in for its shadow
		VectorMA(l->origin, Q_min(RT_AREA_OUT, l->area * 0.25f) + 1.0f, l->spotDir, l->origin);
		count++;
		if (r_rtDebug->integer) {
			ri.Printf(PRINT_ALL, "  lit surface at (%.0f %.0f %.0f) facing (%.2f %.2f %.2f) %.0f across, intensity %.0f\n", l->origin[0],
				l->origin[1], l->origin[2], l->spotDir[0], l->spotDir[1], l->spotDir[2], l->area * 2.0f,
				Q_max(l->color[0], Q_max(l->color[1], l->color[2])) / 7500.0f);
		}
	}
	if (count || merged) {
		ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d lit surfaces (%d pieces taken into others)\n", count, merged);
	}
}

/*
=============================================================================

DAYLIGHT THROUGH WINDOWS

The compiler lit a room by day with no more than its sun on what the sun
reached through a window and its sky; the level designers lit the rest with
spot lights in the windows, wide and pointing in, where the daylight comes
from (m3l2: 24 of its 46 lights). With r_realtimeLighting 2 the sun and the
sky come in through the windows themselves, and the bounce of their light off
what they reach lights the room (R_RtBounce): those lights would light it a
second time, from nowhere. A spot light is one of them when it is indoors,
not at a lamp, does not point down (a lamp's cone does), and the way it
points from leads out through an opening to where the sky is overhead.
=============================================================================
*/

#define RT_WINDOW_BACK 384.0f // how far back the way it points the outside is looked for

// Whether the sky is overhead at p: a roof, or open sky
static qboolean R_RtSkyOverhead(const vec3_t p)
{
	trace_t trace;
	vec3_t  zero = {0, 0, 0}, up;

	VectorCopy(p, up);
	up[2] += 8192.0f;
	ri.CM_BoxTrace(&trace, p, up, zero, zero, 0, CONTENTS_SOLID, qfalse);
	return (!trace.startsolid && (trace.fraction >= 1.0f || (trace.surfaceFlags & SURF_SKY))) ? qtrue : qfalse;
}

static qboolean R_RtIsWindowLight(const rtLight_t *l)
{
	trace_t trace;
	vec3_t  zero = {0, 0, 0}, p, side, up = {0, 0, 1}, offsets[5], start;
	float   back;
	int     o;

	if (!l->spot || l->lamp || l->fixture >= 0 || l->spotDir[2] < -0.7f) {
		return qfalse;
	}
	if (R_RtSkyOverhead(l->origin)) {
		// outdoors itself: a lamp post's, a searchlight's; unless it is in a
		// window's outer face, shining in, a few steps from a roof
		for (back = 16.0f; back <= 128.0f; back += 16.0f) {
			VectorMA(l->origin, back, l->spotDir, p);
			ri.CM_BoxTrace(&trace, l->origin, p, zero, zero, 0, CONTENTS_SOLID, qfalse);
			if (!trace.startsolid && trace.fraction < 1.0f) {
				return qfalse;
			}
			if (!R_RtSkyOverhead(p)) {
				return qtrue;
			}
		}
		return qfalse;
	}
	// straight back, and a little to each side and up and down of it: a
	// window's frame or mullion in the way of one is not in all of them
	CrossProduct(l->spotDir, up, side);
	if (VectorNormalize(side) <= 0) {
		VectorSet(side, 1, 0, 0);
	}
	VectorClear(offsets[0]);
	VectorScale(side, 24.0f, offsets[1]);
	VectorScale(side, -24.0f, offsets[2]);
	VectorSet(offsets[3], 0, 0, 24.0f);
	VectorSet(offsets[4], 0, 0, -24.0f);
	for (o = 0; o < 5; o++) {
		VectorAdd(l->origin, offsets[o], start);
		if (o) {
			// the step aside, clear of the wall
			ri.CM_BoxTrace(&trace, l->origin, start, zero, zero, 0, CONTENTS_SOLID, qfalse);
			if (trace.fraction < 1.0f && !trace.startsolid) {
				continue;
			}
		}
		for (back = 16.0f; back <= RT_WINDOW_BACK; back += 16.0f) {
			VectorMA(start, -back, l->spotDir, p);
			// a wall between: not an opening this way (the light may be in the wall's face)
			ri.CM_BoxTrace(&trace, start, p, zero, zero, 0, CONTENTS_SOLID, qfalse);
			if (!trace.startsolid && trace.fraction < 1.0f) {
				break;
			}
			if (R_RtSkyOverhead(p)) {
				return qtrue;
			}
		}
	}
	return qfalse;
}

static void R_RtDropWindowLights(void)
{
	int i, dropped = 0;

	if (r_realtimeLighting->integer != 2 || r_rtWindowLights->integer || (!rt.sun && VectorLength(rt.skyColor) <= 0)) {
		return;
	}
	for (i = 0; i < rt.numLights; i++) {
		rtLight_t *l = &rt.lights[i];

		if (l->range > 0 && R_RtIsWindowLight(l)) {
			if (r_rtDebug->integer) {
				ri.Printf(PRINT_ALL, "  light %d at (%.0f %.0f %.0f) pointing (%.2f %.2f %.2f): daylight through a window, gone\n", l->source,
					l->origin[0], l->origin[1], l->origin[2], l->spotDir[0], l->spotDir[1], l->spotDir[2]);
			}
			l->range      = 0;
			l->mergedInto = -1;
			dropped++;
		}
	}
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d of the map's lights were daylight through its windows\n", dropped);
}

static void R_RtLightsAtFixtures(void)
{
	int   i, j, merged = 0, fixtures = 0;
	byte *isLamp = ri.Hunk_AllocateTempMemory(Q_max(1, tr.world->numStaticModels));

	Com_Memset(isLamp, 0, Q_max(1, tr.world->numStaticModels));
	if (r_realtimeLighting->integer == 2 && r_rtLamps->integer) {
		R_RtLightsAtLamps(isLamp);
	}
	R_RtDropWindowLights();
	R_RtMergeAreaLights();

	for (i = 0; i < tr.world->numStaticModels; i++) {
		cStaticModelUnpacked_t *sm = &tr.world->staticModels[i];
		vec3_t                  centre, sum, colour, boxMins, boxMaxs;
		qboolean                tall;
		float                   weight = 0;
		int                     count  = 0, keep = -1;

		if (isLamp[i]) {
			continue;
		}
		sm->rtFixture = qfalse;
		if (!sm->tiki || !R_RtIsFixture(sm)) {
			continue;
		}
		sm->rtFixture = qtrue;
		fixtures++;

		// its middle, and its box in its own frame
		VectorScale(sm->tiki->a->mins, sm->tiki->load_scale * sm->scale, boxMins);
		VectorScale(sm->tiki->a->maxs, sm->tiki->load_scale * sm->scale, boxMaxs);
		VectorAdd(boxMins, boxMaxs, centre);
		VectorScale(centre, 0.5f, centre);
		{
			vec3_t local;

			VectorCopy(centre, local);
			VectorCopy(sm->origin, centre);
			for (j = 0; j < 3; j++) {
				VectorMA(centre, local[j], sm->axis[j], centre);
			}
		}
		// A long one -- a bulb at the end of a wire two hundred units long --
		// takes the lights near any of it, and its light stays where they
		// are, at the bulb, rather than going to the middle of the wire.
		tall = (boxMaxs[0] - boxMins[0] > RT_FIXTURE_TALL || boxMaxs[1] - boxMins[1] > RT_FIXTURE_TALL
				|| boxMaxs[2] - boxMins[2] > RT_FIXTURE_TALL) ? qtrue : qfalse;

		if (!r_rtLightsAtFixtures->integer) {
			continue;
		}

		VectorClear(sum);
		VectorClear(colour);
		for (j = 0; j < rt.numLights; j++) {
			rtLight_t *l = &rt.lights[j];
			float      w;

			if (l->range <= 0 || l->lamp || l->area > 0) {
				continue;
			}
			if (tall) {
				vec3_t d;
				float  out = 0;
				int    k;

				// how far outside its box
				VectorSubtract(l->origin, sm->origin, d);
				for (k = 0; k < 3; k++) {
					const float v = DotProduct(d, sm->axis[k]);

					out += Square(v < boxMins[k] ? boxMins[k] - v : v > boxMaxs[k] ? v - boxMaxs[k] : 0);
				}
				if (out > Square(RT_FIXTURE_REACH)) {
					continue;
				}
			} else if (Distance(l->origin, centre) > RT_FIXTURE_REACH) {
				continue;
			}

			// A spot light at a lamp (a shaded lamp's cone down onto what it
			// lights) stays one of its own, and goes and turns with the lamp
			if (l->spot) {
				if (l->fixture < 0) {
					l->fixture = i;
				}
				continue;
			}

			w = l->color[0] + l->color[1] + l->color[2];
			VectorMA(sum, w, l->origin, sum);
			VectorAdd(colour, l->color, colour);
			weight += w;
			count++;

			if (keep < 0) {
				keep = j;
			} else {
				// taken into the first
				l->range      = 0;
				l->mergedInto = keep;
				merged++;
			}
		}

		if (keep >= 0) {
			rtLight_t *l = &rt.lights[keep];
			float      brightest;

			l->fixture = i;
			if (tall) {
				VectorScale(sum, 1.0f / Q_max(weight, 1e-6f), l->origin);
			} else {
				VectorCopy(centre, l->origin);
			}
			VectorCopy(colour, l->color);
			brightest = Q_max(l->color[0], Q_max(l->color[1], l->color[2]));
			l->range  = Q_min(4096.0f, sqrt(brightest / RT_CUTOFF));
		}
	}

	// The compiler's lamps are often a few weak point lights a hand apart (m1l1:
	// three of intensity 25 in a triangle 20 to 40 units across). As lights of
	// their own they take three of the few lit and come and go one at a time,
	// a lamp dimming in steps as the view moves away. One lamp, one light.
	for (i = 0; i < rt.numLights; i++) {
		rtLight_t *l = &rt.lights[i];
		vec3_t     sum;
		float      weight;
		int        count = 0;

		if (l->spot || l->range <= 0 || l->lamp || l->area > 0) {
			continue;
		}
		weight = l->color[0] + l->color[1] + l->color[2];
		VectorScale(l->origin, weight, sum);

		for (j = i + 1; j < rt.numLights; j++) {
			rtLight_t *o = &rt.lights[j];
			float      w;

			if (o->spot || o->range <= 0 || o->lamp || o->area > 0 || Distance(o->origin, l->origin) > RT_MERGE_REACH) {
				continue;
			}
			w = o->color[0] + o->color[1] + o->color[2];
			VectorMA(sum, w, o->origin, sum);
			VectorAdd(l->color, o->color, l->color);
			weight       += w;
			o->range      = 0;
			o->mergedInto = i;
			count++;
		}

		if (count) {
			float brightest;

			VectorScale(sum, 1.0f / Q_max(weight, 1e-6f), l->origin);
			brightest = Q_max(l->color[0], Q_max(l->color[1], l->color[2]));
			l->range  = Q_min(4096.0f, sqrt(brightest / RT_CUTOFF));
			merged += count;
		}
	}

	// where each lamp's light is on its lamp, so that it goes where the lamp
	// is moved (R_RtFollowFixtures)
	for (i = 0; i < rt.numLights; i++) {
		rtLight_t                    *l = &rt.lights[i];
		const cStaticModelUnpacked_t *sm;
		vec3_t                        d;

		if (l->fixture < 0 || l->range <= 0) {
			continue;
		}
		sm = &tr.world->staticModels[l->fixture];
		VectorSubtract(l->origin, sm->origin, d);
		for (j = 0; j < 3; j++) {
			l->fixtureLocal[j] = DotProduct(d, sm->axis[j]);
			l->spotDirLocal[j] = DotProduct(l->spotDir, sm->axis[j]);
		}
	}

	// those taken into others go; each sphere light is its own light's, or
	// that of the one it went into (the leaves list sphere lights)
	{
		int *newIndex = ri.Hunk_AllocateTempMemory(sizeof(int) * Q_max(1, rt.numLights));

		for (i = 0, j = 0; i < rt.numLights; i++) {
			newIndex[i] = rt.lights[i].range > 0 ? j++ : -1;
		}
		for (i = 0; i < tr.numSLights; i++) {
			rt.fromSphere[i] = -1;
		}
		for (i = 0; i < rt.numLights; i++) {
			const rtLight_t *l = &rt.lights[i];
			int              to = newIndex[i] >= 0 ? newIndex[i] : (l->mergedInto >= 0 ? newIndex[l->mergedInto] : -1);

			if (l->source >= 0) {
				rt.fromSphere[l->source] = to;
			}
		}
		for (i = 0, j = 0; i < rt.numLights; i++) {
			if (rt.lights[i].range > 0) {
				rt.lights[j++] = rt.lights[i];
			}
		}
		rt.numLights = j;
		ri.Hunk_FreeTempMemory(newIndex);
	}

	ri.Hunk_FreeTempMemory(isLamp);
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d fixtures, %d lights taken into them or their neighbours\n", fixtures, merged);
}

// Where each light lights most, for whether the view sees it (R_RtViewSees):
// where its cone's middle meets the world, or a lit surface's way out from
// it, or else straight down, a hand off what it meets
static void R_RtFindPools(void)
{
	int i;

	for (i = 0; i < rt.numLights; i++) {
		rtLight_t *l    = &rt.lights[i];
		vec3_t     zero = {0, 0, 0}, dir, end;
		trace_t    trace;

		if (l->spot || l->area > 0) {
			VectorCopy(l->spotDir, dir);
		} else {
			VectorSet(dir, 0, 0, -1);
		}
		VectorMA(l->origin, Q_min(l->range, 1024.0f), dir, end);
		ri.CM_BoxTrace(&trace, l->origin, end, zero, zero, 0, CONTENTS_SOLID, qfalse);
		if (trace.startsolid) {
			VectorCopy(l->origin, l->pool);
			continue;
		}
		VectorMA(trace.endpos, -8.0f, dir, l->pool);
	}
}

/*
=============================================================================

THE SKY

The map was compiled with a sky that lights what is open to it. In the shade
of the sun, an open wall of a sunlit map has three to six times the
worldspawn's ambient light in its lightmap, about half of what the sun gives
it face on, and an open floor all of it: the sky's light is the sun's colour,
or its sundiffusecolor where the map sets one, from all of the sky. With
r_realtimeLighting 2 the lightmaps give way to the ambient light, and that
shade went black.

So the sky lights live too, from RT_SKY_DIRS directions spread evenly over it.
The depth of the world from each, all of the map in a tile of one atlas, drawn
once a map, says which of them a pixel sees (RtSky in the shaders): a roof
keeps the sky off what is under it, a window lets some in.

=============================================================================
*/

static float rt_skyTexel; // the most world a texel of the sky's depth covers

static void R_RtSetupSky(void)
{
	const float golden = M_PI * (3.0f - sqrtf(5.0f));
	int         k;

	if (VectorCompare(rt.skyColor, vec3_origin) && rt.sun) {
		VectorCopy(rt.sunColor, rt.skyColor);
	}

	// as many from each part of the sky: evenly in height is evenly over it
	for (k = 0; k < RT_SKY_DIRS; k++) {
		const float z = (k + 0.5f) / RT_SKY_DIRS;
		const float r = sqrtf(1.0f - z * z);

		VectorSet(rt.skyDirs[k], r * cosf(k * golden), r * sinf(k * golden), z);
	}

	rt.skyDrawn  = 0;
	rt.skyImage  = NULL;
	rt.skyWeight = 0;
	rt_skyTexel  = 0;
}

// The depth of what stands still, the whole map, looking down from the sky
// along one of its directions, into its tile.
static void R_RtRenderSkyDir(int k)
{
	const int    size = r_rtSkyTile->integer;
	const float *mins = tr.world->bmodels[0].bounds[0];
	const float *maxs = tr.world->bmodels[0].bounds[1];
	viewParms_t  parms;
	vec3_t       axis[3], origin, bounds[2];
	mat4_t       view, mvp;
	int          c, firstDrawSurf, firstSpriteSurf;

	VectorNegate(rt.skyDirs[k], axis[0]);
	if (fabsf(axis[0][2]) > 0.9f) {
		VectorSet(axis[2], 0, 1, 0);
	} else {
		VectorSet(axis[2], 0, 0, 1);
	}
	CrossProduct(axis[2], axis[0], axis[1]);
	VectorNormalize(axis[1]);
	CrossProduct(axis[0], axis[1], axis[2]);

	VectorAdd(mins, maxs, origin);
	VectorScale(origin, 0.5f, origin);

	Mat4View(axis, origin, view);
	ClearBounds(bounds[0], bounds[1]);
	for (c = 0; c < 8; c++) {
		vec4_t point, viewPoint;

		VectorSet4(point, (c & 1) ? maxs[0] : mins[0], (c & 2) ? maxs[1] : mins[1], (c & 4) ? maxs[2] : mins[2], 1.0f);
		Mat4Transform(view, point, viewPoint);
		AddPointToBounds(viewPoint, bounds[0], bounds[1]);
	}
	rt_skyTexel = Q_max(rt_skyTexel, Q_max(bounds[1][1] - bounds[0][1], bounds[1][2] - bounds[0][2]) / size);

	Com_Memset(&parms, 0, sizeof(parms));
	parms.viewportX      = (k % RT_SKY_TILES) * size;
	parms.viewportY      = (k / RT_SKY_TILES) * size;
	parms.viewportWidth  = size;
	parms.viewportHeight = size;
	parms.fovX           = 90;
	parms.fovY           = 90;
	parms.targetFbo      = tr.rtSkyFbo;
	parms.flags          = VPF_DEPTHSHADOW | VPF_DEPTHCLAMP | VPF_ORTHOGRAPHIC | VPF_NOVIEWMODEL | VPF_RTSKY;
	parms.zFar           = bounds[1][0];
	VectorCopy(origin, parms.ori.origin);
	VectorCopy(axis[0], parms.ori.axis[0]);
	VectorCopy(axis[1], parms.ori.axis[1]);
	VectorCopy(axis[2], parms.ori.axis[2]);
	VectorCopy(origin, parms.pvsOrigin);

	// as the sun's cascades are drawn (R_RenderSunShadowMaps), with the
	// world alone
	tr.viewCount++;
	tr.viewParms                = parms;
	tr.viewParms.frameSceneNum = tr.frameSceneNum;
	tr.viewParms.frameCount    = tr.frameCount;
	firstDrawSurf               = tr.refdef.numDrawSurfs;
	firstSpriteSurf             = tr.refdef.numSpriteSurfs;
	tr.viewCount++;

	R_RotateForViewer();
	R_SetupProjectionOrtho(&tr.viewParms, bounds);
	R_AddWorldSurfaces();
	R_SortDrawSurfs(tr.refdef.drawSurfs + firstDrawSurf, tr.refdef.numDrawSurfs - firstDrawSurf,
		tr.refdef.spriteSurfs + firstSpriteSurf, tr.refdef.numSpriteSurfs - firstSpriteSurf);

	// the rows of the projection that give the tile's x, y and depth, for
	// the shaders, and the direction
	Mat4Multiply(tr.viewParms.projectionMatrix, tr.viewParms.world.modelMatrix, mvp);
	for (c = 0; c < 3; c++) {
		VectorSet4(tr.rtSkyRows[k * 4 + c], mvp[c], mvp[4 + c], mvp[8 + c], mvp[12 + c]);
	}
	VectorCopy(rt.skyDirs[k], tr.rtSkyRows[k * 4 + 3]);
	tr.rtSkyRows[k * 4 + 3][3] = 0;
	tr.rtSkyGeneration++;
}

/*
The sky each texel of a lightmap sees, as RtSky works it out for a pixel,
worked out once a map: a lightmapped surface then reads it as it reads its
lightmap (u_RtSurface.y 2), one fetch for the sky's sixteen. The surfaces of
each page are drawn into a page of their own, at their lightmap coordinates,
each texel from where it is on its surface (the terrain at its full detail);
the texels no surface covers take their neighbours', as the lightmap's own
filtering reads them at the edges.
*/
typedef struct {
	vec3_t          xyz;
	vec3_t          normal;
	vec2_t          lm;
	const shader_t *shader; // what it is, for the colour it bounces (R_RtBounce)
} rtBakeVert_t;

typedef struct {
	int    first, count; // its vertexes in the list
	vec3_t mins, maxs;
} rtBakeChunk_t;

typedef struct {
	rtBakeVert_t  *verts;
	int            num, max;
	rtBakeChunk_t *chunks; // a surface or a terrain patch each, kept when asked (R_RtBakeLamps)
	int            numChunks, maxChunks;
	qboolean       keepChunks;
} rtBakeList_t;

// The vertexes added since first, as one chunk of the list
static void R_RtBakeChunk(rtBakeList_t *list, int first)
{
	rtBakeChunk_t *c;
	int            v;

	if (!list->keepChunks || list->num <= first) {
		return;
	}
	if (list->numChunks == list->maxChunks) {
		rtBakeChunk_t *grown;

		list->maxChunks = Q_max(1024, list->maxChunks * 2);
		grown           = ri.Malloc(sizeof(rtBakeChunk_t) * list->maxChunks);
		if (list->chunks) {
			Com_Memcpy(grown, list->chunks, sizeof(rtBakeChunk_t) * list->numChunks);
			ri.Free(list->chunks);
		}
		list->chunks = grown;
	}
	c        = &list->chunks[list->numChunks++];
	c->first = first;
	c->count = list->num - first;
	ClearBounds(c->mins, c->maxs);
	for (v = first; v < list->num; v++) {
		AddPointToBounds(list->verts[v].xyz, c->mins, c->maxs);
	}
}

static void R_RtBakeAdd(rtBakeList_t *list, const vec3_t xyz, const vec3_t normal, float s, float t, const shader_t *shader)
{
	rtBakeVert_t *v;

	if (list->num == list->max) {
		rtBakeVert_t *grown;

		list->max = Q_max(4096, list->max * 2);
		grown     = ri.Malloc(sizeof(rtBakeVert_t) * list->max);
		if (list->verts) {
			Com_Memcpy(grown, list->verts, sizeof(rtBakeVert_t) * list->num);
			ri.Free(list->verts);
		}
		list->verts = grown;
	}
	v = &list->verts[list->num++];
	VectorCopy(xyz, v->xyz);
	VectorCopy(normal, v->normal);
	v->lm[0] = s;
	v->lm[1] = t;
	v->shader = shader;
}

// The triangles of the surfaces lit by one page of the lightmaps
static void R_RtBakeGather(int page, rtBakeList_t *list)
{
	int i, j, r, c, first;

	for (i = 0; i < tr.world->numWorldSurfaces; i++) {
		const msurface_t      *surf = &tr.world->surfaces[i];
		const srfBspSurface_t *bsp  = (const srfBspSurface_t *)surf->data;

		if (!surf->shader || surf->shader->lightmapIndex != page || !surf->data) {
			continue;
		}
		if (*surf->data != SF_FACE && *surf->data != SF_GRID && *surf->data != SF_TRIANGLES) {
			continue;
		}
		first = list->num;
		for (j = 0; j < bsp->numIndexes; j++) {
			srfVert_t *v = &bsp->verts[bsp->indexes[j]];
			vec3_t     normal;

			R_VaoUnpackNormal(normal, v->normal);
			R_RtBakeAdd(list, v->xyz, normal, v->lightmap[0], v->lightmap[1], surf->shader);
		}
		R_RtBakeChunk(list, first);
	}

	// the terrain, a 9 by 9 grid a patch, its lightmap spread over it as
	// R_PreTessellateTerrain spreads it: from the middle of its first texel
	// to the middle of its last, so that a patch's edge runs through the
	// middles of the texels along it, which the GPU leaves out of two of the
	// four edges of what it fills. Those texels took the mean of those round
	// them, the next chart's in the page among them, and half as bright as
	// they should be they drew a line along every patch's edge near a lamp
	// (the lamps' bake): its outer edges go half a texel further out.
	for (i = 0; i < tr.world->numTerraPatches; i++) {
		cTerraPatchUnpacked_t *patch = &tr.world->terraPatches[i];
		const float            size  = (float)(patch->drawinfo.lmapSize - 1) / 128.0f;
		const float            sizeS = tr.fatLightmapCols > 0 ? size / tr.fatLightmapCols : size;
		const float            sizeT = tr.fatLightmapRows > 0 ? size / tr.fatLightmapRows : size;
		const float            halfS = tr.lightmaps && tr.lightmaps[page] ? 0.5f / tr.lightmaps[page]->width : 0.0f;
		const float            halfT = tr.lightmaps && tr.lightmaps[page] ? 0.5f / tr.lightmaps[page]->height : 0.0f;

		if (!patch->shader || patch->shader->lightmapIndex != page) {
			continue;
		}
		first = list->num;
		for (r = 0; r < 8; r++) {
			for (c = 0; c < 8; c++) {
				static const int corner[6][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 1, 0 } };

				for (j = 0; j < 6; j++) {
					const int   rr = r + corner[j][0], cc = c + corner[j][1];
					const float outS = cc == 0 ? -1.0f : cc == 8 ? 1.0f : 0.0f;
					const float outT = rr == 0 ? -1.0f : rr == 8 ? 1.0f : 0.0f;
					vec3_t      xyz, normal;

					VectorSet(xyz, patch->x0 + cc * 64.0f, patch->y0 + rr * 64.0f, patch->heightmap[rr * 9 + cc] * 2.0f + patch->z0);
					// half a texel out, as far in the world as that is
					if (sizeS > 0 && sizeT > 0) {
						xyz[0] += outS * halfS * 512.0f / sizeS;
						xyz[1] += outT * halfT * 512.0f / sizeT;
					}
					R_VaoUnpackNormal(normal, patch->normals[rr * 9 + cc]);
					R_RtBakeAdd(list, xyz, normal, patch->s + sizeS * cc / 8.0f + outS * halfS, patch->t + sizeT * rr / 8.0f + outT * halfT,
						patch->shader);
				}
			}
		}
		R_RtBakeChunk(list, first);
	}
}

// The texels no surface covered (alpha 0) take the average of the covered
// ones around them, a ring at a time.
static void R_RtBakeDilate(byte *texels, int width, int height, int rings)
{
	byte *next = ri.Malloc(width * height * 4);
	int   n, x, y;

	for (n = 0; n < rings; n++) {
		Com_Memcpy(next, texels, width * height * 4);
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				int sum = 0, count = 0, dx, dy;

				if (texels[(y * width + x) * 4 + 3]) {
					continue;
				}
				for (dy = -1; dy <= 1; dy++) {
					for (dx = -1; dx <= 1; dx++) {
						const int nx = x + dx, ny = y + dy;

						if (nx < 0 || ny < 0 || nx >= width || ny >= height || !texels[(ny * width + nx) * 4 + 3]) {
							continue;
						}
						sum += texels[(ny * width + nx) * 4];
						count++;
					}
				}
				if (count) {
					next[(y * width + x) * 4]     = sum / count;
					next[(y * width + x) * 4 + 3] = 255;
				}
			}
		}
		Com_Memcpy(texels, next, width * height * 4);
	}
	ri.Free(next);
}

/*
=============================================================================

THE BOUNCE

With r_realtimeLighting 2 the lightmaps give way, and with them all the light
the compiler bounced: a room by day was its windows' patches of sun, its sky,
and the dark (once the spot lights that stood in for the daylight had gone,
R_RtDropWindowLights). So the light the sun, the sky and the lamps put on
what they reach is bounced once more, live, onto the rest: a sunlit floor
lights the ceiling over it, a lamp's pool the walls round it.

It is worked out once a map, on the lightmaps' first page, a texel for every
RT_BOUNCE_DOWN of theirs a side, and read where a pixel reads its sky
(u_RtBounceMap at rt_skyLM). Each texel is a sample: where its surface is,
which way it faces, and the colour of its texture (R_RtAlbedo):

  1. what lights each sample straight: the sun (a trace to the sky), the sky
     (as much of it as its sky lightmap texel sees), and each lamp in reach
     (a trace to it);
  2. what bounces onto each sample that is not out under the sky: rays over
     the half of the world it faces, each to the sample nearest where it hits
     (a hash of them), whose colour times its light comes back;
  3. once more, from what the second step gave each (two bounces).

It is done a few milliseconds a frame (r_rtBounceMsec), eased in when done,
and kept in rtbounce/<map>.bnc, where the next load reads it, as long as the
lights, the sun and the sky are as they were.
=============================================================================
*/

#define RT_BOUNCE_DOWN    4      // lightmap texels a side for a texel of the bounce
#define RT_BOUNCE_REACH   2048.0f
#define RT_BOUNCE_HASH    64.0f  // the size of the hash's cells: the samples are a lightmap texel
                                 // (16 units) RT_BOUNCE_DOWN apart on a big wall
#define RT_BOUNCE_OUTDOOR 0.6f   // seeing this much of the sky it is out under it: lit live
#define RT_BOUNCE_VERSION 8
#define RT_BOUNCE_VALUES  6 // each sample's bounce, the daylight's then the lamps'
#define RT_BOUNCE_FILE_SIZE(n) (20 + sizeof(float) * (RT_BOUNCE_VALUES + 6) * (n))
#define RT_BOUNCE_ALBEDO  0.35f  // the colour of what no sample is near

enum {
	RT_BOUNCE_IDLE,
	RT_BOUNCE_DIRECT,
	RT_BOUNCE_GATHER,
	RT_BOUNCE_DONE,
	RT_BOUNCE_OFF
};

typedef struct {
	vec3_t xyz;
	vec3_t normal;
	vec3_t albedo;
	float  sky;
	float  indoor; // R_RtBounceIndoor of it
	vec3_t direct; // raw lightmap units, as the sun's and the sky's colours are
	vec3_t first;  // what bounced onto it once
	vec3_t given;  // what it gives back: its colour times all of that, for the probes
	vec3_t directL; // and the same of the lamps' light, apart (RT_BOUNCE_LAMPS)
	vec3_t firstL;
	vec3_t givenL;
	int    cell;
} rtBounceSample_t;

static struct {
	int               state;
	int               width, height; // the bounce's texels
	rtBounceSample_t *samples;
	int               numSamples;
	int              *cellSample;    // each texel's sample, -1 none
	int              *hits;          // rays a sample: the sample each hit, -1 none
	int               rays;
	int               next;          // the next sample for the step in hand
	int64_t          *hashKeys;      // the samples by their hash cell
	int              *hashOrder;
	byte             *sky;           // the sky lightmap's first page, kept from its bake
	int               skyWidth, skyHeight;
	unsigned          key;           // what the lights were
	int               started, spent;
	int               misses;
	int               models;        // rays that stopped on a model's clip brush
	float             weight;
} rt_bounce;


/*
The bounce is for where the sky does not reach. Out under it, the sky's fill
(R_RtSetupSky gives it the sun's colour) already stands in for the light off
the ground and the walls round about: the bounce there as well lit dm/mohdm1's
courtyards twice, half as bright again and the shade gone from under the trees.
So it comes in as less of the sky is seen, all of it deep in a room.
*/
#define RT_BOUNCE_IN_FROM 0.05f // seeing this share of the sky it would out in the open, it is in a room: all of the bounce
#define RT_BOUNCE_IN_TO   0.25f // and this share (a courtyard, the street): none

/*
The lamps' light bounces apart from the daylight's (RT_BOUNCE_LAMPS): what
stands in for the daylight's out under the sky is the sky's fill, and lamp
light off the ground beside a lamp at night is no sky's. Its bounce is as it
would be, under a roof or in the street, and r_rtBounce's boost is the
daylight's alone: kept together, a prop by a lamp at the edge of the eaves
went from three times the bounce to none as it was nudged out from under.
*/

// How far in a point is (0 out under the sky, 1 deep in a room), by the share
// of the sky it sees of what it would see facing the same way in the open
static float R_RtBounceIndoor(float share)
{
	const float t = Q_min(1.0f, Q_max(0.0f, (share - RT_BOUNCE_IN_FROM) / (RT_BOUNCE_IN_TO - RT_BOUNCE_IN_FROM)));

	return 1.0f - t * t * (3.0f - 2.0f * t);
}

// The sky a surface facing normal sees in the open, as its sky lightmap counts it
// (RtSky, an open floor 1): a wall out under the sky sees not half as much
static float R_RtSkyOpen(const vec3_t normal)
{
	float sum = 0;
	int   k;

	for (k = 0; k < RT_SKY_DIRS; k++) {
		sum += Q_max(0.0f, DotProduct(normal, rt.skyDirs[k]));
	}
	return sum / (RT_SKY_DIRS * 0.5f);
}

// The bounce's scale at a point as far in as indoor (0 out under the sky, 1 deep in a room)
static float R_RtBounceScale(float indoor)
{
	return (r_rtBounce ? Q_max(0.0f, r_rtBounce->value) : 0.0f) * indoor;
}

static vec3_t rt_albedo[MAX_SHADERS];
static byte   rt_albedoDone[MAX_SHADERS];

// The sky lightmap's first page, which its bake hands over rather than frees
static void R_RtBounceKeepSky(byte *texels, int width, int height)
{
	if (rt_bounce.sky) {
		ri.Free(rt_bounce.sky);
	}
	rt_bounce.sky       = texels;
	rt_bounce.skyWidth  = width;
	rt_bounce.skyHeight = height;
}

static void R_RtProbesFree(void);

static void R_RtBounceFree(void)
{
	if (rt_bounce.samples) {
		ri.Free(rt_bounce.samples);
	}
	if (rt_bounce.cellSample) {
		ri.Free(rt_bounce.cellSample);
	}
	if (rt_bounce.hits) {
		ri.Free(rt_bounce.hits);
	}
	if (rt_bounce.hashKeys) {
		ri.Free(rt_bounce.hashKeys);
	}
	if (rt_bounce.hashOrder) {
		ri.Free(rt_bounce.hashOrder);
	}
	if (rt_bounce.sky) {
		ri.Free(rt_bounce.sky);
	}
	Com_Memset(&rt_bounce, 0, sizeof(rt_bounce));
	R_RtProbesFree();
	// the shaders are numbered again for the next map
	Com_Memset(rt_albedoDone, 0, sizeof(rt_albedoDone));
	if (tr.rtBounceLampImage) {
		R_FreeImage(tr.rtBounceLampImage);
		tr.rtBounceLampImage = NULL;
	}
	if (tr.rtBounceImage) {
		R_FreeImage(tr.rtBounceImage);
		tr.rtBounceImage = NULL;
	}
	VectorClear4(tr.rtBounce);
}

/*
The colour a shader's surface bounces: the average of its first texture, as
the GPU has it (a texture pack's DDS too), read back at a mipmap of a few
texels. Kept for each shader.
*/

static void R_RtAlbedo(const shader_t *shader, vec3_t out)
{
	int i;

	VectorSet(out, 0.4f, 0.4f, 0.4f);
	if (!shader) {
		return;
	}
	if (rt_albedoDone[shader->index]) {
		VectorCopy(rt_albedo[shader->index], out);
		return;
	}
	rt_albedoDone[shader->index] = 1;
	VectorCopy(out, rt_albedo[shader->index]);
	if (!qglGetTexImageOPM) {
		return;
	}
	for (i = 0; i < MAX_SHADER_STAGES && shader->stages[i]; i++) {
		const shaderStage_t *stage = shader->stages[i];
		image_t             *image = stage->bundle[0].image[0];
		int                  level = 0, w, h, k;
		byte                *pixels;
		float                sum[3] = { 0, 0, 0 }, alpha = 0;

		if (!stage->active || !image || stage->bundle[0].isLightmap || stage->bundle[0].tcGen == TCGEN_LIGHTMAP
			|| image == tr.whiteImage || image->imgName[0] == '*') {
			continue;
		}
		if (image->flags & IMGFLAG_MIPMAP) {
			while ((image->uploadWidth >> (level + 1)) >= 8 && (image->uploadHeight >> (level + 1)) >= 8) {
				level++;
			}
		}
		w      = Q_max(1, image->uploadWidth >> level);
		h      = Q_max(1, image->uploadHeight >> level);
		pixels = ri.Hunk_AllocateTempMemory(w * h * 4);
		GL_BindToTMU(image, TB_COLORMAP);
		qglGetTexImageOPM(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		for (k = 0; k < w * h; k++) {
			const float a = pixels[k * 4 + 3] / 255.0f;

			sum[0] += pixels[k * 4] * a;
			sum[1] += pixels[k * 4 + 1] * a;
			sum[2] += pixels[k * 4 + 2] * a;
			alpha  += a;
		}
		ri.Hunk_FreeTempMemory(pixels);
		if (alpha > 0) {
			for (k = 0; k < 3; k++) {
				out[k] = Com_Clamp(0.03f, 0.85f, sum[k] / (alpha * 255.0f));
			}
		}
		VectorCopy(out, rt_albedo[shader->index]);
		return;
	}
}

static int64_t R_RtBounceHashKey(const vec3_t p, int dx, int dy, int dz)
{
	const int64_t x = (int64_t)floor(p[0] / RT_BOUNCE_HASH) + dx + 32768;
	const int64_t y = (int64_t)floor(p[1] / RT_BOUNCE_HASH) + dy + 32768;
	const int64_t z = (int64_t)floor(p[2] / RT_BOUNCE_HASH) + dz + 32768;

	return (x << 40) | (y << 20) | z;
}

static int64_t *rt_sortKeys;
static int      rt_missByShader[4096]; // what the bounce's rays find no sample nor model on, by shader (developer)

static int R_RtBounceCompareKeys(const void *a, const void *b)
{
	const int64_t ka = rt_sortKeys[*(const int *)a], kb = rt_sortKeys[*(const int *)b];

	return ka < kb ? -1 : ka > kb ? 1 : 0;
}

// The sample nearest p facing about the way normal does, -1 none near
static int R_RtBounceNearest(const vec3_t p, const vec3_t normal)
{
	float best = Square(72.0f);
	int   found = -1, dx, dy, dz;

	for (dz = -1; dz <= 1; dz++) {
		for (dy = -1; dy <= 1; dy++) {
			for (dx = -1; dx <= 1; dx++) {
				const int64_t key = R_RtBounceHashKey(p, dx, dy, dz);
				int           lo = 0, hi = rt_bounce.numSamples, k;

				while (lo < hi) {
					const int mid = (lo + hi) / 2;

					if (rt_bounce.hashKeys[mid] < key) {
						lo = mid + 1;
					} else {
						hi = mid;
					}
				}
				for (k = lo; k < rt_bounce.numSamples && rt_bounce.hashKeys[k] == key; k++) {
					const rtBounceSample_t *s = &rt_bounce.samples[rt_bounce.hashOrder[k]];
					const float             d = DistanceSquared(s->xyz, p);

					if (d < best && DotProduct(s->normal, normal) > 0.5f) {
						best  = d;
						found = rt_bounce.hashOrder[k];
					}
				}
			}
		}
	}
	return found;
}

// The samples: each texel of the bounce its surfaces cover, from the
// triangles the sky's lightmaps were drawn from
static void R_RtBounceSamples(void)
{
	rtBakeList_t list;
	const int    w = rt_bounce.width, h = rt_bounce.height;
	int          i, max = w * h;

	Com_Memset(&list, 0, sizeof(list));
	R_RtBakeGather(0, &list);

	rt_bounce.samples    = ri.Malloc(sizeof(rtBounceSample_t) * Q_max(1, max));
	rt_bounce.cellSample = ri.Malloc(sizeof(int) * Q_max(1, max));
	for (i = 0; i < max; i++) {
		rt_bounce.cellSample[i] = -1;
	}

	for (i = 0; i + 2 < list.num; i += 3) {
		const rtBakeVert_t *v[3] = { &list.verts[i], &list.verts[i + 1], &list.verts[i + 2] };
		float               x[3], y[3], area;
		int                 x0, x1, y0, y1, cx, cy, k, inside = 0;

		for (k = 0; k < 3; k++) {
			x[k] = v[k]->lm[0] * w;
			y[k] = v[k]->lm[1] * h;
		}
		area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
		if (fabs(area) < 1e-8f) {
			continue;
		}
		x0 = Q_max(0, (int)floor(Q_min(x[0], Q_min(x[1], x[2]))));
		x1 = Q_min(w - 1, (int)ceil(Q_max(x[0], Q_max(x[1], x[2]))));
		y0 = Q_max(0, (int)floor(Q_min(y[0], Q_min(y[1], y[2]))));
		y1 = Q_min(h - 1, (int)ceil(Q_max(y[0], Q_max(y[1], y[2]))));
		for (cy = y0; cy <= y1 && cy <= y0 + 1024; cy++) {
			for (cx = x0; cx <= x1 && cx <= x0 + 1024; cx++) {
				const float px = cx + 0.5f, py = cy + 0.5f;
				float       b[3];

				b[0] = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
				b[1] = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
				b[2] = 1.0f - b[0] - b[1];
				if (b[0] < -0.01f || b[1] < -0.01f || b[2] < -0.01f) {
					continue;
				}
				inside++;
				if (rt_bounce.cellSample[cy * w + cx] < 0) {
					rtBounceSample_t *s = &rt_bounce.samples[rt_bounce.numSamples];

					Com_Memset(s, 0, sizeof(*s));
					for (k = 0; k < 3; k++) {
						VectorMA(s->xyz, b[k], v[k]->xyz, s->xyz);
						VectorMA(s->normal, b[k], v[k]->normal, s->normal);
					}
					VectorNormalize(s->normal);
					R_RtAlbedo(v[0]->shader, s->albedo);
					s->cell = cy * w + cx;
					rt_bounce.cellSample[s->cell] = rt_bounce.numSamples++;
				}
			}
		}
		// a triangle smaller than a texel: its middle
		if (!inside) {
			const int mx = (int)((x[0] + x[1] + x[2]) / 3), my = (int)((y[0] + y[1] + y[2]) / 3);

			if (mx >= 0 && my >= 0 && mx < w && my < h && rt_bounce.cellSample[my * w + mx] < 0) {
				rtBounceSample_t *s = &rt_bounce.samples[rt_bounce.numSamples];

				Com_Memset(s, 0, sizeof(*s));
				for (k = 0; k < 3; k++) {
					VectorMA(s->xyz, 1.0f / 3, v[k]->xyz, s->xyz);
					VectorMA(s->normal, 1.0f / 3, v[k]->normal, s->normal);
				}
				VectorNormalize(s->normal);
				R_RtAlbedo(v[0]->shader, s->albedo);
				s->cell = my * w + mx;
				rt_bounce.cellSample[s->cell] = rt_bounce.numSamples++;
			}
		}
	}
	if (list.verts) {
		ri.Free(list.verts);
	}

	// the sky each sees, from its sky lightmap texel
	for (i = 0; i < rt_bounce.numSamples; i++) {
		rtBounceSample_t *s = &rt_bounce.samples[i];

		if (rt_bounce.sky) {
			const int cx = s->cell % w, cy = s->cell / w;
			const int sx = Q_min(rt_bounce.skyWidth - 1, (int)((cx + 0.5f) * rt_bounce.skyWidth / w));
			const int sy = Q_min(rt_bounce.skyHeight - 1, (int)((cy + 0.5f) * rt_bounce.skyHeight / h));

			s->sky = rt_bounce.sky[(sy * rt_bounce.skyWidth + sx) * 4] / 255.0f;
		}
		{
			const float open = R_RtSkyOpen(s->normal);

			// what faces down sees none of it anywhere: under the eaves is in
			s->indoor = R_RtBounceIndoor(open > 0.05f ? s->sky / open : 0.0f);
		}
	}

	// the hash
	rt_bounce.hashKeys  = ri.Malloc(sizeof(int64_t) * Q_max(1, rt_bounce.numSamples));
	rt_bounce.hashOrder = ri.Malloc(sizeof(int) * Q_max(1, rt_bounce.numSamples));
	{
		int64_t *keys = ri.Malloc(sizeof(int64_t) * Q_max(1, rt_bounce.numSamples));

		for (i = 0; i < rt_bounce.numSamples; i++) {
			keys[i]                 = R_RtBounceHashKey(rt_bounce.samples[i].xyz, 0, 0, 0);
			rt_bounce.hashOrder[i] = i;
		}
		rt_sortKeys = keys;
		qsort(rt_bounce.hashOrder, rt_bounce.numSamples, sizeof(int), R_RtBounceCompareKeys);
		for (i = 0; i < rt_bounce.numSamples; i++) {
			rt_bounce.hashKeys[i] = keys[rt_bounce.hashOrder[i]];
		}
		ri.Free(keys);
	}
}

// What the sun and the sky (out) and the lamps (lamps) put straight on a
// point facing normal, which sees sky of the sky, raw
static void R_RtBounceDirectAt(const vec3_t xyz, const vec3_t normal, float sky, vec3_t out, vec3_t lamps)
{
	trace_t trace;
	// a little box, not a line: a line slipped through the seam where two
	// walls meet in a corner, and the floor there was lit by the moon in a
	// shut room, and lit the walls round it (m3l2)
	vec3_t  zero = {-1, -1, -1}, one = {1, 1, 1}, from, to;
	int     i;

	VectorMA(xyz, 3.0f, normal, from);
	VectorClear(out);
	VectorClear(lamps);
	if (rt.sun) {
		const float ndl = DotProduct(normal, rt.sunDir);

		if (ndl > 0) {
			VectorMA(from, 16384.0f, rt.sunDir, to);
			ri.CM_BoxTrace(&trace, from, to, zero, one, 0, CONTENTS_SOLID, qfalse);
			if (!trace.startsolid && (trace.fraction >= 1.0f || (trace.surfaceFlags & SURF_SKY))) {
				VectorMA(out, ndl, rt.sunColor, out);
			}
		}
	}
	VectorMA(out, sky * Q_max(0.0f, r_rtSky->value), rt.skyColor, out);

	for (i = 0; i < rt.numLights; i++) {
		const rtLight_t *l = &rt.lights[i];
		vec3_t           toLight;
		float            d2, d, ndl, fade, att;

		if (l->range <= 0 || l->entity >= 0) {
			continue;
		}
		VectorSubtract(l->origin, xyz, toLight);
		d2 = DotProduct(toLight, toLight);
		if (d2 >= Square(l->range)) {
			continue;
		}
		d   = sqrt(d2);
		ndl = DotProduct(normal, toLight) / Q_max(d, 1e-3f);
		if (ndl <= 0) {
			continue;
		}
		fade = 1.0f - d2 / Square(l->range);
		att  = ndl * fade * fade / Q_max(d2, l->lamp ? Square(RT_LAMP_NEAR) : l->area > 0 ? Square(l->area) : 256.0f);
		if (l->area > 0) {
			// less the further from straight out of its surface
			att *= Q_max(0.0f, -DotProduct(toLight, l->spotDir) / Q_max(d, 1e-3f));
			if (att <= 0) {
				continue;
			}
		}
		if (l->spot) {
			const float along = -DotProduct(toLight, l->spotDir);
			vec3_t      off;

			if (along <= 0.001f) {
				continue;
			}
			VectorMA(toLight, along, l->spotDir, off);
			if (VectorLength(off) >= l->radiusByDist * along) {
				continue;
			}
		}
		if (Q_max(l->color[0], Q_max(l->color[1], l->color[2])) * att * r_rtScale->value < 0.5f) {
			continue;
		}
		if (!l->noShadow) {
			ri.CM_BoxTrace(&trace, from, l->origin, zero, one, 0, CONTENTS_SOLID, qfalse);
			if (trace.startsolid || trace.fraction < 0.98f) {
				continue;
			}
		}
		VectorMA(lamps, att * r_rtScale->value, l->color, lamps);
	}
}

static void R_RtBounceDirect(rtBounceSample_t *s)
{
	R_RtBounceDirectAt(s->xyz, s->normal, s->sky, s->direct, s->directL);
}

// A point no sample is near (a model, a door): how much of the sky is over
// it, by a ray up the way it faces
static float R_RtBounceSkyAt(const vec3_t xyz, const vec3_t normal)
{
	trace_t trace;
	vec3_t  zero = {0, 0, 0}, from, dir, to;

	VectorSet(dir, normal[0], normal[1], normal[2] + 1.0f);
	if (VectorNormalize(dir) <= 0) {
		return 0;
	}
	VectorMA(xyz, 2.0f, normal, from);
	VectorMA(from, 16384.0f, dir, to);
	ri.CM_BoxTrace(&trace, from, to, zero, zero, 0, CONTENTS_SOLID, qfalse);
	return (!trace.startsolid && (trace.fraction >= 1.0f || (trace.surfaceFlags & SURF_SKY))) ? 0.5f * dir[2] : 0.0f;
}

// The sample nearest p, of any way it faces but towards the ray that came
// from dir: what is seen where a ray stopped on a face never drawn (caulk
// behind a curve, the edge of the terrain), -1 none near
static int R_RtBounceNearestSeen(const vec3_t p, const vec3_t dir)
{
	float best = Square(96.0f);
	int   found = -1, dx, dy, dz;

	for (dz = -1; dz <= 1; dz++) {
		for (dy = -1; dy <= 1; dy++) {
			for (dx = -1; dx <= 1; dx++) {
				const int64_t key = R_RtBounceHashKey(p, dx, dy, dz);
				int           lo = 0, hi = rt_bounce.numSamples, k;

				while (lo < hi) {
					const int mid = (lo + hi) / 2;

					if (rt_bounce.hashKeys[mid] < key) {
						lo = mid + 1;
					} else {
						hi = mid;
					}
				}
				for (k = lo; k < rt_bounce.numSamples && rt_bounce.hashKeys[k] == key; k++) {
					const rtBounceSample_t *s = &rt_bounce.samples[rt_bounce.hashOrder[k]];
					const float             d = DistanceSquared(s->xyz, p);

					if (d < best && DotProduct(s->normal, dir) < -0.2f) {
						best  = d;
						found = rt_bounce.hashOrder[k];
					}
				}
			}
		}
	}
	return found;
}

// The colour of the static model a point is in (a ray stopped on the clip
// brush that stands in for a chair, a hedge), and whether there is one
static qboolean R_RtModelAlbedoAt(const vec3_t p, vec3_t albedo)
{
	float best = 1e30f;
	int   i, found = -1;

	for (i = 0; i < tr.world->numStaticModels; i++) {
		const cStaticModelUnpacked_t *sm = &tr.world->staticModels[i];
		float                         d;

		if (!sm->tiki || sm->cull_radius <= 0) {
			continue;
		}
		d = DistanceSquared(sm->origin, p);
		if (d < Square(sm->cull_radius + 16.0f) && d < best) {
			best  = d;
			found = i;
		}
	}
	if (found < 0) {
		return qfalse;
	}
	{
		const dtiki_t *tiki = tr.world->staticModels[found].tiki;
		int            k;

		for (k = 0; k < tiki->num_surfaces; k++) {
			const dtikisurface_t *surf = &tiki->surfaces[k];

			if (surf->numskins && surf->hShader[0] > 0 && surf->hShader[0] < tr.numShaders) {
				R_RtAlbedo(tr.shaders[surf->hShader[0]], albedo);
				return qtrue;
			}
		}
	}
	return qfalse;
}

/*
What comes back along a ray from dir that stopped where trace says: what the
sample there gives back; on a face never drawn, the sample seen behind it;
on the clip brush round a model, the light there times the model's colour;
else the light there times RT_BOUNCE_ALBEDO. Its sample, -1 none.
*/
static int R_RtBounceRadiance(const trace_t *trace, const vec3_t dir, qboolean withFirst, vec3_t out, vec3_t outL)
{
	vec3_t albedo;
	int    hit = -1;

	if (!(trace->surfaceFlags & SURF_NODRAW)) {
		hit = R_RtBounceNearest(trace->endpos, trace->plane.normal);
	}
	if (hit < 0) {
		hit = R_RtBounceNearestSeen(trace->endpos, dir);
	}
	if (hit >= 0) {
		const rtBounceSample_t *o = &rt_bounce.samples[hit];

		if (withFirst) {
			VectorCopy(o->given, out);
			VectorCopy(o->givenL, outL);
		} else {
			VectorCopy(o->direct, out);
			VectorCopy(o->directL, outL);
			out[0] *= o->albedo[0];
			out[1] *= o->albedo[1];
			out[2] *= o->albedo[2];
			outL[0] *= o->albedo[0];
			outL[1] *= o->albedo[1];
			outL[2] *= o->albedo[2];
		}
		return hit;
	}
	R_RtBounceDirectAt(trace->endpos, trace->plane.normal, R_RtBounceSkyAt(trace->endpos, trace->plane.normal), out, outL);
	if ((trace->surfaceFlags & SURF_NODRAW) && R_RtModelAlbedoAt(trace->endpos, albedo)) {
		out[0] *= albedo[0];
		out[1] *= albedo[1];
		out[2] *= albedo[2];
		outL[0] *= albedo[0];
		outL[1] *= albedo[1];
		outL[2] *= albedo[2];
		rt_bounce.models++;
	} else {
		VectorScale(out, RT_BOUNCE_ALBEDO, out);
		VectorScale(outL, RT_BOUNCE_ALBEDO, outL);
		rt_bounce.misses++;
		if (trace->shaderNum >= 0 && trace->shaderNum < 4096) {
			rt_missByShader[trace->shaderNum]++;
		}
	}
	return -1;
}

// What bounces onto a sample once: rays over the half of the world it faces
static void R_RtBounceGather(int index)
{
	rtBounceSample_t *s    = &rt_bounce.samples[index];
	int              *hits = &rt_bounce.hits[index * rt_bounce.rays];
	vec3_t            zero = {0, 0, 0}, from, t1, t2;
	float             spin;
	int               r;

	for (r = 0; r < rt_bounce.rays; r++) {
		hits[r] = -1;
	}
	VectorClear(s->first);
	VectorClear(s->firstL);
	VectorMA(s->xyz, 2.0f, s->normal, from);
	PerpendicularVector(t1, s->normal);
	CrossProduct(s->normal, t1, t2);
	spin = (float)((index * 2654435761u) & 0xffff) / 65536.0f;

	for (r = 0; r < rt_bounce.rays; r++) {
		// cosine weighted: a point of a disc lifted onto the half sphere
		const float u   = (r + 0.5f) / rt_bounce.rays;
		const float phi = 2.0f * M_PI * (r * 0.618034f + spin);
		const float rad = sqrt(u);
		vec3_t      dir, to;
		trace_t     trace;
		int         hit;

		VectorScale(s->normal, sqrt(1.0f - u), dir);
		VectorMA(dir, rad * cos(phi), t1, dir);
		VectorMA(dir, rad * sin(phi), t2, dir);
		VectorMA(from, RT_BOUNCE_REACH, dir, to);
		ri.CM_BoxTrace(&trace, from, to, zero, zero, 0, CONTENTS_SOLID, qfalse);
		if (trace.startsolid || trace.fraction >= 1.0f || (trace.surfaceFlags & SURF_SKY)) {
			continue;
		}
		{
			vec3_t given, givenL;

			// a sample's is counted again for the second bounce; what had
			// none (a model) once
			hit     = R_RtBounceRadiance(&trace, dir, qfalse, given, givenL);
			hits[r] = hit;
			VectorMA(s->first, 1.0f / rt_bounce.rays, given, s->first);
			VectorMA(s->firstL, 1.0f / rt_bounce.rays, givenL, s->firstL);
		}
	}
	// out under the sky its fill stands in for the daylight's bounce, not
	// for the lamps' (which is why the rays are traced out there at all)
	if (s->sky >= RT_BOUNCE_OUTDOOR) {
		VectorClear(s->first);
	}
}


/*
The bounce for what has no lightmap (people, furniture, the lamps): probes on
a grid of RT_PROBE_GRID, each the light coming to it from every side, as the
samples give it back (what lights them and what bounced onto them), worked
out where a model is drawn and kept for the map. A model takes the eight
round where it is, as m + v . N for each colour (u_RtProbe).
*/
#define RT_PROBE_GRID  64.0f
#define RT_PROBE_SLOTS 65536 // a power of two
#define RT_PROBE_RAYS  48
#define RT_PROBE_QUEUE 4096
#define RT_PROBE_FADE  48.0f // a probe's line to a model stopping this far past its reach short of it: none of its light

enum {
	RT_PROBE_EMPTY,
	RT_PROBE_WANTED,
	RT_PROBE_READY,
	RT_PROBE_SOLID
};

typedef struct {
	int    x, y, z;
	int    state;
	vec4_t c[3]; // r, g, b: the way it is brightest (x y z), the mean (w)
	vec4_t cl[3]; // the same of the lamps' light, which bounces as it would wherever it is
	float  indoor; // R_RtBounceIndoor of the share of the sky it sees over it
} rtProbe_t;

static rtProbe_t *rt_probes;
static int        rt_probeQueue[RT_PROBE_QUEUE];
static int        rt_probeQueued;

static void R_RtProbesFree(void)
{
	if (rt_probes) {
		ri.Free(rt_probes);
		rt_probes = NULL;
	}
	rt_probeQueued = 0;
}

// The probe at x y z, wanted if it is not there yet; NULL with the table full
static rtProbe_t *R_RtProbe(int x, int y, int z, qboolean want)
{
	unsigned h = ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u) ^ ((unsigned)z * 83492791u);
	int      n;

	if (!rt_probes) {
		return NULL;
	}
	for (n = 0; n < 64; n++) {
		const int  slot = (h + n) & (RT_PROBE_SLOTS - 1);
		rtProbe_t *p    = &rt_probes[slot];

		if (p->state == RT_PROBE_EMPTY) {
			if (!want || rt_probeQueued >= RT_PROBE_QUEUE) {
				return NULL;
			}
			p->x     = x;
			p->y     = y;
			p->z     = z;
			p->state = RT_PROBE_WANTED;
			rt_probeQueue[rt_probeQueued++] = slot;
			return p;
		}
		if (p->x == x && p->y == y && p->z == z) {
			return p;
		}
	}
	return NULL;
}

static void R_RtProbeCompute(rtProbe_t *p)
{
	vec3_t         at, zero = {0, 0, 0};
	const mnode_t *leaf;
	float          up, open; // the rays up the sky, as a lightmap's sky counts them
	int            r, k, inside = 0;

	VectorSet(at, p->x * RT_PROBE_GRID, p->y * RT_PROBE_GRID, p->z * RT_PROBE_GRID);
	leaf = R_PointInLeaf(at);
	if (!leaf || leaf->cluster < 0) {
		p->state = RT_PROBE_SOLID;
		return;
	}
	// Under the ground: the terrain is no solid of the BSP's, and a probe a
	// little under it saw nothing but its underside, black, and lit what
	// stood over it so wherever the ground rose over the row of probes at its
	// height. A floor facing up just over it, that is not the top of a slab
	// whose underside it sees first, is the ground it is under.
	{
		trace_t below, over;
		vec3_t  above;

		VectorCopy(at, above);
		above[2] += RT_PROBE_GRID;
		ri.CM_BoxTrace(&below, above, at, zero, zero, 0, CONTENTS_SOLID, qfalse);
		if (!below.startsolid && below.fraction < 1.0f && below.plane.normal[2] > 0.5f) {
			ri.CM_BoxTrace(&over, at, above, zero, zero, 0, CONTENTS_SOLID, qfalse);
			if (over.startsolid || over.fraction >= 1.0f || over.plane.normal[2] > -0.5f || over.endpos[2] > below.endpos[2] + 1.0f) {
				p->state = RT_PROBE_SOLID;
				return;
			}
		}
	}
	for (k = 0; k < 3; k++) {
		VectorClear4(p->c[k]);
		VectorClear4(p->cl[k]);
	}
	up = open = 0;
	for (r = 0; r < RT_PROBE_RAYS; r++) {
		// spread over the sphere: a spiral of even area
		const float z   = 1.0f - (2.0f * r + 1.0f) / RT_PROBE_RAYS;
		const float rad = sqrt(Q_max(0.0f, 1.0f - z * z));
		const float phi = r * 2.399963f;
		vec3_t      dir, to, given, givenL;
		trace_t     trace;

		VectorSet(dir, rad * cos(phi), rad * sin(phi), z);
		VectorMA(at, RT_BOUNCE_REACH, dir, to);
		ri.CM_BoxTrace(&trace, at, to, zero, zero, 0, CONTENTS_SOLID, qfalse);
		inside += trace.startsolid;
		if (z > 0) {
			up += z;
		}
		if (trace.startsolid || trace.fraction >= 1.0f || (trace.surfaceFlags & SURF_SKY)) {
			if (z > 0 && !trace.startsolid) {
				open += z;
			}
			continue;
		}
		R_RtBounceRadiance(&trace, dir, qtrue, given, givenL);
		// what comes from dir: m + v . N over the sphere (a cosine's two first terms)
		for (k = 0; k < 3; k++) {
			p->c[k][3] += given[k] / RT_PROBE_RAYS;
			VectorMA(p->c[k], 2.0f * given[k] / RT_PROBE_RAYS, dir, p->c[k]);
			p->cl[k][3] += givenL[k] / RT_PROBE_RAYS;
			VectorMA(p->cl[k], 2.0f * givenL[k] / RT_PROBE_RAYS, dir, p->cl[k]);
		}
	}
	// In a wall: the BSP's leaves leave the detail brushes out, and a probe
	// in one saw nothing, black, "indoors" for no sky, and darkened what
	// stood by the wall all at once as it was nudged into its cell
	if (inside > RT_PROBE_RAYS / 2) {
		p->state = RT_PROBE_SOLID;
		return;
	}
	p->indoor = R_RtBounceIndoor(up > 0 ? open / up : 0.0f);
	p->state  = RT_PROBE_READY;
}

// The probes wanted since, as many as there is time for
static void R_RtProbesWork(int start)
{
	while (rt_probeQueued > 0 && ri.Milliseconds() - start < r_rtBounceMsec->integer) {
		rtProbe_t *p = &rt_probes[rt_probeQueue[--rt_probeQueued]];

		if (p->state == RT_PROBE_WANTED) {
			R_RtProbeCompute(p);
		}
	}
}

// The bounce at a model's origin, from the eight probes round it, those not
// worked out yet wanted
static qboolean R_RtProbesAt(const vec3_t origin, vec4_t out[3])
{
	vec3_t g;
	int    base[3], c, k;
	float  total = 0;

	for (k = 0; k < 3; k++) {
		VectorClear4(out[k]);
		g[k]    = origin[k] / RT_PROBE_GRID;
		base[k] = (int)floor(g[k]);
		g[k] -= base[k];
	}
	for (c = 0; c < 8; c++) {
		const int  dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
		const float w = (dx ? g[0] : 1 - g[0]) * (dy ? g[1] : 1 - g[1]) * (dz ? g[2] : 1 - g[2]);
		rtProbe_t *p = R_RtProbe(base[0] + dx, base[1] + dy, base[2] + dz, qtrue);

		if (!p || p->state != RT_PROBE_READY || w <= 0) {
			continue;
		}
		for (k = 0; k < 3; k++) {
			VectorMA4(out[k], w * R_RtBounceScale(p->indoor), p->c[k], out[k]);
			VectorMA4(out[k], w, p->cl[k], out[k]);
		}
		total += w;
	}
	if (total <= 0) {
		return qfalse;
	}
	for (k = 0; k < 3; k++) {
		VectorScale4(out[k], 1.0f / total, out[k]);
	}
	return qtrue;
}

// The middle of a static model, where its bounce is taken: not its origin,
// which a model may have at a corner, or inside the wall it stands against;
// and how far it reaches from it
static float R_RtStaticModelMiddle(const cStaticModelUnpacked_t *sm, vec3_t out)
{
	vec3_t local, half;
	int    k;

	VectorCopy(sm->origin, out);
	if (!sm->tiki || !sm->tiki->a) {
		out[2] += 8.0f;
		return 16.0f;
	}
	for (k = 0; k < 3; k++) {
		local[k] = (sm->tiki->a->mins[k] + sm->tiki->a->maxs[k]) * 0.5f * sm->tiki->load_scale * sm->scale;
		half[k]  = (sm->tiki->a->maxs[k] - sm->tiki->a->mins[k]) * 0.5f * sm->tiki->load_scale * sm->scale;
	}
	for (k = 0; k < 3; k++) {
		VectorMA(out, local[k], sm->axis[k], out);
	}
	return VectorLength(half) + 2.0f;
}

// How far in a room a point is (R_RtBounceIndoor), by how much of the sky
// over it is seen, as a probe works it out for itself
static float R_RtIndoorAt(const vec3_t at)
{
	vec3_t zero = {0, 0, 0};
	float  up = 0, open = 0;
	int    r;

	for (r = 0; r < RT_PROBE_RAYS / 2; r++) {
		// the upper half of the probes' spiral
		const float z   = 1.0f - (2.0f * r + 1.0f) / RT_PROBE_RAYS;
		const float rad = sqrt(Q_max(0.0f, 1.0f - z * z));
		const float phi = r * 2.399963f;
		vec3_t      dir, to;
		trace_t     trace;

		VectorSet(dir, rad * cos(phi), rad * sin(phi), z);
		VectorMA(at, RT_BOUNCE_REACH, dir, to);
		ri.CM_BoxTrace(&trace, at, to, zero, zero, 0, CONTENTS_SOLID, qfalse);
		up += z;
		if (!trace.startsolid && (trace.fraction >= 1.0f || (trace.surfaceFlags & SURF_SKY))) {
			open += z;
		}
	}
	return R_RtBounceIndoor(up > 0 ? open / up : 0.0f);
}

/*
The bounce at a static model's middle: the eight probes round it, blended by
how near, those alone that a line from reaches it, or stops no further than
reach short of it (it stands in a clip box of its own size, or against a
wall). None of them seeing it: all eight. qfalse while one of them is not
worked out yet (*waiting) or none is in the world.
*/
static qboolean R_RtProbesRound(const vec3_t at, float reach, vec4_t out[3], qboolean *waiting)
{
	vec3_t g, zero = {0, 0, 0};
	vec4_t lamps[3];
	int    base[3], c, k, pass;
	float  total = 0, dayScale;

	*waiting = qfalse;
	for (k = 0; k < 3; k++) {
		g[k]    = at[k] / RT_PROBE_GRID;
		base[k] = (int)floor(g[k]);
		g[k] -= base[k];
	}
	for (pass = 0; pass < 2 && total <= 0; pass++) {
		for (k = 0; k < 3; k++) {
			VectorClear4(out[k]);
			VectorClear4(lamps[k]);
		}
		for (c = 0; c < 8; c++) {
			const int   dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
			const float w  = (dx ? g[0] : 1 - g[0]) * (dy ? g[1] : 1 - g[1]) * (dz ? g[2] : 1 - g[2]);
			rtProbe_t  *p  = R_RtProbe(base[0] + dx, base[1] + dy, base[2] + dz, qtrue);
			float       vis;

			if (!p || w <= 0) {
				continue;
			}
			if (p->state == RT_PROBE_WANTED) {
				*waiting = qtrue;
				continue;
			}
			if (p->state != RT_PROBE_READY) {
				continue;
			}
			// less as the line from it stops further short of it: a small move
			// is no probe gone all at once
			vis = 1.0f;
			if (pass == 0) {
				trace_t trace;
				vec3_t  from;

				VectorSet(from, p->x * RT_PROBE_GRID, p->y * RT_PROBE_GRID, p->z * RT_PROBE_GRID);
				ri.CM_BoxTrace(&trace, from, at, zero, zero, 0, CONTENTS_SOLID, qfalse);
				if (trace.startsolid) {
					continue;
				}
				if (trace.fraction < 1.0f) {
					const float x = Com_Clamp(0.0f, 1.0f, (Distance(trace.endpos, at) - reach) / RT_PROBE_FADE);

					vis = 1.0f - x * x * (3.0f - 2.0f * x);
				}
				if (vis <= 0) {
					continue;
				}
			}
			for (k = 0; k < 3; k++) {
				VectorMA4(out[k], w * vis, p->c[k], out[k]);
				VectorMA4(lamps[k], w * vis, p->cl[k], lamps[k]);
			}
			total += w * vis;
		}
	}
	if (total <= 0) {
		return qfalse;
	}
	// the daylight's as far in a room as the model is, not as each probe
	// round it: those by the ground see little sky past the bumps of it, and
	// a prop on open ground took three times the bounce from them, and none
	// from those over them, as it was nudged from one to the others
	dayScale = R_RtBounceScale(R_RtIndoorAt(at));
	for (k = 0; k < 3; k++) {
		VectorScale4(out[k], dayScale, out[k]);
		VectorAdd4(out[k], lamps[k], out[k]);
		VectorScale4(out[k], 1.0f / total, out[k]);
	}
	return qtrue;
}

// A static model's bounce, worked out once
static qboolean R_RtStaticModelProbe(cStaticModelUnpacked_t *sm, vec4_t out[3])
{
	// The probes round its middle, blended (R_RtProbesRound), the same as it
	// stands and knocked about: those it took that "saw it", farther out
	// where a wall or its clip box hid the near ones, were out in the lit
	// street from a dark corner, and as it turned it went from lit to dark
	if (sm->moved) {
		vec3_t   middle;
		qboolean waiting;
		float    reach = R_RtStaticModelMiddle(sm, middle);

		return R_RtProbesRound(middle, reach, out, &waiting);
	}
	if (sm->rtProbeState == 0) {
		vec3_t   middle;
		qboolean waiting, on;
		float    reach = R_RtStaticModelMiddle(sm, middle);

		on = R_RtProbesRound(middle, reach, sm->rtProbe, &waiting);
		if (waiting) {
			// asked for; looked at again when they are worked out
			Com_Memcpy(out, sm->rtProbe, sizeof(sm->rtProbe));
			return on;
		}
		sm->rtProbeState = on ? 1 : 2;
	}
	Com_Memcpy(out, sm->rtProbe, sizeof(sm->rtProbe));
	return sm->rtProbeState == 1;
}

// r_rtProbeInfo <static model>: where its bounce comes from
static void R_RtProbeInfo_f(void)
{
	cStaticModelUnpacked_t *sm;
	vec3_t                  middle;
	float                   reach;
	int                     n = atoi(ri.Cmd_Argv(1));

	if (!tr.world || n < 0 || n >= tr.world->numStaticModels) {
		ri.Printf(PRINT_ALL, "r_rtProbeInfo <static model number>\n");
		return;
	}
	sm = &tr.world->staticModels[n];
	reach = R_RtStaticModelMiddle(sm, middle);
	ri.Printf(PRINT_ALL, "%s origin %.0f %.0f %.0f middle %.0f %.0f %.0f reach %.0f, state %d\n", sm->model, sm->origin[0], sm->origin[1],
		sm->origin[2], middle[0], middle[1], middle[2], reach, sm->rtProbeState);
	ri.Printf(PRINT_ALL, "  mean %.2f %.2f %.2f, toward %.2f %.2f %.2f (red)\n", sm->rtProbe[0][3], sm->rtProbe[1][3], sm->rtProbe[2][3],
		sm->rtProbe[0][0], sm->rtProbe[0][1], sm->rtProbe[0][2]);
	// the eight probes round its middle, as R_RtProbesRound takes them now
	{
		vec3_t   g, zero = {0, 0, 0};
		vec4_t   out[3];
		int      base[3], c, k;
		qboolean waiting, on;

		for (k = 0; k < 3; k++) {
			g[k]    = middle[k] / RT_PROBE_GRID;
			base[k] = (int)floor(g[k]);
			g[k] -= base[k];
		}
		for (c = 0; c < 8; c++) {
			const int   dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
			const float w  = (dx ? g[0] : 1 - g[0]) * (dy ? g[1] : 1 - g[1]) * (dz ? g[2] : 1 - g[2]);
			rtProbe_t  *p  = R_RtProbe(base[0] + dx, base[1] + dy, base[2] + dz, qfalse);
			trace_t     trace;
			vec3_t      from;

			if (!p) {
				ri.Printf(PRINT_ALL, "  probe %d: none (weight %.2f)\n", c, w);
				continue;
			}
			VectorSet(from, p->x * RT_PROBE_GRID, p->y * RT_PROBE_GRID, p->z * RT_PROBE_GRID);
			ri.CM_BoxTrace(&trace, from, middle, zero, zero, 0, CONTENTS_SOLID, qfalse);
			ri.Printf(PRINT_ALL, "  probe %d at (%.0f %.0f %.0f): state %d weight %.2f indoor %.2f mean %.2f %.2f %.2f lamps %.2f %.2f %.2f; line %s %.2f, stops %.0f short\n", c,
				from[0], from[1], from[2], p->state, w, p->indoor, p->c[0][3], p->c[1][3], p->c[2][3], p->cl[0][3], p->cl[1][3], p->cl[2][3],
				trace.startsolid ? "startsolid" : "", trace.fraction, Distance(trace.endpos, middle));
		}
		on = R_RtProbesRound(middle, reach, out, &waiting);
		ri.Printf(PRINT_ALL, "  blended: %s%s mean %.2f %.2f %.2f, toward %.2f %.2f %.2f (red); moved %d\n", on ? "on" : "off", waiting ? " (waiting)" : "",
			out[0][3], out[1][3], out[2][3], out[0][0], out[0][1], out[0][2], sm->moved);
	}
}

// r_rtProbeTest x y z: whether a probe there is buried or in a wall, step by step
static void R_RtProbeTest_f(void)
{
	vec3_t         at, above, zero = {0, 0, 0};
	trace_t        below, over;
	const mnode_t *leaf;

	VectorSet(at, atof(ri.Cmd_Argv(1)), atof(ri.Cmd_Argv(2)), atof(ri.Cmd_Argv(3)));
	leaf = R_PointInLeaf(at);
	ri.Printf(PRINT_ALL, "probe at %.0f %.0f %.0f: leaf cluster %d\n", at[0], at[1], at[2], leaf ? leaf->cluster : -9);
	VectorCopy(at, above);
	above[2] += RT_PROBE_GRID;
	ri.CM_BoxTrace(&below, above, at, zero, zero, 0, CONTENTS_SOLID, qfalse);
	ri.Printf(PRINT_ALL, "  down from +%.0f: startsolid %d fraction %.3f end z %.1f normal %.2f %.2f %.2f contents %d\n", RT_PROBE_GRID, below.startsolid,
		below.fraction, below.endpos[2], below.plane.normal[0], below.plane.normal[1], below.plane.normal[2], below.contents);
	ri.CM_BoxTrace(&over, at, above, zero, zero, 0, CONTENTS_SOLID, qfalse);
	ri.Printf(PRINT_ALL, "  up: startsolid %d fraction %.3f end z %.1f normal %.2f %.2f %.2f contents %d\n", over.startsolid, over.fraction, over.endpos[2],
		over.plane.normal[0], over.plane.normal[1], over.plane.normal[2], over.contents);
}

// What the lights, the sun and the sky were, for the kept bounce
static unsigned R_RtBounceKey(void)
{
	unsigned h = 2166136261u;
	float    v[20];
	int      i, k, n;

#define RT_HASH(x, count) for (k = 0; k < (int)(count) * (int)sizeof(float); k++) { h = (h ^ ((const byte *)(x))[k]) * 16777619u; }
	n = 0;
	v[n++] = RT_BOUNCE_VERSION;
	v[n++] = (float)rt_bounce.width;
	v[n++] = (float)rt_bounce.height;
	v[n++] = (float)rt_bounce.numSamples;
	v[n++] = (float)rt_bounce.rays;
	v[n++] = r_rtSky->value;
	v[n++] = r_rtScale->value;
	VectorCopy(rt.sunDir, &v[n]);
	n += 3;
	VectorCopy(rt.sunColor, &v[n]);
	n += 3;
	VectorCopy(rt.skyColor, &v[n]);
	n += 3;
	RT_HASH(v, n);
	for (i = 0; i < rt.numMapLights; i++) {
		RT_HASH(rt.lights[i].origin, 3);
		RT_HASH(rt.lights[i].color, 3);
		RT_HASH(&rt.lights[i].range, 1);
	}
#undef RT_HASH
	return h;
}

static const char *R_RtBounceFile(void)
{
	return va("rtbounce/%s.bnc", tr.world->baseName);
}

// The bounce made into its image, each texel no sample covers taking its neighbours'
// The bounce's texture, at the lightmap's own size: each texel from the
// samples round where it is (R_RtBounceAround), not one sample a texel, which
// spread one over faces apart (a wall and the side of its window) and into
// the next chart of the page
static qboolean R_RtBounceAround(const vec3_t xyz, const vec3_t normal, const float *values, vec4_t out);

// One of the bounce's images: the daylight's (offset 0, with how far in each
// texel is in its alpha) or the lamps' (offset 3)
static image_t *R_RtBounceImageOf(const float *values, int offset, const char *name)
{
	const int    w = tr.lightmaps[0]->width, h = tr.lightmaps[0]->height;
	float       *texels = ri.Malloc(sizeof(float) * 4 * w * h);
	float       *next   = ri.Malloc(sizeof(float) * 4 * w * h);
	rtBakeList_t list;
	int          i, n, x, y;

	Com_Memset(texels, 0, sizeof(float) * 4 * w * h);
	Com_Memset(&list, 0, sizeof(list));
	R_RtBakeGather(0, &list);
	for (i = 0; i + 2 < list.num; i += 3) {
		const rtBakeVert_t *v[3] = { &list.verts[i], &list.verts[i + 1], &list.verts[i + 2] };
		float               tx[3], ty[3], area;
		int                 x0, x1, y0, y1, k;

		for (k = 0; k < 3; k++) {
			tx[k] = v[k]->lm[0] * w;
			ty[k] = v[k]->lm[1] * h;
		}
		area = (tx[1] - tx[0]) * (ty[2] - ty[0]) - (tx[2] - tx[0]) * (ty[1] - ty[0]);
		if (fabs(area) < 1e-8f) {
			continue;
		}
		x0 = Q_max(0, (int)floor(Q_min(tx[0], Q_min(tx[1], tx[2]))));
		x1 = Q_min(w - 1, (int)ceil(Q_max(tx[0], Q_max(tx[1], tx[2]))));
		y0 = Q_max(0, (int)floor(Q_min(ty[0], Q_min(ty[1], ty[2]))));
		y1 = Q_min(h - 1, (int)ceil(Q_max(ty[0], Q_max(ty[1], ty[2]))));
		for (y = y0; y <= y1 && y <= y0 + 4096; y++) {
			for (x = x0; x <= x1 && x <= x0 + 4096; x++) {
				const float px = x + 0.5f, py = y + 0.5f;
				float       b[3];
				vec3_t      xyz = { 0, 0, 0 }, normal = { 0, 0, 0 };

				if (texels[(y * w + x) * 4 + 3] > 0) {
					continue;
				}
				b[0] = ((tx[1] - px) * (ty[2] - py) - (tx[2] - px) * (ty[1] - py)) / area;
				b[1] = ((tx[2] - px) * (ty[0] - py) - (tx[0] - px) * (ty[2] - py)) / area;
				b[2] = 1.0f - b[0] - b[1];
				if (b[0] < -0.01f || b[1] < -0.01f || b[2] < -0.01f) {
					continue;
				}
				for (k = 0; k < 3; k++) {
					VectorMA(xyz, b[k], v[k]->xyz, xyz);
					VectorMA(normal, b[k], v[k]->normal, normal);
				}
				// its alpha 1 + how far in it is, while 0 says not yet filled
				if (VectorNormalize(normal) > 0 && R_RtBounceAround(xyz, normal, values + offset, &texels[(y * w + x) * 4])) {
					texels[(y * w + x) * 4 + 3] += 1.0f;
				}
			}
		}
	}
	if (list.verts) {
		ri.Free(list.verts);
	}

	// round each chart's edge, for the filtering
	for (n = 0; n < 2; n++) {
		Com_Memcpy(next, texels, sizeof(float) * 4 * w * h);
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				vec4_t sum = { 0, 0, 0, 0 };
				int    count = 0, dx, dy;

				if (texels[(y * w + x) * 4 + 3] > 0) {
					continue;
				}
				for (dy = -1; dy <= 1; dy++) {
					for (dx = -1; dx <= 1; dx++) {
						const int nx = x + dx, ny = y + dy;

						if (nx < 0 || ny < 0 || nx >= w || ny >= h || texels[(ny * w + nx) * 4 + 3] <= 0) {
							continue;
						}
						VectorAdd4(sum, &texels[(ny * w + nx) * 4], sum);
						count++;
					}
				}
				if (count) {
					VectorScale4(sum, 1.0f / count, &next[(y * w + x) * 4]);
				}
			}
		}
		Com_Memcpy(texels, next, sizeof(float) * 4 * w * h);
	}
	for (i = 0; i < w * h; i++) {
		texels[i * 4 + 3] = Q_max(0.0f, texels[i * 4 + 3] - 1.0f);
	}
	{
		image_t *image = R_CreateImage(name, NULL, w, h, IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F);

		qglTextureSubImage2DEXT(image->texnum, GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, texels);
		ri.Free(texels);
		ri.Free(next);
		return image;
	}
}

static void R_RtBounceUpload(const float *values)
{
	tr.rtBounceImage     = R_RtBounceImageOf(values, 0, "*rtbounce");
	tr.rtBounceLampImage = R_RtBounceImageOf(values, 3, "*rtbouncelamps");
}

// The bounce at a point of a surface, from the samples round it on the same
// surface: near (RT_BOUNCE_SMOOTH), facing the same way and in the same plane,
// nearer ones more; else those near facing about the same way. Each sample is
// a few rays, and a texel of the bounce's took the wall a window is in and its
// side looking out together: the wall came out in patches.
#define RT_BOUNCE_SMOOTH 96.0f

static qboolean R_RtBounceAround(const vec3_t xyz, const vec3_t normal, const float *values, vec4_t out)
{
	int pass;

	for (pass = 0; pass < 2; pass++) {
		vec3_t sum = { 0, 0, 0 };
		float  total = 0, indoor = 0;
		int    dx, dy, dz;

		for (dz = -1; dz <= 1; dz++) {
			for (dy = -1; dy <= 1; dy++) {
				for (dx = -1; dx <= 1; dx++) {
					const int64_t key = R_RtBounceHashKey(xyz, dx, dy, dz);
					int           lo = 0, hi = rt_bounce.numSamples, k;

					while (lo < hi) {
						const int mid = (lo + hi) / 2;

						if (rt_bounce.hashKeys[mid] < key) {
							lo = mid + 1;
						} else {
							hi = mid;
						}
					}
					for (k = lo; k < rt_bounce.numSamples && rt_bounce.hashKeys[k] == key; k++) {
						const int               j = rt_bounce.hashOrder[k];
						const rtBounceSample_t *o = &rt_bounce.samples[j];
						vec3_t                  delta;
						float                   d, w;

						if (DotProduct(o->normal, normal) < (pass ? 0.5f : 0.9f)) {
							continue;
						}
						VectorSubtract(o->xyz, xyz, delta);
						if (!pass && fabs(DotProduct(delta, normal)) > 16.0f) {
							continue;
						}
						d = VectorLength(delta);
						if (d >= RT_BOUNCE_SMOOTH) {
							continue;
						}
						w = 1.0f - d / RT_BOUNCE_SMOOTH;
						w *= w;
						VectorMA(sum, w, &values[j * RT_BOUNCE_VALUES], sum);
						indoor += w * o->indoor;
						total += w;
					}
				}
			}
		}
		if (total > 0) {
			VectorScale(sum, 1.0f / total, out);
			out[3] = indoor / total;
			return qtrue;
		}
	}
	return qfalse;
}

static void R_RtBounceFinish(void)
{
	const int w = rt_bounce.width, h = rt_bounce.height;
	float    *values = ri.Malloc(sizeof(float) * RT_BOUNCE_VALUES * Q_max(1, rt_bounce.numSamples));
	int       i, r, indoor = 0;

	// the second bounce: what the first gave each, given back once more
	for (i = 0; i < rt_bounce.numSamples; i++) {
		const rtBounceSample_t *s    = &rt_bounce.samples[i];
		const int              *hits = &rt_bounce.hits[i * rt_bounce.rays];
		vec3_t                  total, totalL;
		int                     k;

		VectorCopy(s->first, total);
		VectorCopy(s->firstL, totalL);
		for (r = 0; r < rt_bounce.rays; r++) {
			if (hits[r] >= 0) {
				const rtBounceSample_t *o = &rt_bounce.samples[hits[r]];

				for (k = 0; k < 3; k++) {
					total[k] += o->first[k] * o->albedo[k] / rt_bounce.rays;
					totalL[k] += o->firstL[k] * o->albedo[k] / rt_bounce.rays;
				}
			}
		}
		if (s->sky < RT_BOUNCE_OUTDOOR) {
			indoor++;
		} else {
			VectorClear(total); // the sky's fill there (R_RtBounceGather)
		}
		VectorCopy(total, &values[i * RT_BOUNCE_VALUES]);
		VectorCopy(totalL, &values[i * RT_BOUNCE_VALUES + 3]);
	}
	for (i = 0; i < rt_bounce.numSamples; i++) {
		rtBounceSample_t *s = &rt_bounce.samples[i];
		int               k;

		VectorAdd(s->direct, s->first, s->given);
		VectorAdd(s->directL, s->firstL, s->givenL);
		for (k = 0; k < 3; k++) {
			s->given[k] *= s->albedo[k];
			s->givenL[k] *= s->albedo[k];
		}
	}

	// kept: the key, the size, each sample's bounce and what it gives back
	{
		const int size = RT_BOUNCE_FILE_SIZE(rt_bounce.numSamples);
		byte     *file = ri.Malloc(size);
		byte     *at;
		int       head[5] = { RT_BOUNCE_VERSION, (int)rt_bounce.key, w, h, rt_bounce.numSamples };

		Com_Memcpy(file, head, 20);
		Com_Memcpy(file + 20, values, sizeof(float) * RT_BOUNCE_VALUES * rt_bounce.numSamples);
		at = file + 20 + sizeof(float) * RT_BOUNCE_VALUES * rt_bounce.numSamples;
		for (i = 0; i < rt_bounce.numSamples; i++, at += 24) {
			Com_Memcpy(at, rt_bounce.samples[i].given, 12);
			Com_Memcpy(at + 12, rt_bounce.samples[i].givenL, 12);
		}
		ri.FS_WriteFile(R_RtBounceFile(), file, size);
		ri.Free(file);
	}
	R_RtBounceUpload(values);
	ri.Free(values);
	rt_probes = ri.Malloc(sizeof(rtProbe_t) * RT_PROBE_SLOTS);
	Com_Memset(rt_probes, 0, sizeof(rtProbe_t) * RT_PROBE_SLOTS);
	{
		int k, n;

		for (n = 0; n < 15; n++) {
			int best = -1;

			for (k = 0; k < 4096 && k < tr.world->numShaders; k++) {
				if (rt_missByShader[k] > 0 && (best < 0 || rt_missByShader[k] > rt_missByShader[best])) {
					best = k;
				}
			}
			if (best < 0) {
				break;
			}
			ri.Printf(PRINT_DEVELOPER, "  missed %d on %s (flags %x contents %x)\n", rt_missByShader[best], tr.world->shaders[best].shader,
				tr.world->shaders[best].surfaceFlags, tr.world->shaders[best].contentFlags);
			rt_missByShader[best] = 0;
		}
		Com_Memset(rt_missByShader, 0, sizeof(rt_missByShader));
	}
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: the bounce, %dx%d, %d samples (%d indoors), %d rays each, %d on models, %d missed, %.1f s\n",
		w, h, rt_bounce.numSamples, indoor, rt_bounce.rays, rt_bounce.models, rt_bounce.misses, rt_bounce.spent / 1000.0f);
	rt_bounce.state = RT_BOUNCE_DONE;
}

// Read from rtbounce/<map>.bnc, if it is for these lights
static qboolean R_RtBounceLoad(void)
{
	union {
		byte *b;
		void *v;
	} buffer;
	const int w = rt_bounce.width, h = rt_bounce.height;
	long      len = ri.FS_ReadFile(R_RtBounceFile(), &buffer.v);
	qboolean  ok  = qfalse;

	if (len <= 0) {
		return qfalse;
	}
	if (len == (long)RT_BOUNCE_FILE_SIZE(rt_bounce.numSamples)) {
		int head[5];

		Com_Memcpy(head, buffer.b, 20);
		if (head[0] == RT_BOUNCE_VERSION && (unsigned)head[1] == rt_bounce.key && head[2] == w && head[3] == h
			&& head[4] == rt_bounce.numSamples) {
			const byte *given = buffer.b + 20 + sizeof(float) * RT_BOUNCE_VALUES * rt_bounce.numSamples;
			int         i;

			R_RtBounceUpload((const float *)(buffer.b + 20));
			for (i = 0; i < rt_bounce.numSamples; i++) {
				Com_Memcpy(rt_bounce.samples[i].given, given + i * 24, 12);
				Com_Memcpy(rt_bounce.samples[i].givenL, given + i * 24 + 12, 12);
			}
			rt_probes = ri.Malloc(sizeof(rtProbe_t) * RT_PROBE_SLOTS);
			Com_Memset(rt_probes, 0, sizeof(rtProbe_t) * RT_PROBE_SLOTS);
			ok = qtrue;
		}
	}
	ri.FS_FreeFile(buffer.v);
	return ok;
}

/*
Once a frame, from R_RtUpdateSky once the sky's lightmaps are drawn: the next
few milliseconds of the bounce, and its scale for the shaders.
*/
static void R_RtBounceStep(float bake, float ease)
{
	int start;

	VectorClear4(tr.rtBounce);
	if (!R_RtLitLive() || !r_rtBounce || r_rtBounce->value <= 0 || !rt_bounceCompiled || !tr.lightmaps || tr.numLightmaps <= 0 || !tr.lightmaps[0]) {
		return;
	}
	// the sky's lightmaps first, which say how much of the sky each sample sees
	if (!tr.rtSkyLightmaps && tr.rtSkyFbo && r_rtSky->value > 0 && !VectorCompare(rt.skyColor, vec3_origin)) {
		return;
	}
	// its image went with the others (vid_restart): read again from where it was kept
	if (rt_bounce.state == RT_BOUNCE_DONE && !tr.rtBounceImage) {
		byte     *sky = rt_bounce.sky;
		const int sw = rt_bounce.skyWidth, sh = rt_bounce.skyHeight;

		rt_bounce.sky = NULL;
		R_RtBounceFree();
		if (sky) {
			R_RtBounceKeepSky(sky, sw, sh);
		}
	}

	if (rt_bounce.state == RT_BOUNCE_IDLE) {
		rt_bounce.width  = Q_max(1, tr.lightmaps[0]->width / RT_BOUNCE_DOWN);
		rt_bounce.height = Q_max(1, tr.lightmaps[0]->height / RT_BOUNCE_DOWN);
		rt_bounce.rays   = Com_Clamp(4, 128, r_rtBounceRays->integer);
		start            = ri.Milliseconds();
		R_RtBounceSamples();
		rt_bounce.key = R_RtBounceKey();
		if (R_RtBounceLoad()) {
			rt_bounce.state = RT_BOUNCE_DONE;
			ri.Printf(PRINT_DEVELOPER, "realtime lighting: the bounce, read from %s\n", R_RtBounceFile());
		} else {
			rt_bounce.hits  = ri.Malloc(sizeof(int) * Q_max(1, rt_bounce.numSamples * rt_bounce.rays));
			rt_bounce.state = RT_BOUNCE_DIRECT;
			rt_bounce.next  = 0;
			ri.Printf(PRINT_DEVELOPER, "realtime lighting: working out the bounce, %d samples\n", rt_bounce.numSamples);
		}
		rt_bounce.spent += ri.Milliseconds() - start;
		return;
	}

	start = ri.Milliseconds();
	while (rt_bounce.state == RT_BOUNCE_DIRECT || rt_bounce.state == RT_BOUNCE_GATHER) {
		if (ri.Milliseconds() - start >= r_rtBounceMsec->integer) {
			break;
		}
		if (rt_bounce.next >= rt_bounce.numSamples) {
			rt_bounce.next = 0;
			if (rt_bounce.state == RT_BOUNCE_DIRECT) {
				rt_bounce.state = RT_BOUNCE_GATHER;
			} else {
				rt_bounce.spent += ri.Milliseconds() - start;
				R_RtBounceFinish();
				break;
			}
			continue;
		}
		{
			int k;

			// a few at a time between looks at the clock
			for (k = 0; k < 16 && rt_bounce.next < rt_bounce.numSamples; k++, rt_bounce.next++) {
				if (rt_bounce.state == RT_BOUNCE_DIRECT) {
					R_RtBounceDirect(&rt_bounce.samples[rt_bounce.next]);
				} else {
					R_RtBounceGather(rt_bounce.next);
				}
			}
		}
	}
	if (rt_bounce.state != RT_BOUNCE_DONE) {
		rt_bounce.spent += ri.Milliseconds() - start;
		return;
	}
	R_RtProbesWork(ri.Milliseconds());

	rt_bounce.weight = Q_min(1.0f, rt_bounce.weight + ease);
	VectorSet4(tr.rtBounce, bake * rt_bounce.weight * R_RtBounceScale(0), bake * rt_bounce.weight * R_RtBounceScale(1),
		bake * rt_bounce.weight, tr.rtBounceImage ? 1.0f : 0.0f);
	// r_rtBakedDebug 3: the lamps' bounce alone; 4: the daylight's alone
	if (r_rtBakedDebug->integer == 3) {
		tr.rtBounce[0] = tr.rtBounce[1] = 0;
	} else if (r_rtBakedDebug->integer == 4) {
		tr.rtBounce[2] = 0;
	}
}

static void R_RtBakeSkyLightmaps(void)
{
	rtBakeList_t list;
	vec4_t       color, off = { 0, 0, 0, 0 };
	int          page, total = 0;

	if (!tr.rtSkyBakeShader.program || !tr.rtSkyBakeFbo || !tr.lightmaps || tr.numLightmaps <= 0 || tr.rtSkyLightmaps) {
		return;
	}

	tr.rtNumSkyLightmaps = tr.numLightmaps;
	tr.rtSkyLightmaps    = ri.Malloc(sizeof(image_t *) * tr.numLightmaps);
	Com_Memset(tr.rtSkyLightmaps, 0, sizeof(image_t *) * tr.numLightmaps);
	Com_Memset(&list, 0, sizeof(list));

	// RtSky's sum over an open floor is half its directions: kept in a byte
	VectorSet4(color, 2.0f / RT_SKY_DIRS, 2.0f / RT_SKY_DIRS, 2.0f / RT_SKY_DIRS, rt_skyTexel);

	for (page = 0; page < tr.numLightmaps; page++) {
		const image_t *lightmap = tr.lightmaps[page];
		image_t       *image;
		byte          *texels;
		GLuint         vao, vbo;

		if (!lightmap) {
			continue;
		}
		list.num = 0;
		R_RtBakeGather(page, &list);
		if (!list.num) {
			continue;
		}
		total += list.num / 3;

		image = R_CreateImage(va("*rtskylm%d", page), NULL, lightmap->width, lightmap->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8);

		FBO_Bind(tr.rtSkyBakeFbo);
		FBO_AttachImage(tr.rtSkyBakeFbo, image, GL_COLOR_ATTACHMENT0_EXT, 0);
		qglDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
		if (!R_CheckFBO(tr.rtSkyBakeFbo)) {
			FBO_Bind(NULL);
			R_FreeImage(image);
			break;
		}
		qglViewport(0, 0, image->width, image->height);
		qglScissor(0, 0, image->width, image->height);
		qglClearColor(0, 0, 0, 0);
		qglClear(GL_COLOR_BUFFER_BIT);

		GL_State(GLS_DEPTHTEST_DISABLE);
		GL_Cull(CT_TWO_SIDED);
		GLSL_BindProgram(&tr.rtSkyBakeShader);
		GLSL_SetUniformVec4(&tr.rtSkyBakeShader, UNIFORM_RTSKYCOLOR, color);
		GLSL_SetUniformVec4(&tr.rtSkyBakeShader, UNIFORM_RTSURFACE, off);
		GLSL_SetUniformRtSky(&tr.rtSkyBakeShader, UNIFORM_RTSKY, tr.rtSkyRows, tr.rtSkyGeneration);
		GL_BindToTMU(tr.rtSkyImage, TMU_RTSKY);

		R_BindNullVao();
		qglGenVertexArrays(1, &vao);
		qglBindVertexArray(vao);
		qglGenBuffers(1, &vbo);
		qglBindBuffer(GL_ARRAY_BUFFER, vbo);
		qglBufferData(GL_ARRAY_BUFFER, sizeof(rtBakeVert_t) * list.num, list.verts, GL_STATIC_DRAW);
		qglEnableVertexAttribArray(ATTR_INDEX_POSITION);
		qglEnableVertexAttribArray(ATTR_INDEX_NORMAL);
		qglEnableVertexAttribArray(ATTR_INDEX_TEXCOORD);
		qglVertexAttribPointer(ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, xyz));
		qglVertexAttribPointer(ATTR_INDEX_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, normal));
		qglVertexAttribPointer(ATTR_INDEX_TEXCOORD, 2, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, lm));

		// the triangles, then their edges, for the texels along them whose
		// middles the triangles miss
		qglDrawArrays(GL_TRIANGLES, 0, list.num);
		qglPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		qglDrawArrays(GL_TRIANGLES, 0, list.num);
		qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		texels = ri.Malloc(image->width * image->height * 4);
		qglReadPixels(0, 0, image->width, image->height, GL_RGBA, GL_UNSIGNED_BYTE, texels);
		R_RtBakeDilate(texels, image->width, image->height, 3);
		qglTextureSubImage2DEXT(image->texnum, GL_TEXTURE_2D, 0, 0, 0, image->width, image->height, GL_RGBA, GL_UNSIGNED_BYTE, texels);
		if (page == 0) {
			// the sky each texel sees, for what bounces (R_RtBounce)
			R_RtBounceKeepSky(texels, image->width, image->height);
		} else {
			ri.Free(texels);
		}

		qglDisableVertexAttribArray(ATTR_INDEX_POSITION);
		qglDisableVertexAttribArray(ATTR_INDEX_NORMAL);
		qglDisableVertexAttribArray(ATTR_INDEX_TEXCOORD);
		qglBindBuffer(GL_ARRAY_BUFFER, 0);
		qglDeleteBuffers(1, &vbo);
		qglBindVertexArray(0);
		qglDeleteVertexArrays(1, &vao);
		FBO_Bind(NULL);

		tr.rtSkyLightmaps[page] = image;
	}

	if (list.verts) {
		ri.Free(list.verts);
	}
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: the sky over %d triangles of %d lightmap pages\n", total, tr.numLightmaps);
}

void R_RtFreeSkyLightmaps(void)
{
	int i;

	if (!tr.rtSkyLightmaps) {
		return;
	}
	for (i = 0; i < tr.rtNumSkyLightmaps; i++) {
		if (tr.rtSkyLightmaps[i]) {
			R_FreeImage(tr.rtSkyLightmaps[i]);
		}
	}
	ri.Free(tr.rtSkyLightmaps);
	tr.rtSkyLightmaps    = NULL;
	tr.rtNumSkyLightmaps = 0;
}

#define RT_SKY_PER_FRAME 4
#define RT_SKY_WAIT      8 // world frames after a map comes in: the terrain
                           // the main view picked is what the depth views draw

// The sky's light for the shaders (tr.rtSkyColor), its directions drawn first,
// a few a frame.
static void R_RtUpdateSky(float bake, float ease)
{
	float norm = 0;
	int   k, drawn = 0;

	VectorClear4(tr.rtSkyColor);
	if (!R_RtLitLive() || !tr.rtSkyFbo || r_rtSky->value <= 0 || VectorCompare(rt.skyColor, vec3_origin)) {
		rt.skyWeight = 0;
		return;
	}

	// drawn again into new images (vid_restart)
	if (rt.skyImage != tr.rtSkyImage) {
		R_RtFreeSkyLightmaps();
		rt.skyImage  = tr.rtSkyImage;
		rt.skyDrawn  = 0;
		rt.skyWeight = 0;
		rt_skyTexel  = 0;
	}

	while (rt.skyDrawn < RT_SKY_DIRS && rt.frame > RT_SKY_WAIT && drawn < RT_SKY_PER_FRAME) {
		R_RtRenderSkyDir(rt.skyDrawn++);
		drawn++;
		if (rt.skyDrawn == RT_SKY_DIRS) {
			ri.Printf(PRINT_DEVELOPER, "realtime lighting: the sky from %d directions, %.0f units a texel\n", RT_SKY_DIRS, rt_skyTexel);
		}
	}
	if (rt.skyDrawn < RT_SKY_DIRS) {
		return;
	}
	// a frame after the last of them, which the backend has drawn by now
	if (!drawn && !tr.rtSkyLightmaps) {
		R_RtBakeSkyLightmaps();
	}

	// an open floor sees all of it
	for (k = 0; k < RT_SKY_DIRS; k++) {
		norm += rt.skyDirs[k][2];
	}

	rt.skyWeight = Q_min(1.0f, rt.skyWeight + ease);
	VectorScale(rt.skyColor, bake * r_rtSky->value * rt.skyWeight / norm, tr.rtSkyColor);
	tr.rtSkyColor[3] = rt_skyTexel;
}

/*
=============================================================================

THE BAKED LAMPS

Every lamp of the map lit live, with its shadows, every frame, is more than a
frame has time for: a few dozen are, the nearest and those the view sees, and
the rest gave no light at all. The view turning to a street of lamps found
them dark, coming on one after another as they were chosen; a room lit
behind a window across the way was dark until its lamp was among them.

So the light each lamp of the map puts on the lightmapped world, with its
shadow of the world alone (the static models move, and the doors), is baked
once a map onto pages of its own, the lightmaps' size (R_RtBakeLamps), and
read where a pixel reads its lightmap: all of them, always, for one fetch.
The few lit live add what they change of their baked light (RtLight): their
sharp shadows, of what moves too, their shine and their flicker, less their
light as baked, which the world alone in the third atlas, in small tiles as
it was baked, gives again at each pixel. What has no lightmap (people,
furniture) takes the lamps lit live, as before.

A lamp's six faces of the world alone are drawn into the third atlas, a few
lamps a frame (RT_BAKE_BATCH), and the frame after, the backend having drawn
them, each lamp's light is drawn at the lightmap coordinates of the surfaces
it reaches, added up. The texels no surface covers take their neighbours'.
=============================================================================
*/

static float R_RtReach(const vec4_t colorRange, float cutoff);
static float R_RtSlotCode(int slot);
static void  R_RtRenderFace(const rtLight_t *l, int atlas, int slot, int face);

#define RT_BAKE_BATCH  16  // lamps whose faces are drawn a frame: half of the whole tiles each frame
#define RT_BAKE_WAIT   (RT_SKY_WAIT + 2)
#define RT_BAKE_CUTOFF 0.0005f // the least it bakes a lamp giving a texel (r_rtCutoff's default)

typedef struct {
	image_t     *image; // rgb what the lamps give, in raw lightmap units; a 1 where a surface is
	image_t     *dir;   // the way it comes from, as long as it is bright, for the normal maps
	image_t     *live, *liveDir; // the same of the lamps lit live, which the shaders take back out (R_RtLiveIn)
	image_t     *fill[2];        // a lamp's, as R_RtBakeFinish leaves it (R_RtLivePass)
	image_t     *covered;        // r how many surfaces were drawn over each texel
	rtBakeList_t list;
	GLuint       vao, vbo;
	GLint       *firsts;
	GLsizei     *counts;
} rtBakePage_t;

static struct {
	qboolean      started, done;
	int           next;                    // the next light whose faces are drawn
	int           pending[RT_BAKE_BATCH];  // those drawn last frame, in region
	int           numPending, region;
	rtBakePage_t *pages;
	int           numPages;
	image_t      *atlas;                   // the third atlas, as it was (a vid_restart makes another)
	GLuint        atlasTexnum;
	float         weight, scale;           // eased in once done; the lights' scale (R_RtRenderShadows)
	int           generation, lights, startTime;
	GLuint        quadVao, quadVbo;        // a triangle over the whole of a page
	int           liveAdded;               // lamps taken out this frame (R_RtLiveIn)
	int           liveFrame;
	int           livePasses, liveClears;  // since the last r_rtDebug report
} rt_bake;

// Whether a light draws its faces of the world alone into the third atlas
static qboolean R_RtWantsBakedFaces(const rtLight_t *l)
{
	if (!tr.rtShadowFbo[2]) {
		return qfalse;
	}
	if (r_realtimeLighting->integer == 1) {
		return qtrue;
	}
	return (rt_bake.done && l->baked && !l->noShadow) ? qtrue : qfalse;
}

void R_RtFreeBakedLamps(void)
{
	// the images and buffers went with the renderer if it was restarted
	const qboolean ours = rt_bake.atlas && rt_bake.atlas == tr.rtShadowImage[2] && rt_bake.atlasTexnum == tr.rtShadowImage[2]->texnum;
	int            i;

	for (i = 0; i < rt_bake.numPages && rt_bake.pages; i++) {
		rtBakePage_t *page = &rt_bake.pages[i];

		if (ours && page->image) {
			R_FreeImage(page->image);
		}
		if (ours && page->dir) {
			R_FreeImage(page->dir);
		}
		if (ours && page->live) {
			R_FreeImage(page->live);
		}
		if (ours && page->liveDir) {
			R_FreeImage(page->liveDir);
		}
		if (ours && page->fill[0]) {
			R_FreeImage(page->fill[0]);
		}
		if (ours && page->fill[1]) {
			R_FreeImage(page->fill[1]);
		}
		if (ours && page->covered) {
			R_FreeImage(page->covered);
		}
		if (ours && page->vbo) {
			qglDeleteBuffers(1, &page->vbo);
		}
		if (ours && page->vao) {
			qglDeleteVertexArrays(1, &page->vao);
		}
		if (page->list.verts) {
			ri.Free(page->list.verts);
		}
		if (page->list.chunks) {
			ri.Free(page->list.chunks);
		}
		if (page->firsts) {
			ri.Free(page->firsts);
		}
		if (page->counts) {
			ri.Free(page->counts);
		}
	}
	if (rt_bake.pages) {
		ri.Free(rt_bake.pages);
	}
	if (ours && rt_bake.quadVbo) {
		qglDeleteBuffers(1, &rt_bake.quadVbo);
	}
	if (ours && rt_bake.quadVao) {
		qglDeleteVertexArrays(1, &rt_bake.quadVao);
	}
	Com_Memset(&rt_bake, 0, sizeof(rt_bake));
	for (i = 0; rt.lights && i < rt.numLights; i++) {
		rt.lights[i].baked      = qfalse;
		rt.lights[i].bakedFaces = 0;
		rt.lights[i].live       = qfalse;
	}
}

// A light's shape for the shaders (data[2]), and its kind (data[0].w)
static float R_RtLightShape(const rtLight_t *l, vec4_t shape)
{
	if (l->area > 0) {
		// a half of a sphere: what is behind its surface is shut off (all
		// of it, but not longer than a whole: that is a ring)
		VectorScale(l->spotDir, -0.995f, shape);
	} else {
		VectorCopy(l->spotDir, shape);
	}
	// a point light's: how near it its light stops growing (RtLight)
	// (a lit surface's less than nothing: its light falls off from straight out of it)
	shape[3] = l->spot ? l->radiusByDist : l->lamp ? RT_LAMP_NEAR : l->area > 0 ? -l->area : 0.0f;
	return l->spot ? 1.0f : (l->lamp || l->area > 0) ? 3.0f : 0.0f;
}

// A page's vertexes on the card, for each lamp's draws
static qboolean R_RtBakeUpload(rtBakePage_t *page)
{
	qglGenVertexArrays(1, &page->vao);
	qglBindVertexArray(page->vao);
	qglGenBuffers(1, &page->vbo);
	qglBindBuffer(GL_ARRAY_BUFFER, page->vbo);
	qglBufferData(GL_ARRAY_BUFFER, sizeof(rtBakeVert_t) * page->list.num, page->list.verts, GL_STATIC_DRAW);
	qglEnableVertexAttribArray(ATTR_INDEX_POSITION);
	qglEnableVertexAttribArray(ATTR_INDEX_NORMAL);
	qglEnableVertexAttribArray(ATTR_INDEX_TEXCOORD);
	qglVertexAttribPointer(ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, xyz));
	qglVertexAttribPointer(ATTR_INDEX_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, normal));
	qglVertexAttribPointer(ATTR_INDEX_TEXCOORD, 2, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, lm));
	qglBindVertexArray(0);
	qglBindBuffer(GL_ARRAY_BUFFER, 0);
	page->firsts = ri.Malloc(sizeof(GLint) * Q_max(1, page->list.numChunks));
	page->counts = ri.Malloc(sizeof(GLsizei) * Q_max(1, page->list.numChunks));
	return qtrue;
}

// Bound to draw into a page's image (its light, or the way it comes from)
static qboolean R_RtBakeTarget(rtBakePage_t *page, image_t *image)
{
	FBO_Bind(tr.rtSkyBakeFbo);
	FBO_AttachImage(tr.rtSkyBakeFbo, image, GL_COLOR_ATTACHMENT0_EXT, 0);
	qglDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
	if (!R_CheckFBO(tr.rtSkyBakeFbo)) {
		return qfalse;
	}
	qglViewport(0, 0, image->width, image->height);
	qglScissor(0, 0, image->width, image->height);
	qglBindVertexArray(page->vao);
	return qtrue;
}

static void R_RtBakeBegin(float alpha, int state)
{
	vec4_t off = { 0, 0, 0, 0 }, params = { 1, 2, 1, 0 };

	params[3] = alpha;
	GL_State(state);
	GL_Cull(CT_TWO_SIDED);
	GLSL_BindProgram(&tr.rtLampBakeShader);
	GLSL_SetUniformVec4(&tr.rtLampBakeShader, UNIFORM_RTSURFACE, off);
	GLSL_SetUniformVec4(&tr.rtLampBakeShader, UNIFORM_RTPARAMS, params);
	GL_BindToTMU(tr.rtShadowImage[2], TMU_RTSHADOW_BAKED);
	R_BindNullVao();
}

// What the lamps are drawn as: 1 their light, 2 the way it comes from
static void R_RtBakeMode(float mode, float alpha)
{
	vec4_t params = { 1, 2, 1, 0 };

	params[0] = mode;
	params[3] = alpha;
	GLSL_SetUniformVec4(&tr.rtLampBakeShader, UNIFORM_RTPARAMS, params);
}

static void R_RtBakeEnd(void)
{
	qglBindVertexArray(0);
	FBO_Bind(NULL);
}

static void R_RtBakeStart(void)
{
	vec4_t none[RT_LIGHT_VEC4S];
	int    p, i;

	rt_bake.started     = qtrue;
	rt_bake.atlas       = tr.rtShadowImage[2];
	rt_bake.atlasTexnum = tr.rtShadowImage[2] ? tr.rtShadowImage[2]->texnum : 0;
	rt_bake.startTime   = ri.Milliseconds();
	if (!tr.rtLampBakeShader.program || !tr.rtSkyBakeFbo || !tr.rtShadowFbo[2] || !tr.lightmaps || tr.numLightmaps <= 0) {
		ri.Printf(PRINT_DEVELOPER, "realtime lighting: the lamps are not baked (no program, atlas or lightmaps)\n");
		return;
	}

	rt_bake.numPages = tr.numLightmaps;
	rt_bake.pages    = ri.Malloc(sizeof(rtBakePage_t) * rt_bake.numPages);
	Com_Memset(rt_bake.pages, 0, sizeof(rtBakePage_t) * rt_bake.numPages);

	// nothing yet, but where each surface is (a of 1), for the texels round
	// them that none covers to take their neighbours' (R_RtBakeFinish)
	Com_Memset(none, 0, sizeof(none));
	none[3][0] = -1.0f;
	// a of each texel: how many surfaces were drawn over it. The compiler
	// gave faces in one plane the same texels at times, and each lamp's
	// light was added there once for each: a line along the face's edge,
	// bright on the one side (R_RtBakeFinish takes the mean)
	R_RtBakeBegin(1.0f, GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	GLSL_SetUniformRtLights(&tr.rtLampBakeShader, UNIFORM_RTLIGHTS, (const vec4_t *)none, 1, --rt_bake.generation);
	for (p = 0; p < rt_bake.numPages; p++) {
		rtBakePage_t *page = &rt_bake.pages[p];

		if (!tr.lightmaps[p]) {
			continue;
		}
		page->list.keepChunks = qtrue;
		R_RtBakeGather(p, &page->list);
		if (!page->list.num) {
			continue;
		}
		page->image = R_CreateImage(va("*rtlamplm%d", p), NULL, tr.lightmaps[p]->width, tr.lightmaps[p]->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F);
		page->dir   = R_CreateImage(va("*rtlampdir%d", p), NULL, tr.lightmaps[p]->width, tr.lightmaps[p]->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F);
		// the lamps lit live, added and taken out again and again: whole floats
		page->live    = R_CreateImage(va("*rtlamplive%d", p), NULL, tr.lightmaps[p]->width, tr.lightmaps[p]->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA32F);
		page->liveDir = R_CreateImage(va("*rtlamplivedir%d", p), NULL, tr.lightmaps[p]->width, tr.lightmaps[p]->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA32F);
		page->fill[0] = R_CreateImage(va("*rtlampfill%da", p), NULL, tr.lightmaps[p]->width, tr.lightmaps[p]->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA32F);
		page->fill[1] = R_CreateImage(va("*rtlampfill%db", p), NULL, tr.lightmaps[p]->width, tr.lightmaps[p]->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA32F);
		page->covered = R_CreateImage(va("*rtlampcovered%d", p), NULL, tr.lightmaps[p]->width, tr.lightmaps[p]->height, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA16F);
		R_RtBakeUpload(page);
		{
			image_t *both[4];
			int      k;

			both[0] = page->image;
			both[1] = page->dir;
			both[2] = page->live;
			both[3] = page->liveDir;
			for (k = 0; k < 4; k++) {
				if (!R_RtBakeTarget(page, both[k])) {
					R_RtBakeEnd();
					ri.Printf(PRINT_WARNING, "realtime lighting: the lamps are not baked (no target)\n");
					R_RtFreeBakedLamps();
					rt_bake.started = qtrue;
					return;
				}
				qglClearColor(0, 0, 0, 0);
				qglClear(GL_COLOR_BUFFER_BIT);
				// the lamps lit live start as none
				if (k < 2) {
					qglDrawArrays(GL_TRIANGLES, 0, page->list.num);
				}
			}
		}
	}
	R_RtBakeEnd();

	for (i = 0; i < rt.numMapLights; i++) {
		if (rt.lights[i].range > 0) {
			rt.lights[i].baked = qtrue;
			rt_bake.lights++;
		}
	}
}

// A triangle over the whole of a page, for R_RtLiveFill
static qboolean R_RtBakeQuad(void)
{
	rtBakeVert_t verts[3];

	if (rt_bake.quadVao) {
		return qtrue;
	}
	Com_Memset(verts, 0, sizeof(verts));
	verts[1].lm[0] = 2.0f;
	verts[2].lm[1] = 2.0f;
	qglGenVertexArrays(1, &rt_bake.quadVao);
	qglBindVertexArray(rt_bake.quadVao);
	qglGenBuffers(1, &rt_bake.quadVbo);
	qglBindBuffer(GL_ARRAY_BUFFER, rt_bake.quadVbo);
	qglBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	qglEnableVertexAttribArray(ATTR_INDEX_POSITION);
	qglEnableVertexAttribArray(ATTR_INDEX_NORMAL);
	qglEnableVertexAttribArray(ATTR_INDEX_TEXCOORD);
	qglVertexAttribPointer(ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, xyz));
	qglVertexAttribPointer(ATTR_INDEX_NORMAL, 3, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, normal));
	qglVertexAttribPointer(ATTR_INDEX_TEXCOORD, 2, GL_FLOAT, GL_FALSE, sizeof(rtBakeVert_t), (void *)offsetof(rtBakeVert_t, lm));
	qglBindVertexArray(0);
	qglBindBuffer(GL_ARRAY_BUFFER, 0);
	return qtrue;
}

// One pass of the fill program from src into dst (R_RtLivePass)
static qboolean R_RtLiveFillPass(rtBakePage_t *page, image_t *src, image_t *dst, float mode, float sign, int state)
{
	vec4_t params = { 0, 0, 0, 0 };

	if (!R_RtBakeTarget(page, dst)) {
		return qfalse;
	}
	qglBindVertexArray(rt_bake.quadVao);
	GL_State(state);
	GL_Cull(CT_TWO_SIDED);
	GLSL_BindProgram(&tr.rtBakeFillShader);
	params[0] = mode;
	params[1] = sign;
	GLSL_SetUniformVec4(&tr.rtBakeFillShader, UNIFORM_RTPARAMS, params);
	GL_BindToTMU(src, TMU_RTDIRECT);
	GL_BindToTMU(page->covered, TMU_RTDIRECTDIR);
	qglDrawArrays(GL_TRIANGLES, 0, 3);
	return qtrue;
}

// A lamp's light drawn into fill[0] made what R_RtBakeFinish made of it (the
// mean of the surfaces over a texel, the texels none covers filled from those
// round them), then added onto dst, times sign
static void R_RtLiveFill(rtBakePage_t *page, image_t *dst, float sign)
{
	const int state = GLS_DEPTHTEST_DISABLE;

	if (!R_RtLiveFillPass(page, page->fill[0], page->fill[1], 0.0f, 0.0f, state)
		|| !R_RtLiveFillPass(page, page->fill[1], page->fill[0], 1.0f, 0.0f, state)
		|| !R_RtLiveFillPass(page, page->fill[0], page->fill[1], 1.0f, 0.0f, state)
		|| !R_RtLiveFillPass(page, page->fill[1], page->fill[0], 1.0f, 0.0f, state)) {
		return;
	}
	R_RtLiveFillPass(page, page->fill[0], dst, 2.0f, sign, GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
}

// One lamp's light added onto the pages, its faces in slot of the third atlas
// (shadowed), as the bake draws it (sign 0); or, as R_RtBakeFinish left it,
// onto the pages of the lamps lit live, times sign (R_RtLiveIn)
static void R_RtBakeDraw(int index, int slot, qboolean shadowed, float sign)
{
	rtLight_t *l = &rt.lights[index];
	vec4_t     data[RT_LIGHT_VEC4S], scaled;
	float      reach;
	int        p, c;

	VectorScale(l->color, rt_bake.scale, scaled);
	scaled[3] = l->range;
	reach     = R_RtReach(scaled, RT_BAKE_CUTOFF);
	if (reach <= 0) {
		return;
	}
	if (sign != 0 && (!tr.rtBakeFillShader.program || !R_RtBakeQuad())) {
		return;
	}
	if (sign == 0) {
		VectorCopy(l->origin, l->bakedOrigin);
	}

	VectorCopy(l->origin, data[0]);
	data[0][3] = R_RtLightShape(l, data[2]);
	VectorCopy(l->color, data[1]);
	data[1][3] = l->range;
	VectorSet4(data[3], shadowed ? R_RtSlotCode(slot) : -1.0f, -1.0f, -1.0f, l->range);
	GLSL_SetUniformRtLights(&tr.rtLampBakeShader, UNIFORM_RTLIGHTS, (const vec4_t *)data, 1, --rt_bake.generation);

	for (p = 0; p < rt_bake.numPages; p++) {
		rtBakePage_t *page = &rt_bake.pages[p];
		int           n    = 0, mode;

		if (!page->image || (sign != 0 && (!page->live || !page->liveDir || !page->fill[0] || !page->fill[1] || !page->covered))) {
			continue;
		}
		// the surfaces in its reach
		for (c = 0; c < page->list.numChunks; c++) {
			const rtBakeChunk_t *chunk = &page->list.chunks[c];
			float                d2    = 0;
			int                  k;

			for (k = 0; k < 3; k++) {
				const float v = l->origin[k] < chunk->mins[k] ? chunk->mins[k] - l->origin[k]
							  : l->origin[k] > chunk->maxs[k] ? l->origin[k] - chunk->maxs[k] : 0;
				d2 += v * v;
			}
			if (d2 < reach * reach || r_rtBakedDebug->integer == 2) {
				page->firsts[n] = chunk->first;
				page->counts[n] = chunk->count;
				n++;
			}
		}
		if (!n) {
			continue;
		}
		// its light, then the way it comes from
		for (mode = 1; mode <= 2; mode++) {
			image_t *target = sign == 0 ? (mode == 1 ? page->image : page->dir) : page->fill[0];

			if (sign != 0) {
				R_RtBakeBegin(0.0f, GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
			}
			if (!R_RtBakeTarget(page, target)) {
				break;
			}
			if (sign != 0) {
				qglClearColor(0, 0, 0, 0);
				qglClear(GL_COLOR_BUFFER_BIT);
			}
			R_RtBakeMode((float)mode, 0.0f);
			for (c = 0; c < n; c++) {
				qglDrawArrays(GL_TRIANGLES, page->firsts[c], page->counts[c]);
			}
			if (sign != 0) {
				R_RtLiveFill(page, mode == 1 ? page->live : page->liveDir, sign);
			}
		}
	}
}

static void R_RtBakePass(int index, int slot)
{
	const rtLight_t *l = &rt.lights[index];

	R_RtBakeDraw(index, slot, !l->noShadow && r_rtShadows->integer, 0.0f);
}

/*
The baked lamps lit live are taken back out of their bake, each all of it,
onto pages of their own (rtBakePage_t live) the shaders take away from the
bake's: they give all of their light live, and what their bake lent the
surface beside across a face's edge goes with it. Worked out at a pixel, as
before, the lightmap's coarse texels, and that, were left over: faint edges
round a lamp's pool (RtLight still does so for those not out yet). A lamp
goes in when lit fully and its faces of the world alone, which it was baked
with (whole tiles, slot < RT_SLOTS_BIG), are drawn; out the same way, before
it loses those faces or its light eases out.
*/
#define RT_LIVE_ADDS  2 // lamps taken out a frame
#define RT_LIVE_DROPS 6 // past this many put back a frame, all of them are at once

static void R_RtLiveClear(void)
{
	int p, i;

	for (p = 0; p < rt_bake.numPages && rt_bake.pages; p++) {
		rtBakePage_t *page = &rt_bake.pages[p];
		image_t      *both[2];
		int           k;

		both[0] = page->live;
		both[1] = page->liveDir;
		for (k = 0; k < 2; k++) {
			if (both[k] && R_RtBakeTarget(page, both[k])) {
				qglClearColor(0, 0, 0, 0);
				qglClear(GL_COLOR_BUFFER_BIT);
			}
		}
	}
	R_RtBakeEnd();
	for (i = 0; i < rt.numLights; i++) {
		rt.lights[i].live = qfalse;
	}
	rt_bake.liveClears++;
}

// Whether light l's bake is to be taken out (it is lit, chosen and ready)
static qboolean R_RtLiveWanted(const rtLight_t *l, qboolean lit)
{
	if (!lit || l->weight < 1.0f || !rt_bake.done || !r_rtBakedLamps->integer || !l->baked || !tr.rtBakeFillShader.program
		|| Distance(l->origin, l->bakedOrigin) > RT_LIGHT_MOVE_EPSILON) {
		return qfalse;
	}
	if (l->noShadow || !r_rtShadows->integer) {
		return qtrue;
	}
	return l->staticSlot >= 0 && l->staticSlot < RT_SLOTS_BIG && l->bakedFaces == 63 && rt.frame > l->bakedReady;
}

// Light index's bake taken out (in) or put back
static void R_RtLiveSet(int index, qboolean in)
{
	rtLight_t     *l = &rt.lights[index];
	qboolean       shadowed;

	if (l->live == in) {
		return;
	}
	if (rt_bake.liveFrame != rt.frame) {
		rt_bake.liveFrame = rt.frame;
		rt_bake.liveAdded = 0;
	}
	if (in) {
		if (rt_bake.liveAdded >= RT_LIVE_ADDS) {
			return;
		}
		shadowed = !l->noShadow && r_rtShadows->integer;
	} else {
		shadowed = l->liveShadowed;
		// put back with the faces it was taken out with: gone, all of it at once
		if (shadowed && (l->staticSlot < 0 || l->staticSlot >= RT_SLOTS_BIG)) {
			R_RtLiveClear();
			return;
		}
	}
	R_RtBakeDraw(index, l->staticSlot, shadowed, in ? 1.0f : -1.0f);
	R_RtBakeEnd();
	rt_bake.livePasses++;
	l->live         = in;
	l->liveShadowed = shadowed;
	rt_bake.liveAdded += in ? 1 : 0;
}

// The lamps lit live last frame and not this one put back (a few; else all at once)
static void R_RtLiveSweep(void)
{
	int i, drops = 0;

	for (i = 0; i < rt.numLights; i++) {
		if (rt.lights[i].live && rt.lights[i].lastUsed != rt.frame) {
			drops++;
		}
	}
	if (drops > RT_LIVE_DROPS) {
		R_RtLiveClear();
		return;
	}
	for (i = 0; drops && i < rt.numLights; i++) {
		if (rt.lights[i].live && rt.lights[i].lastUsed != rt.frame) {
			R_RtLiveSet(i, qfalse);
		}
	}
}

// The texels no surface covered (a of 0) take the average of the covered
// ones round them, a ring at a time, as the lightmaps' filtering reads them
static void R_RtBakeDilateFloat(float *texels, int width, int height, int rings)
{
	float *next = ri.Malloc(sizeof(float) * width * height * 4);
	int    n, x, y;

	for (n = 0; n < rings; n++) {
		Com_Memcpy(next, texels, sizeof(float) * width * height * 4);
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				vec3_t sum = { 0, 0, 0 };
				int    count = 0, dx, dy;

				if (texels[(y * width + x) * 4 + 3] > 0.5f) {
					continue;
				}
				for (dy = -1; dy <= 1; dy++) {
					for (dx = -1; dx <= 1; dx++) {
						const int nx = x + dx, ny = y + dy;

						if (nx < 0 || ny < 0 || nx >= width || ny >= height || texels[(ny * width + nx) * 4 + 3] < 0.5f) {
							continue;
						}
						VectorAdd(sum, &texels[(ny * width + nx) * 4], sum);
						count++;
					}
				}
				if (count) {
					VectorScale(sum, 1.0f / count, &next[(y * width + x) * 4]);
					next[(y * width + x) * 4 + 3] = 1.0f;
				}
			}
		}
		Com_Memcpy(texels, next, sizeof(float) * width * height * 4);
	}
	ri.Free(next);
}

static void R_RtBakeFinish(void)
{
	int p;

	for (p = 0; p < rt_bake.numPages; p++) {
		rtBakePage_t *page = &rt_bake.pages[p];
		float        *texels;

		image_t      *both[2];
		int           k;

		both[0] = page->image;
		both[1] = page->dir;
		for (k = 0; k < 2; k++) {
			if (!both[k] || !R_RtBakeTarget(page, both[k])) {
				continue;
			}
			texels = ri.Malloc(sizeof(float) * both[k]->width * both[k]->height * 4);
			qglReadPixels(0, 0, both[k]->width, both[k]->height, GL_RGBA, GL_FLOAT, texels);
			// how many surfaces were drawn over each texel, for the lamps lit live (R_RtLiveFill)
			if (k == 0 && page->covered) {
				float *covered = ri.Malloc(sizeof(float) * both[k]->width * both[k]->height * 4);
				int    n;

				for (n = 0; n < both[k]->width * both[k]->height; n++) {
					VectorSet4(&covered[n * 4], texels[n * 4 + 3], 0, 0, 1);
				}
				qglTextureSubImage2DEXT(page->covered->texnum, GL_TEXTURE_2D, 0, 0, 0, both[k]->width, both[k]->height, GL_RGBA, GL_FLOAT, covered);
				ri.Free(covered);
			}
			// the mean of the surfaces drawn over a texel, not their sum
			{
				int n, shared = 0;

				for (n = 0; n < both[k]->width * both[k]->height; n++) {
					float *texel = &texels[n * 4];

					if (texel[3] > 1.5f) {
						VectorScale(texel, 1.0f / texel[3], texel);
						shared++;
					}
					texel[3] = texel[3] > 0.5f ? 1.0f : 0.0f;
				}
				if (k == 0 && shared) {
					ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d lightmap texels of page %d under more than one surface\n", shared, p);
				}
			}
			R_RtBakeDilateFloat(texels, both[k]->width, both[k]->height, 3);
			qglTextureSubImage2DEXT(both[k]->texnum, GL_TEXTURE_2D, 0, 0, 0, both[k]->width, both[k]->height, GL_RGBA, GL_FLOAT, texels);
			ri.Free(texels);
		}
	}
	R_RtBakeEnd();
	rt_bake.done = qtrue;
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d lamps baked onto %d lightmap pages in %d ms\n", rt_bake.lights, rt_bake.numPages,
		ri.Milliseconds() - rt_bake.startTime);
}

// r_rtShadowProbe n x y z: light n's faces of the world alone drawn into the
// third atlas (slot RT_SLOTS_BIG - 1, its owner's redrawn after), and the
// frame after, the depth there towards (x y z) against that point's (a test)
static int    rt_probeLight = -1, rt_probeStage;
static vec3_t rt_probePoint;

static void R_RtShadowProbe_f(void)
{
	rt_probeLight = atoi(ri.Cmd_Argv(1));
	VectorSet(rt_probePoint, atof(ri.Cmd_Argv(2)), atof(ri.Cmd_Argv(3)), atof(ri.Cmd_Argv(4)));
	rt_probeStage = (rt_probeLight >= 0 && rt_probeLight < rt.numLights && tr.rtShadowFbo[2]) ? 1 : 0;
}

static void R_RtShadowProbeStep(void)
{
	const int  slot = RT_SLOTS_BIG - 1, tile = R_RtBakedTile();
	rtLight_t *l;
	vec3_t     v, a;
	float      fwd, left, up, n = RT_ZNEAR, f, want, got[25];
	int        face, f2, x0, y0, px, py, k;

	if (!rt_probeStage) {
		return;
	}
	l = &rt.lights[rt_probeLight];
	if (rt_probeStage == 1) {
		for (f2 = 0; f2 < 6; f2++) {
			R_RtRenderFace(l, 2, slot, f2);
		}
		if (rt.staticOwner[slot] >= 0) {
			rt.lights[rt.staticOwner[slot]].bakedFaces = 0;
		}
		rt_probeStage = 2;
		return;
	}
	rt_probeStage = 0;
	VectorSubtract(rt_probePoint, l->origin, v);
	VectorSet(a, fabs(v[0]), fabs(v[1]), fabs(v[2]));
	if (a[0] >= a[1] && a[0] >= a[2]) {
		face = v[0] > 0 ? 0 : 1; fwd = a[0]; left = v[0] > 0 ? v[1] : -v[1]; up = v[2];
	} else if (a[1] >= a[2]) {
		face = v[1] > 0 ? 2 : 3; fwd = a[1]; left = v[1] > 0 ? -v[0] : v[0]; up = v[2];
	} else {
		face = v[2] > 0 ? 4 : 5; fwd = a[2]; left = v[2] > 0 ? -v[1] : v[1]; up = v[0];
	}
	f    = l->range;
	want = ((f + n) / (f - n) - 2.0f * f * n / ((f - n) * fwd)) * 0.5f + 0.5f;
	x0   = ((slot * 6 + face) % RT_ATLAS_TILES) * tile;
	y0   = ((slot * 6 + face) / RT_ATLAS_TILES) * tile;
	px   = x0 + (int)((-left / fwd * 0.5f + 0.5f) * tile);
	py   = y0 + (int)((up / fwd * 0.5f + 0.5f) * tile);
	Com_Memset(got, 0, sizeof(got));
	FBO_Bind(tr.rtShadowFbo[2]);
	qglReadPixels(Q_max(px - 2, x0), Q_max(py - 2, y0), 5, 5, GL_DEPTH_COMPONENT, GL_FLOAT, got);
	FBO_Bind(NULL);
	ri.Printf(PRINT_ALL, "light %d at %.0f %.0f %.0f to %.0f %.0f %.0f: face %d uv %.3f %.3f, fwd %.1f, depth %.6f\n", rt_probeLight, l->origin[0], l->origin[1],
		l->origin[2], rt_probePoint[0], rt_probePoint[1], rt_probePoint[2], face, -left / fwd * 0.5f + 0.5f, up / fwd * 0.5f + 0.5f, fwd, want);
	for (k = 0; k < 25; k++) {
		const float d = got[k] * 2.0f - 1.0f;
		ri.Printf(PRINT_ALL, "%7.1f%s", 2.0f * f * n / ((f + n) - d * (f - n)), k % 5 == 4 ? "\n" : " ");
	}
}

// r_rtBakeAt x y: the bake's surfaces over (x, y), top down, their normal, the
// lightmap texel there and what is baked in it (a test command)
static void R_RtBakeAt_f(void)
{
	const float x = atof(ri.Cmd_Argv(1)), y = atof(ri.Cmd_Argv(2));
	int         p, i, found = 0;

	// r_rtBakeAt x y z: the surfaces through that point, a wall's too, and
	// the texels round it
	if (ri.Cmd_Argc() > 3) {
		vec3_t pt;

		ri.Printf(PRINT_ALL, "  sun %d towards %.2f %.2f %.2f colour %.1f %.1f %.1f; live sun %.2f %.2f %.2f %.2f\n", rt.sun, rt.sunDir[0], rt.sunDir[1], rt.sunDir[2],
			rt.sunColor[0], rt.sunColor[1], rt.sunColor[2], tr.rtSunColor[0], tr.rtSunColor[1], tr.rtSunColor[2], tr.rtSunDir[3]);

		VectorSet(pt, x, y, atof(ri.Cmd_Argv(3)));
		for (p = 0; p < rt_bake.numPages && rt_bake.pages; p++) {
			rtBakePage_t *page = &rt_bake.pages[p];

			if (!page->image) {
				continue;
			}
			for (i = 0; i + 2 < page->list.num && found < 6; i += 3) {
				const rtBakeVert_t *v[3] = { &page->list.verts[i], &page->list.verts[i + 1], &page->list.verts[i + 2] };
				vec3_t              e1, e2, w, nrm;
				float               d00, d01, d11, d20, d21, den, b1, b2, s, tc, texel[4 * 9];
				int                 c, tx, ty, k;

				VectorSubtract(v[1]->xyz, v[0]->xyz, e1);
				VectorSubtract(v[2]->xyz, v[0]->xyz, e2);
				VectorSubtract(pt, v[0]->xyz, w);
				CrossProduct(e1, e2, nrm);
				if (VectorNormalize(nrm) < 1e-3f || fabs(DotProduct(w, nrm)) > 4.0f) {
					continue;
				}
				d00 = DotProduct(e1, e1);
				d01 = DotProduct(e1, e2);
				d11 = DotProduct(e2, e2);
				d20 = DotProduct(w, e1);
				d21 = DotProduct(w, e2);
				den = d00 * d11 - d01 * d01;
				b1  = (d11 * d20 - d01 * d21) / den;
				b2  = (d00 * d21 - d01 * d20) / den;
				if (b1 < 0 || b2 < 0 || b1 + b2 > 1) {
					continue;
				}
				s  = (1 - b1 - b2) * v[0]->lm[0] + b1 * v[1]->lm[0] + b2 * v[2]->lm[0];
				tc = (1 - b1 - b2) * v[0]->lm[1] + b1 * v[1]->lm[1] + b2 * v[2]->lm[1];
				for (c = 0; c < page->list.numChunks; c++) {
					if (i >= page->list.chunks[c].first && i < page->list.chunks[c].first + page->list.chunks[c].count) {
						break;
					}
				}
				tx = (int)(s * page->image->width);
				ty = (int)(tc * page->image->height);
				Com_Memset(texel, 0, sizeof(texel));
				if (R_RtBakeTarget(page, page->image)) {
					qglReadPixels(Q_max(tx - 1, 0), Q_max(ty - 1, 0), 3, 3, GL_RGBA, GL_FLOAT, texel);
				}
				// and the whole page, raw floats, to look at (a fourth argument)
				if (ri.Cmd_Argc() > 4 && found == 0) {
					float *all = ri.Malloc(sizeof(float) * page->image->width * page->image->height * 4);

					if (R_RtBakeTarget(page, page->image)) {
						qglReadPixels(0, 0, page->image->width, page->image->height, GL_RGBA, GL_FLOAT, all);
						ri.FS_WriteFile(va("%s.raw", ri.Cmd_Argv(4)), all, sizeof(float) * page->image->width * page->image->height * 4);
					}
					ri.Free(all);
				}
				R_RtBakeEnd();
				ri.Printf(PRINT_ALL, "  page %d (%dx%d) %s chunk %d (%d verts) normal %.2f %.2f %.2f (winding %.2f %.2f %.2f) lm %.4f %.4f texel %d %d\n", p,
					page->image->width, page->image->height, v[0]->shader ? v[0]->shader->name : "?", c,
					c < page->list.numChunks ? page->list.chunks[c].count : 0, v[0]->normal[0], v[0]->normal[1], v[0]->normal[2], nrm[0], nrm[1],
					nrm[2], s, tc, tx, ty);
				for (k = 0; k < 3; k++) {
					ri.Printf(PRINT_ALL, "    %6.1f %6.1f %6.1f\n", texel[(k * 3) * 4], texel[(k * 3 + 1) * 4], texel[(k * 3 + 2) * 4]);
				}
				found++;
				// its chunk's vertexes
				if (c < page->list.numChunks) {
					for (k = page->list.chunks[c].first; k < page->list.chunks[c].first + page->list.chunks[c].count; k++) {
						ri.Printf(PRINT_ALL, "    vert %.1f %.1f %.1f lm %.5f %.5f\n", page->list.verts[k].xyz[0], page->list.verts[k].xyz[1],
							page->list.verts[k].xyz[2], page->list.verts[k].lm[0], page->list.verts[k].lm[1]);
					}
				}
				// the baked lamps reaching it, unshadowed and unshaded
				if (found == 1 && ri.Cmd_Argc() > 5) {
					for (k = 0; k < rt.numMapLights; k++) {
						const rtLight_t *l = &rt.lights[k];
						vec3_t           to;
						vec4_t           shape;
						float            dd, ndl, fade, kind;

						if (!l->baked) {
							continue;
						}
						VectorSubtract(l->origin, pt, to);
						dd = VectorLength(to);
						if (dd >= l->range) {
							continue;
						}
						ndl  = DotProduct(to, nrm) / Q_max(dd, 1e-4f);
						fade = 1.0f - dd * dd / (l->range * l->range);
						kind = R_RtLightShape(l, shape);
						ri.Printf(PRINT_ALL, "    lamp %d at %.0f %.0f %.0f kind %.0f dist %.0f ndl %.2f shape %.2f %.2f %.2f %.1f point att %.1f%s\n", k,
							l->origin[0], l->origin[1], l->origin[2], kind, dd, ndl, shape[0], shape[1], shape[2], shape[3],
							Q_max(ndl, 0) * fade * fade / Q_max(dd * dd, 256.0f) * Q_max(l->color[0], Q_max(l->color[1], l->color[2])),
							l->noShadow ? " no shadow" : "");
						if (l->fixture >= 0 && l->fixture < tr.world->numStaticModels) {
							ri.Printf(PRINT_ALL, "      at %s\n", tr.world->staticModels[l->fixture].model);
						}
					}
				}
			}
		}
		if (!found) {
			ri.Printf(PRINT_ALL, "  no baked surface through %.0f %.0f %.0f\n", pt[0], pt[1], pt[2]);
		}
		return;
	}

	for (p = 0; p < rt_bake.numPages && rt_bake.pages; p++) {
		rtBakePage_t *page = &rt_bake.pages[p];

		if (!page->image) {
			continue;
		}
		for (i = 0; i + 2 < page->list.num && found < 6; i += 3) {
			const rtBakeVert_t *v[3] = { &page->list.verts[i], &page->list.verts[i + 1], &page->list.verts[i + 2] };
			float               area, b[3], z = 0, s = 0, tc = 0;
			vec3_t              n = { 0, 0, 0 };
			int                 k, c;

			area = (v[1]->xyz[0] - v[0]->xyz[0]) * (v[2]->xyz[1] - v[0]->xyz[1]) - (v[2]->xyz[0] - v[0]->xyz[0]) * (v[1]->xyz[1] - v[0]->xyz[1]);
			if (fabs(area) < 1e-3f) {
				continue;
			}
			b[0] = ((v[1]->xyz[0] - x) * (v[2]->xyz[1] - y) - (v[2]->xyz[0] - x) * (v[1]->xyz[1] - y)) / area;
			b[1] = ((v[2]->xyz[0] - x) * (v[0]->xyz[1] - y) - (v[0]->xyz[0] - x) * (v[2]->xyz[1] - y)) / area;
			b[2] = 1.0f - b[0] - b[1];
			if (b[0] < 0 || b[1] < 0 || b[2] < 0) {
				continue;
			}
			for (k = 0; k < 3; k++) {
				z += b[k] * v[k]->xyz[2];
				s += b[k] * v[k]->lm[0];
				tc += b[k] * v[k]->lm[1];
				VectorMA(n, b[k], v[k]->normal, n);
			}
			// its chunk: terrain or a face
			for (c = 0; c < page->list.numChunks; c++) {
				if (i >= page->list.chunks[c].first && i < page->list.chunks[c].first + page->list.chunks[c].count) {
					break;
				}
			}
			{
				float texel[4] = { 0, 0, 0, 0 };
				const int tx = (int)(s * page->image->width), ty = (int)(tc * page->image->height);

				if (R_RtBakeTarget(page, page->image)) {
					qglReadPixels(tx, ty, 1, 1, GL_RGBA, GL_FLOAT, texel);
				}
				R_RtBakeEnd();
				ri.Printf(PRINT_ALL, "  z %.1f %s chunk %d (%d verts) normal %.2f %.2f %.2f (len %.2f) lm %.4f %.4f texel %d %d baked %.1f %.1f %.1f\n", z,
					v[0]->shader ? v[0]->shader->name : "?", c, c < page->list.numChunks ? page->list.chunks[c].count : 0, n[0], n[1], n[2],
					VectorLength(n), s, tc, tx, ty, texel[0], texel[1], texel[2]);
			}
			found++;
		}
	}
	if (!found) {
		ri.Printf(PRINT_ALL, "  no baked surface over %.0f %.0f\n", x, y);
	}
}

// A frame of the bake: the lamps drawn last frame onto the pages, the next
// lamps' faces drawn for the next
static void R_RtBakeLamps(float ease)
{
	int k;

	if (r_realtimeLighting->integer != 2 || !rt.loaded) {
		return;
	}
	// a vid_restart: its images are gone, drawn again
	if (rt_bake.started && (rt_bake.atlas != tr.rtShadowImage[2] || (tr.rtShadowImage[2] && rt_bake.atlasTexnum != tr.rtShadowImage[2]->texnum))) {
		R_RtFreeBakedLamps();
	}
	if (!rt_bake.started) {
		if (rt.frame < RT_BAKE_WAIT) {
			return;
		}
		R_RtBakeStart();
	}
	if (rt_bake.done) {
		rt_bake.weight = Q_min(1.0f, rt_bake.weight + ease);
		return;
	}
	if (!rt_bake.pages) {
		return;
	}

	if (rt_bake.numPending) {
		R_RtBakeBegin(0.0f, GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		for (k = 0; k < rt_bake.numPending; k++) {
			R_RtBakePass(rt_bake.pending[k], rt_bake.region * RT_BAKE_BATCH + k);
		}
		R_RtBakeEnd();
		rt_bake.numPending = 0;
	}
	if (rt_bake.next >= rt.numMapLights) {
		R_RtBakeFinish();
		return;
	}

	rt_bake.region ^= 1;
	for (; rt_bake.next < rt.numMapLights && rt_bake.numPending < RT_BAKE_BATCH; rt_bake.next++) {
		rtLight_t *l = &rt.lights[rt_bake.next];
		int        f;

		// r_rtBakedDebug 100 + n: light n alone (a test)
		if (!l->baked || (r_rtBakedDebug->integer >= 100 && rt_bake.next != r_rtBakedDebug->integer - 100)) {
			continue;
		}
		if (!l->noShadow && r_rtShadows->integer) {
			for (f = 0; f < 6; f++) {
				R_RtRenderFace(l, 2, rt_bake.region * RT_BAKE_BATCH + rt_bake.numPending, f);
			}
		}
		rt_bake.pending[rt_bake.numPending++] = rt_bake.next;
	}
}

void R_RtLoadWorld(void)
{
	int i;

	R_RtFreeWorld();

	if (!r_realtimeLighting->integer || !tr.world) {
		return;
	}

	// and a light for each static model, which may be a lamp (R_RtLightsAtLamps)
	rt.lights     = ri.Malloc(sizeof(rtLight_t) * (tr.numSLights + tr.world->numStaticModels + RT_ENTITY_LAMPS));
	rt.fromSphere = ri.Malloc(sizeof(int) * Q_max(1, tr.numSLights));
	rt.numLights  = 0;
	for (i = 0; i < tr.numSLights; i++) {
		rt.fromSphere[i] = -1;
	}

	for (i = 0; i < tr.numSLights; i++) {
		const spherel_t *sl = &tr.sLights[i];
		rtLight_t       *l;
		float            brightest;

		// the dark lights are for models only; a spot whose cone is less than
		// nothing is a lit surface's (R_RtAreaLights), which only
		// r_realtimeLighting 2 takes: in 1 its light is in the lightmaps
		if (sl->intensity <= 0 || (sl->spot_light && sl->spot_radiusbydistance == 0)
			|| (sl->spot_light && sl->spot_radiusbydistance < 0 && r_realtimeLighting->integer != 2)) {
			continue;
		}

		l = &rt.lights[rt.numLights++];
		Com_Memset(l, 0, sizeof(*l));
		l->source     = i;
		l->mergedInto = -1;
		l->fixture    = -1;
		l->entity     = -1;
		VectorCopy(sl->origin, l->origin);
		VectorScale(sl->color, sl->intensity * 7500.0f, l->color);
		l->spot         = sl->spot_light ? qtrue : qfalse;
		l->radiusByDist = sl->spot_radiusbydistance;
		VectorCopy(sl->spot_dir, l->spotDir);
		if (l->spot) {
			VectorNormalize(l->spotDir);
		}
		if (l->spot && l->radiusByDist < 0) {
			R_RtAreaLight(l);
		}

		brightest = Q_max(l->color[0], Q_max(l->color[1], l->color[2]));
		l->range  = Q_min(4096.0f, sqrt(brightest / RT_CUTOFF));
		l->staticSlot = -1;
	}

	// the sun as worldspawn has it (R_Sphere_InitLights scaled it for models),
	// and its sky: before the lights, which daylight lets go of
	rt.sun = s_sun.exists;
	if (rt.sun) {
		VectorCopy(s_sun.direction, rt.sunDir);
		VectorNormalize(rt.sunDir);
		VectorScale(s_sun.color, 1.0f / Q_max(1.0f, tr.overbrightMult), rt.sunColor);
	}
	R_RtParseAmbient();

	R_RtLightsAtFixtures();
	R_RtFindPools();

	// room for the lamps the game moves, which are entities (R_RtEntityLamps)
	rt.numMapLights = rt.numLights;
	for (i = 0; i < RT_ENTITY_LAMPS; i++) {
		rtLight_t *l = &rt.lights[rt.numLights++];

		Com_Memset(l, 0, sizeof(*l));
		l->source     = -1;
		l->mergedInto = -1;
		l->fixture    = -1;
		l->staticSlot = -1;
		l->entity     = -1;
		l->cluster    = -1;
	}
	Com_Memset(rt_modelDefLooked, 0, sizeof(rt_modelDefLooked));

	// whether the map lists the lights reaching each leaf (R_RtMarkSeenLights)
	for (i = tr.world->numDecisionNodes; i < tr.world->numnodes && !rt.leafLights; i++) {
		rt.leafLights = tr.world->nodes[i].numlights > 0;
	}

	for (i = 0; i < RT_SLOTS; i++) {
		rt.staticOwner[i] = -1;
	}

	rt.seenLights  = ri.Malloc(sizeof(int) * Q_max(1, rt.numLights));
	rt.seenCluster = -1;
	rt.candidateOf = ri.Malloc(sizeof(int) * Q_max(1, rt.numLights));
	for (i = 0; i < rt.numLights; i++) {
		rt.candidateOf[i] = -1;
	}

	R_RtSetupSky();

	rt.loaded = qtrue;
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d lights%s\n", rt.numLights, rt.sun ? " and the sun" : "");
}

void R_RtFreeWorld(void)
{
	R_RtFreeBakedLamps();
	if (rt.lights) {
		ri.Free(rt.lights);
	}
	if (rt.fromSphere) {
		ri.Free(rt.fromSphere);
	}
	if (rt.seenLights) {
		ri.Free(rt.seenLights);
	}
	if (rt.candidateOf) {
		ri.Free(rt.candidateOf);
	}
	R_RtBounceFree();
	Com_Memset(&rt, 0, sizeof(rt));
	tr.rtNumActive = 0;

	Com_Memset(rt_entities, 0, sizeof(rt_entities));
	Com_Memset(rt_clientModels, 0, sizeof(rt_clientModels));
	if (rt_staticModels) {
		ri.Free(rt_staticModels);
		rt_staticModels = NULL;
	}
	rt_numStaticModels = 0;
}

/*
=============================================================================

SHADOWS

=============================================================================
*/

// A face's forward, left and up, as RtShadow in the shaders picks them.
static void R_RtFaceAxes(int face, vec3_t axis[3])
{
	switch (face) {
	case 0:
		VectorSet(axis[0], 1, 0, 0);
		VectorSet(axis[1], 0, 1, 0);
		VectorSet(axis[2], 0, 0, 1);
		break;
	case 1:
		VectorSet(axis[0], -1, 0, 0);
		VectorSet(axis[1], 0, -1, 0);
		VectorSet(axis[2], 0, 0, 1);
		break;
	case 2:
		VectorSet(axis[0], 0, 1, 0);
		VectorSet(axis[1], -1, 0, 0);
		VectorSet(axis[2], 0, 0, 1);
		break;
	case 3:
		VectorSet(axis[0], 0, -1, 0);
		VectorSet(axis[1], 1, 0, 0);
		VectorSet(axis[2], 0, 0, 1);
		break;
	case 4:
		VectorSet(axis[0], 0, 0, 1);
		VectorSet(axis[1], 0, -1, 0);
		VectorSet(axis[2], 1, 0, 0);
		break;
	default:
		VectorSet(axis[0], 0, 0, -1);
		VectorSet(axis[1], 0, 1, 0);
		VectorSet(axis[2], 1, 0, 0);
		break;
	}
}

// Where a slot's face is in its atlas (RtShadowAtlas in the shaders)
static void R_RtFaceRect(int atlas, int slot, int face, int *x, int *y, int *size)
{
	const int tile = atlas == 2 ? R_RtBakedTile() : r_rtShadowTile->integer;

	if (atlas == 1 || slot < RT_SLOTS_BIG) {
		const int t = slot * 6 + face;

		*x    = (t % RT_ATLAS_TILES) * tile;
		*y    = (t / RT_ATLAS_TILES) * tile;
		*size = tile;
	} else {
		const int q = (slot - RT_SLOTS_BIG) * 6 + face;

		*x    = (q % (RT_ATLAS_TILES * 2)) * (tile / 2);
		*y    = (RT_SMALL_FIRST / RT_ATLAS_TILES * 2 + q / (RT_ATLAS_TILES * 2)) * (tile / 2);
		*size = tile / 2;
	}
}

// A slot's first face as the shaders are sent it
static float R_RtSlotCode(int slot)
{
	return slot < RT_SLOTS_BIG ? (float)(slot * 6) : (float)(RT_SMALL_CODE + (slot - RT_SLOTS_BIG) * 6);
}

static void R_RtRenderFace(const rtLight_t *l, int atlas, int slot, int face)
{
	viewParms_t parms;
	int         x, y, size;

	R_RtFaceRect(atlas, slot, face, &x, &y, &size);
	Com_Memset(&parms, 0, sizeof(parms));
	parms.viewportX      = x;
	parms.viewportY      = y;
	parms.viewportWidth  = size;
	parms.viewportHeight = size;
	parms.isPortal       = qfalse;
	parms.isMirror       = qfalse;
	parms.fovX           = 90;
	parms.fovY           = 90;
	parms.flags          = VPF_SHADOWMAP | VPF_DEPTHSHADOW | VPF_NOVIEWMODEL | VPF_NOCUBEMAPS
				| (atlas == 0 ? VPF_RTSTATIC : atlas == 1 ? VPF_RTDYNAMIC : VPF_RTBAKED);
	parms.zFar           = l->range;
	parms.zNear          = RT_ZNEAR;
	parms.targetFbo      = tr.rtShadowFbo[atlas];

	VectorCopy(l->origin, parms.ori.origin);
	R_RtFaceAxes(face, parms.ori.axis);
	VectorCopy(l->origin, parms.pvsOrigin);

	R_RenderView(&parms);
	rt.facesDrawn++;
}

// Is a sphere in any of a light's faces? A bit a face.
static int R_RtFacesTouched(const rtLight_t *l, float range, const vec3_t centre, float radius)
{
	vec3_t v;
	int    faces = 0, f;

	VectorSubtract(centre, l->origin, v);
	if (DotProduct(v, v) > Square(range + radius)) {
		return 0;
	}

	for (f = 0; f < 6; f++) {
		vec3_t axis[3];
		float  fwd, left, up;

		R_RtFaceAxes(f, axis);
		fwd  = DotProduct(v, axis[0]);
		left = DotProduct(v, axis[1]);
		up   = DotProduct(v, axis[2]);

		// the four sides of a 90 degree face are |left| = fwd and |up| = fwd
		if (fwd + radius <= 0) {
			continue;
		}
		if ((fwd - left) * RT_SQRT1_2 < -radius || (fwd + left) * RT_SQRT1_2 < -radius) {
			continue;
		}
		if ((fwd - up) * RT_SQRT1_2 < -radius || (fwd + up) * RT_SQRT1_2 < -radius) {
			continue;
		}
		faces |= 1 << f;
	}

	return faces;
}

// Things that move this frame, as spheres.
typedef struct {
	vec3_t   centre;
	float    radius;
	uint64_t lights; // the candidates (R_RtRenderShadows) that reach it, a bit each
	uint64_t *casts; // its entity's or static model's rtLights
	qboolean self;   // the player's own body or what it holds (RF_THIRD_PERSON), drawn for the shadow alone
} rtMover_t;

#define RT_MAX_MOVERS 1024

static void R_RtStandingSphere(const vec3_t centre, float radius)
{
	int i;

	rt.invalidated++;

	for (i = 0; i < rt.numLights; i++) {
		rtLight_t *l = &rt.lights[i];

		if (l->staticSlot >= 0 && l->staticFaces) {
			l->staticFaces &= ~R_RtFacesTouched(l, l->range, centre, radius);
		}
	}
}

// What it is now against what it was, and so which shadows it is drawn in
// (RT_CASTS_*). A new one has just moved.
static int R_RtTrack(rtTracked_t *t, const float *pose, const vec3_t centre, float radius, int now)
{
	qboolean moved = qfalse;
	int      k;

	if (!t->used) {
		Com_Memset(t, 0, sizeof(*t));
		t->used = qtrue;
		moved   = qtrue;
	} else {
		for (k = 0; k < 10; k++) {
			if (fabs(pose[k] - t->pose[k]) > (k < 3 ? 0.05f : 0.001f)) {
				moved = qtrue;
				break;
			}
		}
	}

	if (moved) {
		if (t->joined) {
			// it leaves what stands still, where it stood
			R_RtStandingSphere(t->centre, t->radius);
			t->joined = 0;
		}
		t->lastMoved = now;
		Com_Memcpy(t->pose, pose, sizeof(t->pose));
		VectorCopy(centre, t->centre);
		t->radius = radius;
	} else if (!t->joined && now - t->lastMoved >= RT_SETTLE_MSEC) {
		// it joins what stands still
		t->joined = now;
		R_RtStandingSphere(t->centre, t->radius);
	}

	t->seen = rt.frame;

	if (!t->joined) {
		return RT_CASTS_MOVING;
	}
	return now - t->joined < RT_HANDOVER_MSEC ? (RT_CASTS_MOVING | RT_CASTS_STANDING) : RT_CASTS_STANDING;
}

// Those not seen this frame: if they stood with what stands still, they have
// gone from it.
static void R_RtForget(rtTracked_t *list, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		rtTracked_t *t = &list[i];

		if (t->used && t->seen != rt.frame) {
			if (t->joined) {
				R_RtStandingSphere(t->centre, t->radius);
			}
			t->used = qfalse;
		}
	}
}

static float R_RtAnimation(const refEntity_t *e)
{
	float sum = 0;
	int   i;

	for (i = 0; i < MAX_FRAMEINFOS; i++) {
		if (e->frameInfo[i].weight > 0) {
			sum += e->frameInfo[i].index * 7.0f + e->frameInfo[i].time * 1000.0f + e->frameInfo[i].weight * 100.0f;
		}
	}
	return sum;
}

// Which entities and moved static models cast shadows, standing or moving,
// and the moving ones as spheres.
static int R_RtTrackCasters(rtMover_t *movers)
{
	const int now   = ri.Milliseconds();
	int       count = 0, i;

	for (i = 0; i < tr.refdef.num_entities; i++) {
		trRefEntity_t *ent = &tr.refdef.entities[i];
		const model_t *model;
		rtTracked_t   *t;
		vec3_t         centre;
		float          radius, pose[10];

		ent->rtCaster = 0;
		ent->rtLights = ~(uint64_t)0;
		if (ent->e.reType != RT_MODEL) {
			continue;
		}
		if (ent->e.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK | RF_NOSHADOW | RF_SKYENTITY)) {
			continue;
		}

		model = R_GetModelByHandle(ent->e.hModel);
		if (!model) {
			continue;
		}
		// a lamp (lamps.txt): its light is in it
		if (R_RtModelLampDef(ent->e.hModel)) {
			continue;
		}

		// people and bodies keep the sun's shadows alone, unless asked, and
		// so do what they carry
		if (!r_rtShadowCharacters->integer) {
			if (R_RtIsCharacter(&ent->e)) {
				continue;
			}
			if (ent->e.parentEntity != ENTITYNUM_NONE && ent->e.parentEntity >= 0 && ent->e.parentEntity < tr.refdef.num_entities
				&& R_RtIsCharacter(&tr.refdef.entities[ent->e.parentEntity].e)) {
				continue;
			}
		}

		if (model->type == MOD_BRUSH && model->bmodel) {
			vec3_t size, local;

			VectorSubtract(model->bmodel->bounds[1], model->bmodel->bounds[0], size);
			radius = VectorLength(size) * 0.5f;
			VectorAdd(model->bmodel->bounds[0], model->bmodel->bounds[1], local);
			VectorScale(local, 0.5f, local);
			VectorCopy(ent->e.origin, centre);
			VectorMA(centre, local[0], ent->e.axis[0], centre);
			VectorMA(centre, local[1], ent->e.axis[1], centre);
			VectorMA(centre, local[2], ent->e.axis[2], centre);
		} else if (model->type == MOD_TIKI) {
			radius = R_GetRadius(&ent->e);
			VectorCopy(ent->e.origin, centre);
		} else {
			continue;
		}
		// spent cases and chips: too small for a shadow anyone sees, and
		// being followed by none they were movers, every light near them
		// drawing its faces again each frame they flew
		if (radius <= 0 || radius < r_rtCasterMinRadius->value) {
			continue;
		}

		VectorCopy(ent->e.origin, pose);
		VectorCopy(ent->e.axis[0], pose + 3);
		VectorCopy(ent->e.axis[1], pose + 6);
		pose[9] = R_RtAnimation(&ent->e);

		if (ent->e.entityNumber >= 0 && ent->e.entityNumber < ENTITYNUM_WORLD) {
			t = &rt_entities[ent->e.entityNumber];
		} else if (ent->e.hModel > 0 && ent->e.hModel < MAX_MOD_KNOWN) {
			t = &rt_clientModels[ent->e.hModel];
		} else {
			t = NULL;
		}

		if (!t || (t->used && t->seen == rt.frame)) {
			// nothing to follow it by, or two share it: it moves
			ent->rtCaster = RT_CASTS_MOVING;
		} else {
			ent->rtCaster = R_RtTrack(t, pose, centre, radius, now);
		}

		if ((ent->rtCaster & RT_CASTS_MOVING) && count < RT_MAX_MOVERS) {
			VectorCopy(centre, movers[count].centre);
			movers[count].radius = radius;
			movers[count].casts  = &ent->rtLights;
			movers[count].self   = (ent->e.renderfx & RF_THIRD_PERSON) != 0;
			count++;
		}
	}

	// the static models the client moved
	if (rt_numStaticModels != tr.world->numStaticModels) {
		if (rt_staticModels) {
			ri.Free(rt_staticModels);
		}
		rt_numStaticModels = tr.world->numStaticModels;
		rt_staticModels    = ri.Malloc(sizeof(rtTracked_t) * Q_max(1, rt_numStaticModels));
		Com_Memset(rt_staticModels, 0, sizeof(rtTracked_t) * Q_max(1, rt_numStaticModels));
	}

	for (i = 0; i < tr.world->numStaticModels; i++) {
		cStaticModelUnpacked_t *sm = &tr.world->staticModels[i];
		float                   pose[10];

		sm->rtLights = ~(uint64_t)0;
		if (!sm->moved || !sm->tiki) {
			continue;
		}

		VectorCopy(sm->origin, pose);
		VectorCopy(sm->axis[0], pose + 3);
		VectorCopy(sm->axis[1], pose + 6);
		pose[9]      = 0;
		sm->rtCaster = R_RtTrack(&rt_staticModels[i], pose, sm->origin, sm->cull_radius, now);

		if ((sm->rtCaster & RT_CASTS_MOVING) && count < RT_MAX_MOVERS) {
			VectorCopy(sm->origin, movers[count].centre);
			movers[count].radius = sm->cull_radius;
			movers[count].casts  = &sm->rtLights;
			movers[count].self   = qfalse;
			count++;
		}
	}

	R_RtForget(rt_entities, MAX_GENTITIES);
	R_RtForget(rt_clientModels, MAX_MOD_KNOWN);

	return count;
}

// A free slot of whole tiles (big) or quarter tiles in the atlas of what
// stands still, else the one used longest ago; -1 none
static int R_RtFindSlot(qboolean big)
{
	const int first = big ? 0 : RT_SLOTS_BIG, last = big ? RT_SLOTS_BIG : RT_SLOTS;
	int       i, best = -1, oldest = 0x7fffffff;

	for (i = first; i < last; i++) {
		int owner = rt.staticOwner[i];

		if (owner < 0) {
			return i;
		}
		if (rt.lights[owner].lastUsed == rt.frame) {
			continue;
		}
		if (rt.lights[owner].lastUsed < oldest) {
			oldest = rt.lights[owner].lastUsed;
			best   = i;
		}
	}
	return best;
}

// A slot in the atlas of what stands still: whole tiles for the nearest
// (big), quarter tiles for those further off, either if the other is full.
static int R_RtStaticSlot(int light, qboolean big)
{
	int best = R_RtFindSlot(big);

	if (best < 0) {
		best = R_RtFindSlot(!big);
	}
	if (best < 0) {
		return -1;
	}
	// a lamp whose bake is out goes back in with the faces it went out with,
	// before they are lost
	R_RtLiveSet(light, qfalse);
	if (rt.staticOwner[best] >= 0) {
		R_RtLiveSet(rt.staticOwner[best], qfalse);
	}
	if (rt.lights[light].staticSlot >= 0) {
		rt.staticOwner[rt.lights[light].staticSlot] = -1;
	}

	if (rt.staticOwner[best] >= 0) {
		rtLight_t *old = &rt.lights[rt.staticOwner[best]];

		old->staticSlot  = -1;
		old->staticFaces = 0;
		old->staticReady = qfalse;
		old->bakedFaces  = 0;
	}
	rt.staticOwner[best] = light;
	rt.lights[light].staticSlot  = best;
	rt.lights[light].staticFaces = 0;
	rt.lights[light].staticReady = qfalse;
	rt.lights[light].bakedFaces  = 0;
	return best;
}

/*
The bodies of the people in the view, as capsules (R_RtCharacterCapsules), for
the lights that do not draw them into their shadow faces: those past the few
with faces for what moves (r_rtDynamicLights), or all with r_rtShadowCharacters
off.

Each body is a sphere around all of it, then its capsules, so that the shaders
pass a body with one test: out[b * 2] is the sphere, out[b * 2 + 1].x how many
capsules follow it.
*/
#define RT_MAX_BODY_ENTRIES 1024

static vec4_t bodies[RT_MAX_BODY_ENTRIES * 2];

static int R_RtGatherBodies(vec4_t *out)
{
	int count = 0, i, c;

	if (!r_rtShadows->integer || !r_rtCapsules->integer || !tr.rtCapsuleImage) {
		return 0;
	}

	for (i = 0; i < tr.refdef.num_entities && count + 2 < RT_MAX_BODY_ENTRIES; i++) {
		trRefEntity_t *ent = &tr.refdef.entities[i];
		vec4_t        *head = &out[count * 2];
		vec3_t         mins, maxs;
		float          radius = 0;
		int            n;

		if (ent->e.reType != RT_MODEL || (ent->e.renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK | RF_NOSHADOW | RF_SKYENTITY))) {
			continue;
		}
		if (!R_RtIsCharacter(&ent->e)) {
			continue;
		}
		// one that has lain still long enough is drawn into the shadow faces
		// of what stands still, of every light: as capsules too, it cast two
		// shadows wherever a light had no faces for what moves
		if (r_rtShadowCharacters->integer && ent->rtCaster == RT_CASTS_STANDING) {
			continue;
		}
		n = R_RtCharacterCapsules(&ent->e, &out[(count + 1) * 2], RT_MAX_BODY_ENTRIES - count - 1);
		if (!n) {
			continue;
		}

		ClearBounds(mins, maxs);
		for (c = 0; c < n * 2; c++) {
			AddPointToBounds(out[(count + 1) * 2 + c], mins, maxs);
		}
		VectorAdd(mins, maxs, head[0]);
		VectorScale(head[0], 0.5f, head[0]);
		for (c = 0; c < n * 2; c++) {
			const float *e = out[(count + 1) * 2 + c];
			radius = MAX(radius, Distance(e, head[0]) + e[3]);
		}
		head[0][3] = radius;
		VectorSet4(head[1], n, 0, 0, 0);
		count += 1 + n;
	}

	return count;
}

// The light's own run of the bodies within its reach, packed for the shaders:
// where it starts, with how many entries in the fraction; -1 if none.
static float R_RtLightCapsules(const rtLight_t *l, const vec4_t *entries, int numEntries)
{
	int start = rt.numPacked, i, n;

	for (i = 0; i < numEntries; i += 1 + n) {
		const float *sphere = entries[i * 2];

		n = (int)entries[i * 2 + 1][0];
		if (rt.numPacked - start + 1 + n > RT_MAX_RUN || rt.numPacked + 1 + n > RT_MAX_CAPSULES) {
			break;
		}
		if (DistanceSquared(sphere, l->origin) > Square(l->range + sphere[3])) {
			continue;
		}
		// a lamp in someone's hands: no shadow of them
		if (Distance(sphere, l->origin) < sphere[3] * 0.5f) {
			continue;
		}
		// out of a spot light's cone, soft edge and all, and so of the light
		// on whatever it could shade
		if (l->spot) {
			vec3_t d, perp;
			float  along;

			VectorSubtract(sphere, l->origin, d);
			along = DotProduct(d, l->spotDir);
			if (along < -sphere[3]) {
				continue;
			}
			VectorMA(d, -along, l->spotDir, perp);
			if (VectorLength(perp) > l->radiusByDist * (MAX(along, 0) + sphere[3]) + 32.0f + sphere[3]) {
				continue;
			}
		}

		Com_Memcpy(rt.packed[rt.numPacked * 2], entries[i * 2], (1 + n) * 2 * sizeof(vec4_t));
		rt.numPacked += 1 + n;
	}

	return rt.numPacked > start ? start + (rt.numPacked - start) / (float)(RT_MAX_RUN + 1) : -1.0f;
}

static int R_RtCompareDistance(const void *a, const void *b)
{
	const float da = ((const float *)a)[1], db = ((const float *)b)[1];

	return da < db ? -1 : da > db ? 1 : 0;
}

// What stood still has changed there (furniture came loose, a static model
// moved): the lights that reach it draw it again.
void R_RtStandingChanged(const vec3_t mins, const vec3_t maxs)
{
	vec3_t centre, size;

	if (!rt.loaded) {
		return;
	}

	VectorAdd(mins, maxs, centre);
	VectorScale(centre, 0.5f, centre);
	VectorSubtract(maxs, mins, size);
	R_RtStandingSphere(centre, VectorLength(size) * 0.5f);
}

/*
How far a light gives a pixel facing it at least cutoff, unshadowed: the
shaders pass it over beyond (RtLight), so the lists of the lights a surface
may take (R_RtSphereMask and the rest) can leave it out there. What it gives
falls with distance all the way to its reach (the compiler's falloff,
faded to nothing at the edge), so the distance is found by halving.

Every light gives most near its reach least: a lamp of the map is lit out to
where it adds a raw lightmap unit, faded, and most of the pixels it reaches
are in its outer shell, where it adds a fraction of the smallest step a
screen shows, and each was paying for its shadow taps all the same. -1: it
gives cutoff nowhere.
*/
static float R_RtReach(const vec4_t colorRange, float cutoff)
{
	const float range = colorRange[3];
	const float peak  = Q_max(colorRange[0], Q_max(colorRange[1], colorRange[2]));
	float       lo = 0, hi = range;
	int         i;

	if (cutoff <= 0 || range <= 0) {
		return range;
	}
	// what it gives at d, as RtLight works it out with ndl 1 and no spot cone
#define RT_GIVES(d) (peak * Square(1.0f - Square(d) / Square(range)) / Q_max(Square(d), 256.0f))
	if (RT_GIVES(0.0f) < cutoff) {
		return -1.0f;
	}
	for (i = 0; i < 20; i++) {
		const float mid = (lo + hi) * 0.5f;

		if (RT_GIVES(mid) >= cutoff) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
#undef RT_GIVES
	return hi;
}

/*
The candidate lights that reach a mover: those the map lists for any leaf its
bounds touch (the lists R_RtMarkSeenLights reads). A lamp behind a wall from it
is not among them, and a shadow is cast only between a light and what it
lights: the mover can darken nothing for that lamp, and its faces need not be
drawn again as it moves. All of them where the map has no such lists.
*/
static uint64_t R_RtLeafLights(const mnode_t *node, const vec3_t mins, const vec3_t maxs, const int *candidateOf)
{
	uint64_t lights = 0;
	int      k;

	while (node->contents == CONTENTS_NODE) {
		const int side = BoxOnPlaneSide(mins, maxs, node->plane);

		if (side == 3) {
			lights |= R_RtLeafLights(node->children[0], mins, maxs, candidateOf);
			node = node->children[1];
		} else {
			node = node->children[side == 1 ? 0 : 1];
		}
	}

	for (k = 0; k < node->numlights; k++) {
		const spherel_t *sl = node->lights[k];
		int              index, c;

		if (sl == &tr.sSunLight) {
			continue;
		}
		index = rt.fromSphere[sl - tr.sLights];
		if (index >= 0 && (c = candidateOf[index]) >= 0) {
			lights |= (uint64_t)1 << c;
		}
	}
	return lights;
}

/*
A lamp's light goes where its lamp is: a lantern knocked off a table, a lamp
set swinging. Its shadow of what stands still was drawn from where it was, and
is drawn again from where it is (R_RtRenderShadows).
*/
static void R_RtFollowFixtures(void)
{
	int i, j;

	for (i = 0; i < rt.numLights; i++) {
		rtLight_t                    *l = &rt.lights[i];
		const cStaticModelUnpacked_t *sm;
		vec3_t                        origin, dir;
		qboolean                      turned = qfalse;

		if (l->fixture < 0 || l->fixture >= tr.world->numStaticModels) {
			continue;
		}
		sm = &tr.world->staticModels[l->fixture];
		if (!sm->moved) {
			continue;
		}
		VectorCopy(sm->origin, origin);
		VectorClear(dir);
		for (j = 0; j < 3; j++) {
			VectorMA(origin, l->fixtureLocal[j], sm->axis[j], origin);
			VectorMA(dir, l->spotDirLocal[j], sm->axis[j], dir);
		}
		// a spot light turns with it. The axes may be scaled down to almost
		// nothing, for a model the client draws in its place, and
		// VectorNormalize leaves a vector that short as it is: brought back
		// to their length first.
		{
			const float length = VectorLength(sm->axis[2]);

			if (length > 0.0f) {
				VectorScale(dir, 1.0f / length, dir);
			}
		}
		if (l->lamp && !l->spot) {
			// its shade turns with it
			VectorCopy(dir, l->spotDir);
		} else if (l->spot && VectorNormalize(dir) > 0.5f && DotProduct(dir, l->spotDir) < 0.9998f) {
			VectorCopy(dir, l->spotDir);
			turned = qtrue;
		}
		if (!turned && DistanceSquared(origin, l->origin) < Square(RT_LIGHT_MOVE_EPSILON)) {
			continue;
		}
		VectorCopy(origin, l->origin);
		l->movedFrame  = rt.frame;
		l->staticFaces = 0;
		if (l->lamp) {
			const int cluster = R_RtClusterAt(origin);

			if (cluster != l->cluster) {
				l->cluster     = cluster;
				rt.seenCluster = -1; // the view's list is made again
			}
		}
	}
}

/*
===============
R_RtRenderShadows

Once a frame, before the main view: the lights that light what can be seen,
their shadows, and what the shaders are told of them.
===============
*/
// Whether a light reaches into the view's four sides at all
static qboolean R_RtReachesView(const rtLight_t *l, float range, const cplane_t frustum[4])
{
	int k;

	for (k = 0; k < 4; k++) {
		if (DotProduct(l->origin, frustum[k].normal) - frustum[k].dist < -range) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
Which of a light's faces may be seen: a bit each. A face holds the shadows of
what is within its 90 degree pyramid out to the light's reach, which is read
only by the pixels in it; a face whose pyramid is wholly behind one of the
view's sides, or behind the view, is read by none of the view's pixels. The
pyramid is its apex and the four corners of its far side, and the sides are
moved out a little, for the shaders' offset off a surface and the shadow
taps' spread.
*/
#define RT_FACE_VIEW_MARGIN 32.0f
#define RT_OUT_OF_VIEW      1e6f // after every light that reaches into the view
#define RT_HIDDEN           1e3f // after every light whose bulbs or pool the view sees
#define RT_SEEN_EVERY       4    // frames between looks at whether the view sees a light
#define RT_VIEW_MARGIN      64.0f
#define RT_DLIGHT_ROOM      8    // of the lights the shaders take, kept for the game's
#define RT_FACES_BURST      3    // r_rtFacesPerFrame this many times more for lights come into the view
#define RT_ENTER_FRAMES     30   // a light this many frames come into the view is lit at once

static int R_RtFacesInView(const rtLight_t *l, float range, const cplane_t frustum[5])
{
	int faces = 0, f, c, k;

	for (f = 0; f < 6; f++) {
		vec3_t axis[3], points[5];

		R_RtFaceAxes(f, axis);
		VectorCopy(l->origin, points[0]);
		for (c = 0; c < 4; c++) {
			VectorMA(l->origin, range, axis[0], points[c + 1]);
			VectorMA(points[c + 1], (c & 1) ? range : -range, axis[1], points[c + 1]);
			VectorMA(points[c + 1], (c & 2) ? range : -range, axis[2], points[c + 1]);
		}

		for (k = 0; k < 5; k++) {
			for (c = 0; c < 5; c++) {
				if (DotProduct(points[c], frustum[k].normal) - frustum[k].dist > -RT_FACE_VIEW_MARGIN) {
					break;
				}
			}
			if (c == 5) {
				break; // all of it behind this side
			}
		}
		if (k == 5) {
			faces |= 1 << f;
		}
	}

	return faces;
}

// Whether a point is in the view, out to margin past its sides
static qboolean R_RtPointInView(const vec3_t p, const cplane_t frustum[5], float margin)
{
	int k;

	for (k = 0; k < 5; k++) {
		if (DotProduct(p, frustum[k].normal) - frustum[k].dist < -margin) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
Whether a light's light is in the view: its bulbs, or its pool, or the view
well inside its reach (a lamp overhead lights the floor ahead). Its reach
alone takes in the view from most of the map: a lamp of the map gives a
little light a long way.
*/
static qboolean R_RtInView(const rtLight_t *l, float reach, const vec3_t vieworg, const cplane_t frustum[5])
{
	if (DistanceSquared(l->origin, vieworg) < Square(reach * 0.5f)) {
		return qtrue;
	}
	return (R_RtPointInView(l->origin, frustum, RT_VIEW_MARGIN) || R_RtPointInView(l->pool, frustum, RT_VIEW_MARGIN)) ? qtrue : qfalse;
}

// Whether a line from the view gets to p
static qboolean R_RtClearTo(const vec3_t vieworg, const vec3_t p)
{
	trace_t trace;
	vec3_t  zero = {0, 0, 0};

	ri.CM_BoxTrace(&trace, vieworg, p, zero, zero, 0, CONTENTS_SOLID, qfalse);
	return (!trace.startsolid && (trace.fraction >= 0.999f || Distance(trace.endpos, p) < RT_LAMP_SEEN)) ? qtrue : qfalse;
}

/*
Whether the view sees a light's bulbs or its pool: the map's lists say what
may be seen from the view's cluster, which out of doors is most of the map,
the lamps in every house round about. Looked at again every few frames, a
few lights a frame.
*/
static qboolean R_RtViewSees(rtLight_t *l, int index, const vec3_t vieworg)
{
	if (!l->seenTested || rt.frame - l->seenTested >= RT_SEEN_EVERY * 4
		|| (rt.frame + index) % RT_SEEN_EVERY == 0) {
		l->seenByView = (R_RtClearTo(vieworg, l->origin) || R_RtClearTo(vieworg, l->pool)) ? qtrue : qfalse;
		l->seenTested = rt.frame;
	}
	return l->seenByView;
}

/*
The lights that may reach what the view can see: those the map lists for any
leaf in the view's PVS. A lamp across town whose reach takes in the view cone
but whose light never gets past the walls in between is not one of them, and
does not push the ones that matter out of the few that are lit.
False when the map has no such lists, or the view is outside the world.

They are the same for as long as the view stays in its cluster, so they are
listed once for it rather than every leaf of the map gone through each frame.
*/
static qboolean R_RtMarkSeenLights(const vec3_t vieworg)
{
	const mnode_t *leaf;
	const byte    *vis;
	int            i, k;

	if (!rt.leafLights || !tr.world->vis || r_novis->integer) {
		return qfalse;
	}

	leaf = R_PointInLeaf(vieworg);
	if (!leaf || leaf->cluster < 0 || leaf->cluster >= tr.world->numClusters) {
		return qfalse;
	}
	if (leaf->cluster == rt.seenCluster) {
		for (i = 0; i < rt.numSeen; i++) {
			rt.lights[rt.seenLights[i]].visFrame = rt.frame;
		}
		return qtrue;
	}
	rt.seenCluster = leaf->cluster;
	rt.numSeen     = 0;

	vis = tr.world->vis + leaf->cluster * tr.world->clusterBytes;

	for (i = tr.world->numDecisionNodes; i < tr.world->numnodes; i++) {
		const mnode_t *n = &tr.world->nodes[i];

		if (!n->numlights || n->cluster < 0 || n->cluster >= tr.world->numClusters) {
			continue;
		}
		if (!(vis[n->cluster >> 3] & (1 << (n->cluster & 7)))) {
			continue;
		}
		for (k = 0; k < n->numlights; k++) {
			const spherel_t *sl = n->lights[k];
			int              index;

			if (sl == &tr.sSunLight) {
				continue;
			}
			index = rt.fromSphere[sl - tr.sLights];
			if (index >= 0 && rt.lights[index].visFrame != rt.frame) {
				rt.lights[index].visFrame   = rt.frame;
				rt.seenLights[rt.numSeen++] = index;
			}
		}
	}
	// a lamp's light is not where the map's lists put the lights it took
	// (nor in them, if it is its own): it is seen where its cluster is
	for (i = 0; i < rt.numLights; i++) {
		rtLight_t *l = &rt.lights[i];

		if (l->lamp && l->visFrame != rt.frame
			&& (l->cluster < 0 || (vis[l->cluster >> 3] & (1 << (l->cluster & 7))))) {
			l->visFrame                 = rt.frame;
			rt.seenLights[rt.numSeen++] = i;
		}
	}
	return qtrue;
}

// A flame's light: a few waves out of step, mostly slow, with a quick one
// over them, never brighter than the lamp, and down by flicker at the most
static float R_RtFlicker(const rtLight_t *l, int time)
{
	float t, w;

	if (l->flicker <= 0) {
		return 1.0f;
	}
	t = time * 0.001f + l->seed;
	w = 0.45f * sin(t * 2.3f) + 0.3f * sin(t * 5.9f + 1.3f) + 0.25f * sin(t * 13.7f + 0.4f);
	return 1.0f - l->flicker * (0.5f + 0.5f * w);
}

void R_RtRenderShadows(const refdef_t *fd)
{
	float     sorted[1024][2];
	int       numSorted = 0, numChosen, numCandidates, numInView, room;
	int       candidates[RT_MAX_LIGHTS];
	float     ease;
	int       numBodies;
	rtMover_t movers[RT_MAX_MOVERS];
	int       numMovers;
	int       i, budget, burst, dynamicSlots = 0, dynamicLights = 0, movingLights = 0, selfLights = 0;
	float     scale, bake, reach;
	int       shift;
	qboolean  seen;
	cplane_t  frustum[5];

	if (!R_RtActive()) {
		tr.rtNumActive = 0;
		return;
	}
	// A scene without the world (a model in a menu) takes no part in the
	// lights, and leaves them as the world's scene set them: the backend
	// draws every scene of the frame after all of them are set up, and the
	// world's surfaces were told which of these lights reach them.
	if (fd->rdflags & RDF_NOWORLDMODEL) {
		return;
	}
	tr.rtNumActive = 0;

	rt.frame++;
	rt.facesDrawn  = 0;
	rt.staticDrawn = 0;
	rt.invalidated = 0;
	rt.movingDrawn = 0;
	rt.waiting     = 0;
	rt.easing      = 0;

	R_RtFollowFixtures();
	R_RtEntityLamps();

	// lightmap units: what R_ColorShiftLightingBytes does to them, over 255
	shift = r_mapOverBrightBits->integer - tr.overbrightBits;
	// r_rtScale is for the lights' falloff, brighter near a lamp than the
	// compiler's: the sun, the sky and the ambient light are as the map has them
	bake  = (float)(1 << Q_max(0, shift)) / 255.0f;
	scale = bake * r_rtScale->value;

	// the four sides of the view: what moves is drawn into the shadows of the
	// lights that reach into it alone (the lighting itself does not look at it)
	{
		float ang = fd->fov_x / 180.0f * M_PI * 0.5f;
		float xs = sin(ang), xc = cos(ang);
		float ys, yc;

		ang = fd->fov_y / 180.0f * M_PI * 0.5f;
		ys  = sin(ang);
		yc  = cos(ang);
		VectorScale(fd->viewaxis[0], xs, frustum[0].normal);
		VectorMA(frustum[0].normal, xc, fd->viewaxis[1], frustum[0].normal);
		VectorScale(fd->viewaxis[0], xs, frustum[1].normal);
		VectorMA(frustum[1].normal, -xc, fd->viewaxis[1], frustum[1].normal);
		VectorScale(fd->viewaxis[0], ys, frustum[2].normal);
		VectorMA(frustum[2].normal, yc, fd->viewaxis[2], frustum[2].normal);
		VectorScale(fd->viewaxis[0], ys, frustum[3].normal);
		VectorMA(frustum[3].normal, -yc, fd->viewaxis[2], frustum[3].normal);
		// and what is behind it
		VectorCopy(fd->viewaxis[0], frustum[4].normal);
		for (i = 0; i < 5; i++) {
			frustum[i].dist = DotProduct(fd->vieworg, frustum[i].normal);
		}
	}

	// The lights that may reach what the view can see (the map's lists for
	// the leaves it may see), those whose light reaches into the view first,
	// the biggest on the screen first. A lamp down the street whose pool is
	// in the view was left out for the many nearer, behind the view or
	// beside it, and came on only as the view went up to it. Those out of
	// the view take what room is left, so that one just off the screen is
	// most often lit already as the view turns to it.
	seen = R_RtMarkSeenLights(fd->vieworg);
	for (i = 0; i < rt.numLights && numSorted < 1024; i++) {
		rtLight_t *l = &rt.lights[i];
		vec4_t     colorRange;
		float      lightReach;

		// an entity lamp is in no list of the map's: those that may be seen
		// are where the view sees them (R_RtViewSees), and gone from the
		// scene, they are not lit
		if (l->range <= 0 || (seen && l->entity < 0 && l->visFrame != rt.frame)
			|| (l->entity >= 0 && l->entityFrame != rt.frame)) {
			continue;
		}
		// how far it gives a pixel enough to see (R_RtReach)
		VectorScale(l->color, scale, colorRange);
		colorRange[3] = l->range;
		lightReach    = R_RtReach(colorRange, r_rtCutoff->value);
		if (lightReach == 0.0f || lightReach < -0.5f) {
			continue; // it gives nothing anywhere
		}

		// how big the light's pool is on the screen, as reach over distance,
		// kept inverted to sort nearest first; the lit a little ahead, so none
		// flickers in and out where the few lit end
		sorted[numSorted][0] = (float)i;
		sorted[numSorted][1] = Q_max(64.0f, Distance(l->origin, fd->vieworg)) / lightReach;
		if (l->weight > 0) {
			sorted[numSorted][1] *= 0.8f;
		}
		if (!R_RtInView(l, lightReach, fd->vieworg, frustum)) {
			sorted[numSorted][1] += RT_OUT_OF_VIEW;
		} else {
			if (l->inViewFrame != rt.frame - 1) {
				l->enteredView = rt.frame;
			}
			l->inViewFrame = rt.frame;
			if (!R_RtViewSees(l, i, fd->vieworg)) {
				sorted[numSorted][1] += RT_HIDDEN;
			}
		}
		numSorted++;
	}
	qsort(sorted, numSorted, sizeof(sorted[0]), R_RtCompareDistance);
	// room left for the game's lights (muzzle flashes, blasts) among them
	room      = RT_MAX_LIGHTS - (r_rtDlights->value > 0 ? Q_min(tr.refdef.num_dlights, RT_DLIGHT_ROOM) : 0);
	numChosen = Q_min(numSorted, Q_min(room, r_rtMaxLights->integer));
	for (i = 0, numInView = 0; i < numSorted; i++) {
		numInView += sorted[i][1] < RT_OUT_OF_VIEW;
	}

	// Each light eases towards full while it is chosen and its shadow is
	// whole, and away when it is not. One on its way out stays among them
	// while there is room, so that none pops out as the view moves.
	{
		const int now = ri.Milliseconds();
		const int dt  = rt.lastTime ? Q_min(250, now - rt.lastTime) : 0;

		rt.lastTime = now;
		ease        = dt / (1000.0f * RT_EASE_SECONDS);
	}

	numCandidates = 0;
	for (i = 0; i < numChosen; i++) {
		candidates[numCandidates++] = (int)sorted[i][0];
		rt.lights[(int)sorted[i][0]].chosen = rt.frame;
	}
	for (i = 0; i < rt.numLights && numCandidates < room; i++) {
		if (rt.lights[i].weight > 0 && rt.lights[i].chosen != rt.frame) {
			candidates[numCandidates++] = i;
		}
	}

	numMovers = r_rtShadows->integer ? R_RtTrackCasters(movers) : 0;
	budget    = r_rtFacesPerFrame->integer;
	// and as much again three times over for the lights come into the view
	// whose shadows are not drawn: the view turned to a street of lamps
	// found them dark a frame or two each, coming on one or two a frame
	burst     = r_rtFacesPerFrame->integer * RT_FACES_BURST;
	rt.moverPairs = rt.moverCulled = 0;

	// which of the candidates reach each mover, by the map's light lists
	for (i = 0; i < numCandidates; i++) {
		rt.candidateOf[candidates[i]] = i;
	}
	{
		uint64_t lamps = 0;

		// lamps are not in the map's lists: every mover may shadow them
		for (i = 0; i < numCandidates; i++) {
			if (rt.lights[candidates[i]].lamp) {
				lamps |= (uint64_t)1 << i;
			}
		}
	for (i = 0; i < numMovers; i++) {
		if (rt.leafLights && r_rtShadowCull->integer) {
			vec3_t mins, maxs;

			VectorSet(mins, movers[i].centre[0] - movers[i].radius, movers[i].centre[1] - movers[i].radius, movers[i].centre[2] - movers[i].radius);
			VectorSet(maxs, movers[i].centre[0] + movers[i].radius, movers[i].centre[1] + movers[i].radius, movers[i].centre[2] + movers[i].radius);
			movers[i].lights = R_RtLeafLights(tr.world->nodes, mins, maxs, rt.candidateOf) | lamps;
		} else {
			movers[i].lights = ~(uint64_t)0;
		}
	}
	}
	for (i = 0; i < numCandidates; i++) {
		rt.candidateOf[candidates[i]] = -1;
	}
	numBodies = R_RtGatherBodies(bodies);
	rt.numPacked = 0;

	for (i = 0; i < numCandidates && tr.rtNumActive < room; i++) {
		const int      index  = candidates[i];
		rtLight_t     *l      = &rt.lights[index];
		const qboolean chosen = l->chosen == rt.frame ? qtrue : qfalse;
		qboolean       ready  = qtrue;
		vec4_t        *data;
		float          dynamicTile = -1;

		l->lastUsed = rt.frame;

		if (r_rtShadows->integer && !l->noShadow) {
			int f;

			// what stands still: drawn once, a few faces a frame; whole
			// tiles for the nearest, quarter tiles further off
			if (l->staticSlot < 0 && R_RtStaticSlot(index, i < RT_SLOTS_BIG - RT_SLOTS_SPARE) < 0) {
				l->weight = 0;
				continue;
			}
			// one in quarter tiles come near: to whole tiles, if one is to be
			// had, its faces drawn there now so that it is not dark a frame
			if (l->staticSlot >= RT_SLOTS_BIG && l->staticReady && i < RT_SLOTS_UPGRADE && R_RtFindSlot(qtrue) >= 0) {
				R_RtStaticSlot(index, qtrue);
				for (f = 0; f < 6; f++) {
					R_RtRenderFace(l, 0, l->staticSlot, f);
					if (R_RtWantsBakedFaces(l)) {
						R_RtRenderFace(l, 2, l->staticSlot, f);
					}
				}
				rt.staticDrawn += R_RtWantsBakedFaces(l) ? 12 : 6;
				l->staticFaces = 63;
				l->bakedFaces  = R_RtWantsBakedFaces(l) ? 63 : 0;
				l->bakedReady  = rt.frame;
				l->staticReady = qtrue;
			}
			// a lamp that moved this frame: the faces the view reads, now,
			// for the nearest few (r_rtMovingLights); the rest of its faces
			// once it has come to rest
			if (l->movedFrame == rt.frame && chosen && movingLights < r_rtMovingLights->integer && l->staticReady) {
				const int inView = R_RtFacesInView(l, l->range, frustum);

				movingLights++;
				for (f = 0; f < 6; f++) {
					if (inView & (1 << f)) {
						R_RtRenderFace(l, 0, l->staticSlot, f);
						rt.movingDrawn++;
						l->staticFaces |= 1 << f;
					}
				}
			}
			{
				const qboolean rushed = chosen && !l->staticReady && l->inViewFrame == rt.frame;

#define RT_FACE_ALLOWED (budget > 0 || (rushed && burst > 0))
#define RT_FACE_SPENT() (budget > 0 ? budget-- : burst--)
				for (f = 0; f < 6 && RT_FACE_ALLOWED && rt.frame - l->movedFrame > RT_LIGHT_SETTLE_FRAMES; f++) {
					if (l->staticFaces & (1 << f)) {
						continue;
					}
					R_RtRenderFace(l, 0, l->staticSlot, f);
					rt.staticDrawn++;
					l->staticFaces |= 1 << f;
					RT_FACE_SPENT();
				}
				// the world as compiled, once; or alone, for a baked lamp
				if (R_RtWantsBakedFaces(l)) {
					for (f = 0; f < 6 && RT_FACE_ALLOWED; f++) {
						if (l->bakedFaces & (1 << f)) {
							continue;
						}
						R_RtRenderFace(l, 2, l->staticSlot, f);
						rt.staticDrawn++;
						l->bakedFaces |= 1 << f;
						l->bakedReady = rt.frame;
						RT_FACE_SPENT();
					}
				}
#undef RT_FACE_ALLOWED
#undef RT_FACE_SPENT
			}
			// lit when what stands still is drawn; with the map's lighting kept,
			// the world as compiled too (all of it live, a baked lamp's world
			// alone is only for what it changes of its baked light)
			if (l->staticFaces == 63 && (r_realtimeLighting->integer != 1 || !tr.rtShadowFbo[2] || l->bakedFaces == 63)) {
				l->staticReady = qtrue;
			}
			// not lit until its shadow is whole: it would shine through walls
			ready = l->staticReady;

			// what moves: drawn every frame, in the faces it is in, for the
			// nearest lights. A shadow falls between a light and what it
			// lights, so what is past the light's reach (where it gives a
			// pixel less than r_rtCutoff, R_RtReach), or that its light does
			// not reach (the map's light lists), darkens nothing that shows.
			if (r_rtShadowCull->integer) {
				vec4_t colorRange;

				VectorScale(l->color, scale, colorRange);
				colorRange[3] = l->range;
				reach = R_RtReach(colorRange, r_rtCutoff->value);
			} else {
				reach = l->range;
			}
			if (ready && chosen && reach > 0 && dynamicLights < r_rtDynamicLights->integer && R_RtReachesView(l, reach, frustum))
			{
				int      faces = 0, m;
				qboolean self  = qfalse;

				for (m = 0; m < numMovers; m++) {
					int touched;

					// the player's own shadow, for the nearest few lights: as
					// they fired or walked, every lamp in reach drew its faces
					// again each frame for it, and took a second shadow lookup
					// for every pixel it lit
					if (movers[m].self && selfLights >= r_rtSelfShadowLights->integer) {
						*movers[m].casts &= ~((uint64_t)1 << i);
						continue;
					}
					if (!(movers[m].lights & ((uint64_t)1 << i))) {
						if (r_rtDebug->integer && R_RtFacesTouched(l, l->range, movers[m].centre, movers[m].radius)) {
							rt.moverPairs++;
							rt.moverCulled++;
						}
						// nor is it drawn in the faces the others need
						*movers[m].casts &= ~((uint64_t)1 << i);
						continue;
					}
					touched = R_RtFacesTouched(l, reach, movers[m].centre, movers[m].radius);
					if (!touched && r_rtShadowCull->integer) {
						*movers[m].casts &= ~((uint64_t)1 << i);
					}
					if (r_rtDebug->integer && R_RtFacesTouched(l, l->range, movers[m].centre, movers[m].radius)) {
						rt.moverPairs++;
						rt.moverCulled += !touched;
					}
					faces |= touched;
					self |= movers[m].self && touched;
				}
				selfLights += self;
				if (faces && dynamicSlots < RT_DYNAMIC_SLOTS) {
					const int slot = dynamicSlots++;
					// None of the view's pixels reads the faces out of it, so
					// they are not drawn. The light keeps its place among the
					// few all the same: had the faces out of view not counted,
					// the places went round the lights as the view turned, and
					// what moves lost and found its shadows in patches.
					const int drawn = faces & R_RtFacesInView(l, reach, frustum);

					dynamicLights++;
					tr.rtCandidate = i;
					for (f = 0; f < 6; f++) {
						if (drawn & (1 << f)) {
							R_RtRenderFace(l, 1, slot, f);
						}
					}
					// the faces go in the fraction, a bit each; with none drawn
					// the people still take no capsules for this light, as
					// when they were drawn into faces of it
					dynamicTile = slot * 6 + drawn / 64.0f;
				}
			}
		}

		if (chosen && ready && rt.frame - l->enteredView < RT_ENTER_FRAMES) {
			// come into the view as it turned: on at once, its light was off
			// the screen while it was dark (eased in, the lamps of a street
			// the view turned to came up slowly one after another)
			l->weight = 1.0f;
		} else if (chosen && ready) {
			l->weight = Q_min(1.0f, l->weight + ease);
		} else {
			l->weight = Q_max(0.0f, l->weight - ease);
		}
		if (chosen && !ready) {
			rt.waiting++;
		} else if (chosen && l->weight < 1.0f) {
			rt.easing++;
		}
		// its bake taken out while it is lit fully (R_RtLiveIn)
		R_RtLiveSet(index, R_RtLiveWanted(l, chosen && ready));
		if (l->weight <= 0 || !ready) {
			continue;
		}

		data = &tr.rtLightData[tr.rtNumActive * RT_LIGHT_VEC4S];
		VectorCopy(l->origin, data[0]);
		// its kind: 0 a point light, 1 a spot light, 3 a lamp's or a lit
		// surface's (its shade and glow in data[2]); eight more when its
		// light on a lightmap is baked, the world alone in its third atlas
		data[0][3] = R_RtLightShape(l, data[2]);
		if (l->baked && !l->live && rt_bake.done && r_rtBakedLamps->integer && (l->noShadow || !r_rtShadows->integer || l->bakedFaces == 63)) {
			data[0][3] += 8.0f;
		}
		// eased as it is lit: smoothstep; a flame dips and comes back
		VectorScale(l->color, scale * l->weight * l->weight * (3.0f - 2.0f * l->weight) * R_RtFlicker(l, fd->time), data[1]);
		data[1][3] = l->range;
		data[3][0] = r_rtShadows->integer && !l->noShadow ? R_RtSlotCode(l->staticSlot) : -1.0f;
		data[3][1] = dynamicTile;
		// the people are in its shadow faces as they are, or else capsules
		data[3][2] = r_rtShadowCharacters->integer && dynamicTile >= 0 ? -1.0f : R_RtLightCapsules(l, bodies, numBodies);
		data[3][3] = l->range;
		rt.active[tr.rtNumActive] = index;
		tr.rtNumActive++;
	}

	// The lights the game adds (muzzle flashes, blasts): no shadows, over the
	// map's lighting, bright enough to show at night. A MOH:AA dynamic light
	// is a colour and a radius; here it brings 96 raw units half way out.
	for (i = 0; i < tr.refdef.num_dlights && tr.rtNumActive < RT_MAX_LIGHTS && r_rtDlights->value > 0; i++) {
		const dlight_t *dl = &tr.refdef.dlights[i];
		vec4_t         *data;
		float           intensity;

		if (dl->radius <= 0 || (i < MAX_DLIGHTS && rt.dlightTaken[i])) {
			continue; // a searchlight's pool, lit from its lamp (R_RtEntityLamps)
		}
		intensity = 96.0f * Square(dl->radius * 0.5f);

		data = &tr.rtLightData[tr.rtNumActive * RT_LIGHT_VEC4S];
		VectorCopy(dl->origin, data[0]);
		data[0][3] = 2.0f;
		VectorScale(dl->color, intensity * scale * r_rtDlights->value, data[1]);
		data[1][3] = dl->radius;
		VectorClear4(data[2]);
		VectorSet4(data[3], -1.0f, -1.0f, -1.0f, dl->radius);
		rt.active[tr.rtNumActive] = -1;
		tr.rtNumActive++;
	}

	// the bodies' capsules, a run for each light
	if (rt.numPacked && tr.rtCapsuleImage) {
		qglTextureSubImage2DEXT(tr.rtCapsuleImage->texnum, GL_TEXTURE_2D, 0, 0, 0, rt.numPacked * 2, 1, GL_RGBA, GL_FLOAT, rt.packed);
	}

	// the rest are out of the view: gone
	for (i = 0; i < rt.numLights; i++) {
		if (rt.lights[i].lastUsed != rt.frame) {
			rt.lights[i].weight = 0;
		}
	}

	VectorSet4(tr.rtParams, (float)tr.rtNumActive, (float)r_realtimeLighting->integer, r_rtShadows->integer ? 1.0f : 0.0f,
		Com_Clamp(0.0f, 1.0f, r_rtDarkest->value));
	// how far each light's light is worth looking up (R_RtReach)
	for (i = 0; i < tr.rtNumActive; i++) {
		tr.rtLightReach[i] = R_RtReach(tr.rtLightData[i * RT_LIGHT_VEC4S + 1], r_rtCutoff->value);
	}

	// each program is sent the lights once a frame, not compared with them
	// at every draw (GLSL_SetUniformRtLights)
	tr.rtLightGeneration++;

	if (rt.sun) {
		VectorCopy(rt.sunDir, tr.rtSunDir);
		tr.rtSunDir[3] = 1.0f;
		VectorScale(rt.sunColor, bake, tr.rtSunColor);
		// the screen shadow the depth prepass leaves (RB_RenderDrawSurfList)
		tr.rtSunColor[3] = (tr.screenShadowImage && tr.sunShadows && r_depthPrepass->value && r_sunlightMode->integer) ? 1.0f : 0.0f;
	} else {
		VectorClear4(tr.rtSunDir);
		VectorClear4(tr.rtSunColor);
	}

	VectorScale(rt.ambient, bake * r_rtAmbient->value, tr.rtAmbient);
	R_RtUpdateSky(bake, ease);
	R_RtBounceStep(bake, ease);
	rt_bake.scale = scale;
	R_RtShadowProbeStep();
	// the lamps lit live last frame and no longer: their bake put back
	R_RtLiveSweep();
	R_RtBakeLamps(ease);
	// the least a light may add to a pixel and be looked up there (RtLight)
	tr.rtAmbient[3] = Q_max(0.0f, r_rtCutoff->value);

	if (r_rtDebug->integer) {
		ri.Printf(PRINT_ALL, "realtime lighting: %d of %d lights lit (%d may be seen, %d reach into the view; %d waiting for shadows, %d easing in), %d movers (%d of %d in a dynamic light's range out of its reach or light lists), faces drawn %d standing %d moving (%d lights) %d for lights that moved, %d changes to what stands, %d bodies and parts (%d sent), %d particle draws (%d vertexes)\n",
			tr.rtNumActive, numChosen, numSorted, numInView, rt.waiting, rt.easing, numMovers, rt.moverCulled, rt.moverPairs, rt.staticDrawn, rt.facesDrawn - rt.staticDrawn - rt.movingDrawn, dynamicSlots, rt.movingDrawn, rt.invalidated, numBodies, rt.numPacked,
			rt.particleDraws, rt.particleVerts);
		{
			int live = 0, k;

			for (k = 0; k < rt.numLights; k++) {
				live += rt.lights[k].live ? 1 : 0;
			}
			ri.Printf(PRINT_ALL, "realtime lighting: %d lamps lit live with their bake taken out (%d taken out or put back, %d times all put back)\n", live,
				rt_bake.livePasses, rt_bake.liveClears);
			rt_bake.livePasses = rt_bake.liveClears = 0;
		}
		rt.particleDraws = rt.particleVerts = 0;
		for (i = 0; r_rtDebug->integer > 2 && i < numBodies; i += 1 + (int)bodies[i * 2 + 1][0]) {
			int c;

			ri.Printf(PRINT_ALL, "  body at (%.0f %.0f %.0f) radius %.0f, %d capsules\n",
				bodies[i * 2][0], bodies[i * 2][1], bodies[i * 2][2], bodies[i * 2][3], (int)bodies[i * 2 + 1][0]);
			for (c = i + 1; r_rtDebug->integer > 3 && c <= i + (int)bodies[i * 2 + 1][0]; c++) {
				ri.Printf(PRINT_ALL, "    (%.0f %.0f %.0f) to (%.0f %.0f %.0f) radius %.1f\n",
					bodies[c * 2][0], bodies[c * 2][1], bodies[c * 2][2],
					bodies[c * 2 + 1][0], bodies[c * 2 + 1][1], bodies[c * 2 + 1][2], bodies[c * 2][3]);
			}
		}
		for (i = 0; r_rtDebug->integer > 1 && i < tr.rtNumActive; i++) {
			const vec4_t *d = &tr.rtLightData[i * RT_LIGHT_VEC4S];
			ri.Printf(PRINT_ALL, "  light %d at (%.0f %.0f %.0f) type %.0f range %.0f color (%.2f %.2f %.2f) bodies %.3f spot (%.2f %.2f %.2f) %.3f shadow %.0f\n",
				i, d[0][0], d[0][1], d[0][2], d[0][3], d[1][3], d[1][0], d[1][1], d[1][2], d[3][2], d[2][0], d[2][1], d[2][2], d[2][3], d[3][0]);
			if (d[0][3] < 1.5f || d[0][3] > 2.5f) { // not one the game added
				const rtLight_t *l = &rt.lights[rt.active[i]];

				ri.Printf(PRINT_ALL, "    bake out %d: weight %.2f baked %d slot %d faces %d ready %d moved %.2f shadowed %d\n", l->live, l->weight, l->baked,
					l->staticSlot, l->bakedFaces, rt.frame - l->bakedReady, Distance(l->origin, l->bakedOrigin), !l->noShadow && r_rtShadows->integer);
			}
		}
	}
}

/*
=============================================================================

SHADING

=============================================================================
*/

/*
Which of the lights a surface may take.

Every pixel the lights shade went through all of them, though a light gives
nothing past its reach and most surfaces are within reach of a few. So the
surfaces are told, as they are added to a view, which lights reach their
bounds (drawSurf_t.rtMask, a bit for each in tr.rtLightData), and each draw
sends the shaders the list of those (u_RtList). The test is the one the
shaders make at each pixel, against the bounds, a little wider: a light left
out of a list is one that would have given the surface nothing.
*/

// The lights there are to choose from in this view; RT_MASK_ALL in a view
// they take no part in (a shadow), so that nothing is culled there for none.
uint64_t R_RtViewMask(void)
{
	if (!R_RtActive() || !r_rtLightLists->integer || (tr.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))
		|| (tr.refdef.rdflags & RDF_NOWORLDMODEL)) {
		return RT_MASK_ALL;
	}
	// all 64 is every bit: a shift by 64 is none
	return tr.rtNumActive >= 64 ? RT_MASK_ALL : ((uint64_t)1 << tr.rtNumActive) - 1;
}

/*
Whether a world surface about to join the batch should start a new one: it
would bring lights the batch does not have, and every pixel of the batch
would then go through them: a shader's surfaces across a room together take
more of its lights than any one of them, and the batch was shaded with all
of them. More draws for fewer lights a pixel: m3l2's lamp room 864 -> 985
draws, main3d 4.73 -> 4.28 ms on the card.
*/
qboolean R_RtSplitBatch(uint64_t surfMask)
{
	if (!r_rtBatchSplit->integer || !tess.numIndexes || surfMask == RT_MASK_ALL || tess.rtMask == RT_MASK_ALL
		|| backEnd.depthFill || (backEnd.viewParms.flags & (VPF_DEPTHSHADOW | VPF_SHADOWMAP))
		|| backEnd.currentEntity != &tr.worldEntity || backEnd.currentStaticModel || !R_RtActive()) {
		return qfalse;
	}
	return (surfMask & ~tess.rtMask) ? qtrue : qfalse;
}

uint64_t R_RtSphereMask(const vec3_t centre, float radius, uint64_t candidates)
{
	uint64_t mask = 0, m;
	int      i;

	if (candidates == RT_MASK_ALL) {
		return RT_MASK_ALL;
	}

	for (i = 0, m = candidates; m && i < tr.rtNumActive; i++, m >>= 1) {
		const float *origin = tr.rtLightData[i * RT_LIGHT_VEC4S];
		const float  reach  = tr.rtLightReach[i] + radius + RT_CULL_MARGIN;

		if ((m & 1) && tr.rtLightReach[i] >= 0 && DistanceSquared(origin, centre) < reach * reach) {
			mask |= (uint64_t)1 << i;
		}
	}
	return mask;
}

uint64_t R_RtBoxMask(const vec3_t mins, const vec3_t maxs, uint64_t candidates)
{
	uint64_t mask = 0, m;
	int      i, k;

	if (candidates == RT_MASK_ALL) {
		return RT_MASK_ALL;
	}

	for (i = 0, m = candidates; m && i < tr.rtNumActive; i++, m >>= 1) {
		const float *origin = tr.rtLightData[i * RT_LIGHT_VEC4S];
		const float  reach  = tr.rtLightReach[i] + RT_CULL_MARGIN;
		float        d2     = 0;

		if (!(m & 1) || tr.rtLightReach[i] < 0) {
			continue;
		}
		// from the light to the nearest point of the box
		for (k = 0; k < 3; k++) {
			if (origin[k] < mins[k]) {
				d2 += Square(mins[k] - origin[k]);
			} else if (origin[k] > maxs[k]) {
				d2 += Square(origin[k] - maxs[k]);
			}
		}
		if (d2 < reach * reach) {
			mask |= (uint64_t)1 << i;
		}
	}
	return mask;
}

// The lights that reach the front of a plane, and through back those that
// reach behind it: the world's nodes split them as they split the world.
uint64_t R_RtPlaneMask(const cplane_t *plane, uint64_t candidates, uint64_t *back)
{
	uint64_t front = 0, m;
	int      i;

	*back = 0;
	if (candidates == RT_MASK_ALL) {
		*back = RT_MASK_ALL;
		return RT_MASK_ALL;
	}

	for (i = 0, m = candidates; m && i < tr.rtNumActive; i++, m >>= 1) {
		const float *origin = tr.rtLightData[i * RT_LIGHT_VEC4S];
		const float  reach  = tr.rtLightReach[i] + RT_CULL_MARGIN;
		float        dist;

		if (!(m & 1) || tr.rtLightReach[i] < 0) {
			continue;
		}
		dist = DotProduct(origin, plane->normal) - plane->dist;
		if (dist > -reach) {
			front |= (uint64_t)1 << i;
		}
		if (dist < reach) {
			*back |= (uint64_t)1 << i;
		}
	}
	return front;
}

// How a stage takes the lights (RT_STAGE_*). lightall adds them by itself.
/*
Whether the generic program lights particles at their corners: its vertex
shader has the lights then, which takes GLSL 1.30 (texture() there).
*/
qboolean R_RtVertexParticles(void)
{
	return r_realtimeLighting->integer
		&& (glRefConfig.glslMajorVersion > 1 || (glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 30));
}

int R_RtStageKind(const shader_t *shader, const shaderStage_t *pStage, qboolean lightall)
{
	const int dstBlend = pStage->stateBits & GLS_DSTBLEND_BITS;
	int       kind;

	// added on top: a glow, not something lit. Before lightall's: a muzzle
	// flash, fire or sparks (rgbGen identity) go to lightall without a light
	// type, and lit there they ran every light for every pixel of them
	if (dstBlend == GLS_DSTBLEND_ONE) {
		return RT_STAGE_NONE;
	}
	// a lit window: what lights it is its own light, which the compiler put in
	// its lightmap (the realtime light it gives is in front of it)
	if (shader && shader->rtEmissive) {
		return RT_STAGE_NONE;
	}

	if (lightall) {
		// dust and smoke that went there the same way: unlit, as the generic
		// program has them (the default below), not the lights' loop per pixel
		if (shader && shader->rtParticle && !(pStage->stateBits & GLS_DEPTHMASK_TRUE)
			&& !(pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK)) {
			return RT_STAGE_NONE;
		}
		return RT_STAGE_LIT;
	}

	// with the direct light live alone, what is lit at run time is lit as ever
	if (r_realtimeLighting->integer == 1 && (pStage->rgbGen == CGEN_LIGHTING_SPHERICAL || pStage->rgbGen == CGEN_LIGHTING_GRID
		|| pStage->rgbGen == CGEN_LIGHTING_DIFFUSE)) {
		return RT_STAGE_NONE;
	}

	if (pStage->bundle[0].isLightmap || pStage->bundle[0].tcGen == TCGEN_LIGHTMAP) {
		return RT_STAGE_LIGHTING;
	}
	if (pStage->bundle[1].image[0] && (pStage->bundle[1].isLightmap || pStage->bundle[1].tcGen == TCGEN_LIGHTMAP)
		&& pStage->multitextureEnv != GL_ADD) {
		return RT_STAGE_LIGHTING2;
	}

	switch (pStage->rgbGen) {
	case CGEN_LIGHTING_SPHERICAL:
	case CGEN_LIGHTING_GRID:
	case CGEN_LIGHTING_DIFFUSE:
		kind = RT_STAGE_LIT;
		break;
	case CGEN_STATIC:
	case CGEN_EXACT_VERTEX:
	case CGEN_EXACT_VERTEX_LIT:
	case CGEN_VERTEX:
	case CGEN_VERTEX_LIT:
		kind = RT_STAGE_LIT_BAKED;
		break;
	default:
		return RT_STAGE_NONE;
	}

	// dust and smoke: big translucent quads piled deep, lit at their corners
	// rather than the whole light loop for every pixel of every layer
	if (shader && shader->rtParticle && !(pStage->stateBits & GLS_DEPTHMASK_TRUE)) {
		rt.particleDraws++;
		rt.particleVerts += tess.numVertexes;
		if (r_rtParticles->integer && R_RtVertexParticles()) {
			return kind == RT_STAGE_LIT ? RT_STAGE_PARTICLE_LIT : RT_STAGE_PARTICLE;
		}
	}
	return kind;
}

// Whether the sky's light is worked out at the vertexes of what has no
// lightmap (r_realtimeLighting 2): it takes texture fetches in the vertex
// shader (GLSL 1.30)
qboolean R_RtVertexSky(void)
{
	return r_realtimeLighting->integer == 2 && R_RtVertexParticles();
}

// Whether the bounce lights what has no lightmap from its probes
qboolean R_RtProbesLive(void)
{
	return R_RtLitLive() && rt_probes && tr.rtBounce[3] > 0 ? qtrue : qfalse;
}

// Leaves: two sided, and cut out by their alpha
qboolean R_RtIsFoliage(const shader_t *shader)
{
	return (shader && shader->cullType == CT_TWO_SIDED && shader->stages[0] && (shader->stages[0]->stateBits & GLS_ATEST_BITS))
		? qtrue : qfalse;
}

// stage < 0: a view the lights take no part in (2D, shadows, models in menus)
// Whether the sun's widest shadow cascade is there for the particles: drawn
// with the sun's shadows this frame (RE_RenderScene)
static qboolean R_RtSunCascade(void)
{
	return r_rtParticleCascade->integer && tr.rtSunColor[3] > 0.5f && tr.sunShadowDepthImage[3] && r_sunlightMode->integer && r_depthPrepass->value
		&& !(backEnd.refdef.rdflags & RDF_NOWORLDMODEL);
}

void R_RtSetUniforms(shaderProgram_t *sp, int stage, image_t *lightmap, qboolean lightmapped)
{
	static const vec4_t off = { 0, 0, 0, 0 };

	if (stage < 0) {
		GLSL_SetUniformInt(sp, UNIFORM_RTSTAGE, RT_STAGE_NONE);
		GLSL_SetUniformVec4(sp, UNIFORM_RTPARAMS, off);
		GLSL_SetUniformVec4(sp, UNIFORM_RTSUNDIR, off);
		return;
	}

	{
		// the lights this draw's surfaces may take (R_RtViewMask), and so
		// the shaders' loop over them
		vec4_t   list[RT_MAX_LIGHTS / 4];
		vec4_t   params;
		float   *index = &list[0][0];
		uint64_t m;
		int      i, count = 0;

		for (i = 0, m = tess.rtMask; m && i < tr.rtNumActive; i++, m >>= 1) {
			if (m & 1) {
				index[count++] = (float)i;
			}
		}
		for (i = count; i & 3; i++) {
			index[i] = 0;
		}

		GLSL_SetUniformInt(sp, UNIFORM_RTSTAGE, stage);
		{
			vec4_t surface;

			float sky = lightmapped || !R_RtVertexSky() ? 0.0f : 1.0f;

			// a lightmap's sky is a pixel's own: its vertexes are a wall's
			// corners; read from the sky's page for its lightmap's, if there is one
			if (lightmap && tr.rtSkyLightmaps && r_rtSkyLightmaps->integer && tr.rtSkyColor[3] > 0) {
				int page;

				for (page = 0; page < tr.rtNumSkyLightmaps && page < tr.numLightmaps; page++) {
					if (tr.lightmaps[page] == lightmap && tr.rtSkyLightmaps[page]) {
						GL_BindToTMU(tr.rtSkyLightmaps[page], TMU_RTSKYLM);
						sky = 2.0f;
						break;
					}
				}
			}
			// the lamps' light baked on its page (R_RtBakeLamps), all of
			// them, which those lit live change: the world's own surfaces
			// alone, which were baked (a door, a barrel, a lightmapped model
			// of the map moves, and its page there is no one's)
			{
				vec4_t direct = { 0, 0, 0, 0 };

				if (lightmap && rt_bake.done && rt_bake.weight > 0 && r_rtBakedLamps->integer && !backEnd.currentStaticModel
					&& (!backEnd.currentEntity || backEnd.currentEntity == &tr.worldEntity)) {
					int page;

					for (page = 0; page < rt_bake.numPages && page < tr.numLightmaps; page++) {
						if (tr.lightmaps[page] == lightmap && rt_bake.pages[page].image && rt_bake.pages[page].dir && rt_bake.pages[page].live
							&& rt_bake.pages[page].liveDir) {
							GL_BindToTMU(rt_bake.pages[page].image, TMU_RTDIRECT);
							GL_BindToTMU(rt_bake.pages[page].dir, TMU_RTDIRECTDIR);
							GL_BindToTMU(rt_bake.pages[page].live, TMU_RTLIVE);
							GL_BindToTMU(rt_bake.pages[page].liveDir, TMU_RTLIVEDIR);
							VectorSet4(direct, rt_bake.scale, rt_bake.scale, rt_bake.scale,
								r_rtBakedDebug->integer == 1 ? -rt_bake.weight : rt_bake.weight);
							break;
						}
					}
				}
				GLSL_SetUniformVec4(sp, UNIFORM_RTDIRECT, direct);
			}
			// and the bounce, on the first page; on a model (what has no
			// lightmap), from the probes round it
			{
				vec4_t   none = { 0, 0, 0, 0 }, probe[3];
				qboolean on   = qfalse;

				if (sky > 1.5f && tr.rtBounceImage && tr.rtBounceLampImage && tr.rtBounce[3] > 0 && lightmap == tr.lightmaps[0]) {
					GL_BindToTMU(tr.rtBounceImage, TMU_RTBOUNCE);
					// made with it (R_RtBounceUpload)
					GL_BindToTMU(tr.rtBounceLampImage, TMU_RTBOUNCELAMP);
					on = qtrue;
				} else if (sky < 1.5f && tr.rtBounce[3] > 0 && (backEnd.currentStaticModel || (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity))) {
					vec3_t origin;

					if (backEnd.currentStaticModel) {
						on = R_RtStaticModelProbe(backEnd.currentStaticModel, probe);
					} else {
						if (backEnd.currentEntity->e.renderfx & RF_LIGHTING_ORIGIN) {
							VectorCopy(backEnd.currentEntity->e.lightingOrigin, origin);
						} else {
							VectorCopy(backEnd.currentEntity->e.origin, origin);
							origin[2] += 32.0f; // a person's middle, not their feet
						}
						on = R_RtProbesAt(origin, probe);
					}
				} else if (sky < 1.5f && tr.rtBounce[3] > 0 && tess.polys && tess.numVertexes) {
					// marks and decals: from the probes at the batch's middle,
					// as the wall they are on takes the bounce map
					vec3_t mins, maxs, origin;
					int    v;

					ClearBounds(mins, maxs);
					for (v = 0; v < tess.numVertexes; v++) {
						AddPointToBounds(tess.xyz[v], mins, maxs);
					}
					VectorAdd(mins, maxs, origin);
					VectorScale(origin, 0.5f, origin);
					on = R_RtProbesAt(origin, probe);
				}
				if (on && sky < 1.5f) {
					vec4_t scale; // the probes are scaled each for how far in it is

					VectorSet4(scale, tr.rtBounce[2], tr.rtBounce[2], tr.rtBounce[2], tr.rtBounce[3]);
					GLSL_SetUniformVec4(sp, UNIFORM_RTBOUNCE, scale);
				} else {
					GLSL_SetUniformVec4(sp, UNIFORM_RTBOUNCE, on ? tr.rtBounce : none);
				}
				if (on && sky < 1.5f) {
					GLSL_SetUniformVec4(sp, UNIFORM_RTPROBER, probe[0]);
					GLSL_SetUniformVec4(sp, UNIFORM_RTPROBEG, probe[1]);
					GLSL_SetUniformVec4(sp, UNIFORM_RTPROBEB, probe[2]);
				}
			}
			VectorSet4(surface, R_RtIsFoliage(tess.shader) ? RT_FOLIAGE_THROUGH : 0.0f, sky,
				Q_max(0.0f, r_rtSpecular->value), r_rtShadowSoft->value);
			GLSL_SetUniformVec4(sp, UNIFORM_RTSURFACE, surface);
		}
		VectorCopy4(tr.rtParams, params);
		params[0] = (float)count;
		GLSL_SetUniformVec4(sp, UNIFORM_RTPARAMS, params);
		if (!count && !tr.rtSunDir[3] && tr.rtParams[1] < 1.5f) {
			return;
		}
		GLSL_SetUniformRtLights(sp, UNIFORM_RTLIGHTS, (const vec4_t *)tr.rtLightData, tr.rtNumActive, tr.rtLightGeneration);
		GLSL_SetUniformRtList(sp, UNIFORM_RTLIST, list, count);
	}
	GLSL_SetUniformVec4(sp, UNIFORM_RTSUNDIR, tr.rtSunDir);
	GLSL_SetUniformVec4(sp, UNIFORM_RTSUNCOLOR, tr.rtSunColor);
	// the particles' sun shadow (RtLightParticle): the widest cascade, if the
	// sun has them this frame
	{
		static const mat4_t none = { 0 };

		GLSL_SetUniformMat4(sp, UNIFORM_RTSUNCASCADEMVP,
			R_RtSunCascade() ? backEnd.refdef.sunShadowMvp[3] : none);
	}
	GLSL_SetUniformVec4(sp, UNIFORM_RTAMBIENT, tr.rtAmbient);
	GLSL_SetUniformVec4(sp, UNIFORM_RTSKYCOLOR, tr.rtSkyColor);
	if (tr.rtSkyColor[3] > 0) {
		GLSL_SetUniformRtSky(sp, UNIFORM_RTSKY, tr.rtSkyRows, tr.rtSkyGeneration);
	}
}

void R_RtBindTextures(void)
{
	GL_BindToTMU(tr.rtShadowImage[0], TMU_RTSHADOW_STATIC);
	GL_BindToTMU(tr.rtShadowImage[1], TMU_RTSHADOW_DYNAMIC);
	GL_BindToTMU(tr.rtShadowImage[2] ? tr.rtShadowImage[2] : tr.rtShadowImage[0], TMU_RTSHADOW_BAKED);
	if (tr.rtCapsuleImage) {
		GL_BindToTMU(tr.rtCapsuleImage, TMU_RTCAPSULES);
	}
	GL_BindToTMU(tr.rtSunColor[3] > 0.5f && tr.screenShadowImage ? tr.screenShadowImage : tr.whiteImage, TMU_RTSUNSHADOW);
	GL_BindToTMU(R_RtSunCascade() ? tr.sunShadowDepthImage[3] : tr.rtShadowImage[0], TMU_RTSUNCASCADE);
	// a depth image either way: a shadow sampler on a colour one reads nothing defined
	GL_BindToTMU(tr.rtSkyImage ? tr.rtSkyImage : tr.rtShadowImage[0], TMU_RTSKY);
}

/*
=============================================================================

TESTING

r_rtTestMove <x0> <y0> <z0> <x1> <y1> <z1> <dx> <dy> <dz> [yaw]: the world
surfaces wholly inside a box come out of the world, as the client's furniture
does, and are drawn moved and turned about their middle, without the physics.
With no arguments it stops (they stay out of the world, where they were).

=============================================================================
*/

static struct {
	qhandle_t model;
	vec3_t    centre;
	vec3_t    offset;
	float     yaw;
	int       loadCount;
} rt_test;

static void R_RtTestMove_f(void)
{
	vec3_t mins, maxs;
	int    i;

	if (!tr.world) {
		return;
	}

	if (ri.Cmd_Argc() < 10) {
		rt_test.model = 0;
		return;
	}

	for (i = 0; i < 3; i++) {
		mins[i]           = atof(ri.Cmd_Argv(1 + i));
		maxs[i]           = atof(ri.Cmd_Argv(4 + i));
		rt_test.offset[i] = atof(ri.Cmd_Argv(7 + i));
	}
	rt_test.yaw = ri.Cmd_Argc() > 10 ? atof(ri.Cmd_Argv(10)) : 0;

	if (!rt_test.model) {
		int *list  = ri.Malloc(sizeof(int) * tr.world->numWorldSurfaces);
		int  count = 0;

		for (i = 0; i < tr.world->numWorldSurfaces; i++) {
			const msurface_t *surf = &tr.world->surfaces[i];

			if (surf->detached || !(surf->cullinfo.type & CULLINFO_BOX)) {
				continue;
			}
			if (surf->cullinfo.bounds[0][0] >= mins[0] && surf->cullinfo.bounds[0][1] >= mins[1] && surf->cullinfo.bounds[0][2] >= mins[2]
				&& surf->cullinfo.bounds[1][0] <= maxs[0] && surf->cullinfo.bounds[1][1] <= maxs[1] && surf->cullinfo.bounds[1][2] <= maxs[2]) {
				list[count++] = i;
			}
		}

		rt_test.model = count ? RE_DetachWorldSurfaces(list, count) : 0;
		ri.Free(list);
		ri.Printf(PRINT_ALL, "r_rtTestMove: %d surfaces out of the world\n", count);

		VectorAdd(mins, maxs, rt_test.centre);
		VectorScale(rt_test.centre, 0.5f, rt_test.centre);
	}
}

// Before a scene of the world is drawn: the test piece, where it is put.
/*
r_rtTestBody x y z [yaw]: the first person in the scene, as they stand, again
at x y z (floor level) each frame; with no arguments, gone. For the bodies'
shadows (R_RtLightCapsules), where the map has nobody under a lamp.
*/
static struct {
	qboolean on;
	vec3_t   origin;
	float    yaw;
} rt_testBody;

static void R_RtTestBody_f(void)
{
	int i;

	rt_testBody.on = ri.Cmd_Argc() >= 4;
	for (i = 0; i < 3 && rt_testBody.on; i++) {
		rt_testBody.origin[i] = atof(ri.Cmd_Argv(1 + i));
	}
	rt_testBody.yaw = ri.Cmd_Argc() > 4 ? atof(ri.Cmd_Argv(4)) : 0;
}

static void R_RtAddTestBody(void)
{
	extern int  r_numentities, r_firstSceneEntity;
	refEntity_t ent;
	vec3_t      angles;
	int         i;

	if (!rt_testBody.on || !tr.world) {
		return;
	}

	for (i = r_firstSceneEntity; i < r_numentities; i++) {
		const refEntity_t *e = &backEndData->entities[i].e;

		if (e->reType == RT_MODEL && !(e->renderfx & (RF_FIRST_PERSON | RF_DEPTHHACK)) && R_RtIsCharacter(e)) {
			break;
		}
	}
	if (i >= r_numentities) {
		return;
	}

	ent = backEndData->entities[i].e;
	VectorSet(angles, 0, rt_testBody.yaw, 0);
	AnglesToAxis(angles, ent.axis);
	VectorCopy(rt_testBody.origin, ent.origin);
	VectorCopy(rt_testBody.origin, ent.oldorigin);
	VectorCopy(rt_testBody.origin, ent.lightingOrigin);
	ent.entityNumber = ENTITYNUM_NONE;
	ent.parentEntity = ENTITYNUM_NONE;
	RE_AddRefEntityToScene(&ent);
}

void R_RtAddTestEntity(void)
{
	refEntity_t ent;
	vec3_t      angles, origin;
	int         k;

	R_RtAddTestBody();

	if (!rt_test.model || !tr.world) {
		return;
	}

	Com_Memset(&ent, 0, sizeof(ent));
	ent.reType = RT_MODEL;
	ent.hModel = rt_test.model;
	ent.entityNumber = ENTITYNUM_NONE;
	ent.parentEntity = ENTITYNUM_NONE;
	ent.scale = 1.0f;
	VectorSet(angles, 0, rt_test.yaw, 0);
	AnglesToAxis(angles, ent.axis);

	// turned about its middle, then moved
	VectorAdd(rt_test.centre, rt_test.offset, origin);
	for (k = 0; k < 3; k++) {
		VectorMA(origin, -rt_test.centre[k], ent.axis[k], origin);
	}
	VectorCopy(origin, ent.origin);
	VectorCopy(origin, ent.oldorigin);

	RE_AddRefEntityToScene(&ent);
}

void R_RtTestCommands(qboolean add)
{
	if (add) {
		ri.Cmd_AddCommand("r_rtTestMove", R_RtTestMove_f);
		ri.Cmd_AddCommand("r_rtTestBody", R_RtTestBody_f);
		ri.Cmd_AddCommand("r_rtProbeInfo", R_RtProbeInfo_f);
		ri.Cmd_AddCommand("r_rtProbeTest", R_RtProbeTest_f);
		ri.Cmd_AddCommand("r_rtBakeAt", R_RtBakeAt_f);
		ri.Cmd_AddCommand("r_rtShadowProbe", R_RtShadowProbe_f);
	} else {
		ri.Cmd_RemoveCommand("r_rtTestMove");
		ri.Cmd_RemoveCommand("r_rtTestBody");
		ri.Cmd_RemoveCommand("r_rtProbeInfo");
		ri.Cmd_RemoveCommand("r_rtProbeTest");
		ri.Cmd_RemoveCommand("r_rtBakeAt");
		ri.Cmd_RemoveCommand("r_rtShadowProbe");
		Com_Memset(&rt_test, 0, sizeof(rt_test));
		Com_Memset(&rt_testBody, 0, sizeof(rt_testBody));
	}
}
