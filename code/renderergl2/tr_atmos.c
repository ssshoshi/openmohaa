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
//     // where the wind blows to (degrees, as yaw: 0 is +x, 90 +y), how hard
//     wind    120 1.2
//     // the clouds' shadows: how dark (0 to 1), how big a cloud is (units),
//     // how fast they drift (units a second)
//     clouds  0.5 2600 45
//
// The wind is the one the ground cover leans with and the clouds drift on,
// and the cgame's rain slant and smoke are given it (vss_wind_x, vss_wind_y);
// with no line it is the wind there always was. The clouds' shadows drift
// over what the sun lights (glsl/shadowmask_fp.glsl): the sun's light dimmed
// under them, along its rays, so a wall's shadow meets the ground's. A map
// has none until its file says so; r_cloudShadows scales them.

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
	float    windYaw, windStrength;
	float    cloudDark, cloudSize, cloudSpeed;
	qboolean windGiven; // the cgame's wind cvars are this map's
} atmos;

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

	atmos.found  = qfalse;
	atmos.graded = qfalse;
	R_GradeNeutral(&atmos.grade);
	atmos.windSet      = qfalse;
	atmos.windYaw      = WIND_YAW_DEFAULT;
	atmos.windStrength = 1.0f;
	atmos.cloudDark    = 0.0f;
	atmos.cloudSize    = 2600.0f;
	atmos.cloudSpeed   = 45.0f;
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
			float v[2];
			const int n = R_AtmosNumbers(&text, v, 2);

			if (n < 1) {
				ri.Printf(PRINT_WARNING, "%s: wind needs where it blows to, in degrees\n", atmos.file);
			} else {
				atmos.windSet      = qtrue;
				atmos.windYaw      = v[0];
				atmos.windStrength = n > 1 ? Com_Clamp(0.0f, 4.0f, v[1]) : 1.0f;
			}
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
		ri.Cvar_Set("vss_wind_x", va("%g", dir[0] * 9.0f * strength));
		ri.Cvar_Set("vss_wind_y", va("%g", dir[1] * 9.0f * strength));
		atmos.windGiven = qtrue;
	} else if (atmos.windGiven) {
		ri.Cvar_Set("vss_wind_x", "8");
		ri.Cvar_Set("vss_wind_y", "4");
		atmos.windGiven = qfalse;
	}
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
	}
	{
		vec2_t dir;
		float  strength;

		R_AtmosWind(dir, &strength);
		ri.Printf(PRINT_ALL, "Wind to %g degrees (%.2f %.2f), strength %g%s\n", tr.world ? atmos.windYaw : WIND_YAW_DEFAULT, dir[0],
			dir[1], strength, atmos.windSet ? "" : " (no wind line: as it always was)");
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
	r_cloudShadows = ri.Cvar_Get("r_cloudShadows", "1", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_cloudShadows, "The shadows of clouds drifting over what the sun lights, on maps whose atmos/<map>.txt has clouds: 0 none, 1 as dark as the map has them. Needs r_sunShadows");
	ri.Cmd_AddCommand("atmosinfo", R_AtmosInfo_f);
	ri.Cmd_AddCommand("atmosreload", R_AtmosReload_f);
}

void R_AtmosShutdown(void)
{
	ri.Cmd_RemoveCommand("atmosinfo");
	ri.Cmd_RemoveCommand("atmosreload");
}
