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
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_gputimer.c -- attribute GPU time to individual render passes
//
// The existing timers (backEnd.pc.msec, and the rf:/bk: fields com_speeds
// prints) only measure how long the CPU spent handing commands to the driver.
// A frame that is entirely GPU bound looks free to them, which makes them
// useless for deciding which pass to optimise. GL_ARB_timer_query timestamps
// sit in the GPU's own command stream, so the deltas between them are time the
// GPU actually spent.
//
// Results are read back several frames late and never waited on, so asking for
// them does not stall the pipeline and does not itself change what is being
// measured.

#include "tr_local.h"

typedef struct {
	GLuint   query;
	int      id;
	qboolean isEnd;
} gpuMark_t;

typedef struct {
	gpuMark_t marks[GPUTIMER_MAX_MARKS];
	int       numMarks;
} gpuTimerFrame_t;

static gpuTimerFrame_t gpuFrames[GPUTIMER_FRAMES];
static GLuint          gpuQueries[GPUTIMER_FRAMES * GPUTIMER_MAX_MARKS];
static int             gpuWriteFrame;
static qboolean        gpuInited;
static qboolean        gpuActive;      // latched for the whole frame
static int             gpuOverflowed;

// Running totals, so what gets reported is an average over many frames rather
// than whichever single frame happened to be read back. One frame is a bad
// sample: the first frames of a map are far heavier than the rest.
static double          gpuAccum[GPUTIMER_COUNT];
static int             gpuAccumFrames;

static void R_GpuTimerFrameBegin(void);

const char *const gpuTimerNames[GPUTIMER_COUNT] = {
	"frame",
	"sunshadow",
	"sun0",
	"sun1",
	"sun2",
	"sun3",
	"rtshadow",
	"prepass",
	"shadowmask",
	"main3d",
	"post",
	"present"
};

/*
================
R_GpuTimerInit
================
*/
void R_GpuTimerInit(void)
{
	int i, j;

	R_GpuTimerShutdown();

	if (!glRefConfig.timerQuery)
		return;

	qglGenQueries(ARRAY_LEN(gpuQueries), gpuQueries);

	for (i = 0; i < GPUTIMER_FRAMES; i++)
	{
		for (j = 0; j < GPUTIMER_MAX_MARKS; j++)
			gpuFrames[i].marks[j].query = gpuQueries[i * GPUTIMER_MAX_MARKS + j];

		gpuFrames[i].numMarks = 0;
	}

	gpuWriteFrame = 0;
	gpuOverflowed = 0;
	gpuInited = qtrue;

	R_GpuTimerFrameBegin();
}

/*
================
R_GpuTimerShutdown
================
*/
void R_GpuTimerShutdown(void)
{
	if (!gpuInited)
		return;

	qglDeleteQueries(ARRAY_LEN(gpuQueries), gpuQueries);

	Com_Memset(gpuFrames, 0, sizeof(gpuFrames));
	Com_Memset(&tr.gpuTimer, 0, sizeof(tr.gpuTimer));

	gpuInited = qfalse;
	gpuActive = qfalse;
}

/*
================
R_GpuTimerFrameBegin

Start a new frame: clear the slot about to be written and latch whether it is
being timed. Sampling r_speeds once per frame rather than per scope is what
keeps the begin/end marks balanced when the cvar changes mid-frame.

This is driven from the end of the previous frame, not from RC_DRAW_BUFFER:
that command is issued after the scene has already been submitted, so resetting
on it threw away every mark the 3D passes had just recorded.
================
*/
static void R_GpuTimerFrameBegin(void)
{
	// r_speeds is CVAR_CHEAT, so it is forced back to 0 the moment a demo
	// starts playing -- which is exactly when a benchmark needs these numbers.
	// r_gpuTimers is the same switch without that restriction.
	gpuActive = gpuInited && (r_gpuTimers->integer || r_speeds->integer == 8);

	gpuFrames[gpuWriteFrame].numMarks = 0;
}

