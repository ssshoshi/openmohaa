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
//   2  all of it is live: the map's lighting gives way to its ambient light.
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
cvar_t *r_rtDebug;
cvar_t *r_rtLightLists;
cvar_t *r_rtCutoff;

// the lights a surface may take are a bit each of 64 (drawSurf_t.rtMask),
// sent four indexes a vec4 (u_RtList)
typedef char rt_maxLightsFit[(RT_MAX_LIGHTS <= 64 && RT_MAX_LIGHTS % 4 == 0) ? 1 : -1];

// six tiles a light
#define RT_SLOTS ((RT_ATLAS_TILES * RT_ATLAS_TILES) / 6)

// A light adds less than this to a texel, in the BSP's raw lightmap units:
// it is out of range.
#define RT_CUTOFF 1.0f

#define RT_SQRT1_2 0.70710678f

// how long a light takes to come in or go out
#define RT_EASE_SECONDS 0.5f

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
} rtLight_t;

static struct {
	qboolean   loaded;
	rtLight_t *lights;
	int        numLights;
	int       *fromSphere; // each sphere light's, -1 none (dark, for models only)
	qboolean   leafLights; // the map lists the lights reaching each leaf
	int        seenCluster;  // the view's cluster the list below is for, -1 none
	int       *seenLights;   // the lights listed for the leaves it can see
	int        numSeen;
	int        staticOwner[RT_SLOTS]; // the light in each slot, -1 none

