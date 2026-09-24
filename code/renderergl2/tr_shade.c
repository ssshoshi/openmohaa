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
// tr_shade.c

#include "tr_local.h" 

/*

  THIS ENTIRE FILE IS BACK END

  This file deals with applying shaders to surface data in the tess struct.
*/


/*
==================
R_DrawElements

==================
*/

void R_DrawElements( int numIndexes, int firstIndex )
{
	backEnd.pc.c_drawCalls++;

	if (tess.useCacheVao)
	{
		VaoCache_DrawElements(numIndexes, firstIndex);
	}
	else
	{
		qglDrawElements(GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, BUFFER_OFFSET(firstIndex * sizeof(glIndex_t)));
	}
}


/*
=============================================================

SURFACE SHADERS

=============================================================
*/

shaderCommands_t	tess;


/*
=================
R_BindAnimatedImageToTMU

=================
*/
static void R_BindAnimatedImageToTMU( textureBundle_t *bundle, int tmu ) {
	int64_t index;

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic(bundle->videoMapHandle);
		ri.CIN_UploadCinematic(bundle->videoMapHandle);
		GL_BindToTMU(tr.scratchImage[bundle->videoMapHandle], tmu);
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		GL_BindToTMU( bundle->image[0], tmu);
		return;
	}

	// it is necessary to do this messy calc to make sure animations line up
	// exactly with waveforms of the same frequency
	// MOH:AA's animMapPhase starts the animation part way through, and
	// animMapOnce plays it once and holds the last frame, as in GL1.
	index = ( tess.shaderTime + bundle->imageAnimationPhase ) * bundle->imageAnimationSpeed * FUNCTABLE_SIZE;
	index >>= FUNCTABLE_SIZE2;

	if ( index < 0 ) {
		index = 0;	// may happen with shader time offsets
	}

	if ( bundle->flags & BUNDLE_ANIMATE_ONCE ) {
		if ( index >= bundle->numImageAnimations ) {
			index = bundle->numImageAnimations - 1;
		}
	} else {
		// Windows x86 doesn't load renderer DLL with 64 bit modulus
		//index %= bundle->numImageAnimations;
		while ( index >= bundle->numImageAnimations ) {
			index -= bundle->numImageAnimations;
		}
	}

	GL_BindToTMU( bundle->image[ index ], tmu );
}


/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris (shaderCommands_t *input) {
	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );

	GL_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE );
	qglDepthRange( 0, 0 );

	{
		shaderProgram_t *sp = &tr.textureColorShader;
		vec4_t color;

		GLSL_BindProgram(sp);
		
		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		VectorSet4(color, 1, 1, 1, 1);
		GLSL_SetUniformVec4(sp, UNIFORM_COLOR, color);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		R_DrawElements(input->numIndexes, input->firstIndex);
	}

	qglDepthRange( 0, 1 );
}


/*
================
DrawNormals

Draws vertex normals for debugging
================
*/
static void DrawNormals (shaderCommands_t *input) {
	if (!input) return;

	const int vertexCount = input->numVertexes;
	if (vertexCount <= 0) return;
	if (!input->shader || input->shader->isSky) return;   // do not draw over sky/clouds
	if (input->shader->numDeforms > 0) return;  // skip vertex-deformed surfaces

	// Skip animated entities that are interpolating; otherwise normals will appear to "swim".
	if (backEnd.currentEntity != &tr.worldEntity) {
		const trRefEntity_t* refEnt = backEnd.currentEntity;
		if (refEnt && refEnt->e.backlerp > 0) return;
	}

	const float       lineLength = 6.0f;

	// xyz is vec4_t (x,y,z,1)
	const float*      positionsXYZW   = (const float*)input->xyz;
	const int16_t*    packedNormals   = (const int16_t*)input->normal;
	const float*      floatNormals    = (const float*)input->normal;

	const vao_t*      boundVao        = glState.currentVao;
	qboolean          usePacked       = qfalse;

	if (boundVao) {
		const vaoAttrib_t* a = &boundVao->attribs[ATTR_INDEX_NORMAL];
		usePacked = (a->type == GL_SHORT && a->normalized);
	}
	else {
		const float sx = packedNormals[0] * (1.0f / 32767.0f);
		const float sy = packedNormals[1] * (1.0f / 32767.0f);
		const float sz = packedNormals[2] * (1.0f / 32767.0f);

		const float lp = sx * sx + sy * sy + sz * sz;
		const float lf = floatNormals[0] * floatNormals[0]
			+ floatNormals[1] * floatNormals[1]
			+ floatNormals[2] * floatNormals[2];

		usePacked = fabsf(lp - 1.0f) <= fabsf(lf - 1.0f);
	}

	const GLsizeiptr floatsPerVertex = 6;
	const GLsizeiptr maxFloatCount = floatsPerVertex * (GLsizeiptr)vertexCount;
	const GLsizeiptr maxByteCount = (GLsizeiptr)sizeof(float) * maxFloatCount;

	float* lineVertices = (float*)ri.Hunk_AllocateTempMemory((int)maxByteCount);
	float* writePtr = lineVertices;

	// Build the line list
	for (int i = 0; i < vertexCount; ++i, positionsXYZW += 4) {
		vec3_t n;

		if (usePacked) {
			R_VaoUnpackNormal(n, (int16_t*)packedNormals);
			packedNormals += 4;
		}
		else {
			n[0] = floatNormals[0];
			n[1] = floatNormals[1];
			n[2] = floatNormals[2];
			floatNormals += 4;
		}

		// endpoints in object/world space
		const float startX = positionsXYZW[0];
		const float startY = positionsXYZW[1];
		const float startZ = positionsXYZW[2];

		const float endX = startX + n[0] * lineLength;
		const float endY = startY + n[1] * lineLength;
		const float endZ = startZ + n[2] * lineLength;


		// emit line
		*writePtr++ = startX; *writePtr++ = startY; *writePtr++ = startZ;
		*writePtr++ = endX;   *writePtr++ = endY;   *writePtr++ = endZ;
	}

	const GLsizei numVerts = (GLsizei)((writePtr - lineVertices) / 3);
	if (numVerts == 0) {
		ri.Hunk_FreeTempMemory(lineVertices);
		return;
	}

	// save bindings
	GLint prevProg = 0, prevVao = 0, prevArrayBuf = 0;
	qglGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
	qglGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
	qglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);

	GL_BindToTMU(tr.whiteImage, TB_COLORMAP);
	GL_State(GLS_POLYMODE_LINE);
	GLboolean prevDepthMask;
	qglGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
	qglDepthMask(GL_FALSE);


	R_BindNullVao();

	// temp VAO/VBO
	GLuint vao = 0, vbo = 0;
	qglGenVertexArrays(1, &vao);
	qglBindVertexArray(vao);

	qglGenBuffers(1, &vbo);
	qglBindBuffer(GL_ARRAY_BUFFER, vbo);
	qglBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * numVerts, lineVertices, GL_STREAM_DRAW);

	shaderProgram_t* program = &tr.textureColorShader;
	GLSL_BindProgram(program);
	GLSL_SetUniformMat4(program, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

	vec4_t white;
	VectorSet4(white, 1, 1, 1, 1);
	GLSL_SetUniformVec4(program, UNIFORM_COLOR, white);

	// position at location 0 in this backend
	qglEnableVertexAttribArray(ATTR_INDEX_POSITION);
	qglVertexAttribPointer(ATTR_INDEX_POSITION, 3, GL_FLOAT, GL_FALSE, 0, (const void*)0);

	qglDrawArrays(GL_LINES, 0, numVerts);

	qglDisableVertexAttribArray(ATTR_INDEX_POSITION);

	// restore
	qglDepthMask(prevDepthMask);
	qglBindBuffer(GL_ARRAY_BUFFER, prevArrayBuf);
	qglBindVertexArray(prevVao);
	qglDeleteBuffers(1, &vbo);
	qglDeleteVertexArrays(1, &vao);

	if (prevProg) qglUseProgram((GLuint)prevProg);
	else          qglUseProgram(0);

	ri.Hunk_FreeTempMemory(lineVertices);
}

/*
==============
RB_BeginSurface

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a RB_End due
to overflow.
==============
*/
void RB_BeginSurface( shader_t *shader, int fogNum, int cubemapIndex ) {

	shader_t *state = (shader->remappedShader) ? shader->remappedShader : shader;

	tess.numIndexes = 0;
	tess.firstIndex = 0;
	tess.numVertexes = 0;
	tess.shader = state;
	tess.fogNum = fogNum;
	tess.cubemapIndex = cubemapIndex;
	tess.dlightBits = 0;		// will be OR'd in by surface functions
	tess.pshadowBits = 0;       // will be OR'd in by surface functions
	tess.xstages = state->stages;
	tess.numPasses = state->numUnfoggedPasses;
	tess.currentStageIteratorFunc = state->optimalStageIteratorFunc;
	tess.useInternalVao = qtrue;
	tess.useCacheVao = qfalse;

	tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
	if (tess.shader->clampTime && tess.shaderTime >= tess.shader->clampTime) {
		tess.shaderTime = tess.shader->clampTime;
	}

	if (backEnd.viewParms.flags & VPF_SHADOWMAP)
	{
		tess.currentStageIteratorFunc = RB_StageIteratorGeneric;
	}
}



