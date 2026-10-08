/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "tr_local.h"

static backEndCounters_t pc_save;

/*
=====================
R_SavePerformanceCounters
=====================
*/
void R_SavePerformanceCounters(void) {
    memcpy(&pc_save, &backEnd.pc, sizeof(pc_save));
}

/*
=====================
R_PerformanceCounters
=====================
*/
/*
================
R_ReportGpuTimers

Print the average per-pass GPU cost once every `interval` world frames. The
"other" column is frame time the named scopes did not account for, so a
breakdown that is mostly "other" means the passes are mis-attributed -- which is
what a driver that defers work will do, and what r_gpuTimerSync 1 checks for.
================
*/
static void R_ReportGpuTimers( int interval )
{
	gpuTimerResults_t avg;
	int   numFrames = 0;
	int   i;
	char  line[256];
	float accounted = 0.0f;

	if (!glRefConfig.timerQuery)
	{
		ri.Printf( PRINT_ALL, "gpu: GL_ARB_timer_query not available\n" );
		return;
	}

	if (!R_GpuTimerReport( &avg, &numFrames, interval ))
		return;

	Com_sprintf( line, sizeof(line), "gpu %6.2fms =", avg.msec[GPUTIMER_FRAME] );

	for (i = 0; i < GPUTIMER_COUNT; i++)
	{
		if (i == GPUTIMER_FRAME)
			continue;

		// sun0..sun3 are nested inside sunshadow, so counting both would
		// double up and drive "other" negative.
		if (i < GPUTIMER_SUN0 || i > GPUTIMER_SUN3)
			accounted += avg.msec[i];
		Q_strcat( line, sizeof(line), va(" %s %.2f", gpuTimerNames[i], avg.msec[i]) );
	}

	ri.Printf( PRINT_ALL, "%s other %.2f  (avg of %i frames)\n",
		line, avg.msec[GPUTIMER_FRAME] - accounted, numFrames );

	// How many draw surfaces each cascade submitted last frame. If a cascade
	// that should be cached still shows a count every frame, it is being
	// re-rendered regardless of the cache.
	// Draw calls and buffer uploads are counts, not times, so unlike the ms
	// figures they are immune to the card's clock drifting between runs.
	ri.Printf( PRINT_ALL, "gpu submission: %i draws %i uploads %i streamed (%i waits)\n",
		backEnd.pc.c_drawCalls, backEnd.pc.c_bufferUploads,
		backEnd.pc.c_streamBatches, backEnd.pc.c_streamWaits );

	ri.Printf( PRINT_ALL, "gpu cascade surfs: %i %i %i %i\n",
		backEnd.pc.c_sunCascadeSurfs[0], backEnd.pc.c_sunCascadeSurfs[1],
		backEnd.pc.c_sunCascadeSurfs[2], backEnd.pc.c_sunCascadeSurfs[3] );

	if (avg.overflowed)
	{
		ri.Printf( PRINT_ALL, "gpu: %i marks dropped, raise GPUTIMER_MAX_MARKS\n",
			avg.overflowed );
	}
}

