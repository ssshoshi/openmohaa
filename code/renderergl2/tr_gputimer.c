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
with it. Nothing here blocks: a frame whose results are not ready yet is simply
left for the next attempt, and the reported numbers lag by a few frames.
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

	if (!available)
		return;

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
	"prepass",
	"shadowmask",
	"main3d",
	"tessbuild",
	"tessupload"
};

static double   cpuFrame[CPUTIMER_COUNT];	// the frame being built
static double   cpuOpen[CPUTIMER_COUNT];	// start stamp of an open scope
static double   cpuAccum[CPUTIMER_COUNT];	// running total for the average
static int      cpuAccumFrames;
static double   cpuLastSwap;
static qboolean cpuActive;

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
void R_CpuTimerFrameEnd(void)
{
	int i;

	cpuActive = r_gpuTimers->integer || r_speeds->integer == 8;

	if (!cpuActive)
	{
		Com_Memset(cpuFrame, 0, sizeof(cpuFrame));
		Com_Memset(cpuOpen, 0, sizeof(cpuOpen));
		Com_Memset(cpuAccum, 0, sizeof(cpuAccum));
		cpuAccumFrames = 0;
		return;
	}

	if (cpuFrame[CPUTIMER_MAIN3D] > 0.0)
	{
		for (i = 0; i < CPUTIMER_COUNT; i++)
			cpuAccum[i] += cpuFrame[i];

		cpuAccumFrames++;
	}

	Com_Memset(cpuFrame, 0, sizeof(cpuFrame));
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