extern float EvalWaveForm( const waveForm_t *wf );
extern float EvalWaveFormClamped( const waveForm_t *wf );


static void ComputeTexMods( shaderStage_t *pStage, int bundleNum, vec4_t outMatrix[8])
{
	int tm;
	float matrix[6];
	float tmpmatrix[6];
	float currentmatrix[6];
	float turb[2];
	textureBundle_t *bundle = &pStage->bundle[bundleNum];
	qboolean hasTurb = qfalse;

	currentmatrix[0] = 1.0f; currentmatrix[2] = 0.0f; currentmatrix[4] = 0.0f;
	currentmatrix[1] = 0.0f; currentmatrix[3] = 1.0f; currentmatrix[5] = 0.0f;

	for ( tm = 0; tm < bundle->numTexMods ; tm++ ) {
		switch ( bundle->texMods[tm].type )
		{
			
		case TMOD_NONE:
			matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = 0.0f;
			matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = 0.0f;
			break;

		case TMOD_TURBULENT:
			RB_CalcTurbulentFactors(&bundle->texMods[tm].wave, &turb[0], &turb[1]);
			break;

		case TMOD_ENTITY_TRANSLATE:
			RB_CalcScrollTexMatrix( backEnd.currentEntity->e.shaderTexCoord, matrix );
			break;

		case TMOD_SCROLL:
			RB_CalcScrollTexMatrix( bundle->texMods[tm].scroll,
									 matrix );
			break;

		case TMOD_SCALE:
			RB_CalcScaleTexMatrix( bundle->texMods[tm].scale,
								  matrix );
			break;
		
		case TMOD_STRETCH:
			RB_CalcStretchTexMatrix( &bundle->texMods[tm].wave, 
								   matrix );
			break;

		case TMOD_TRANSFORM:
			RB_CalcTransformTexMatrix( &bundle->texMods[tm],
									 matrix );
			break;

		case TMOD_ROTATE:
			RB_CalcRotateTexMatrix( bundle->texMods[tm].rotateSpeed,
									matrix );
			break;

		//
		// OPENMOHAA-specific stuff
		//=========================
		// All of these just move the texture, the way GL1 adds to s and t
		case TMOD_WAVETRANS:
		case TMOD_WAVETRANT:
			RB_CalcTransWaveTexMatrix( &bundle->texMods[tm].wave,
									   bundle->texMods[tm].type == TMOD_WAVETRANT, matrix );
			break;

		case TMOD_OFFSET:
			RB_CalcOffsetTexMatrix( bundle->texMods[tm].scroll, matrix );
			break;

		case TMOD_PARALLAX:
			RB_CalcParallaxTexMatrix( bundle->texMods[tm].scale, matrix );
			break;

		case TMOD_BULGETRANS:
			// does nothing in GL1 either, see ParseTexMod
			matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = 0.0f;
			matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = 0.0f;
			break;
		//=========================

		default:
			ri.Error( ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'", bundle->texMods[tm].type, tess.shader->name );
			break;
		}

		switch ( bundle->texMods[tm].type )
		{	
		case TMOD_TURBULENT:
			outMatrix[tm*2+0][0] = 1; outMatrix[tm*2+0][1] = 0; outMatrix[tm*2+0][2] = 0;
			outMatrix[tm*2+1][0] = 0; outMatrix[tm*2+1][1] = 1; outMatrix[tm*2+1][2] = 0;

			outMatrix[tm*2+0][3] = turb[0];
			outMatrix[tm*2+1][3] = turb[1];

			hasTurb = qtrue;
			break;

		case TMOD_NONE:
		case TMOD_ENTITY_TRANSLATE:
		case TMOD_SCROLL:
		case TMOD_SCALE:
		case TMOD_STRETCH:
		case TMOD_TRANSFORM:
		case TMOD_ROTATE:
		default:
			outMatrix[tm*2+0][0] = matrix[0]; outMatrix[tm*2+0][1] = matrix[2]; outMatrix[tm*2+0][2] = matrix[4];
			outMatrix[tm*2+1][0] = matrix[1]; outMatrix[tm*2+1][1] = matrix[3]; outMatrix[tm*2+1][2] = matrix[5];

			outMatrix[tm*2+0][3] = 0;
			outMatrix[tm*2+1][3] = 0;

			tmpmatrix[0] = matrix[0] * currentmatrix[0] + matrix[2] * currentmatrix[1];
			tmpmatrix[1] = matrix[1] * currentmatrix[0] + matrix[3] * currentmatrix[1];

			tmpmatrix[2] = matrix[0] * currentmatrix[2] + matrix[2] * currentmatrix[3];
			tmpmatrix[3] = matrix[1] * currentmatrix[2] + matrix[3] * currentmatrix[3];

			tmpmatrix[4] = matrix[0] * currentmatrix[4] + matrix[2] * currentmatrix[5] + matrix[4];
			tmpmatrix[5] = matrix[1] * currentmatrix[4] + matrix[3] * currentmatrix[5] + matrix[5];

			currentmatrix[0] = tmpmatrix[0];
			currentmatrix[1] = tmpmatrix[1];
			currentmatrix[2] = tmpmatrix[2];
			currentmatrix[3] = tmpmatrix[3];
			currentmatrix[4] = tmpmatrix[4];
			currentmatrix[5] = tmpmatrix[5];
			break;
		}
	}

	// if turb isn't used, only one matrix is needed
	if ( !hasTurb ) {
		tm = 0;

		outMatrix[tm*2+0][0] = currentmatrix[0]; outMatrix[tm*2+0][1] = currentmatrix[2]; outMatrix[tm*2+0][2] = currentmatrix[4];
		outMatrix[tm*2+1][0] = currentmatrix[1]; outMatrix[tm*2+1][1] = currentmatrix[3]; outMatrix[tm*2+1][2] = currentmatrix[5];

		outMatrix[tm*2+0][3] = 0;
		outMatrix[tm*2+1][3] = 0;
		tm++;
	}

	for ( ; tm < TR_MAX_TEXMODS ; tm++ ) {
		outMatrix[tm*2+0][0] = 1; outMatrix[tm*2+0][1] = 0; outMatrix[tm*2+0][2] = 0;
		outMatrix[tm*2+1][0] = 0; outMatrix[tm*2+1][1] = 1; outMatrix[tm*2+1][2] = 0;

		outMatrix[tm*2+0][3] = 0;
		outMatrix[tm*2+1][3] = 0;
	}
}


static void ComputeDeformValues(int *deformGen, vec5_t deformParams)
{
	// u_DeformGen
	*deformGen = DGEN_NONE;
	if(!ShaderRequiresCPUDeforms(tess.shader))
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.shader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
			{
				// Resolve MOH:AA's wind sentinels on the CPU and hand the GPU
				// concrete numbers; the vertex shader has no access to the
				// entity state or r_static_shaderdata* cvars they come from.
				waveForm_t rwf;

				RB_ResolveWaveForm( &ds->deformationWave, &rwf );

				*deformGen = rwf.func;

				deformParams[0] = rwf.base;
				deformParams[1] = rwf.amplitude;
				deformParams[2] = rwf.phase;
				deformParams[3] = rwf.frequency;
				deformParams[4] = ds->deformationSpread;
				break;
			}

			case DEFORM_BULGE:
				*deformGen = DGEN_BULGE;

				deformParams[0] = 0;
				deformParams[1] = ds->bulgeHeight; // amplitude
				deformParams[2] = ds->bulgeWidth;  // phase
				deformParams[3] = ds->bulgeSpeed;  // frequency
				deformParams[4] = 0;
				break;

			default:
				break;
		}
	}
}


static void ProjectDlightTexture( void ) {
	int		l;
	vec3_t	origin;
	float	scale;
	float	radius;
	int deformGen;
	vec5_t deformParams;

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

	ComputeDeformValues(&deformGen, deformParams);

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		scale = 1.0f / radius;

		sp = &tr.dlightShader[deformGen == DGEN_NONE ? 0 : 1];

		backEnd.pc.c_dlightDraws++;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);
		
		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		vector[0] = dl->color[0];
		vector[1] = dl->color[1];
		vector[2] = dl->color[2];
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_COLOR, vector);

		vector[0] = origin[0];
		vector[1] = origin[1];
		vector[2] = origin[2];
		vector[3] = scale;
		GLSL_SetUniformVec4(sp, UNIFORM_DLIGHTINFO, vector);
	  
		GL_BindToTMU( tr.dlightImage, TB_COLORMAP );

		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		if ( dl->additive ) {
			GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}
		else {
			GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}

		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 1);

		R_DrawElements(tess.numIndexes, tess.firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		backEnd.pc.c_dlightIndexes += tess.numIndexes;
		backEnd.pc.c_dlightVertexes += tess.numVertexes;
	}
}


