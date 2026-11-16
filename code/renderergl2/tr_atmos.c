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

// tr_atmos.c -- a map's atmosphere, and the grade of the finished picture
//
// Added in OPM. What is a map's own about its air and its light is read from
// atmos/<map>.txt, a line a setting (`atmosreload` reads it again,
// `atmosinfo` prints it). So far that is its grade: the colour of the
// finished picture, after the tone map (r_grade, glsl/tonemap_fp.glsl), as a
// film is graded: cold and blue for the snow, warm for the desert.
//
//     // the darks toward blue, the whole a little less coloured
//     grade lift        0.00 0.01 0.03
//     grade gamma       1 1 1
//     grade gain        0.98 0.99 1.02
//     grade contrast    1.05
//     grade saturation  0.9
//     grade vibrance    0.2
//     grade vignette    0.2
//     grade grain       0.1
//
// lift raises (or tints) the darks and leaves white where it is, gain scales
// everything, gamma bends the middle (over 1: brighter). contrast spreads
// the tones about middle grey, saturation scales the colour, vibrance adds
// colour to what has little and leaves what is vivid. vignette darkens the
// corners, grain is film grain; both 0 to 1.
//
// The player's own grade is the r_grade* cvars, which are applied with the
// map's: r_gradeMap says how much of the map's is taken.
//
// The wind and the clouds:
//
//     // where the wind blows to (degrees, as yaw: 0 is +x, 90 +y), how
//     // hard, and how gusty (0 steady, 1 as any map is, to 2)
//     wind    120 1.2 1
//     // the clouds' shadows: how dark (0 to 1), how big a cloud is (units),
//     // how fast they drift (units a second)
//     clouds  0.5 2600 45
//
// The wind is the one the ground cover leans with and the clouds drift on,
// and the cgame's rain slant and smoke are given it (vss_wind_x, vss_wind_y);
// with no line it blows as it always did. It comes in gusts (r_windGusts,
// glsl/lightall_vp.glsl): fronts of harder wind carried downwind across the
// map, in bursts with lulls between, each from a quarter of its own up to
// some 40 degrees off, under which the grass leans over and flutters. The clouds' shadows drift
// over what the sun lights (glsl/shadowmask_fp.glsl): the sun's light dimmed
// under them, along its rays, so a wall's shadow meets the ground's. A map
// has none until its file says so; r_cloudShadows scales them.
//
// What grows on the ground (r_groundCover, tr_groundcover.c), one line a
// ground: grass, snow, gravel, sand or mud, as the map's shaders name it
// (surfaceparm). How thick, of r_groundCoverDensity, then what grows there
// and how much of it, any of: grass weeds flowers dead scrub reeds dry.
//
//     // the desert: dry scrub and yellowed grass on the stony ground
//     cover gravel 0.25 scrub 0.5 dry 0.5
//     // no flowers in this field, and no reeds by its pond
//     cover grass 1 flowers 0 reeds 0
//
// On grass ground weeds and flowers grow in patches among the grass and the
// grass is what is left over; reeds is the chance a tuft at a water's edge is
// a reed. On the others the shares are of each other. Snow ground grows dead
// grass at 0.5 unless a map says otherwise, gravel and sand nothing; only
// terrain is taken for them, not brushes (the roofs and beams are snow too).
// Mud (and dirt) grows reeds at a water's edge and nothing anywhere else.
// The ground cover reads it as the map loads, so it takes a map reload.

#include "tr_local.h"

cvar_t *r_grade;
static cvar_t *r_gradeMap;
static cvar_t *r_gradeContrast;
static cvar_t *r_gradeSaturation;
static cvar_t *r_gradeVibrance;
static cvar_t *r_gradeLift;
static cvar_t *r_gradeGamma;
static cvar_t *r_gradeGain;
static cvar_t *r_gradeVignette;
static cvar_t *r_gradeGrain;
static cvar_t *r_wind;
static cvar_t *r_windGusts;
static cvar_t *r_cloudShadows;

#define WIND_YAW_DEFAULT 36.87f // the ground cover's wind as it always was: (0.8, 0.6)
#define CLOUD_SIZE       256

typedef struct {
	vec3_t lift, gamma, gain;
	float  contrast, saturation, vibrance, vignette, grain;
} grade_t;

static struct {
	char     file[MAX_QPATH];
	qboolean found;
	qboolean graded; // the file has a grade line
	grade_t  grade;
	qboolean windSet; // the file has a wind line
	float    windYaw, windStrength, windGusts;
	float    cloudDark, cloudSize, cloudSpeed;
	qboolean windGiven; // the cgame's wind cvars are this map's
	atmosCover_t cover[GC_NUM_GROUNDS];
} atmos;