/*
================
R_ReportCpuTimers

The companion to R_ReportGpuTimers, and the one that says whether the GPU
numbers matter. "wall" is swap to swap; "backend" is the main thread inside
RB_ExecuteRenderCommands. Subtract them and what is left is everything else the
main thread did -- renderer frontend, event loop, server frame, game VM -- plus
any time Com_Frame slept to hold com_maxfps. That last one matters when reading
this: a frame sitting on its fps cap shows a large "other" and is not a problem,
so uncap before concluding anything from it.

Against the GPU report: wall at or near the GPU frame time means the card is the
limit. Wall well above it means the main thread is, and the pass breakdown says
where. present is the interesting column either way -- with r_swapInterval 0 and
r_finish 0 nothing explicitly syncs, so a driver throttling a full command queue
shows up as CPU time attributed to whichever call it chose to block in.
================
*/
static void R_ReportCpuTimers( int interval )
{
	double cpu[CPUTIMER_COUNT];
	int    numFrames = 0;
	int    i;
	char   line[256];
	double accounted = 0.0;

	if (!R_CpuTimerReport( cpu, &numFrames, interval ))
		return;

	ri.Printf( PRINT_ALL, "cpu %6.2fms wall = backend %.2f other %.2f  (avg of %i frames)\n",
		cpu[CPUTIMER_FRAME], cpu[CPUTIMER_BACKEND],
		cpu[CPUTIMER_FRAME] - cpu[CPUTIMER_BACKEND], numFrames );

	// The mean above cannot show a hitch and will not be moved much by one:
	// a single 200ms frame inside a 300 frame window shifts it by 0.6ms. The
	// spread is what says whether a frame rate is a ceiling or a stutter.
	{
		double p50, p95, p99, worst;
		int    n;

		if (R_CpuTimerPercentiles( &p50, &p95, &p99, &worst, &n ))
		{
			ri.Printf( PRINT_ALL,
				"cpu frames: p50 %.2f p95 %.2f p99 %.2f max %.2f ms over %i\n",
				p50, p95, p99, worst, n );
		}
	}

	Com_sprintf( line, sizeof(line), "cpu backend =" );

	// One entry per render command, so this line sums to backend. What it
	// misses shows as unattributed, which is the honest place for it.
	for (i = CPUTIMER_DRAWSURFS; i <= CPUTIMER_SWAPBUFFERS; i++)
	{
		accounted += cpu[i];
		Q_strcat( line, sizeof(line), va(" %s %.2f", cpuTimerNames[i], cpu[i]) );
	}

	ri.Printf( PRINT_ALL, "%s unattributed %.2f\n",
		line, cpu[CPUTIMER_BACKEND] - accounted );

	Com_sprintf( line, sizeof(line), "cpu   in drawsurfs:" );
	accounted = 0.0;

	for (i = CPUTIMER_SUNSHADOW; i <= CPUTIMER_MAIN3D; i++)
	{
		accounted += cpu[i];
		Q_strcat( line, sizeof(line), va(" %s %.2f", cpuTimerNames[i], cpu[i]) );
	}

	// Setup either side of the passes: FBO binds, clears and blits. A driver
	// that defers rasterisation to its next flush lands the deferred work
	// here rather than in the pass that issued it, which is what a software
	// rasteriser does and what r_gpuTimerSync 1 exists to check for.
	Q_strcat( line, sizeof(line),
		va(" other %.2f", cpu[CPUTIMER_DRAWSURFS] - accounted) );

	// tessbuild and tessupload are nested deeper again -- inside whichever of
	// the passes above is drawing -- so they are a share of those, not an
	// addition to them. r_vaoCache 0 is what routes static world geometry
	// through both, once per batch, per pass, every frame.
	ri.Printf( PRINT_ALL, "%s | per batch: tessbuild %.2f tessupload %.2f\n",
		line, cpu[CPUTIMER_TESSBUILD], cpu[CPUTIMER_TESSUPLOAD] );
}

/*
================
R_ReportCpuHitch

One line per frame that blew past r_frameHitchMsec, printed as it happens
rather than averaged away.

CPU only, deliberately. GPU query results are read back GPUTIMER_FRAMES late
and never waited on, so by the time a hitch is detected the card's numbers for
that frame do not exist yet -- printing whatever the GPU ring currently holds
would attribute some other frame's work to this one. The CPU buckets are exact
for the frame they describe.
================
*/
static void R_ReportCpuHitch( void )
{
	double cpu[CPUTIMER_COUNT];

	R_CpuTimerLastFrame( cpu );

	if (cpu[CPUTIMER_FRAME] < (double)r_frameHitchMsec->integer)
		return;

	ri.Printf( PRINT_ALL,
		"cpu HITCH %.2fms = backend %.2f (drawsurfs %.2f: sunshadow %.2f rtshadow %.2f prepass %.2f"
		" shadowmask %.2f main3d %.2f; tessbuild %.2f tessupload %.2f)"
		" post %.2f sprites %.2f 2d %.2f present %.2f swap %.2f | other %.2f\n",
		cpu[CPUTIMER_FRAME], cpu[CPUTIMER_BACKEND], cpu[CPUTIMER_DRAWSURFS],
		cpu[CPUTIMER_SUNSHADOW], cpu[CPUTIMER_RTSHADOW], cpu[CPUTIMER_DEPTHPREPASS],
		cpu[CPUTIMER_SHADOWMASK], cpu[CPUTIMER_MAIN3D],
		cpu[CPUTIMER_TESSBUILD], cpu[CPUTIMER_TESSUPLOAD],
		cpu[CPUTIMER_POSTPROCESS], cpu[CPUTIMER_SPRITES], cpu[CPUTIMER_2D],
		cpu[CPUTIMER_PRESENT], cpu[CPUTIMER_SWAPBUFFERS],
		cpu[CPUTIMER_FRAME] - cpu[CPUTIMER_BACKEND] );
}