/*
** RB_WarnUnhandledGen
**
** Several of MOH:AA's rgbGen/alphaGen types have no GL2 implementation yet. They
** used to fall out of the switches below silently, which made them very hard to
** notice. Report each distinct one once per run so the remaining gaps are
** visible without letting a single bad shader flood the console.
*/
static const char * const s_colorGenNames[] = {
	"CGEN_BAD",
	"CGEN_IDENTITY_LIGHTING",
	"CGEN_IDENTITY",
	"CGEN_ENTITY",
	"CGEN_ONE_MINUS_ENTITY",
	"CGEN_EXACT_VERTEX",
	"CGEN_VERTEX",
	"CGEN_EXACT_VERTEX_LIT",
	"CGEN_VERTEX_LIT",
	"CGEN_ONE_MINUS_VERTEX",
	"CGEN_WAVEFORM",
	"CGEN_LIGHTING_DIFFUSE",
	"CGEN_FOG",
	"CGEN_CONST",
	"CGEN_MULTIPLY_BY_WAVEFORM",
	"CGEN_LIGHTING_GRID",
	"CGEN_LIGHTING_SPHERICAL",
	"CGEN_NOISE",
	"CGEN_GLOBAL_COLOR",
	"CGEN_STATIC",
	"CGEN_SCOORD",
	"CGEN_TCOORD",
	"CGEN_DOT",
	"CGEN_ONE_MINUS_DOT",
};

static const char * const s_alphaGenNames[] = {
	"AGEN_IDENTITY",
	"AGEN_SKIP",
	"AGEN_ENTITY",
	"AGEN_ONE_MINUS_ENTITY",
	"AGEN_VERTEX",
	"AGEN_ONE_MINUS_VERTEX",
	"AGEN_LIGHTING_SPECULAR",
	"AGEN_WAVEFORM",
	"AGEN_PORTAL",
	"AGEN_CONST",
	"AGEN_NOISE",
	"AGEN_DOT",
	"AGEN_ONE_MINUS_DOT",
	"AGEN_CONSTANT",
	"AGEN_GLOBAL_ALPHA",
	"AGEN_SKYALPHA",
	"AGEN_ONE_MINUS_SKYALPHA",
	"AGEN_SCOORD",
	"AGEN_TCOORD",
	"AGEN_DIST_FADE",
	"AGEN_ONE_MINUS_DIST_FADE",
	"AGEN_TIKI_DIST_FADE",
	"AGEN_ONE_MINUS_TIKI_DIST_FADE",
	"AGEN_DOT_VIEW",
	"AGEN_ONE_MINUS_DOT_VIEW",
	"AGEN_HEIGHT_FADE",
};

static unsigned int s_warnedRgbGen;
static unsigned int s_warnedAlphaGen;

static void RB_WarnUnhandledGen( unsigned int *warned, const char *what, int value,
		const char * const *names, int numNames )
{
	if ( value < 0 || value >= 32 || ( *warned & ( 1u << value ) ) ) {
		return;
	}

	*warned |= 1u << value;

	ri.Printf( PRINT_WARNING, "WARNING: %s %s is not implemented in the GL2 renderer (first seen in shader '%s')\n",
		what, value < numNames ? names[value] : "(out of range)",
		tess.shader ? tess.shader->name : "<unknown>" );
}


/*
** RB_SetGlobalFogUniforms
**
** MOH:AA's global distance fog. A stage opts in through GLS_FOG_ENABLED, and
** may force the fog colour to black or white, which is how additive and
** modulated blends stay neutral as they fade out. Anything that has asked to be
** left alone, and any stage that did not opt in, gets a zero range so the
** shader's fog factor stays at zero.
*/
static void RB_SetGlobalFogUniforms( shaderProgram_t *sp, const shaderStage_t *pStage )
{
	vec4_t color;
	vec2_t params;

	if ( !backEnd.globalFogEnabled || tess.no_global_fog
		|| !( pStage->stateBits & GLS_FOG_ENABLED ) ) {
		VectorSet4( color, 0.0f, 0.0f, 0.0f, 0.0f );
		params[0] = 0.0f;
		params[1] = 0.0f;
	} else {
		if ( pStage->stateBits & GLS_FOG_BLACK ) {
			VectorSet4( color, 0.0f, 0.0f, 0.0f, 1.0f );
		} else if ( pStage->stateBits & GLS_FOG_WHITE ) {
			VectorSet4( color, 1.0f, 1.0f, 1.0f, 1.0f );
		} else {
			VectorCopy4( backEnd.globalFogColor, color );
		}

		params[0] = backEnd.globalFogStart;
		params[1] = backEnd.globalFogInvRange;
	}

	GLSL_SetUniformVec4( sp, UNIFORM_GLOBALFOGCOLOR, color );
	GLSL_SetUniformVec2( sp, UNIFORM_GLOBALFOGPARAMS, params );
}