/*
================
R_GpuTimerMark
================
*/
void R_GpuTimerMark(int id, qboolean isEnd)
{
	gpuTimerFrame_t *frame;
	gpuMark_t       *mark;

	if (!gpuActive)
		return;

	frame = &gpuFrames[gpuWriteFrame];

	// Bracket the frame around whatever the first timed work turns out to be,
	// so the total does not depend on which command the backend happens to
	// execute first.
	if (!frame->numMarks && !(id == GPUTIMER_FRAME && !isEnd))
		R_GpuTimerMark(GPUTIMER_FRAME, qfalse);

	// Added in OPM
	//  A span that starts where the last one of the same timer ended is the
	//  same span, kept open: the realtime lights' shadow faces are drawn back
	//  to back, a hundred views and more, and took a pair of marks each. The
	//  little between them (binding and clearing their tiles) is theirs too.
	if (!isEnd && frame->numMarks > 0)
	{
		const gpuMark_t *last = &frame->marks[frame->numMarks - 1];

		if (last->isEnd && last->id == id)
		{
			frame->numMarks--;
			return;
		}
	}

	if (frame->numMarks >= GPUTIMER_MAX_MARKS)
	{
		gpuOverflowed++;
		return;
	}

	mark = &frame->marks[frame->numMarks++];
	mark->id = id;
	mark->isEnd = isEnd;

	// A driver that defers or reorders work can land the cost of a pass in a
	// later scope than the one that issued it -- a software rasteriser puts
	// nearly all of it after the frame's glFinish. Draining the pipeline at
	// each boundary destroys the overlap the numbers are meant to measure, so
	// it is not the default, but it is the way to check that a suspiciously
	// lopsided breakdown is the driver's doing and not a misplaced scope.
	if (r_gpuTimerSync->integer)
		qglFinish();

	qglQueryCounter(mark->query, GL_TIMESTAMP);
}

/*
================
R_GpuTimerFrameEnd

Advance the ring, then collect the oldest frame in it if the GPU has finished
with it. Nothing here blocks: while the oldest frame's results are not ready,
new frames go untimed, and the reported numbers lag by a few frames.
================
*/
void R_GpuTimerFrameEnd(void)
{
	gpuTimerFrame_t *frame;
	int              readFrame;
	int              i;
	GLuint           available;
	GLuint64         open[GPUTIMER_COUNT];
	qboolean         isOpen[GPUTIMER_COUNT];
	double           totals[GPUTIMER_COUNT];

	if (!gpuInited)
		return;

	if (gpuActive)
		gpuWriteFrame = (gpuWriteFrame + 1) % GPUTIMER_FRAMES;

	// the oldest slot in the ring, i.e. the one written GPUTIMER_FRAMES ago
	readFrame = (gpuWriteFrame + 1) % GPUTIMER_FRAMES;
	frame = &gpuFrames[readFrame];

	// Prepare the slot about to be written before anything below can bail out,
	// so a frame whose results are not ready yet cannot leave stale marks in it
	// for the next frame to append to.
	R_GpuTimerFrameBegin();

	if (!frame->numMarks)
		return;

	available = 0;
	qglGetQueryObjectuiv(frame->marks[frame->numMarks - 1].query, GL_QUERY_RESULT_AVAILABLE, &available);

	// Keep the oldest frame rather than letting the ring overwrite it: skip
	// timing the next frame, so the ring does not advance, and retry this one.
	// Dropping it instead lost every frame whenever the driver ran as many
	// frames behind as the ring is deep, and the report never printed.
	if (!available)
	{
		gpuActive = qfalse;
		return;
	}

	for (i = 0; i < GPUTIMER_COUNT; i++)
	{
		totals[i] = 0.0;
		isOpen[i] = qfalse;
		open[i] = 0;
	}

	for (i = 0; i < frame->numMarks; i++)
	{
		const gpuMark_t *mark = &frame->marks[i];
		GLuint64         stamp = 0;

		qglGetQueryObjectui64v(mark->query, GL_QUERY_RESULT, &stamp);

		if (mark->id < 0 || mark->id >= GPUTIMER_COUNT)
			continue;

		if (!mark->isEnd)
		{
			open[mark->id] = stamp;
			isOpen[mark->id] = qtrue;
		}
		else if (isOpen[mark->id])
		{
			// nanoseconds
			totals[mark->id] += (double)(stamp - open[mark->id]) / 1000000.0;
			isOpen[mark->id] = qfalse;
		}
	}

	// Only frames that actually drew the world are worth averaging; a menu or
	// loading frame carries just the frame and present marks and would drag
	// every pass towards zero.
	if (frame->numMarks > 4)
	{
		for (i = 0; i < GPUTIMER_COUNT; i++)
			gpuAccum[i] += totals[i];

		gpuAccumFrames++;
	}

	for (i = 0; i < GPUTIMER_COUNT; i++)
		tr.gpuTimer.msec[i] = (float)totals[i];

	tr.gpuTimer.valid = qtrue;
	tr.gpuTimer.overflowed = gpuOverflowed;
	tr.gpuTimer.numMarks = frame->numMarks;

	frame->numMarks = 0;
}