void R_PerformanceCounters( void ) {
	// Every frame, cvar or not: this latches whether the timers are live and
	// resets the frame being built, so a frame rendered while they were off
	// cannot leak into the first window after they are switched on.
	if (R_CpuTimerFrameEnd() && r_frameHitchMsec->integer)
		R_ReportCpuHitch();

	if (r_fps->integer) {
		ri.SetPerformanceCounters(
			backEnd.pc.c_totalIndexes / 3,
			backEnd.pc.c_vertexes,
			R_SumOfUsedImages(),
			pc_save.c_totalIndexes / 3,
			pc_save.c_vertexes,
			backEnd.pc.c_characterlights
			);
	}

	if (r_gpuTimers->integer)
	{
		// Frontend repeat counts are pure CPU-side counters, so they report on
		// their own cadence rather than waiting on GPU query results that may
		// never arrive (a software rasteriser renders too few world frames for
		// the timer ring to complete a readback).
		//
		// Averaged over the window rather than sampled from one frame: the
		// cascades read tr.refdef.render_terrain, which RE_RenderScene does not
		// assign until after they have run, so the first world frame of a map
		// sees a stale zero and is not representative.
		static int    feFrames = 0;
		static float  feWalks, fePrep, feTess, feStatic, feSplits, feMerges;
		static double feShadow, feNode, feScan, fePrepT, feTerrT, feStatT, feGrassT, feMark, feEnt;
		static float  feEntN;

		if (tr.pc.c_worldWalks)
		{
			feWalks  += tr.pc.c_worldWalks;
			fePrep   += tr.pc.c_terrainPrepares;
			feTess   += tr.pc.c_terrainTessellates;
			feStatic += tr.pc.c_staticModelWalks;
			feSplits += tr.pc.c_terrainSplits;
			feMerges += tr.pc.c_terrainMerges;
			feShadow += tr.pc.t_shadowFrontend;
			feNode   += tr.pc.t_worldNode;
			feMark   += tr.pc.t_markLeaves;
			feEnt    += tr.pc.t_entitySurfaces;
			feEntN   += tr.pc.c_entitySubmissions;
			feScan   += tr.pc.t_surfaceScan;
			fePrepT  += tr.pc.t_terrainPrepare;
			feTerrT  += tr.pc.t_terrainSurfaces;
			feStatT  += tr.pc.t_staticModels;
			feGrassT += tr.pc.t_groundCover;
			feFrames++;
		}

		if (feFrames >= r_gpuTimers->integer)
		{
			ri.Printf( PRINT_ALL,
				"frontend per frame: %.1f worldwalks %.1f terrprep %.1f terrtess %.1f statmodels"
				" (%.0f splits %.0f merges) over %i frames\n",
				feWalks / feFrames, fePrep / feFrames, feTess / feFrames,
				feStatic / feFrames, feSplits / feFrames, feMerges / feFrames, feFrames );

			ri.Printf( PRINT_ALL,
				"frontend ms/frame: shadowpasses %.2f | markleaves %.2f worldnode %.2f surfscan %.2f"
				" terrprep %.2f terrsurf %.2f statmodels %.2f groundcover %.2f entities %.2f (%.0f/frame)\n",
				feShadow / feFrames / 1000.0, feMark / feFrames / 1000.0,
				feNode / feFrames / 1000.0,
				feScan / feFrames / 1000.0, fePrepT / feFrames / 1000.0,
				feTerrT / feFrames / 1000.0, feStatT / feFrames / 1000.0,
				feGrassT / feFrames / 1000.0, feEnt / feFrames / 1000.0, feEntN / feFrames );

			feFrames = 0;
			feWalks = fePrep = feTess = feStatic = feSplits = feMerges = 0.0f;
			feShadow = feNode = feScan = fePrepT = feTerrT = feStatT = feGrassT = feMark = feEnt = 0.0;
			feEntN = 0.0f;
		}

		R_ReportGpuTimers( r_gpuTimers->integer );
		R_ReportCpuTimers( r_gpuTimers->integer );
	}

	if ( !r_speeds->integer ) {
		// clear the counters even if we aren't printing
		Com_Memset( &tr.pc, 0, sizeof( tr.pc ) );
		Com_Memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
		return;
	}

	if (r_speeds->integer == 1) {
		ri.Printf (PRINT_ALL, "%i/%i/%i shaders/batches/surfs %i leafs %i verts %i/%i tris %.2f mtex %.2f dc\n",
			backEnd.pc.c_shaders, backEnd.pc.c_surfBatches, backEnd.pc.c_surfaces, tr.pc.c_leafs, backEnd.pc.c_vertexes, 
			backEnd.pc.c_indexes/3, backEnd.pc.c_totalIndexes/3, 
			R_SumOfUsedImages()/(1000000.0f), backEnd.pc.c_overDraw / (float)(glConfig.vidWidth * glConfig.vidHeight) ); 
	} else if (r_speeds->integer == 2) {
		ri.Printf (PRINT_ALL, "(patch) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_patch_in, tr.pc.c_sphere_cull_patch_clip, tr.pc.c_sphere_cull_patch_out, 
			tr.pc.c_box_cull_patch_in, tr.pc.c_box_cull_patch_clip, tr.pc.c_box_cull_patch_out );
		ri.Printf (PRINT_ALL, "(md3) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
			tr.pc.c_sphere_cull_md3_in, tr.pc.c_sphere_cull_md3_clip, tr.pc.c_sphere_cull_md3_out, 
			tr.pc.c_box_cull_md3_in, tr.pc.c_box_cull_md3_clip, tr.pc.c_box_cull_md3_out );
	} else if (r_speeds->integer == 3) {
		ri.Printf (PRINT_ALL, "viewcluster: %i\n", tr.viewCluster );
	} else if (r_speeds->integer == 4) {
		if ( backEnd.pc.c_dlightVertexes ) {
			ri.Printf (PRINT_ALL, "dlight srf:%i  culled:%i  verts:%i  tris:%i\n", 
				tr.pc.c_dlightSurfaces, tr.pc.c_dlightSurfacesCulled,
				backEnd.pc.c_dlightVertexes, backEnd.pc.c_dlightIndexes / 3 );
		}
	} 
	else if (r_speeds->integer == 5 )
	{
		ri.Printf( PRINT_ALL, "zFar: %.0f\n", tr.viewParms.zFar );
	}
	else if (r_speeds->integer == 6 )
	{
		ri.Printf( PRINT_ALL, "flare adds:%i tests:%i renders:%i\n", 
			backEnd.pc.c_flareAdds, backEnd.pc.c_flareTests, backEnd.pc.c_flareRenders );
	}
	else if (r_speeds->integer == 7 )
	{
		ri.Printf( PRINT_ALL, "VAO draws: static %i dynamic %i\n",
			backEnd.pc.c_staticVaoDraws, backEnd.pc.c_dynamicVaoDraws);
		ri.Printf( PRINT_ALL, "GLSL binds: %i  draws: gen %i light %i fog %i dlight %i\n",
			backEnd.pc.c_glslShaderBinds, backEnd.pc.c_genericDraws, backEnd.pc.c_lightallDraws, backEnd.pc.c_fogDraws, backEnd.pc.c_dlightDraws);
	}
	else if (r_speeds->integer == 8 )
	{
		R_ReportGpuTimers( 1 );
	}

	Com_Memset( &tr.pc, 0, sizeof( tr.pc ) );
	Com_Memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
}