static void ComputeShaderColors( shaderStage_t *pStage, vec4_t baseColor, vec4_t vertColor, int blend )
{
	qboolean isBlend = ((blend & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_DST_COLOR)
		|| ((blend & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_ONE_MINUS_DST_COLOR)
		|| ((blend & GLS_DSTBLEND_BITS) == GLS_DSTBLEND_SRC_COLOR)
		|| ((blend & GLS_DSTBLEND_BITS) == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR);

	qboolean is2DDraw = backEnd.currentEntity == &backEnd.entity2D;

	float overbright = (isBlend || is2DDraw) ? 1.0f : (float)(1 << tr.overbrightBits);

	fog_t *fog;

	baseColor[0] = 
	baseColor[1] =
	baseColor[2] =
	baseColor[3] = 1.0f;

	vertColor[0] =
	vertColor[1] =
	vertColor[2] =
	vertColor[3] = 0.0f;

	//
	// rgbGen
	//
	switch ( pStage->rgbGen )
	{
		case CGEN_EXACT_VERTEX:
		case CGEN_EXACT_VERTEX_LIT:
			baseColor[0] = 
			baseColor[1] =
			baseColor[2] = 
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] = overbright;
			vertColor[3] = 1.0f;
			break;
		case CGEN_CONST:
			baseColor[0] = pStage->constantColor[0] / 255.0f;
			baseColor[1] = pStage->constantColor[1] / 255.0f;
			baseColor[2] = pStage->constantColor[2] / 255.0f;
			baseColor[3] = pStage->constantColor[3] / 255.0f;
			break;
		case CGEN_VERTEX:
		case CGEN_VERTEX_LIT:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] =
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] =
			vertColor[3] = 1.0f;
			break;
		case CGEN_ONE_MINUS_VERTEX:
			baseColor[0] = 
			baseColor[1] =
			baseColor[2] = 1.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] = -1.0f;
			break;
		case CGEN_FOG:
			fog = tr.world->fogs + tess.fogNum;

			baseColor[0] = ((unsigned char *)(&fog->colorInt))[0] / 255.0f;
			baseColor[1] = ((unsigned char *)(&fog->colorInt))[1] / 255.0f;
			baseColor[2] = ((unsigned char *)(&fog->colorInt))[2] / 255.0f;
			baseColor[3] = ((unsigned char *)(&fog->colorInt))[3] / 255.0f;
			break;
		case CGEN_WAVEFORM:
			baseColor[0] = 
			baseColor[1] = 
			baseColor[2] = RB_CalcWaveColorSingle( &pStage->rgbWave );
			break;
		case CGEN_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[0] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[0] / 255.0f;
				baseColor[1] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[1] / 255.0f;
				baseColor[2] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[2] / 255.0f;
				baseColor[3] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			break;
		case CGEN_ONE_MINUS_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[0] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[0] / 255.0f;
				baseColor[1] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[1] / 255.0f;
				baseColor[2] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[2] / 255.0f;
				baseColor[3] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			break;
		case CGEN_IDENTITY:
		case CGEN_LIGHTING_DIFFUSE:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] = overbright;
			break;
		case CGEN_IDENTITY_LIGHTING:
		case CGEN_BAD:
			break;
		//
		// OPENMOHAA-specific stuff
		//=========================
		case CGEN_STATIC:
		// Spherical and light grid lighting are evaluated per vertex and written
		// into the vertex colors, so the shader just passes those through. The
		// values themselves are produced by the sphere lighting code.
		case CGEN_LIGHTING_GRID:
		case CGEN_LIGHTING_SPHERICAL:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] =
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] =
			vertColor[3] = 1.0f;
			break;
		case CGEN_MULTIPLY_BY_WAVEFORM:
			{
				float glow = RB_CalcWaveColorSingle( &pStage->rgbWave );

				baseColor[0] = pStage->colorConst[0] / 255.0f * glow;
				baseColor[1] = pStage->colorConst[1] / 255.0f * glow;
				baseColor[2] = pStage->colorConst[2] / 255.0f * glow;
			}
			break;
		case CGEN_GLOBAL_COLOR:
			baseColor[0] = backEnd.color2D[0] / 255.0f;
			baseColor[1] = backEnd.color2D[1] / 255.0f;
			baseColor[2] = backEnd.color2D[2] / 255.0f;
			baseColor[3] = backEnd.color2D[3] / 255.0f;
			break;
		default:
			// CGEN_DOT, CGEN_ONE_MINUS_DOT, CGEN_SCOORD and CGEN_TCOORD are all
			// evaluated per vertex and cannot be expressed as a base/vertex color
			// pair, so they still need doing.
			RB_WarnUnhandledGen( &s_warnedRgbGen, "rgbGen", pStage->rgbGen,
				s_colorGenNames, ARRAY_LEN( s_colorGenNames ) );
			break;
		//=========================
	}

	//
	// alphaGen
	//
	switch ( pStage->alphaGen )
	{
		case AGEN_SKIP:
			break;
		case AGEN_CONST:
			baseColor[3] = pStage->constantColor[3] / 255.0f;
			vertColor[3] = 0.0f;
			break;
		case AGEN_WAVEFORM:
			baseColor[3] = RB_CalcWaveAlphaSingle( &pStage->alphaWave );
			vertColor[3] = 0.0f;
			break;
		case AGEN_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[3] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			vertColor[3] = 0.0f;
			break;
		case AGEN_ONE_MINUS_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[3] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			vertColor[3] = 0.0f;
			break;
		case AGEN_VERTEX:
			baseColor[3] = 0.0f;
			vertColor[3] = 1.0f;
			break;
		case AGEN_ONE_MINUS_VERTEX:
			baseColor[3] = 1.0f;
			vertColor[3] = -1.0f;
			break;
		case AGEN_IDENTITY:
		case AGEN_LIGHTING_SPECULAR:
		case AGEN_PORTAL:
			// Done entirely in vertex program
			baseColor[3] = 1.0f;
			vertColor[3] = 0.0f;
			break;
		//
		// OPENMOHAA-specific stuff
		//=========================
		case AGEN_GLOBAL_ALPHA:
			baseColor[3] = backEnd.color2D[3] / 255.0f;
			vertColor[3] = 0.0f;
			break;
		case AGEN_SKYALPHA:
			baseColor[3] = tr.refdef.sky_alpha;
			vertColor[3] = 0.0f;
			break;
		case AGEN_ONE_MINUS_SKYALPHA:
			baseColor[3] = 1.0f - tr.refdef.sky_alpha;
			vertColor[3] = 0.0f;
			break;
		// The fades vary per vertex and were written into the vertex alpha by
		// RB_ComputeVertexAlphaGen before the batch was uploaded, so here they
		// just need passing through.
		case AGEN_DIST_FADE:
		case AGEN_ONE_MINUS_DIST_FADE:
		case AGEN_TIKI_DIST_FADE:
		case AGEN_ONE_MINUS_TIKI_DIST_FADE:
		case AGEN_HEIGHT_FADE:
			baseColor[3] = 0.0f;
			vertColor[3] = 1.0f;
			break;
		// worked out per stage in the generic vertex program, see CalcColor
		case AGEN_SCOORD:
		case AGEN_TCOORD:
			baseColor[3] = 1.0f;
			vertColor[3] = 0.0f;
			break;
		default:
			// The distance and height fades, and the dot and texture coordinate
			// driven alphas, are all per vertex and still need doing.
			RB_WarnUnhandledGen( &s_warnedAlphaGen, "alphaGen", pStage->alphaGen,
				s_alphaGenNames, ARRAY_LEN( s_alphaGenNames ) );
			break;
		//=========================
	}

}


static void ComputeFogValues(vec4_t fogDistanceVector, vec4_t fogDepthVector, float *eyeT)
{
	// from RB_CalcFogTexCoords()
	fog_t  *fog;
	vec3_t  local;

	if (!tess.fogNum)
		return;

	fog = tr.world->fogs + tess.fogNum;

	VectorSubtract( backEnd.ori.origin, backEnd.viewParms.ori.origin, local );
	fogDistanceVector[0] = -backEnd.ori.modelMatrix[2];
	fogDistanceVector[1] = -backEnd.ori.modelMatrix[6];
	fogDistanceVector[2] = -backEnd.ori.modelMatrix[10];
	fogDistanceVector[3] = DotProduct( local, backEnd.viewParms.ori.axis[0] );

	// scale the fog vectors based on the fog's thickness
	VectorScale4(fogDistanceVector, fog->tcScale, fogDistanceVector);

	// rotate the gradient vector for this orientation
	if ( fog->hasSurface ) {
		fogDepthVector[0] = fog->surface[0] * backEnd.ori.axis[0][0] + 
			fog->surface[1] * backEnd.ori.axis[0][1] + fog->surface[2] * backEnd.ori.axis[0][2];
		fogDepthVector[1] = fog->surface[0] * backEnd.ori.axis[1][0] + 
			fog->surface[1] * backEnd.ori.axis[1][1] + fog->surface[2] * backEnd.ori.axis[1][2];
		fogDepthVector[2] = fog->surface[0] * backEnd.ori.axis[2][0] + 
			fog->surface[1] * backEnd.ori.axis[2][1] + fog->surface[2] * backEnd.ori.axis[2][2];
		fogDepthVector[3] = -fog->surface[3] + DotProduct( backEnd.ori.origin, fog->surface );

		*eyeT = DotProduct( backEnd.ori.viewOrigin, fogDepthVector ) + fogDepthVector[3];
	} else {
		*eyeT = 1;	// non-surface fog always has eye inside
	}
}


static void ComputeFogColorMask( shaderStage_t *pStage, vec4_t fogColorMask )
{
	switch(pStage->adjustColorsForFog)
	{
		case ACFF_MODULATE_RGB:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] = 1.0f;
			fogColorMask[3] = 0.0f;
			break;
		case ACFF_MODULATE_ALPHA:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] = 0.0f;
			fogColorMask[3] = 1.0f;
			break;
		case ACFF_MODULATE_RGBA:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] =
			fogColorMask[3] = 1.0f;
			break;
		default:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] =
			fogColorMask[3] = 0.0f;
			break;
	}
}


static void ForwardDlight( void ) {
	int		l;
	//vec3_t	origin;
	//float	scale;
	float	radius;

	int deformGen;
	vec5_t deformParams;
	
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;

	shaderCommands_t *input = &tess;
	shaderStage_t *pStage = tess.xstages[0];

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}
	
	ComputeDeformValues(&deformGen, deformParams);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT);

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;
		shaderProgram_t *sp;
		vec4_t vector;
		vec4_t texMatrix[8];

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		//VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		//scale = 1.0f / radius;

		//if (pStage->glslShaderGroup == tr.lightallShader)
		{
			int index = pStage->glslShaderIndex;

			index &= ~LIGHTDEF_LIGHTTYPE_MASK;
			index |= LIGHTDEF_USE_LIGHT_VECTOR;

			sp = &tr.lightallShader[index];
		}

		backEnd.pc.c_lightallDraws++;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.ori.origin);
		GLSL_SetUniformVec3(sp, UNIFORM_LOCALVIEWORIGIN, backEnd.ori.viewOrigin);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		if ( input->fogNum ) {
			vec4_t fogColorMask;

			GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
			GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);

			ComputeFogColorMask(pStage, fogColorMask);

			GLSL_SetUniformVec4(sp, UNIFORM_FOGCOLORMASK, fogColorMask);
		}

		{
			vec4_t baseColor;
			vec4_t vertColor;

			ComputeShaderColors(pStage, baseColor, vertColor, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

			RB_SetGlobalFogUniforms(sp, pStage);

			GLSL_SetUniformVec4(sp, UNIFORM_BASECOLOR, baseColor);
			GLSL_SetUniformVec4(sp, UNIFORM_VERTCOLOR, vertColor);
		}

		if (pStage->alphaGen == AGEN_PORTAL)
		{
			GLSL_SetUniformFloat(sp, UNIFORM_PORTALRANGE, tess.shader->portalRange);
		}

		GLSL_SetUniformInt(sp, UNIFORM_COLORGEN, pStage->rgbGen);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHAGEN, pStage->alphaGen);

		GLSL_SetUniformVec3(sp, UNIFORM_DIRECTEDLIGHT, dl->color);

		VectorSet(vector, 0, 0, 0);
		GLSL_SetUniformVec3(sp, UNIFORM_AMBIENTLIGHT, vector);

		VectorCopy(dl->origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);

		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, radius);

		GLSL_SetUniformVec4(sp, UNIFORM_NORMALSCALE, pStage->normalScale);
		GLSL_SetUniformVec4(sp, UNIFORM_SPECULARSCALE, pStage->specularScale);
		
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.ori.transformMatrix);

		if (pStage->bundle[TB_DIFFUSEMAP].image[0])
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP);

		// bind textures that are sampled and used in the glsl shader, and
		// bind whiteImage to textures that are sampled but zeroed in the glsl shader
		//
		// alternatives:
		//  - use the last bound texture
		//     -> costs more to sample a higher res texture then throw out the result
		//  - disable texture sampling in glsl shader with #ifdefs, as before
		//     -> increases the number of shaders that must be compiled
		//

		if (pStage->bundle[TB_NORMALMAP].image[0])
		{
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP);
		}
		else if (r_normalMapping->integer)
			GL_BindToTMU( tr.whiteImage, TB_NORMALMAP );

		if (pStage->bundle[TB_SPECULARMAP].image[0])
		{
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP);
		}
		else if (r_specularMapping->integer)
			GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );

		{
			vec4_t enableTextures;

			VectorSet4(enableTextures, 0.0f, 0.0f, 0.0f, 0.0f);
			GLSL_SetUniformVec4(sp, UNIFORM_ENABLETEXTURES, enableTextures);
		}

		if (r_dlightMode->integer >= 2)
			GL_BindToTMU(tr.shadowCubemaps[l], TB_SHADOWMAP);

		ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7]);

		GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen);

		//
		// draw
		//

		R_DrawElements(input->numIndexes, input->firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		backEnd.pc.c_dlightIndexes += tess.numIndexes;
		backEnd.pc.c_dlightVertexes += tess.numVertexes;
	}
}