const char *gcKindNames[GC_NUM_KINDS]   = { "grass", "grass2", "weeds", "flowers", "dead", "scrub", "reeds", "dry" };
const char *gcGroundNames[GC_NUM_GROUNDS] = { "grass", "snow", "gravel", "sand", "mud" };

static void R_GradeNeutral(grade_t *g)
{
	VectorSet(g->lift, 0.0f, 0.0f, 0.0f);
	VectorSet(g->gamma, 1.0f, 1.0f, 1.0f);
	VectorSet(g->gain, 1.0f, 1.0f, 1.0f);
	g->contrast   = 1.0f;
	g->saturation = 1.0f;
	g->vibrance   = 0.0f;
	g->vignette   = 0.0f;
	g->grain      = 0.0f;
}

// Whether the line has anything more on it. (COM_ParseExt asked for a token
// the line does not have steps onto the next line, which was then passed
// over whole: `grade lift 0.02` took the line after it with it.)
static qboolean R_AtmosMoreOnLine(const char *p)
{
	if (!p) {
		return qfalse;
	}
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return *p && *p != '\n' && *p != '\r' && !(p[0] == '/' && p[1] == '/');
}

static int R_AtmosNumbers(char **text, float *out, int count)
{
	int i;

	for (i = 0; i < count && R_AtmosMoreOnLine(*text); i++) {
		const char *token = COM_ParseExt(text, qfalse);
		if (!token[0]) {
			break;
		}
		out[i] = atof(token);
	}
	return i;
}

// On to the end of the line. (SkipRestOfLine steps past the end of a file
// with no last newline, and takes no text at all, which is what looking for
// a number at the end of the file leaves.)
static void R_AtmosNextLine(char **text)
{
	char *p = *text;

	if (!p) {
		return;
	}
	while (*p && *p != '\n') {
		p++;
	}
	*text = p;
}

// One number for all three, or three
static qboolean R_AtmosColour(char **text, vec3_t out)
{
	float     v[3];
	const int n = R_AtmosNumbers(text, v, 3);

	if (n == 1) {
		VectorSet(out, v[0], v[0], v[0]);
	} else if (n == 3) {
		VectorCopy(v, out);
	} else {
		return qfalse;
	}
	return qtrue;
}

// cover <ground> <density> [<kind> <share>]...
static void R_AtmosCoverLine(char **text)
{
	const char   *token = R_AtmosMoreOnLine(*text) ? COM_ParseExt(text, qfalse) : "";
	atmosCover_t *c     = NULL;
	float         v;
	int           i;

	for (i = 0; i < GC_NUM_GROUNDS; i++) {
		if (!Q_stricmp(token, gcGroundNames[i])) {
			c = &atmos.cover[i];
		}
	}
	if (!c || R_AtmosNumbers(text, &v, 1) < 1) {
		ri.Printf(PRINT_WARNING, "%s: cover needs a ground (grass, snow, gravel, sand or mud) and how thick\n", atmos.file);
		return;
	}
	c->set     = qtrue;
	c->density = Com_Clamp(0.0f, 4.0f, v);
	while (R_AtmosMoreOnLine(*text)) {
		char key[32];

		Q_strncpyz(key, COM_ParseExt(text, qfalse), sizeof(key));
		if (R_AtmosNumbers(text, &v, 1) < 1) {
			ri.Printf(PRINT_WARNING, "%s: cover %s: '%s' wants how much\n", atmos.file, gcGroundNames[c - atmos.cover], key);
			return;
		}
		for (i = 0; i < GC_NUM_KINDS; i++) {
			if (!Q_stricmp(key, gcKindNames[i])) {
				break;
			}
		}
		if (i == GC_NUM_KINDS) {
			ri.Printf(PRINT_WARNING, "%s: cover: '%s' does not grow here (grass weeds flowers dead scrub reeds dry)\n", atmos.file, key);
			continue;
		}
		c->share[i == GC_KIND_GRASS2 ? GC_KIND_GRASS : i] = Q_max(0.0f, v);
	}
}

/*
=============
R_AtmosCover

What the map's file says grows on a ground (set is qfalse when it says nothing).
=============
*/
const atmosCover_t *R_AtmosCover(int ground)
{
	return &atmos.cover[ground];
}