/*
====================
R_IssueRenderCommands
====================
*/
void R_IssueRenderCommands( qboolean runPerformanceCounters ) {
	renderCommandList_t	*cmdList;

	cmdList = &backEndData->commands;
	assert(cmdList);
	// add an end-of-list command
	*(int *)(cmdList->cmds + cmdList->used) = RC_END_OF_LIST;

	// clear it out, in case this is a sync and not a buffer flip
	cmdList->used = 0;

	if ( runPerformanceCounters ) {
		R_PerformanceCounters();
	}

	// actually start the commands going
	if ( !r_skipBackEnd->integer ) {
		// let it start on the new batch
		RB_ExecuteRenderCommands( cmdList->cmds );
	}
}


/*
====================
R_IssuePendingRenderCommands

Issue any pending commands and wait for them to complete.
====================
*/
void R_IssuePendingRenderCommands( void ) {
	if ( !tr.registered ) {
		return;
	}
	R_IssueRenderCommands( qfalse );
}

/*
============
R_GetCommandBufferReserved

make sure there is enough command space
============
*/
void *R_GetCommandBufferReserved( int bytes, int reservedBytes ) {
	renderCommandList_t	*cmdList;

	cmdList = &backEndData->commands;
	bytes = PAD(bytes, sizeof(void *));

	// always leave room for the end of list command
	if ( cmdList->used + bytes + sizeof( int ) + reservedBytes > MAX_RENDER_COMMANDS ) {
		if ( bytes > MAX_RENDER_COMMANDS - sizeof( int ) ) {
			ri.Error( ERR_FATAL, "R_GetCommandBuffer: bad size %i", bytes );
		}
		// if we run out of room, just start dropping commands
		ri.Printf( PRINT_WARNING, "Failed to allocate render command of size %d\n", bytes );
		return NULL;
	}

	cmdList->used += bytes;

	return cmdList->cmds + cmdList->used - bytes;
}