static void ProjectPshadowVBOGLSL( void ) {
	int		l;
	vec3_t	origin;
	float	radius;

	int deformGen;
	vec5_t deformParams;

	shaderCommands_t *input = &tess;

	if ( !backEnd.refdef.num_pshadows ) {
		return;
	}
	
	ComputeDeformValues(&deformGen, deformParams);

	for ( l = 0 ; l < backEnd.refdef.num_pshadows ; l++ ) {
		pshadow_t	*ps;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.pshadowBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this shadow
		}

		ps = &backEnd.refdef.pshadows[l];
		VectorCopy( ps->lightOrigin, origin );
		radius = ps->lightRadius;

		sp = &tr.pshadowShader;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		VectorCopy(origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);

		VectorScale(ps->lightViewAxis[0], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTFORWARD, vector);

		VectorScale(ps->lightViewAxis[1], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTRIGHT, vector);

		VectorScale(ps->lightViewAxis[2], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTUP, vector);

		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, radius);
	  
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		GL_BindToTMU( tr.pshadowMaps[l], TB_DIFFUSEMAP );

		//
		// draw
		//

		R_DrawElements(input->numIndexes, input->firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		//backEnd.pc.c_dlightIndexes += tess.numIndexes;
	}
}



/*
===================
RB_FogPass

Blends a fog texture on top of everything else
===================
*/
static void RB_FogPass( void ) {
	fog_t		*fog;
	vec4_t  color;
	vec4_t	fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float	eyeT = 0;
	shaderProgram_t *sp;

	int deformGen;
	vec5_t deformParams;

	ComputeDeformValues(&deformGen, deformParams);

	{
		int index = 0;

		if (deformGen != DGEN_NONE)
			index |= FOGDEF_USE_DEFORM_VERTEXES;

		if (glState.vertexAnimation)
			index |= FOGDEF_USE_VERTEX_ANIMATION;
		else if (glState.boneAnimation)
			index |= FOGDEF_USE_BONE_ANIMATION;
		
		sp = &tr.fogShader[index];
	}

	backEnd.pc.c_fogDraws++;

	GLSL_BindProgram(sp);

	fog = tr.world->fogs + tess.fogNum;

	GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

	GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

	if (glState.boneAnimation)
	{
		GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
	}
	
	GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
	if (deformGen != DGEN_NONE)
	{
		GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
		GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
	}

	color[0] = ((unsigned char *)(&fog->colorInt))[0] / 255.0f;
	color[1] = ((unsigned char *)(&fog->colorInt))[1] / 255.0f;
	color[2] = ((unsigned char *)(&fog->colorInt))[2] / 255.0f;
	color[3] = ((unsigned char *)(&fog->colorInt))[3] / 255.0f;
	GLSL_SetUniformVec4(sp, UNIFORM_COLOR, color);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT);

	GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
	GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
	GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);

	if ( tess.shader->fogPass == FP_EQUAL ) {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	} else {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	}
	GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

	R_DrawElements(tess.numIndexes, tess.firstIndex);
}


static unsigned int RB_CalcShaderVertexAttribs( shaderCommands_t *input )
{
	unsigned int vertexAttribs = input->shader->vertexAttribs;

	if(glState.vertexAnimation)
	{
		vertexAttribs |= ATTR_POSITION2;
		if (vertexAttribs & ATTR_NORMAL)
		{
			vertexAttribs |= ATTR_NORMAL2;
			vertexAttribs |= ATTR_TANGENT2;
		}
	}

	return vertexAttribs;
}

/*
** RB_SetSecondBundle
**
** MOH:AA's nextBundle draws a second texture in the same pass, with its own
** texture coordinates and tcMods, modulated or added onto the first, as GL1's
** multitexture does. The generic program keeps these uniforms between draws,
** so every draw through it has to say whether it wants the second texture.
*/
static void RB_SetSecondBundle( shaderProgram_t *sp, shaderStage_t *pStage, qboolean use )
{
	vec4_t texMatrix[8];

	if ( !use || !pStage->multitextureEnv || !pStage->bundle[1].image[0] ) {
		GLSL_SetUniformInt( sp, UNIFORM_TEXTURE1ENV, 0 );
		return;
	}

	R_BindAnimatedImageToTMU( &pStage->bundle[1], TB_LIGHTMAP );

	ComputeTexMods( pStage, 1, texMatrix );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX0, texMatrix[0] );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX1, texMatrix[1] );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX2, texMatrix[2] );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX3, texMatrix[3] );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX4, texMatrix[4] );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX5, texMatrix[5] );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX6, texMatrix[6] );
	GLSL_SetUniformVec4( sp, UNIFORM_TEXTURE1MATRIX7, texMatrix[7] );

	GLSL_SetUniformInt( sp, UNIFORM_TCGEN1, pStage->bundle[1].tcGen );
	GLSL_SetUniformInt( sp, UNIFORM_TEXTURE1ENV, pStage->multitextureEnv == GL_ADD ? 2 : 1 );
}