static void R_AtmosGradeLine(char **text)
{
	const char *token = R_AtmosMoreOnLine(*text) ? COM_ParseExt(text, qfalse) : "";
	grade_t    *g     = &atmos.grade;
	float       v;
	qboolean    ok = qtrue;
	char        key[32];

	Q_strncpyz(key, token, sizeof(key));
	if (!Q_stricmp(key, "lift")) {
		ok = R_AtmosColour(text, g->lift);
	} else if (!Q_stricmp(key, "gamma")) {
		ok = R_AtmosColour(text, g->gamma);
	} else if (!Q_stricmp(key, "gain")) {
		ok = R_AtmosColour(text, g->gain);
	} else if (R_AtmosNumbers(text, &v, 1) != 1) {
		ok = qfalse;
	} else if (!Q_stricmp(key, "contrast")) {
		g->contrast = v;
	} else if (!Q_stricmp(key, "saturation")) {
		g->saturation = v;
	} else if (!Q_stricmp(key, "vibrance")) {
		g->vibrance = v;
	} else if (!Q_stricmp(key, "vignette")) {
		g->vignette = v;
	} else if (!Q_stricmp(key, "grain")) {
		g->grain = v;
	} else {
		ri.Printf(PRINT_WARNING, "%s: unknown grade '%s'\n", atmos.file, key);
		return;
	}
	if (!ok) {
		ri.Printf(PRINT_WARNING, "%s: grade %s needs a number (lift, gamma, gain: one or three)\n", atmos.file, key);
		return;
	}
	atmos.graded = qtrue;
}

static void R_AtmosReadFile(const char *mapName)
{
	union {
		char *c;
		void *v;
	} buffer;
	char *text;
	int   i;

	atmos.found  = qfalse;
	atmos.graded = qfalse;
	R_GradeNeutral(&atmos.grade);
	atmos.windSet      = qfalse;
	atmos.windYaw      = WIND_YAW_DEFAULT;
	atmos.windStrength = 1.0f;
	atmos.windGusts    = 1.0f;
	atmos.cloudDark    = 0.0f;
	atmos.cloudSize    = 2600.0f;
	atmos.cloudSpeed   = 45.0f;
	for (i = 0; i < GC_NUM_GROUNDS; i++) {
		int k;

		atmos.cover[i].set     = qfalse;
		atmos.cover[i].density = 0.0f;
		for (k = 0; k < GC_NUM_KINDS; k++) {
			atmos.cover[i].share[k] = -1.0f;
		}
	}
	Com_sprintf(atmos.file, sizeof(atmos.file), "atmos/%s.txt", mapName);
	if (ri.FS_ReadFile(atmos.file, &buffer.v) <= 0) {
		return;
	}
	atmos.found = qtrue;

	text = buffer.c;
	for (;;) {
		const char *token = COM_ParseExt(&text, qtrue);

		if (!token[0]) {
			break;
		}
		if (!Q_stricmp(token, "grade")) {
			R_AtmosGradeLine(&text);
		} else if (!Q_stricmp(token, "wind")) {
			float v[3];
			const int n = R_AtmosNumbers(&text, v, 3);

			if (n < 1) {
				ri.Printf(PRINT_WARNING, "%s: wind needs where it blows to, in degrees\n", atmos.file);
			} else {
				atmos.windSet      = qtrue;
				atmos.windYaw      = v[0];
				atmos.windStrength = n > 1 ? Com_Clamp(0.0f, 4.0f, v[1]) : 1.0f;
				atmos.windGusts    = n > 2 ? Com_Clamp(0.0f, 2.0f, v[2]) : 1.0f;
			}
		} else if (!Q_stricmp(token, "cover")) {
			R_AtmosCoverLine(&text);
		} else if (!Q_stricmp(token, "clouds")) {
			float v[3];
			const int n = R_AtmosNumbers(&text, v, 3);

			if (n < 1) {
				ri.Printf(PRINT_WARNING, "%s: clouds needs how dark their shadows are\n", atmos.file);
			} else {
				atmos.cloudDark  = Com_Clamp(0.0f, 1.0f, v[0]);
				atmos.cloudSize  = n > 1 ? Q_max(64.0f, v[1]) : 2600.0f;
				atmos.cloudSpeed = n > 2 ? v[2] : 45.0f;
			}
		} else {
			// a newer file's setting: not this build's to read
			ri.Printf(PRINT_DEVELOPER, "%s: '%s' is not known to this build\n", atmos.file, token);
		}
		R_AtmosNextLine(&text);
	}
	ri.FS_FreeFile(buffer.v);
}