/*
=============
R_GetCommandBuffer

returns NULL if there is not enough space for important commands
=============
*/
void *R_GetCommandBuffer( int bytes ) {
	return R_GetCommandBufferReserved( bytes, PAD( sizeof( swapBuffersCommand_t ), sizeof(void *) ) );
}


/*
=============
R_AddDrawSurfCmd

=============
*/
void	R_AddDrawSurfCmd( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	drawSurfsCommand_t	*cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_SURFS;

	cmd->drawSurfs = drawSurfs;
	cmd->numDrawSurfs = numDrawSurfs;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;
}


/*
=============
R_AddCapShadowmapCmd

=============
*/
void	R_AddCapShadowmapCmd( int map, int cubeSide ) {
	capShadowmapCommand_t	*cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_CAPSHADOWMAP;

	cmd->map = map;
	cmd->cubeSide = cubeSide;
}


/*
=============
R_AddPostProcessCmd

=============
*/
void	R_AddPostProcessCmd( void ) {
	postProcessCommand_t	*cmd;

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_POSTPROCESS;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;
}

/*
=============
RE_SetColor

Passing NULL will set the color to white
=============
*/
void	RE_SetColor( const float *rgba ) {
	setColorCommand_t	*cmd;

  if ( !tr.registered ) {
    return;
  }
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SET_COLOR;
	if ( !rgba ) {
		static float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		rgba = white;
	}

	cmd->color[0] = rgba[0];
	cmd->color[1] = rgba[1];
	cmd->color[2] = rgba[2];
	cmd->color[3] = rgba[3];
}


/*
=============
RE_StretchPic
=============
*/
void RE_StretchPic ( float x, float y, float w, float h, 
					  float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	stretchPicCommand_t	*cmd;

  if (!tr.registered) {
    return;
  }
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
}

#define MODE_RED_CYAN	1
#define MODE_RED_BLUE	2
#define MODE_RED_GREEN	3
#define MODE_GREEN_MAGENTA 4
#define MODE_MAX	MODE_GREEN_MAGENTA

void R_SetColorMode(GLboolean *rgba, stereoFrame_t stereoFrame, int colormode)
{
	rgba[0] = rgba[1] = rgba[2] = rgba[3] = GL_TRUE;
	
	if(colormode > MODE_MAX)
	{
		if(stereoFrame == STEREO_LEFT)
			stereoFrame = STEREO_RIGHT;
		else if(stereoFrame == STEREO_RIGHT)
			stereoFrame = STEREO_LEFT;
		
		colormode -= MODE_MAX;
	}
	
	if(colormode == MODE_GREEN_MAGENTA)
	{
		if(stereoFrame == STEREO_LEFT)
			rgba[0] = rgba[2] = GL_FALSE;
		else if(stereoFrame == STEREO_RIGHT)
			rgba[1] = GL_FALSE;
	}
	else
	{
		if(stereoFrame == STEREO_LEFT)
			rgba[1] = rgba[2] = GL_FALSE;
		else if(stereoFrame == STEREO_RIGHT)
		{
			rgba[0] = GL_FALSE;
		
			if(colormode == MODE_RED_BLUE)
				rgba[1] = GL_FALSE;
			else if(colormode == MODE_RED_GREEN)
				rgba[2] = GL_FALSE;
		}
	}
}