static void RB_IterateStagesGeneric( shaderCommands_t *input )
{
	int stage;
	
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;

	int deformGen;
	vec5_t deformParams;

	qboolean renderToCubemap = tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo;

	ComputeDeformValues(&deformGen, deformParams);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT);

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
	{
		shaderStage_t *pStage = input->xstages[stage];
		shaderProgram_t *sp;
		vec4_t texMatrix[8];

		if ( !pStage )
		{
			break;
		}

		if (backEnd.depthFill)
		{
			if (pStage->glslShaderGroup == tr.lightallShader)
			{
				int index = 0;

				if (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity)
				{
					if (glState.boneAnimation)
					{
						index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
					}
					else
					{
						index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
					}
				}

				if (pStage->stateBits & GLS_ATEST_BITS)
				{
					index |= LIGHTDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = GLSL_GetLightallShader(index);
			}
			else
			{
				int shaderAttribs = 0;

				if (tess.shader->numDeforms && !ShaderRequiresCPUDeforms(tess.shader))
				{
					shaderAttribs |= GENERICDEF_USE_DEFORM_VERTEXES;
				}

				if (glState.vertexAnimation)
				{
					shaderAttribs |= GENERICDEF_USE_VERTEX_ANIMATION;
				}
				else if (glState.boneAnimation)
				{
					shaderAttribs |= GENERICDEF_USE_BONE_ANIMATION;
				}

				if (pStage->stateBits & GLS_ATEST_BITS)
				{
					shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = &tr.genericShader[shaderAttribs];
			}
		}
		else if (pStage->glslShaderGroup == tr.lightallShader)
		{
			int index = pStage->glslShaderIndex;

			if (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity)
			{
				if (glState.boneAnimation)
				{
					index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
				}
				else
				{
					index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
				}
			}

			if (r_sunlightMode->integer && (backEnd.viewParms.flags & VPF_USESUNLIGHT) && (index & LIGHTDEF_LIGHTTYPE_MASK))
			{
				index |= LIGHTDEF_USE_SHADOWMAP;
			}

			if (r_lightmap->integer && ((index & LIGHTDEF_LIGHTTYPE_MASK) == LIGHTDEF_USE_LIGHTMAP))
			{
				index = LIGHTDEF_USE_TCGEN_AND_TCMOD;
			}

			sp = GLSL_GetLightallShader(index);

			backEnd.pc.c_lightallDraws++;
		}
		else
		{
			sp = GLSL_GetGenericShaderProgram(stage);

			backEnd.pc.c_genericDraws++;
		}

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.ori.origin);
		GLSL_SetUniformVec3(sp, UNIFORM_LOCALVIEWORIGIN, backEnd.ori.viewOrigin);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		if (glState.boneAnimation)
		{
			GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
		}
		
		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		if ( input->fogNum ) {
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
			GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);
		}

		// GL1 never depth tests anything drawn in 2D. Scripted menu and HUD
		// shaders keep Q3's default depth test, and the view weapon leaves
		// near depth behind it, so without this the ESC menu and the HUD are
		// cut away wherever the gun was last drawn.
		if (backEnd.projection2D)
			GL_State( pStage->stateBits | GLS_DEPTHTEST_DISABLE );
		else
			GL_State( pStage->stateBits );
		if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_GT_0)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 1);
		}
		else if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_LT_80)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 2);
		}
		else if ((pStage->stateBits & GLS_ATEST_BITS) == GLS_ATEST_GE_80)
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 3);
		}
		else
		{
			GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);
		}


		{
			vec4_t baseColor;
			vec4_t vertColor;

			ComputeShaderColors(pStage, baseColor, vertColor, pStage->stateBits);

			RB_SetGlobalFogUniforms(sp, pStage);

			GLSL_SetUniformVec4(sp, UNIFORM_BASECOLOR, baseColor);
			GLSL_SetUniformVec4(sp, UNIFORM_VERTCOLOR, vertColor);
		}

		if (pStage->rgbGen == CGEN_LIGHTING_DIFFUSE)
		{
			vec4_t vec;

			VectorScale(backEnd.currentEntity->ambientLight, 1.0f / 255.0f, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_AMBIENTLIGHT, vec);

			VectorScale(backEnd.currentEntity->directedLight, 1.0f / 255.0f, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_DIRECTEDLIGHT, vec);
			
			VectorCopy(backEnd.currentEntity->lightDir, vec);
			vec[3] = 0.0f;
			GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_MODELLIGHTDIR, backEnd.currentEntity->modelLightDir);

			GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, 0.0f);
		}

		if (pStage->alphaGen == AGEN_PORTAL)
		{
			GLSL_SetUniformFloat(sp, UNIFORM_PORTALRANGE, tess.shader->portalRange);
		}

		// the ramp the generic vertex program works out per vertex, see CalcColor
		if (pStage->alphaGen == AGEN_SCOORD || pStage->alphaGen == AGEN_TCOORD)
		{
			vec4_t params;

			VectorSet4(params, pStage->alphaMin, pStage->alphaMax, pStage->alphaConstMin, pStage->alphaConst);
			GLSL_SetUniformVec4(sp, UNIFORM_ALPHAGENPARAMS, params);
		}

		GLSL_SetUniformInt(sp, UNIFORM_COLORGEN, pStage->rgbGen);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHAGEN, pStage->alphaGen);

		if ( input->fogNum )
		{
			vec4_t fogColorMask;

			ComputeFogColorMask(pStage, fogColorMask);

			GLSL_SetUniformVec4(sp, UNIFORM_FOGCOLORMASK, fogColorMask);
		}

		if (r_lightmap->integer)
		{
			vec4_t st[2];
			VectorSet4(st[0], 1.0f, 0.0f, 0.0f, 0.0f);
			VectorSet4(st[1], 0.0f, 1.0f, 0.0f, 0.0f);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, st[1]);

			GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, TCGEN_LIGHTMAP);
		}
		else
		{
			ComputeTexMods(pStage, TB_DIFFUSEMAP, texMatrix);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7]);

			GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen);
			if (pStage->bundle[0].tcGen == TCGEN_VECTOR)
			{
				vec3_t vec;

				VectorCopy(pStage->bundle[0].tcGenVectors[0], vec);
				GLSL_SetUniformVec3(sp, UNIFORM_TCGEN0VECTOR0, vec);
				VectorCopy(pStage->bundle[0].tcGenVectors[1], vec);
				GLSL_SetUniformVec3(sp, UNIFORM_TCGEN0VECTOR1, vec);
			}
		}

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.ori.transformMatrix);

		GLSL_SetUniformVec4(sp, UNIFORM_NORMALSCALE, pStage->normalScale);

		{
			vec4_t specularScale;
			Vector4Copy(pStage->specularScale, specularScale);

			if (renderToCubemap)
			{
				// force specular to nonmetal if rendering cubemaps
				if (r_pbr->integer)
					specularScale[1] = 0.0f;
			}

			GLSL_SetUniformVec4(sp, UNIFORM_SPECULARSCALE, specularScale);
		}

		//GLSL_SetUniformFloat(sp, UNIFORM_MAPLIGHTSCALE, backEnd.refdef.mapLightScale);

		//
		// do multitexture
		//
		if ( backEnd.depthFill )
		{
			if (!(pStage->stateBits & GLS_ATEST_BITS))
				GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
			else if ( pStage->bundle[TB_COLORMAP].image[0] != 0 )
				R_BindAnimatedImageToTMU( &pStage->bundle[TB_COLORMAP], TB_COLORMAP );

			// the second texture's alpha counts towards the alpha test
			RB_SetSecondBundle( sp, pStage, ( pStage->stateBits & GLS_ATEST_BITS ) != 0 );
		}
		else if ( pStage->glslShaderGroup == tr.lightallShader )
		{
			int i;
			vec4_t enableTextures;

			if (r_sunlightMode->integer && (backEnd.viewParms.flags & VPF_USESUNLIGHT) && (pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK))
			{
				// FIXME: screenShadowImage is NULL if no framebuffers
				if (tr.screenShadowImage)
					GL_BindToTMU(tr.screenShadowImage, TB_SHADOWMAP);
				GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTAMBIENT, backEnd.refdef.sunAmbCol);
				if (r_pbr->integer)
				{
					vec3_t color;

					color[0] = backEnd.refdef.sunCol[0] * backEnd.refdef.sunCol[0];
					color[1] = backEnd.refdef.sunCol[1] * backEnd.refdef.sunCol[1];
					color[2] = backEnd.refdef.sunCol[2] * backEnd.refdef.sunCol[2];
					GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTCOLOR, color);
				}
				else
				{
					GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTCOLOR, backEnd.refdef.sunCol);
				}
				GLSL_SetUniformVec4(sp, UNIFORM_PRIMARYLIGHTORIGIN,  backEnd.refdef.sunDir);
			}

			VectorSet4(enableTextures, 0, 0, 0, 0);
			if ((r_lightmap->integer == 1 || r_lightmap->integer == 2) && pStage->bundle[TB_LIGHTMAP].image[0])
			{
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if (i == TB_COLORMAP)
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], i);
					else
						GL_BindToTMU( tr.whiteImage, i );
				}
			}
			else if (r_lightmap->integer == 3 && pStage->bundle[TB_DELUXEMAP].image[0])
			{
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if (i == TB_COLORMAP)
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], i);
					else
						GL_BindToTMU( tr.whiteImage, i );
				}
			}
			else
			{
				qboolean light = (pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK) != 0;
				qboolean fastLight = !(r_normalMapping->integer || r_specularMapping->integer);

				if (pStage->bundle[TB_DIFFUSEMAP].image[0])
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP);

				if (pStage->bundle[TB_LIGHTMAP].image[0])
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], TB_LIGHTMAP);

				// bind textures that are sampled and used in the glsl shader, and
				// bind whiteImage to textures that are sampled but zeroed in the glsl shader
				//
				// alternatives:
				//  - use the last bound texture
				//     -> costs more to sample a higher res texture then throw out the result
				//  - disable texture sampling in glsl shader with #ifdefs, as before
				//     -> increases the number of shaders that must be compiled
				//
				if (light && !fastLight)
				{
					if (pStage->bundle[TB_NORMALMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP);
						enableTextures[0] = 1.0f;
					}
					else if (r_normalMapping->integer)
						GL_BindToTMU( tr.flatNormalImage, TB_NORMALMAP );

					if (pStage->bundle[TB_DELUXEMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], TB_DELUXEMAP);
						enableTextures[1] = 1.0f;
					}
					else if (r_deluxeMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_DELUXEMAP );

					if (pStage->bundle[TB_SPECULARMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP);
						enableTextures[2] = 1.0f;
					}
					else if (r_specularMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );
				}

				enableTextures[3] = (r_cubeMapping->integer && !(tr.viewParms.flags & VPF_NOCUBEMAPS) && input->cubemapIndex) ? 1.0f : 0.0f;
			}

			GLSL_SetUniformVec4(sp, UNIFORM_ENABLETEXTURES, enableTextures);
		}
		else 
		{
			//
			// set state
			//
			R_BindAnimatedImageToTMU( &pStage->bundle[0], 0 );

			RB_SetSecondBundle( sp, pStage, qtrue );
		}

		//
		// testing cube map
		//
		if (!(tr.viewParms.flags & VPF_NOCUBEMAPS) && input->cubemapIndex && r_cubeMapping->integer)
		{
			vec4_t vec;
			cubemap_t *cubemap = &tr.cubemaps[input->cubemapIndex - 1];

			// FIXME: cubemap image could be NULL if cubemap isn't renderer or loaded
			if (cubemap->image)
				GL_BindToTMU( cubemap->image, TB_CUBEMAP);

			VectorSubtract(cubemap->origin, backEnd.viewParms.ori.origin, vec);
			vec[3] = 1.0f;

			VectorScale4(vec, 1.0f / cubemap->parallaxRadius, vec);

			GLSL_SetUniformVec4(sp, UNIFORM_CUBEMAPINFO, vec);
		}

		//
		// draw
		//
		R_DrawElements(input->numIndexes, input->firstIndex);

		// allow skipping out to show just lightmaps during development
		if ( r_lightmap->integer && ( pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap ) )
		{
			break;
		}

		if (backEnd.depthFill)
			break;
	}
}