/*
=============
R_AtmosLoadWorld

The map's atmosphere, as it loads (or `atmosreload`).
=============
*/
void R_AtmosLoadWorld(const char *mapName)
{
	R_AtmosReadFile(mapName);
	// the last map's cloud image went with its images
	tr.cloudShadowImage = NULL;

	// the cgame's wind, which slants the rain and carries the smoke: this
	// map's, or back to what it was when the last map had its own
	if (atmos.windSet) {
		vec2_t dir;
		float  strength;

		R_AtmosWind(dir, &strength);
		ri.Cvar_Set("vss_wind_x", va("%.2f", dir[0] * 9.0f * strength));
		ri.Cvar_Set("vss_wind_y", va("%.2f", dir[1] * 9.0f * strength));
		atmos.windGiven = qtrue;
	} else if (atmos.windGiven) {
		ri.Cvar_Set("vss_wind_x", "8");
		ri.Cvar_Set("vss_wind_y", "4");
		atmos.windGiven = qfalse;
	}
}

/*
=============
R_AtmosGusts

How gusty the wind is: 0 steady, 1 as any map is, to 2 (r_windGusts scales
the map's).
=============
*/
float R_AtmosGusts(void)
{
	return Com_Clamp(0.0f, 2.0f, (tr.world ? atmos.windGusts : 1.0f) * Q_max(0.0f, r_windGusts->value));
}

/*
==================
Blasts

Added in OPM: where something blew up lately (RE_AddBlast, from cgame). What
grows near is thrown back from it and swings to rest: R_BlastSway for a tree
or a bush, R_BlastPushers for the grass, which lies flat round it and stands
up again from the outside in.
==================
*/
#define MAX_BLASTS   8
#define BLAST_SWAY   4.5f // seconds a tree takes to come to rest
#define BLAST_FLAT   7.0f // seconds grass lies

static struct {
	vec3_t origin;
	float  radius, strength;
	float  time; // the scene's (refdef.floatTime): every pass of a frame has the same, so puts a tree in the same place
} blasts[MAX_BLASTS];
static int numBlasts;

int RE_AddBlast(const float *origin, float radius, float strength, float *shaken, int maxShaken)
{
	int i, k, oldest = 0, count = 0;

	if (!origin || radius <= 0.0f) {
		return 0;
	}
	if (numBlasts < MAX_BLASTS) {
		oldest = numBlasts++;
	} else {
		for (i = 1; i < MAX_BLASTS; i++) {
			if (blasts[i].time < blasts[oldest].time) {
				oldest = i;
			}
		}
	}
	VectorCopy(origin, blasts[oldest].origin);
	blasts[oldest].radius   = radius;
	blasts[oldest].strength = strength;
	blasts[oldest].time     = tr.refdef.floatTime;

	// the trees and bushes it shakes hard: within twice its radius, the nearest first
	if (shaken && maxShaken > 0 && tr.world) {
		float dists[16];

		maxShaken = Q_min(maxShaken, 16);
		for (i = 0; i < tr.world->numStaticModels; i++) {
			const cStaticModelUnpacked_t *sm = &tr.world->staticModels[i];
			float                         tall, d;

			if (sm->floraHeight <= 0.0f || !sm->tiki) {
				continue;
			}
			tall = sm->floraHeight * sm->tiki->load_scale * sm->scale;
			d    = (sm->origin[0] - origin[0]) * (sm->origin[0] - origin[0]) + (sm->origin[1] - origin[1]) * (sm->origin[1] - origin[1]);
			if (d > radius * radius * 4.0f || origin[2] < sm->origin[2] - radius || origin[2] > sm->origin[2] + tall + radius) {
				continue;
			}
			for (k = count; k > 0 && dists[k - 1] > d; k--) {
				if (k < maxShaken) {
					dists[k] = dists[k - 1];
					Vector4Copy(&shaken[(k - 1) * 4], &shaken[k * 4]);
				}
			}
			if (k < maxShaken) {
				dists[k] = d;
				VectorSet4(&shaken[k * 4], sm->origin[0], sm->origin[1], sm->origin[2] + tall * 0.62f, tall);
				count = Q_min(count + 1, maxShaken);
			}
		}
	}
	return count;
}