/*
================
R_GpuTimerReport

Average of every world frame seen since the last call, provided at least
minFrames have accumulated; otherwise keep collecting. Reporting
periodically rather than per frame is what makes this usable during a timedemo:
a per-frame line floods the log with tens of thousands of entries, most of them
menu frames, and buries the handful that matter.
================
*/
qboolean R_GpuTimerReport(gpuTimerResults_t *out, int *numFrames, int minFrames)
{
	int i;

	if (gpuAccumFrames < minFrames || !gpuAccumFrames)
		return qfalse;

	for (i = 0; i < GPUTIMER_COUNT; i++)
	{
		out->msec[i] = (float)(gpuAccum[i] / gpuAccumFrames);
		gpuAccum[i] = 0.0;
	}

	out->valid = qtrue;
	out->overflowed = gpuOverflowed;
	out->numMarks = tr.gpuTimer.numMarks;

	*numFrames = gpuAccumFrames;
	gpuAccumFrames = 0;

	return qtrue;
}

// ---------------------------------------------------------------------------
// CPU pass timers
//
// The GPU timers above answer "what did the card spend the frame on". They
// cannot answer "was the card the limit at all", because a main thread that is
// blocked in the driver looks identical to one that is idle: both leave the
// GPU numbers unchanged. The wall clock frame time here is the number that
// settles it. If wall matches the GPU frame, the card is the limit; if wall is
// materially larger, the gap is the main thread and the passes below say where
// it went.
//
// No queries, no ring, no readback latency: these are straight
// SDL_GetPerformanceCounter deltas on the calling thread, so unlike the GPU
// side they are exact for the frame they are reported against.
// ---------------------------------------------------------------------------

const char *const cpuTimerNames[CPUTIMER_COUNT] = {
	"frame",
	"backend",
	"drawsurfs",
	"post",
	"sprites",
	"2d",
	"present",
	"swapbuffers",
	"sunshadow",
	"rtshadow",
	"prepass",
	"shadowmask",
	"main3d",
	"tessbuild",
	"tessupload"
};

static double   cpuFrame[CPUTIMER_COUNT];	// the frame being built
static double   cpuOpen[CPUTIMER_COUNT];	// start stamp of an open scope
static double   cpuAccum[CPUTIMER_COUNT];	// running total for the average
static double   cpuLast[CPUTIMER_COUNT];	// the frame just folded, for hitches
static int      cpuAccumFrames;
static double   cpuLastSwap;
static qboolean cpuActive;

// Wall frame times kept individually as well as summed, because an average
// cannot show a hitch: one 200ms frame inside a 300 frame window moves the
// mean by 0.6ms. Percentiles are what distinguish "everything is slow" from
// "almost everything is fine".
#define CPUTIMER_RING 4096
static float    cpuRing[CPUTIMER_RING];
static int      cpuRingCount;

/*
================
R_CpuTimerMark

Scopes of the same id repeat within a frame (one sunshadow pair per cascade,
one tessupload pair per batch) but never nest inside themselves, so a single
open stamp per id is enough.
================
*/
void R_CpuTimerMark(int id, qboolean isEnd)
{
	if (!cpuActive)
		return;

	if (!isEnd)
	{
		cpuOpen[id] = R_MicroSeconds();
		return;
	}

	// A scope that was opened while the timers were off would measure from a
	// stale stamp, which on the frame the cvar is set reads as a spike.
	if (cpuOpen[id] == 0.0)
		return;

	cpuFrame[id] += R_MicroSeconds() - cpuOpen[id];
	cpuOpen[id] = 0.0;
}