static void RB_RenderShadowmap( shaderCommands_t *input )
{
	int deformGen;
	vec5_t deformParams;

	ComputeDeformValues(&deformGen, deformParams);

	{
		shaderProgram_t *sp = &tr.shadowmapShader[0];

		if (glState.vertexAnimation)
		{
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_VERTEX_ANIMATION];
		}
		else if (glState.boneAnimation)
		{
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_BONE_ANIMATION];
		}

		vec4_t vector;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.ori.transformMatrix);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		if (glState.boneAnimation)
		{
			GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
		}

		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		VectorCopy(backEnd.viewParms.ori.origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);
		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, backEnd.viewParms.zFar);

		GL_State( 0 );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		//
		// do multitexture
		//
		//if ( pStage->glslShaderGroup )
		{
			//
			// draw
			//

			R_DrawElements(input->numIndexes, input->firstIndex);
		}
	}
}



/*
** RB_StageIteratorGeneric
*/
/*
** RB_StageUsesVertexAlphaGen
**
** The distance and height fades vary per vertex, so they cannot be folded into
** the base/vertex colour pair the way the constant alphaGens are.
*/
static qboolean RB_StageUsesVertexAlphaGen( const shaderStage_t *pStage )
{
	switch ( pStage->alphaGen )
	{
		case AGEN_DIST_FADE:
		case AGEN_ONE_MINUS_DIST_FADE:
		case AGEN_TIKI_DIST_FADE:
		case AGEN_ONE_MINUS_TIKI_DIST_FADE:
		case AGEN_HEIGHT_FADE:
			return qtrue;
		default:
			return qfalse;
	}
}

/*
** RB_ComputeVertexAlphaGen
**
** MOH:AA fades vegetation and detail props out with distance rather than
** popping them, and fades some surfaces by height. Both are per vertex, and
** like the entity lighting they have to be written into the vertex colours
** before the batch is uploaded, so that both the generic and the lightall paths
** pick them up.
**
** The distance is measured from the viewer to the vertex in model space, which
** is what the GL1 renderer arrives at by transforming the view origin through
** the model's axes.
*/
static void RB_ComputeVertexAlphaGen( void )
{
	const shaderStage_t *pStage = NULL;
	float                distNear, distRange;
	vec3_t               localViewOrigin;
	int                  i;

	for ( i = 0; i < MAX_SHADER_STAGES; i++ ) {
		if ( !tess.xstages[i] ) {
			continue;
		}
		if ( RB_StageUsesVertexAlphaGen( tess.xstages[i] ) ) {
			pStage = tess.xstages[i];
			break;
		}
	}

	if ( !pStage ) {
		return;
	}

	VectorCopy( backEnd.ori.viewOrigin, localViewOrigin );

	distNear  = tess.shader->fDistNear;
	distRange = tess.shader->fDistRange;

	if ( pStage->alphaGen == AGEN_HEIGHT_FADE )
	{
		float alphaMin = pStage->alphaMin;
		float alphaMax = pStage->alphaMax;
		float span     = alphaMax - alphaMin;

		if ( span == 0.0f ) {
			return;
		}

		for ( i = 0; i < tess.numVertexes; i++ ) {
			float dist  = fabs( localViewOrigin[2] - tess.xyz[i][2] );
			float alpha;

			dist  = Q_clamp_float( dist, alphaMin, alphaMax );
			alpha = 1.0f - ( dist - alphaMin ) / span;

			tess.color[i][3] = (uint16_t)( alpha * 65535.0f );
		}
		return;
	}

	if ( distRange == 0.0f ) {
		// GL1 divides by the range regardless, so a zero range degenerates to a
		// hard on/off at fDistNear (e.g. static_tree1_1's "distFade 2304 0").
		// Match that instead of leaving the surface permanently opaque.
		for ( i = 0; i < tess.numVertexes; i++ ) {
			vec3_t org;
			float  alpha;

			VectorSubtract( tess.xyz[i], localViewOrigin, org );
			alpha = ( VectorLength( org ) >= distNear ) ? 1.0f : 0.0f;
			if ( pStage->alphaGen == AGEN_DIST_FADE || pStage->alphaGen == AGEN_TIKI_DIST_FADE ) {
				alpha = 1.0f - alpha;
			}
			tess.color[i][3] = (uint16_t)( alpha * 65535.0f );
		}
		return;
	}

	if ( pStage->alphaGen == AGEN_TIKI_DIST_FADE || pStage->alphaGen == AGEN_ONE_MINUS_TIKI_DIST_FADE )
	{
		// These fade the whole model together on its origin rather than per
		// vertex, so one value covers the batch.
		vec3_t org;
		float  frac, alpha;

		if ( backEnd.currentStaticModel ) {
			VectorSubtract( backEnd.currentStaticModel->origin, backEnd.viewParms.ori.origin, org );
		} else if ( backEnd.currentEntity ) {
			VectorSubtract( backEnd.currentEntity->e.origin, backEnd.viewParms.ori.origin, org );
		} else {
			// GL1 treats this as a shader authoring error; just leave it alone
			return;
		}

		frac  = ( VectorLength( org ) - distNear ) / distRange;
		frac  = Q_clamp_float( frac, 0.0f, 1.0f );
		alpha = ( pStage->alphaGen == AGEN_TIKI_DIST_FADE ) ? 1.0f - frac : frac;

		for ( i = 0; i < tess.numVertexes; i++ ) {
			tess.color[i][3] = (uint16_t)( alpha * 65535.0f );
		}
		return;
	}

	// AGEN_DIST_FADE / AGEN_ONE_MINUS_DIST_FADE, per vertex
	for ( i = 0; i < tess.numVertexes; i++ ) {
		vec3_t org;
		float  frac, alpha;

		VectorSubtract( tess.xyz[i], localViewOrigin, org );

		frac  = ( VectorLength( org ) - distNear ) / distRange;
		frac  = Q_clamp_float( frac, 0.0f, 1.0f );
		alpha = ( pStage->alphaGen == AGEN_DIST_FADE ) ? 1.0f - frac : frac;

		tess.color[i][3] = (uint16_t)( alpha * 65535.0f );
	}
}