/*
What a tree standing at a point feels: out[0..1] which way it is thrown, as a
part of its height its top goes; out[2] how hard it shakes (0 to 1 and over).
*/
void R_BlastSway(const vec3_t at, vec4_t out)
{
	const float now = backEnd.refdef.floatTime;
	int         i;

	VectorSet4(out, 0, 0, 0, 0);
	for (i = 0; i < numBlasts; i++) {
		const float age   = now - blasts[i].time;
		const float reach = blasts[i].radius * 3.0f;
		float       dx = at[0] - blasts[i].origin[0], dy = at[1] - blasts[i].origin[1];
		float       dist = sqrt(dx * dx + dy * dy);
		float       f, env;

		if (age < 0.0f || age > BLAST_SWAY || dist > reach || fabs(at[2] - blasts[i].origin[2]) > reach) {
			continue;
		}
		// the wave takes a moment to reach it
		if (age < dist / 3000.0f) {
			continue;
		}
		f   = 1.0f - dist / reach;
		f   = f * f * blasts[i].strength;
		env = exp(-age * 1.3f);
		if (dist < 1.0f) {
			dx = 1.0f, dy = 0.0f, dist = 1.0f;
		}
		// thrown away from it, then swinging back and forth to rest
		out[0] += dx / dist * f * env * cos(age * 5.5f) * 0.07f;
		out[1] += dy / dist * f * env * cos(age * 5.5f) * 0.07f;
		out[2] = Q_max(out[2], f * env * 1.6f);
	}
}

/*
blasttest [distance] [radius] [strength]: a blast ahead of the view, as an
explosion there gives (nothing else of one: to see what grows react).
*/
static void R_BlastTest_f(void)
{
	const float dist = ri.Cmd_Argc() > 1 ? atof(ri.Cmd_Argv(1)) : 300.0f;
	vec3_t      at;

	VectorMA(tr.refdef.vieworg, dist, tr.refdef.viewaxis[0], at);
	at[2] = tr.refdef.vieworg[2] - 60.0f;
	RE_AddBlast(at, ri.Cmd_Argc() > 2 ? atof(ri.Cmd_Argv(2)) : 300.0f, ri.Cmd_Argc() > 3 ? atof(ri.Cmd_Argv(3)) : 1.0f, NULL, 0);
}

/*
The grass's: where it lies flat, as its pushers are (x, y, z, how far round).
*/
int R_BlastPushers(vec4_t *out, int max)
{
	const float now = backEnd.refdef.floatTime;
	int         i, n = 0;

	// the newest first
	for (i = numBlasts - 1; i >= 0 && n < max; i--) {
		const float age = now - blasts[i].time;

		if (age < 0.0f || age > BLAST_FLAT) {
			continue;
		}
		VectorCopy(blasts[i].origin, out[n]);
		out[n][3] = blasts[i].radius * 0.8f * (1.0f - (age / BLAST_FLAT) * (age / BLAST_FLAT));
		n++;
	}
	return n;
}

/*
=============
R_AtmosWind

Where the wind blows to, flat and of length one, and how hard: 1 the wind
there always was (r_wind scales it).
=============
*/
void R_AtmosWind(vec2_t dir, float *strength)
{
	const float yaw = DEG2RAD(tr.world ? atmos.windYaw : WIND_YAW_DEFAULT);

	dir[0]    = cos(yaw);
	dir[1]    = sin(yaw);
	*strength = (tr.world ? atmos.windStrength : 1.0f) * Q_max(0.0f, r_wind->value);
}

// A noise of clouds that meets itself at its edges: 0 clear sky, 255 the
// thick of a cloud
static void R_AtmosCloudImage(void)
{
	static const int cells[4]  = {4, 8, 16, 32};
	static const float gain[4] = {0.5f, 0.27f, 0.15f, 0.08f};
	byte            *pic = ri.Hunk_AllocateTempMemory(CLOUD_SIZE * CLOUD_SIZE * 4);
	unsigned int     seed = 0x1e5f3a7u;
	byte             knots[4][32 * 32];
	int              o, x, y;

	for (o = 0; o < 4; o++) {
		for (x = 0; x < cells[o] * cells[o]; x++) {
			seed        = seed * 1664525u + 1013904223u;
			knots[o][x] = (byte)(seed >> 24);
		}
	}
	for (y = 0; y < CLOUD_SIZE; y++) {
		for (x = 0; x < CLOUD_SIZE; x++) {
			float sum = 0.0f;

			for (o = 0; o < 4; o++) {
				const int   n  = cells[o];
				const float fx = x * (float)n / CLOUD_SIZE, fy = y * (float)n / CLOUD_SIZE;
				const int   x0 = (int)fx, y0 = (int)fy, x1 = (x0 + 1) % n, y1 = (y0 + 1) % n;
				float       tx = fx - x0, ty = fy - y0;
				float       a, b;

				tx  = tx * tx * (3.0f - 2.0f * tx);
				ty  = ty * ty * (3.0f - 2.0f * ty);
				a   = knots[o][y0 * n + x0] + (knots[o][y0 * n + x1] - knots[o][y0 * n + x0]) * tx;
				b   = knots[o][y1 * n + x0] + (knots[o][y1 * n + x1] - knots[o][y1 * n + x0]) * tx;
				sum += (a + (b - a) * ty) * gain[o];
			}
			pic[(y * CLOUD_SIZE + x) * 4 + 0] = pic[(y * CLOUD_SIZE + x) * 4 + 1] = pic[(y * CLOUD_SIZE + x) * 4 + 2] =
				(byte)Com_Clamp(0.0f, 255.0f, sum);
			pic[(y * CLOUD_SIZE + x) * 4 + 3] = 255;
		}
	}
	tr.cloudShadowImage = R_CreateImage("*cloudShadow", pic, CLOUD_SIZE, CLOUD_SIZE, IMGTYPE_COLORALPHA,
		IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION, 0);
	ri.Hunk_FreeTempMemory(pic);
}