	vec3_t     ambient;  // raw
	qboolean   sun;
	vec3_t     sunDir;   // towards the sun
	vec3_t     sunColor; // raw

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
	r_rtMaxLights = ri.Cvar_Get("r_rtMaxLights", "40", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtMaxLights, 0, RT_MAX_LIGHTS, qtrue);
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
	ri.Cvar_SetDescription(r_rtParticles, "Dust and smoke sprites take the realtime lights at their corners, not per pixel (0: per pixel, which piles of them make very slow). Not archived: an A/B switch");
	r_rtDebug = ri.Cvar_Get("r_rtDebug", "0", 0);
	r_rtCutoff = ri.Cvar_Get("r_rtCutoff", "0.0005", CVAR_ARCHIVE);
	ri.Cvar_CheckRange(r_rtCutoff, 0, 0.05, qfalse);
	ri.Cvar_SetDescription(r_rtCutoff, "A realtime light that would add less than this to a pixel (1 is full brightness), shadow or not, is passed over there without its shadow being looked up (0: none passed over)");
	r_rtLightLists = ri.Cvar_Get("r_rtLightLists", "1", 0);
	ri.Cvar_SetDescription(r_rtLightLists, "Each draw takes only the realtime lights that reach its surfaces' bounds (0: every draw takes them all). Not archived: an A/B switch");
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
#define RT_MERGE_REACH   48.0f // point lights this close are one lamp

static void R_RtLightsAtFixtures(void)
{
	int i, j, merged = 0, fixtures = 0;

	for (i = 0; i < tr.world->numStaticModels; i++) {
		cStaticModelUnpacked_t *sm = &tr.world->staticModels[i];
		vec3_t                  centre, sum, colour;
		float                   weight = 0;
		int                     count  = 0, keep = -1;

		sm->rtFixture = qfalse;
		if (!sm->tiki || !R_RtIsFixture(sm)) {
			continue;
		}
		sm->rtFixture = qtrue;
		fixtures++;

		// its middle
		VectorAdd(sm->tiki->a->mins, sm->tiki->a->maxs, centre);
		VectorScale(centre, 0.5f * sm->tiki->load_scale * sm->scale, centre);
		{
			vec3_t local;

			VectorCopy(centre, local);
			VectorCopy(sm->origin, centre);
			for (j = 0; j < 3; j++) {
				VectorMA(centre, local[j], sm->axis[j], centre);
			}
		}

		if (!r_rtLightsAtFixtures->integer) {
			continue;
		}

		VectorClear(sum);
		VectorClear(colour);
		for (j = 0; j < rt.numLights; j++) {
			rtLight_t *l = &rt.lights[j];
			float      w;

			if (l->spot || l->range <= 0 || Distance(l->origin, centre) > RT_FIXTURE_REACH) {
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

			VectorCopy(centre, l->origin);
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

		if (l->spot || l->range <= 0) {
			continue;
		}
		weight = l->color[0] + l->color[1] + l->color[2];
		VectorScale(l->origin, weight, sum);

		for (j = i + 1; j < rt.numLights; j++) {
			rtLight_t *o = &rt.lights[j];
			float      w;

			if (o->spot || o->range <= 0 || Distance(o->origin, l->origin) > RT_MERGE_REACH) {
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

			rt.fromSphere[l->source] = to;
		}
		for (i = 0, j = 0; i < rt.numLights; i++) {
			if (rt.lights[i].range > 0) {
				rt.lights[j++] = rt.lights[i];
			}
		}
		rt.numLights = j;
		ri.Hunk_FreeTempMemory(newIndex);
	}

	ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d fixtures, %d lights taken into them or their neighbours\n", fixtures, merged);
}

void R_RtLoadWorld(void)
{
	int i;

	R_RtFreeWorld();

	if (!r_realtimeLighting->integer || !tr.world) {
		return;
	}

	rt.lights     = ri.Malloc(sizeof(rtLight_t) * Q_max(1, tr.numSLights));
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
		// nothing lights nothing, here or in the stock sphere lighting
		if (sl->intensity <= 0 || (sl->spot_light && sl->spot_radiusbydistance <= 0)) {
			continue;
		}

		l = &rt.lights[rt.numLights++];
		Com_Memset(l, 0, sizeof(*l));
		l->source     = i;
		l->mergedInto = -1;
		VectorCopy(sl->origin, l->origin);
		VectorScale(sl->color, sl->intensity * 7500.0f, l->color);
		l->spot         = sl->spot_light ? qtrue : qfalse;
		l->radiusByDist = sl->spot_radiusbydistance;
		VectorCopy(sl->spot_dir, l->spotDir);
		if (l->spot) {
			VectorNormalize(l->spotDir);
		}

		brightest = Q_max(l->color[0], Q_max(l->color[1], l->color[2]));
		l->range  = Q_min(4096.0f, sqrt(brightest / RT_CUTOFF));
		l->staticSlot = -1;
	}

	R_RtLightsAtFixtures();

	// whether the map lists the lights reaching each leaf (R_RtMarkSeenLights)
	for (i = tr.world->numDecisionNodes; i < tr.world->numnodes && !rt.leafLights; i++) {
		rt.leafLights = tr.world->nodes[i].numlights > 0;
	}

	for (i = 0; i < RT_SLOTS; i++) {
		rt.staticOwner[i] = -1;
	}

	rt.seenLights  = ri.Malloc(sizeof(int) * Q_max(1, rt.numLights));
	rt.seenCluster = -1;

	// the sun as worldspawn has it (R_Sphere_InitLights scaled it for models)
	rt.sun = s_sun.exists;
	if (rt.sun) {
		VectorCopy(s_sun.direction, rt.sunDir);
		VectorNormalize(rt.sunDir);
		VectorScale(s_sun.color, 1.0f / Q_max(1.0f, tr.overbrightMult), rt.sunColor);
	}

	R_RtParseAmbient();

	rt.loaded = qtrue;
	ri.Printf(PRINT_DEVELOPER, "realtime lighting: %d lights%s\n", rt.numLights, rt.sun ? " and the sun" : "");
}

void R_RtFreeWorld(void)
{
	if (rt.lights) {
		ri.Free(rt.lights);
	}
	if (rt.fromSphere) {
		ri.Free(rt.fromSphere);
	}
	if (rt.seenLights) {
		ri.Free(rt.seenLights);
	}
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

static void R_RtRenderFace(const rtLight_t *l, int atlas, int tile, int face)
{
	viewParms_t parms;
	const int   size = r_rtShadowTile->integer;

	Com_Memset(&parms, 0, sizeof(parms));
	parms.viewportX      = (tile % RT_ATLAS_TILES) * size;
	parms.viewportY      = (tile / RT_ATLAS_TILES) * size;
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
static int R_RtFacesTouched(const rtLight_t *l, const vec3_t centre, float radius)
{
	vec3_t v;
	int    faces = 0, f;

	VectorSubtract(centre, l->origin, v);
	if (DotProduct(v, v) > Square(l->range + radius)) {
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
	vec3_t centre;
	float  radius;
} rtMover_t;

#define RT_MAX_MOVERS 1024

static void R_RtStandingSphere(const vec3_t centre, float radius)
{
	int i;

	rt.invalidated++;

	for (i = 0; i < rt.numLights; i++) {
		rtLight_t *l = &rt.lights[i];

		if (l->staticSlot >= 0 && l->staticFaces) {
			l->staticFaces &= ~R_RtFacesTouched(l, centre, radius);
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
		if (radius <= 0) {
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
			count++;
		}
	}

	R_RtForget(rt_entities, MAX_GENTITIES);
	R_RtForget(rt_clientModels, MAX_MOD_KNOWN);

	return count;
}

// A slot in the atlas of what stands still, taking the one used longest ago
// if none is free.
static int R_RtStaticSlot(int light)
{
	int i, best = -1, oldest = 0x7fffffff;

	for (i = 0; i < RT_SLOTS; i++) {
		int owner = rt.staticOwner[i];

		if (owner < 0) {
			best = i;
			break;
		}
		if (rt.lights[owner].lastUsed == rt.frame) {
			continue;
		}
		if (rt.lights[owner].lastUsed < oldest) {
			oldest = rt.lights[owner].lastUsed;
			best   = i;
		}
	}

	if (best < 0) {
		return -1;
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
===============
R_RtRenderShadows

Once a frame, before the main view: the lights that light what can be seen,
their shadows, and what the shaders are told of them.
===============
*/
// Whether a light reaches into the view's four sides at all
static qboolean R_RtReachesView(const rtLight_t *l, const cplane_t frustum[4])
{
	int k;

	for (k = 0; k < 4; k++) {
		if (DotProduct(l->origin, frustum[k].normal) - frustum[k].dist < -l->range) {
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

static int R_RtFacesInView(const rtLight_t *l, const cplane_t frustum[5])
{
	int faces = 0, f, c, k;

	for (f = 0; f < 6; f++) {
		vec3_t axis[3], points[5];

		R_RtFaceAxes(f, axis);
		VectorCopy(l->origin, points[0]);
		for (c = 0; c < 4; c++) {
			VectorMA(l->origin, l->range, axis[0], points[c + 1]);
			VectorMA(points[c + 1], (c & 1) ? l->range : -l->range, axis[1], points[c + 1]);
			VectorMA(points[c + 1], (c & 2) ? l->range : -l->range, axis[2], points[c + 1]);
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
	return qtrue;
}

void R_RtRenderShadows(const refdef_t *fd)
{
	float     sorted[1024][2];
	int       numSorted = 0, numChosen, numCandidates;
	int       candidates[RT_MAX_LIGHTS];
	float     ease;
	int       numBodies;
	rtMover_t movers[RT_MAX_MOVERS];
	int       numMovers;
	int       i, budget, dynamicSlots = 0, dynamicLights = 0;
	float     scale;
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

	// lightmap units: what R_ColorShiftLightingBytes does to them, over 255
	shift = r_mapOverBrightBits->integer - tr.overbrightBits;
	scale = (float)(1 << Q_max(0, shift)) / 255.0f * r_rtScale->value;

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

	// The lights that may reach what the view can see, the biggest on the
	// screen first. Which way the view looks does not count: a lamp left out
	// while just off the screen eased back in as the view turned to it, the
	// world brightening as it came round.
	seen = R_RtMarkSeenLights(fd->vieworg);
	for (i = 0; i < rt.numLights && numSorted < 1024; i++) {
		const rtLight_t *l = &rt.lights[i];

		if (seen && l->visFrame != rt.frame) {
			continue;
		}

		// how big the light's pool is on the screen, as reach over distance,
		// kept inverted to sort nearest first; the lit a little ahead, so none
		// flickers in and out where the few lit end
		sorted[numSorted][0] = (float)i;
		sorted[numSorted][1] = Q_max(64.0f, Distance(l->origin, fd->vieworg)) / l->range;
		if (l->weight > 0) {
			sorted[numSorted][1] *= 0.8f;
		}
		numSorted++;
	}
	qsort(sorted, numSorted, sizeof(sorted[0]), R_RtCompareDistance);
	numChosen = Q_min(numSorted, r_rtMaxLights->integer);

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
	for (i = 0; i < rt.numLights && numCandidates < RT_MAX_LIGHTS; i++) {
		if (rt.lights[i].weight > 0 && rt.lights[i].chosen != rt.frame) {
			candidates[numCandidates++] = i;
		}
	}

	numMovers = r_rtShadows->integer ? R_RtTrackCasters(movers) : 0;
	budget    = r_rtFacesPerFrame->integer;
	numBodies = R_RtGatherBodies(bodies);
	rt.numPacked = 0;

	for (i = 0; i < numCandidates && tr.rtNumActive < RT_MAX_LIGHTS; i++) {
		const int      index  = candidates[i];
		rtLight_t     *l      = &rt.lights[index];
		const qboolean chosen = l->chosen == rt.frame ? qtrue : qfalse;
		qboolean       ready  = qtrue;
		vec4_t        *data;
		float          dynamicTile = -1;

		l->lastUsed = rt.frame;

		if (r_rtShadows->integer) {
			int f;

			// what stands still: drawn once, a few faces a frame
			if (l->staticSlot < 0 && R_RtStaticSlot(index) < 0) {
				l->weight = 0;
				continue;
			}
			for (f = 0; f < 6 && budget > 0; f++) {
				if (l->staticFaces & (1 << f)) {
					continue;
				}
				R_RtRenderFace(l, 0, l->staticSlot * 6 + f, f);
				rt.staticDrawn++;
				l->staticFaces |= 1 << f;
				budget--;
			}
			// the world as compiled, once
			if (tr.rtShadowFbo[2]) {
				for (f = 0; f < 6 && budget > 0; f++) {
					if (l->bakedFaces & (1 << f)) {
						continue;
					}
					R_RtRenderFace(l, 2, l->staticSlot * 6 + f, f);
					rt.staticDrawn++;
					l->bakedFaces |= 1 << f;
					budget--;
				}
			}
			if (l->staticFaces == 63 && (!tr.rtShadowFbo[2] || l->bakedFaces == 63)) {
				l->staticReady = qtrue;
			}
			// not lit until its shadow is whole: it would shine through walls
			ready = l->staticReady;

			// what moves: drawn every frame, in the faces it is in, for the
			// nearest lights
			if (ready && chosen && dynamicLights < r_rtDynamicLights->integer && R_RtReachesView(l, frustum))
			{
				int faces = 0, m;

				for (m = 0; m < numMovers; m++) {
					faces |= R_RtFacesTouched(l, movers[m].centre, movers[m].radius);
				}
				if (faces && dynamicSlots < RT_SLOTS) {
					const int slot = dynamicSlots++;
					// None of the view's pixels reads the faces out of it, so
					// they are not drawn. The light keeps its place among the
					// few all the same: had the faces out of view not counted,
					// the places went round the lights as the view turned, and
					// what moves lost and found its shadows in patches.
					const int drawn = faces & R_RtFacesInView(l, frustum);

					dynamicLights++;
					for (f = 0; f < 6; f++) {
						if (drawn & (1 << f)) {
							R_RtRenderFace(l, 1, slot * 6 + f, f);
						}
					}
					// the faces go in the fraction, a bit each; with none drawn
					// the people still take no capsules for this light, as
					// when they were drawn into faces of it
					dynamicTile = slot * 6 + drawn / 64.0f;
				}
			}
		}

		if (chosen && ready) {
			l->weight = Q_min(1.0f, l->weight + ease);
		} else {
			l->weight = Q_max(0.0f, l->weight - ease);
		}
		if (l->weight <= 0 || !ready) {
			continue;
		}

		data = &tr.rtLightData[tr.rtNumActive * RT_LIGHT_VEC4S];
		VectorCopy(l->origin, data[0]);
		data[0][3] = l->spot ? 1.0f : 0.0f;
		// eased as it is lit: smoothstep
		VectorScale(l->color, scale * l->weight * l->weight * (3.0f - 2.0f * l->weight), data[1]);
		data[1][3] = l->range;
		VectorCopy(l->spotDir, data[2]);
		data[2][3] = l->radiusByDist;
		data[3][0] = r_rtShadows->integer ? (float)(l->staticSlot * 6) : -1.0f;
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

		if (dl->radius <= 0) {
			continue;
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
		VectorScale(rt.sunColor, scale, tr.rtSunColor);
		// the screen shadow the depth prepass leaves (RB_RenderDrawSurfList)
		tr.rtSunColor[3] = (tr.screenShadowImage && tr.sunShadows && r_depthPrepass->value && r_sunlightMode->integer) ? 1.0f : 0.0f;
	} else {
		VectorClear4(tr.rtSunDir);
		VectorClear4(tr.rtSunColor);
	}

	VectorScale(rt.ambient, scale * r_rtAmbient->value, tr.rtAmbient);
	// the least a light may add to a pixel and be looked up there (RtLight)
	tr.rtAmbient[3] = Q_max(0.0f, r_rtCutoff->value);

	if (r_rtDebug->integer) {
		ri.Printf(PRINT_ALL, "realtime lighting: %d of %d lights lit (%d reach the view), %d movers, faces drawn %d standing %d moving (%d lights), %d changes to what stands, %d bodies and parts (%d sent), %d particle draws (%d vertexes)\n",
			tr.rtNumActive, numChosen, numSorted, numMovers, rt.staticDrawn, rt.facesDrawn - rt.staticDrawn, dynamicSlots, rt.invalidated, numBodies, rt.numPacked,
			rt.particleDraws, rt.particleVerts);
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
			ri.Printf(PRINT_ALL, "  light %d at (%.0f %.0f %.0f) type %.0f range %.0f color (%.2f %.2f %.2f) bodies %.3f spot (%.2f %.2f %.2f) %.3f\n",
				i, d[0][0], d[0][1], d[0][2], d[0][3], d[1][3], d[1][0], d[1][1], d[1][2], d[3][2], d[2][0], d[2][1], d[2][2], d[2][3]);
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
	return ((uint64_t)1 << tr.rtNumActive) - 1;
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

	if (lightall) {
		return RT_STAGE_LIT;
	}

	// with the direct light live alone, what is lit at run time is lit as ever
	if (r_realtimeLighting->integer == 1 && (pStage->rgbGen == CGEN_LIGHTING_SPHERICAL || pStage->rgbGen == CGEN_LIGHTING_GRID
		|| pStage->rgbGen == CGEN_LIGHTING_DIFFUSE)) {
		return RT_STAGE_NONE;
	}

	// added on top: a glow, not something lit
	if (dstBlend == GLS_DSTBLEND_ONE) {
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

// stage < 0: a view the lights take no part in (2D, shadows, models in menus)
void R_RtSetUniforms(shaderProgram_t *sp, int stage)
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
	GLSL_SetUniformVec4(sp, UNIFORM_RTAMBIENT, tr.rtAmbient);
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
	} else {
		ri.Cmd_RemoveCommand("r_rtTestMove");
		ri.Cmd_RemoveCommand("r_rtTestBody");
		Com_Memset(&rt_test, 0, sizeof(rt_test));
		Com_Memset(&rt_testBody, 0, sizeof(rt_testBody));
	}
}