/*
====================
RE_BeginFrame

If running in stereo, RE_BeginFrame will be called twice
for each RE_EndFrame
====================
*/
void RE_BeginFrame( stereoFrame_t stereoFrame ) {
	drawBufferCommand_t	*cmd = NULL;
	colorMaskCommand_t *colcmd = NULL;

	if ( !tr.registered ) {
		return;
	}
	glState.finishCalled = qfalse;

	tr.frameCount++;
	tr.frameSceneNum = 0;

	//
	// do overdraw measurement
	//
	if ( r_measureOverdraw->integer )
	{
		if ( qglesMajorVersion >= 1 && !glRefConfig.readStencil )
		{
			ri.Printf( PRINT_WARNING, "OpenGL ES needs GL_NV_read_stencil to read stencil bits to measure overdraw\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else if ( glConfig.stencilBits < 4 )
		{
			ri.Printf( PRINT_ALL, "Warning: not enough stencil bits to measure overdraw: %d\n", glConfig.stencilBits );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else if ( r_shadows->integer == 2 )
		{
			ri.Printf( PRINT_ALL, "Warning: stencil shadows and overdraw measurement are mutually exclusive\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		}
		else
		{
			R_IssuePendingRenderCommands();
			qglEnable( GL_STENCIL_TEST );
			qglStencilMask( ~0U );
			qglClearStencil( 0U );
			qglStencilFunc( GL_ALWAYS, 0U, ~0U );
			qglStencilOp( GL_KEEP, GL_INCR, GL_INCR );
		}
		r_measureOverdraw->modified = qfalse;
	}
	else
	{
		// this is only reached if it was on and is now off
		if ( r_measureOverdraw->modified ) {
			R_IssuePendingRenderCommands();
			qglDisable( GL_STENCIL_TEST );
		}
		r_measureOverdraw->modified = qfalse;
	}

	//
	// texturemode stuff
	//
	if ( r_textureMode->modified ) {
		R_IssuePendingRenderCommands();
		GL_TextureMode( r_textureMode->string );
		r_textureMode->modified = qfalse;
	}

	//
	// gamma stuff
	//
	if ( r_gamma->modified ) {
		r_gamma->modified = qfalse;

		R_IssuePendingRenderCommands();
		R_SetColorMappings();
	}

	// check for errors
	if ( !r_ignoreGLErrors->integer )
	{
		int	err;

		R_IssuePendingRenderCommands();
		if ((err = qglGetError()) != GL_NO_ERROR)
			ri.Error(ERR_FATAL, "RE_BeginFrame() - glGetError() failed (0x%x)!", err);
	}

	if (glConfig.stereoEnabled) {
		if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
			return;
			
		cmd->commandId = RC_DRAW_BUFFER;
		
		if ( stereoFrame == STEREO_LEFT ) {
			cmd->buffer = (int)GL_BACK_LEFT;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			cmd->buffer = (int)GL_BACK_RIGHT;
		} else {
			ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
		}
	}
	else
	{
		if (qglesMajorVersion >= 1 && r_anaglyphMode->integer)
		{
			ri.Printf( PRINT_WARNING, "OpenGL ES does not support drawing to separate buffer for anaglyph mode\n" );
			ri.Cvar_Set( "r_anaglyphMode", "0" );
			r_anaglyphMode->modified = qfalse;
		}

		if(r_anaglyphMode->integer)
		{
			if(r_anaglyphMode->modified)
			{
				// clear both, front and backbuffer.
				qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				backEnd.colorMask[0] = GL_FALSE;
				backEnd.colorMask[1] = GL_FALSE;
				backEnd.colorMask[2] = GL_FALSE;
				backEnd.colorMask[3] = GL_FALSE;
								
				if (glRefConfig.framebufferObject)
				{
					// clear all framebuffers
					if (tr.msaaResolveFbo)
					{
						FBO_Bind(tr.msaaResolveFbo);
						qglClear(GL_COLOR_BUFFER_BIT);
					}

					if (tr.renderFbo)
					{
						FBO_Bind(tr.renderFbo);
						qglClear(GL_COLOR_BUFFER_BIT);
					}

					FBO_Bind(NULL);
				}

				qglDrawBuffer(GL_FRONT);
				qglClear(GL_COLOR_BUFFER_BIT);
				qglDrawBuffer(GL_BACK);
				qglClear(GL_COLOR_BUFFER_BIT);

				r_anaglyphMode->modified = qfalse;
			}
			
			if(stereoFrame == STEREO_LEFT)
			{
				if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
					return;
				
				if( !(colcmd = R_GetCommandBuffer(sizeof(*colcmd))) )
					return;
			}
			else if(stereoFrame == STEREO_RIGHT)
			{
				clearDepthCommand_t *cldcmd;
				
				if( !(cldcmd = R_GetCommandBuffer(sizeof(*cldcmd))) )
					return;

				cldcmd->commandId = RC_CLEARDEPTH;

				if( !(colcmd = R_GetCommandBuffer(sizeof(*colcmd))) )
					return;
			}
			else
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );

			R_SetColorMode(colcmd->rgba, stereoFrame, r_anaglyphMode->integer);
			colcmd->commandId = RC_COLORMASK;
		}
		else
		{
			if(stereoFrame != STEREO_CENTER)
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is disabled, but stereoFrame was %i", stereoFrame );

			if( !(cmd = R_GetCommandBuffer(sizeof(*cmd))) )
				return;
		}

		if(cmd)
		{
			cmd->commandId = RC_DRAW_BUFFER;

			if(r_anaglyphMode->modified)
			{
				qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				backEnd.colorMask[0] = 0;
				backEnd.colorMask[1] = 0;
				backEnd.colorMask[2] = 0;
				backEnd.colorMask[3] = 0;
				r_anaglyphMode->modified = qfalse;
			}

			if (!Q_stricmp(r_drawBuffer->string, "GL_FRONT"))
				cmd->buffer = (int)GL_FRONT;
			else
				cmd->buffer = (int)GL_BACK;
		}
	}
	
    tr.refdef.stereoFrame = stereoFrame;

	//
	// OPENMOHAA-specific stuff
	//

    g_nStaticSurfaces = 0;
}


/*
=============
RE_EndFrame

Returns the number of msec spent in the back end
=============
*/
void RE_EndFrame( int *frontEndMsec, int *backEndMsec ) {
	swapBuffersCommand_t	*cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = R_GetCommandBufferReserved( sizeof( *cmd ), 0 );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SWAP_BUFFERS;

	R_IssueRenderCommands( qtrue );

	R_InitNextFrame();

	if ( frontEndMsec ) {
		*frontEndMsec = tr.frontEndMsec;
	}
	tr.frontEndMsec = 0;
	if ( backEndMsec ) {
		*backEndMsec = backEnd.pc.msec;
	}
	backEnd.pc.msec = 0;
}

/*
=============
RE_TakeVideoFrame
=============
*/
void RE_TakeVideoFrame( int width, int height,
		byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg )
{
	videoFrameCommand_t	*cmd;

	if( !tr.registered ) {
		return;
	}

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if( !cmd ) {
		return;
	}

	cmd->commandId = RC_VIDEOFRAME;

	cmd->width = width;
	cmd->height = height;
	cmd->captureBuffer = captureBuffer;
	cmd->encodeBuffer = encodeBuffer;
	cmd->motionJpeg = motionJpeg;
}

//
// OPENMOHAA-specific stuff
//

/*
=============
R_AddSpriteSurfCmd

=============
*/
void	R_AddSpriteSurfCmd(drawSurf_t* drawSurfs, int numDrawSurfs) {
    drawSurfsCommand_t* cmd;

    cmd = R_GetCommandBuffer(sizeof(*cmd));
    if (!cmd) {
        return;
    }
    cmd->commandId = RC_SPRITE_SURFS;

    cmd->drawSurfs = drawSurfs;
    cmd->numDrawSurfs = numDrawSurfs;

    cmd->refdef = tr.refdef;
    cmd->viewParms = tr.viewParms;
}