/*
=============
R_AtmosCloudShadows

The clouds' shadows for the sun's shadow mask (glsl/shadowmask_fp.glsl): how
dark (0: none, and nothing is bound), one over a cloud's size, and how far
they have drifted, in the image's own measure. Binds the cloud image.
=============
*/
void R_AtmosCloudShadows(vec4_t out, int tmu)
{
	const float dark = (tr.world ? atmos.cloudDark : 0.0f) * Com_Clamp(0.0f, 1.0f, r_cloudShadows->value);
	vec2_t      dir;
	float       strength, drift;

	VectorClear4(out);
	if (dark <= 0.0f) {
		return;
	}
	if (!tr.cloudShadowImage) {
		R_AtmosCloudImage();
	}
	if (!tr.cloudShadowImage) {
		return;
	}
	R_AtmosWind(dir, &strength);
	// (the shadows go the way the wind does: the image is read the other way)
	drift = (float)fmod(backEnd.refdef.floatTime, 36000.0) * atmos.cloudSpeed * strength / atmos.cloudSize;
	VectorSet4(out, dark, 1.0f / atmos.cloudSize, -dir[0] * drift, -dir[1] * drift);
	out[2] -= floor(out[2]);
	out[3] -= floor(out[3]);
	GL_BindToTMU(tr.cloudShadowImage, tmu);
}

static void R_GradeFromCvars(grade_t *g)
{
	R_GradeNeutral(g);
	sscanf(r_gradeLift->string, "%f %f %f", &g->lift[0], &g->lift[1], &g->lift[2]);
	if (sscanf(r_gradeGamma->string, "%f %f %f", &g->gamma[0], &g->gamma[1], &g->gamma[2]) == 1) {
		g->gamma[1] = g->gamma[2] = g->gamma[0];
	}
	if (sscanf(r_gradeGain->string, "%f %f %f", &g->gain[0], &g->gain[1], &g->gain[2]) == 1) {
		g->gain[1] = g->gain[2] = g->gain[0];
	}
	g->contrast   = r_gradeContrast->value;
	g->saturation = r_gradeSaturation->value;
	g->vibrance   = r_gradeVibrance->value;
	g->vignette   = r_gradeVignette->value;
	g->grain      = r_gradeGrain->value;
}

// The player's grade with as much of the map's as r_gradeMap takes
static void R_GradeCombined(grade_t *out)
{
	const float    k = (atmos.graded && tr.world) ? Com_Clamp(0.0f, 1.0f, r_gradeMap->value) : 0.0f;
	const grade_t *m = &atmos.grade;
	int            i;

	R_GradeFromCvars(out);
	for (i = 0; i < 3; i++) {
		out->lift[i]  += m->lift[i] * k;
		out->gamma[i] *= 1.0f + (m->gamma[i] - 1.0f) * k;
		out->gain[i]  *= 1.0f + (m->gain[i] - 1.0f) * k;
		out->lift[i]   = Com_Clamp(-0.5f, 0.5f, out->lift[i]);
		out->gamma[i]  = Com_Clamp(0.2f, 5.0f, out->gamma[i]);
		out->gain[i]   = Com_Clamp(0.0f, 4.0f, out->gain[i]);
	}
	out->contrast   = Com_Clamp(0.0f, 3.0f, out->contrast * (1.0f + (m->contrast - 1.0f) * k));
	out->saturation = Com_Clamp(0.0f, 3.0f, out->saturation * (1.0f + (m->saturation - 1.0f) * k));
	out->vibrance   = Com_Clamp(-1.0f, 2.0f, out->vibrance + m->vibrance * k);
	out->vignette   = Com_Clamp(0.0f, 1.0f, out->vignette + m->vignette * k);
	out->grain      = Com_Clamp(0.0f, 1.0f, out->grain + m->grain * k);
}