/*
================
R_CpuTimerSwap

Wall clock, swap to swap. Measured here rather than around the backend because
everything outside the renderer -- the event loop, the server frame, the game
VM, com_maxfps throttling -- lands in the gap between two swaps, and that gap
is exactly what "the game is only getting N fps" means.
================
*/
void R_CpuTimerSwap(void)
{
	double now;

	if (!cpuActive)
	{
		cpuLastSwap = 0.0;
		return;
	}

	now = R_MicroSeconds();

	if (cpuLastSwap != 0.0)
		cpuFrame[CPUTIMER_FRAME] = now - cpuLastSwap;

	cpuLastSwap = now;
}

/*
================
R_CpuTimerFrameEnd

Called at the top of the next frame, once the previous frame's backend has
fully unwound -- the backend total closes after the swap command, so folding
this in at the swap itself would drop it.

Frames that never ran main3d are menu, console and loading frames. Averaging
them in drags every pass towards zero, which is the same reason the GPU side
counts marks before accumulating.
================
*/
qboolean R_CpuTimerFrameEnd(void)
{
	qboolean worldFrame;
	int      i;

	cpuActive = r_gpuTimers->integer || r_speeds->integer == 8
		|| r_frameHitchMsec->integer;

	if (!cpuActive)
	{
		Com_Memset(cpuFrame, 0, sizeof(cpuFrame));
		Com_Memset(cpuOpen, 0, sizeof(cpuOpen));
		Com_Memset(cpuAccum, 0, sizeof(cpuAccum));
		cpuAccumFrames = 0;
		cpuRingCount = 0;
		return qfalse;
	}

	worldFrame = (cpuFrame[CPUTIMER_MAIN3D] > 0.0);

	if (worldFrame)
	{
		for (i = 0; i < CPUTIMER_COUNT; i++)
		{
			cpuAccum[i] += cpuFrame[i];
			cpuLast[i] = cpuFrame[i] / 1000.0;
		}

		if (cpuRingCount < CPUTIMER_RING)
			cpuRing[cpuRingCount++] = (float)(cpuFrame[CPUTIMER_FRAME] / 1000.0);

		cpuAccumFrames++;
	}

	Com_Memset(cpuFrame, 0, sizeof(cpuFrame));

	return worldFrame;
}

/*
================
R_CpuTimerLastFrame
================
*/
void R_CpuTimerLastFrame(double *out)
{
	int i;

	for (i = 0; i < CPUTIMER_COUNT; i++)
		out[i] = cpuLast[i];
}

static int R_CpuTimerSortFrames(const void *a, const void *b)
{
	float fa = *(const float *)a;
	float fb = *(const float *)b;

	return (fa > fb) - (fa < fb);
}

/*
================
R_CpuTimerPercentiles

Sorted once per report rather than per frame, so the cost lands on the report
and not on what is being measured. The ring is capped: once it is full the
window stops sampling rather than evicting, which keeps the percentiles honest
about how many frames they describe instead of silently sliding.
================
*/
qboolean R_CpuTimerPercentiles(double *p50, double *p95, double *p99, double *max, int *numFrames)
{
	static float sorted[CPUTIMER_RING];
	int n = cpuRingCount;

	if (n < 1)
		return qfalse;

	Com_Memcpy(sorted, cpuRing, n * sizeof(sorted[0]));
	qsort(sorted, n, sizeof(sorted[0]), R_CpuTimerSortFrames);

	*p50 = sorted[(int)(n * 0.50)];
	*p95 = sorted[(int)(n * 0.95)];
	*p99 = sorted[(int)(n * 0.99)];
	*max = sorted[n - 1];
	*numFrames = n;

	cpuRingCount = 0;

	return qtrue;
}

/*
================
Surface profile (RB_SurfProfBegin)

Which kind of surface a pass's CPU time went to. The pass timers say the sun
cascades cost so much; this says whether that was the characters, the static
models, the terrain or setting up and drawing the batches. A surface function
is timed with whatever it does, and RB_EndSurface apart from whichever kind
was being added when the batch filled.
================
*/
#define SURFPROF_PASSES 4