/*
** RB_ComputeEntityLightColors
**
** MOH:AA evaluates spherical and light grid lighting per vertex on the CPU.
** The GL1 renderer did this into a scratch color array once per stage; GL2
** uploads vertex colors once per batch, so it has to happen here, before
** RB_UpdateTessVao, and the stages using rgbGen lightingSpherical / lightingGrid
** / static then just pass the vertex color through.
*/
static void RB_ComputeEntityLightColors( void )
{
	static byte colors[SHADER_MAX_VERTEXES][4];
	int         i;

	if ( !tess.shader->needsLSpherical && !tess.shader->needsLGrid ) {
		return;
	}

	// Light grid shading is always applied; only the spherical lights respect
	// r_drawspherelights, which is also what makes the backend fall back to the
	// grid for these shaders when it is off.
	if ( !tess.shader->needsLGrid && !r_drawspherelights->integer ) {
		return;
	}

	if ( !backEnd.currentStaticModel ) {
		if ( !backEnd.currentSphere || !backEnd.currentSphere->TessFunction ) {
			return;
		}

		backEnd.currentSphere->TessFunction( (unsigned char *)colors );

		for ( i = 0; i < tess.numVertexes; i++ ) {
			// 0-255 to 0-65535
			tess.color[i][0] = colors[i][0] * 257;
			tess.color[i][1] = colors[i][1] * 257;
			tess.color[i][2] = colors[i][2] * 257;
			tess.color[i][3] = colors[i][3] * 257;
		}

		return;
	}

	// Static props keep their baked vertex colors and, when the model asks for
	// it, take the dynamic lights affecting it on top.
	if ( backEnd.currentStaticModel->useSpecialLighting )
	{
		float *xyz    = ( float * ) tess.xyz;
		int16_t *packedNormal = tess.normal[0];

		for ( i = 0; i < tess.numVertexes; i++, xyz += 4, packedNormal += 4 )
		{
			vec3_t normal;
			vec3_t colorout;
			int    j, r, g, b;

			R_VaoUnpackNormal( normal, packedNormal );

			colorout[0] = tess.color[i][0] / 257.0f;
			colorout[1] = tess.color[i][1] / 257.0f;
			colorout[2] = tess.color[i][2] / 257.0f;

			for ( j = 0; j < backEnd.currentStaticModel->numdlights; j++ )
			{
				float     ooLightDistSquared;
				float     dot;
				vec3_t    diff;
				dlight_t *dl;

				dl = &backEnd.refdef.dlights[backEnd.currentStaticModel->dlights[j].index];
				VectorSubtract( backEnd.currentStaticModel->dlights[j].transformed, xyz, diff );

				dot = DotProduct( diff, normal );
				if ( dot >= 0 )
				{
					float ooLen = 1.0f / VectorLengthSquared( diff );

					ooLightDistSquared = dot * ( 7500.0f * dl->radius * ooLen * sqrt( ooLen ) );
					colorout[0] += dl->color[0] * ooLightDistSquared;
					colorout[1] += dl->color[1] * ooLightDistSquared;
					colorout[2] += dl->color[2] * ooLightDistSquared;
				}
			}

			r = colorout[0];
			g = colorout[1];
			b = colorout[2];

			if ( tr.overbrightShift )
			{
				r = (int)( (float)r * tr.overbrightMult );
				g = (int)( (float)g * tr.overbrightMult );
				b = (int)( (float)b * tr.overbrightMult );
			}

			// normalize by color rather than saturating to white
			if ( r > 0xFF || g > 0xFF || b > 0xFF )
			{
				float t = 255.0f / (float)Q_max( r, Q_max( g, b ) );

				r = (int)( (float)r * t );
				g = (int)( (float)g * t );
				b = (int)( (float)b * t );
			}

			tess.color[i][0] = r * 257;
			tess.color[i][1] = g * 257;
			tess.color[i][2] = b * 257;
			// alpha is left alone so vertices are not hidden
		}
	}
	else if ( tr.identityLight != 1.0f )
	{
		for ( i = 0; i < tess.numVertexes; i++ ) {
			tess.color[i][0] *= tr.identityLight;
			tess.color[i][1] *= tr.identityLight;
			tess.color[i][2] *= tr.identityLight;
		}
	}
}


void RB_StageIteratorGeneric( void )
{
	shaderCommands_t *input;
	unsigned int vertexAttribs = 0;

	input = &tess;
	
	if (!input->numVertexes || !input->numIndexes)
	{
		return;
	}

	// Split so the two halves of the internal-VAO path can be told apart: the
	// build is CPU work this renderer chose to do, the upload is what it then
	// costs to hand to the driver. r_vaoCache 0 routes static world geometry
	// through both of them, once per batch, once per pass, every frame.
	if (tess.useInternalVao)
	{
		R_CpuTimerBegin(CPUTIMER_TESSBUILD);
		RB_DeformTessGeometry();
		R_CpuTimerEnd(CPUTIMER_TESSBUILD);
	}

	vertexAttribs = RB_CalcShaderVertexAttribs( input );

	if (tess.useInternalVao)
	{
		R_CpuTimerBegin(CPUTIMER_TESSBUILD);
		RB_ComputeEntityLightColors();
		RB_ComputeVertexAlphaGen();
		R_CpuTimerEnd(CPUTIMER_TESSBUILD);

		R_CpuTimerBegin(CPUTIMER_TESSUPLOAD);
		RB_UpdateTessVao(vertexAttribs);
		R_CpuTimerEnd(CPUTIMER_TESSUPLOAD);
	}
	else
	{
		backEnd.pc.c_staticVaoDraws++;
	}

	//
	// log this call
	//
	if ( r_logFile->integer ) 
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment( va("--- RB_StageIteratorGeneric( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	if (input->shader->cullType == CT_TWO_SIDED)
	{
		GL_Cull( CT_TWO_SIDED );
	}
	else
	{
		qboolean cullFront = (input->shader->cullType == CT_FRONT_SIDED);

		if ( backEnd.viewParms.flags & VPF_DEPTHSHADOW )
			cullFront = !cullFront;

		if ( backEnd.viewParms.isMirror )
			cullFront = !cullFront;

		// Unlike Q3, GL1 does not flip culling for an entity placed with a
		// mirrored axis, and MOH:AA's content was built against GL1. Flipping
		// it here culls the faces that GL1 draws for such an entity.

		if (cullFront)
			GL_Cull( CT_FRONT_SIDED );
		else
			GL_Cull( CT_BACK_SIDED );
	}

	// set polygon offset if necessary
	if ( input->shader->polygonOffset )
	{
		qglEnable( GL_POLYGON_OFFSET_FILL );
	}

	//
	// render depth if in depthfill mode
	//
	if (backEnd.depthFill)
	{
		RB_IterateStagesGeneric( input );

		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset )
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}

		return;
	}

	//
	// render shadowmap if in shadowmap mode
	//
	if (backEnd.viewParms.flags & VPF_SHADOWMAP)
	{
		if ( input->shader->sort == SS_OPAQUE )
		{
			RB_RenderShadowmap( input );
		}
		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset )
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}

		return;
	}

	//
	//
	// call shader function
	//
	RB_IterateStagesGeneric( input );

	//
	// pshadows!
	//
	if (glRefConfig.framebufferObject && r_shadows->integer == 4 && tess.pshadowBits
		&& tess.shader->sort <= SS_OPAQUE && !(tess.shader->surfaceFlags & (SURF_NODLIGHT | SURF_SKY) ) ) {
		ProjectPshadowVBOGLSL();
	}


	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE && r_lightmap->integer == 0
		&& !(tess.shader->surfaceFlags & (SURF_NODLIGHT | SURF_SKY) ) ) {
		if (tess.shader->numUnfoggedPasses == 1 && tess.xstages[0]->glslShaderGroup == tr.lightallShader
			&& (tess.xstages[0]->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK) && r_dlightMode->integer)
		{
			ForwardDlight();
		}
		else
		{
			ProjectDlightTexture();
		}
	}

	//
	// now do fog
	//
	if ( tess.fogNum && tess.shader->fogPass ) {
		RB_FogPass();
	}

	//
	// reset polygon offset
	//
	if ( input->shader->polygonOffset )
	{
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}
}

/*
** RB_EndSurface
*/
void RB_EndSurface( void ) {
	shaderCommands_t *input;

	input = &tess;

	if (input->numIndexes == 0 || input->numVertexes == 0) {
		return;
	}

	if (input->indexes[SHADER_MAX_INDEXES-1] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_INDEXES hit");
	}	
	if (input->xyz[SHADER_MAX_VERTEXES-1][0] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_VERTEXES hit");
	}

	if ( tess.shader == tr.shadowShader ) {
		RB_ShadowTessEnd();
		return;
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if ( r_debugSort->integer && r_debugSort->integer < tess.shader->sort ) {
		return;
	}

	if (tess.useCacheVao)
	{
		// upload indexes now
		VaoCache_Commit();
	}

	//
	// update performance counters
	//
	backEnd.pc.c_shaders++;
	backEnd.pc.c_vertexes += tess.numVertexes;
	backEnd.pc.c_indexes += tess.numIndexes;
	backEnd.pc.c_totalIndexes += tess.numIndexes * tess.numPasses;

	//
	// call off to shader specific tess end function
	//
	tess.currentStageIteratorFunc();

	//
	// draw debugging stuff
	//
	if ( r_showtris->integer ) {
		DrawTris (input);
	}
	if ( r_shownormals->integer ) {
		DrawNormals (input);
	}
	// clear shader so we can tell we don't have any unclosed surfaces
	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.firstIndex = 0;
	tess.useCacheVao = qfalse;
	tess.useInternalVao = qfalse;

	GLimp_LogComment( "----------\n" );
}