static qboolean R_GradeIsNeutral(const grade_t *g)
{
	const float e = 0.0005f;
	int         i;

	for (i = 0; i < 3; i++) {
		if (fabs(g->lift[i]) > e || fabs(g->gamma[i] - 1.0f) > e || fabs(g->gain[i] - 1.0f) > e) {
			return qfalse;
		}
	}
	return fabs(g->contrast - 1.0f) <= e && fabs(g->saturation - 1.0f) <= e && fabs(g->vibrance) <= e
		&& g->vignette <= e && g->grain <= e;
}

/*
=============
R_GradeUniforms

The grade for the tone map's program (glsl/tonemap_fp.glsl): lift with the
saturation, one over the gamma with the vibrance, gain with the contrast,
and the vignette, the grain, a number that changes every frame and whether
there is any grade at all.
=============
*/
void R_GradeUniforms(vec4_t lift, vec4_t gamma, vec4_t gain, vec4_t misc)
{
	grade_t g;

	if (r_grade->integer) {
		R_GradeCombined(&g);
	} else {
		R_GradeNeutral(&g);
	}
	VectorSet4(lift, g.lift[0], g.lift[1], g.lift[2], g.saturation);
	VectorSet4(gamma, 1.0f / g.gamma[0], 1.0f / g.gamma[1], 1.0f / g.gamma[2], g.vibrance);
	VectorSet4(gain, g.gain[0], g.gain[1], g.gain[2], g.contrast);
	VectorSet4(misc, g.vignette, g.grain, (float)(tr.frameCount & 1023), R_GradeIsNeutral(&g) ? 0.0f : 1.0f);
}

static void R_AtmosInfo_f(void)
{
	grade_t g;

	if (!atmos.file[0]) {
		ri.Printf(PRINT_ALL, "No map loaded\n");
	} else if (!atmos.found) {
		ri.Printf(PRINT_ALL, "%s: not there\n", atmos.file);
	} else {
		const grade_t *m = &atmos.grade;

		ri.Printf(PRINT_ALL, "%s:%s\n", atmos.file, atmos.graded ? "" : " no grade");
		if (atmos.graded) {
			ri.Printf(PRINT_ALL, "  grade lift %g %g %g  gamma %g %g %g  gain %g %g %g\n", m->lift[0], m->lift[1], m->lift[2],
				m->gamma[0], m->gamma[1], m->gamma[2], m->gain[0], m->gain[1], m->gain[2]);
			ri.Printf(PRINT_ALL, "  grade contrast %g  saturation %g  vibrance %g  vignette %g  grain %g\n", m->contrast,
				m->saturation, m->vibrance, m->vignette, m->grain);
		}
		for (int i = 0; i < GC_NUM_GROUNDS; i++) {
			const atmosCover_t *c = &atmos.cover[i];
			char                line[256];

			if (!c->set) {
				continue;
			}
			Com_sprintf(line, sizeof(line), "  cover %s %g", gcGroundNames[i], c->density);
			for (int k = 0; k < GC_NUM_KINDS; k++) {
				if (c->share[k] >= 0.0f) {
					Q_strcat(line, sizeof(line), va(" %s %g", gcKindNames[k], c->share[k]));
				}
			}
			ri.Printf(PRINT_ALL, "%s\n", line);
		}
	}
	{
		vec2_t dir;
		float  strength;

		R_AtmosWind(dir, &strength);
		ri.Printf(PRINT_ALL, "Wind to %g degrees (%.2f %.2f), strength %g, gusts %g%s\n", tr.world ? atmos.windYaw : WIND_YAW_DEFAULT,
			dir[0], dir[1], strength, R_AtmosGusts(), atmos.windSet ? "" : " (no wind line: where it always blew)");
		if (atmos.cloudDark > 0.0f) {
			ri.Printf(PRINT_ALL, "Cloud shadows %g dark (x r_cloudShadows %g), clouds %g units, drifting %g a second\n", atmos.cloudDark,
				r_cloudShadows->value, atmos.cloudSize, atmos.cloudSpeed * strength);
		} else {
			ri.Printf(PRINT_ALL, "No cloud shadows\n");
		}
	}
	if (!r_grade->integer) {
		ri.Printf(PRINT_ALL, "r_grade 0: the picture is not graded\n");
		return;
	}
	if (!(r_hdr->integer && r_postProcess->integer && (r_toneMap->integer || r_forceToneMap->integer))) {
		ri.Printf(PRINT_ALL, "No tone map (r_hdr, r_postProcess, r_toneMap): the picture is not graded\n");
		return;
	}
	R_GradeCombined(&g);
	ri.Printf(PRINT_ALL, "The picture's grade%s (the r_grade cvars with %g of the map's):\n",
		R_GradeIsNeutral(&g) ? ", none" : "", (atmos.graded && tr.world) ? Com_Clamp(0.0f, 1.0f, r_gradeMap->value) : 0.0f);
	ri.Printf(PRINT_ALL, "  lift %g %g %g  gamma %g %g %g  gain %g %g %g\n", g.lift[0], g.lift[1], g.lift[2], g.gamma[0],
		g.gamma[1], g.gamma[2], g.gain[0], g.gain[1], g.gain[2]);
	ri.Printf(PRINT_ALL, "  contrast %g  saturation %g  vibrance %g  vignette %g  grain %g\n", g.contrast, g.saturation,
		g.vibrance, g.vignette, g.grain);
}