static const char *const surfProfPassNames[SURFPROF_PASSES] = { "sunshadow", "rtshadow", "prepass", "main3d" };
static const char *const surfProfKindNames[SURFPROF_COUNT] = { "list", "world", "terrain", "static", "skel", "other", "draw" };

static int    spPass = -1;
static int    spKind;
static double spStamp;
static double spTime[SURFPROF_PASSES][SURFPROF_COUNT];
static int    spCount[SURFPROF_PASSES][SURFPROF_COUNT];

void RB_SurfProfBegin(void)
{
	if (!cpuActive)
	{
		spPass = -1;
		return;
	}

	if (backEnd.depthFill)
	{
		if (backEnd.viewParms.flags & (VPF_RTSTATIC | VPF_RTDYNAMIC | VPF_RTBAKED))
			spPass = 1;
		else if (backEnd.viewParms.flags & VPF_DEPTHSHADOW)
			spPass = 0;
		else
			spPass = 2;
	}
	else
	{
		spPass = 3;
	}

	spKind  = SURFPROF_LIST;
	spStamp = R_MicroSeconds();
}

int RB_SurfProfSwitch(int kind)
{
	const int prev = spKind;
	double    now;

	if (spPass < 0)
		return prev;

	now = R_MicroSeconds();
	spTime[spPass][spKind] += now - spStamp;
	spStamp = now;
	spKind  = kind;
	return prev;
}

void RB_SurfProfSurface(surfaceType_t type)
{
	int kind;

	if (spPass < 0)
		return;

	switch (type)
	{
	case SF_FACE:
	case SF_GRID:
	case SF_TRIANGLES:
		kind = SURFPROF_WORLD;
		break;
	case SF_TERRAIN_PATCH:
		kind = SURFPROF_TERRAIN;
		break;
	case SF_TIKI_STATIC:
		kind = SURFPROF_STATIC;
		break;
	case SF_TIKI_SKEL:
		kind = SURFPROF_SKEL;
		break;
	default:
		kind = SURFPROF_OTHER;
		break;
	}

	spCount[spPass][kind]++;
	RB_SurfProfSwitch(kind);
}

void RB_SurfProfEnd(void)
{
	if (spPass < 0)
		return;

	RB_SurfProfSwitch(SURFPROF_LIST);
	spPass = -1;
}

// ms a frame for each kind, and how many surfaces of it, for each pass that ran
void R_SurfProfReport(int numFrames)
{
	char line[512];
	int  p, k;

	for (p = 0; p < SURFPROF_PASSES; p++)
	{
		double total = 0.0;

		for (k = 0; k < SURFPROF_COUNT; k++)
			total += spTime[p][k];

		if (total > 0.0 && numFrames > 0)
		{
			Com_sprintf(line, sizeof(line), "cpu   %s by kind:", surfProfPassNames[p]);
			for (k = 0; k < SURFPROF_COUNT; k++)
			{
				if (k == SURFPROF_LIST || k == SURFPROF_DRAW)
					Q_strcat(line, sizeof(line), va(" %s %.2f", surfProfKindNames[k], spTime[p][k] / numFrames / 1000.0));
				else
					Q_strcat(line, sizeof(line), va(" %s %.2f (%d)", surfProfKindNames[k],
						spTime[p][k] / numFrames / 1000.0, spCount[p][k] / numFrames));
			}
			ri.Printf(PRINT_ALL, "%s\n", line);
		}
	}

	Com_Memset(spTime, 0, sizeof(spTime));
	Com_Memset(spCount, 0, sizeof(spCount));
}

/*
================
R_CpuTimerReport

Average in milliseconds over every world frame since the last call. Mirrors
R_GpuTimerReport so the two can be printed against each other.
================
*/
qboolean R_CpuTimerReport(double *out, int *numFrames, int minFrames)
{
	int i;

	if (cpuAccumFrames < minFrames || !cpuAccumFrames)
		return qfalse;

	for (i = 0; i < CPUTIMER_COUNT; i++)
	{
		out[i] = cpuAccum[i] / cpuAccumFrames / 1000.0;
		cpuAccum[i] = 0.0;
	}

	*numFrames = cpuAccumFrames;
	cpuAccumFrames = 0;

	return qtrue;
}