static void R_AtmosReload_f(void)
{
	if (!tr.world) {
		ri.Printf(PRINT_ALL, "No map loaded\n");
		return;
	}
	R_AtmosLoadWorld(tr.world->baseName);
	R_AtmosInfo_f();
}

void R_AtmosInit(void)
{
	r_grade = ri.Cvar_Get("r_grade", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_grade, "Grades the colour of the finished picture, after the tone map: the r_grade* cvars and the map's own grade (atmos/<map>.txt). 0: off. Needs r_hdr and r_toneMap");
	r_gradeMap = ri.Cvar_Get("r_gradeMap", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeMap, "How much of the map's own grade is taken (atmos/<map>.txt: cold for the snow, warm for the desert), 0 none to 1 all");
	r_gradeContrast = ri.Cvar_Get("r_gradeContrast", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeContrast, "The picture's contrast about middle grey (1: as it is; 1.1 a little more)");
	r_gradeSaturation = ri.Cvar_Get("r_gradeSaturation", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeSaturation, "How coloured the picture is (0 grey, 1 as it is)");
	r_gradeVibrance = ri.Cvar_Get("r_gradeVibrance", "0", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeVibrance, "Adds colour to what has little of it and leaves what is vivid alone (0: none; 0.3 is plenty)");
	r_gradeLift = ri.Cvar_Get("r_gradeLift", "0 0 0", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeLift, "Red, green and blue added to the picture's darks, white left where it is (\"0 0.01 0.03\": blue shadows)");
	r_gradeGamma = ri.Cvar_Get("r_gradeGamma", "1 1 1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeGamma, "Bends the picture's middle tones, one number or red, green and blue (over 1: brighter)");
	r_gradeGain = ri.Cvar_Get("r_gradeGain", "1 1 1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeGain, "Scales the picture, one number or red, green and blue (\"1.03 1 0.96\": warmer)");
	r_gradeVignette = ri.Cvar_Get("r_gradeVignette", "0", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeVignette, "Darkens the picture's corners, 0 to 1");
	r_gradeGrain = ri.Cvar_Get("r_gradeGrain", "0", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_gradeGrain, "Film grain over the picture, 0 to 1");
	r_wind = ri.Cvar_Get("r_wind", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_wind, "How hard the wind blows: the ground cover leans with it and the clouds' shadows drift on it (1: as the map has it, 0 still)");
	r_windGusts = ri.Cvar_Get("r_windGusts", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_windGusts, "The wind comes in gusts: fronts of harder wind crossing the map in bursts, each from its own quarter, under which the grass leans over and flutters. 0: the steady sway there always was, 1: as the map has it (atmos/<map>.txt), up to 2");
	r_cloudShadows = ri.Cvar_Get("r_cloudShadows", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_cloudShadows, "The shadows of clouds drifting over what the sun lights, on maps whose atmos/<map>.txt has clouds: 0 none, 1 as dark as the map has them. Needs r_sunShadows");
	ri.Cmd_AddCommand("atmosinfo", R_AtmosInfo_f);
	ri.Cmd_AddCommand("atmosreload", R_AtmosReload_f);
	ri.Cmd_AddCommand("blasttest", R_BlastTest_f);
}

void R_AtmosShutdown(void)
{
	ri.Cmd_RemoveCommand("atmosinfo");
	ri.Cmd_RemoveCommand("atmosreload");
	ri.Cmd_RemoveCommand("blasttest");
}
