/*
===========================================================================
Copyright (C) 2011 Andrei Drexler, Richard Allen, James Canete

This file is part of Reaction source code.

Reaction source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Reaction source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Reaction source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "tr_local.h"

/*
=============
RB_EyeAdapt

Added in OPM: eye adaptation (r_autoExposure). The view's average light is
worked out on the card every few frames as ioq3 did (calclevels4x, down to a
texel), read back a frame or two later without waiting for it (a pixel buffer
and a fence), and the exposure eased toward what it asks for here, by the
clock and not by the frame: an eye gets used to the dark slowly and to the
light quicker, a part of the way, and so many stops at most. ioq3 blended 3% a
frame toward it, both ways alike, twice as fast at 144 fps as at 72. The tone
map reads the result as the level it always read (tr.calcLevelsImage), whose
white does not move with it (tonemap_fp.glsl).
=============
*/
static struct {
	GLuint   pbo;
	GLsync   fence;
	qboolean pending, haveAvg;
	float    logAvg; // the view's average light, log2, as light
	float    ev;     // the exposure, stops off the fixed one
	int      lastMs, printMs;
} eye;

static void RB_EyeAdapt(qboolean measured)
{
	const int now = ri.Milliseconds();
	float     dt, target, rate, shift, level;
	vec4_t    texel;

	if (!eye.pbo)
	{
		qglGenBuffers(1, &eye.pbo);
		qglBindBuffer(GL_PIXEL_PACK_BUFFER, eye.pbo);
		qglBufferData(GL_PIXEL_PACK_BUFFER, sizeof(vec4_t), NULL, GL_STREAM_READ);
		qglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		eye.lastMs = now;
	}

	// a reading the card has done
	if (eye.pending && qglClientWaitSync(eye.fence, 0, 0) != GL_TIMEOUT_EXPIRED)
	{
		const float *v;

		qglDeleteSync(eye.fence);
		eye.pending = qfalse;
		qglBindBuffer(GL_PIXEL_PACK_BUFFER, eye.pbo);
		v = qglMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sizeof(vec4_t), GL_MAP_READ_BIT);
		if (v)
		{
			// calclevels4x_fp: log2 of the luminance, kept as 0.05 x + 0.5;
			// a gamma frame's numbers are its light ^ 1 / 2.2
			eye.logAvg = (v[1] - 0.5f) * 20.0f * (R_RtLinear() ? 1.0f : 2.2f);
			qglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			if (!eye.haveAvg)
			{
				// the first look: as used to it already
				eye.ev = Com_Clamp(r_autoExposureMinEV->value, r_autoExposureMaxEV->value,
					r_autoExposureStrength->value * (r_autoExposureKey->value - eye.logAvg));
			}
			eye.haveAvg = qtrue;
		}
		qglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	}

	// a new one, not waited for
	if (measured && !eye.pending)
	{
		FBO_Bind(tr.targetLevelsFbo);
		qglBindBuffer(GL_PIXEL_PACK_BUFFER, eye.pbo);
		qglReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, NULL);
		qglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		eye.fence   = qglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		eye.pending = qtrue;
	}

	dt         = Com_Clamp(0.0f, 0.25f, (now - eye.lastMs) * 0.001f);
	eye.lastMs = now;
	if (eye.haveAvg)
	{
		target = Com_Clamp(r_autoExposureMinEV->value, r_autoExposureMaxEV->value,
			r_autoExposureStrength->value * (r_autoExposureKey->value - eye.logAvg));
		// brighter: into the dark, slowly
		rate   = target > eye.ev ? r_autoExposureDarkSpeed->value : r_autoExposureBrightSpeed->value;
		eye.ev += (target - eye.ev) * (1.0f - exp(-Q_max(rate, 0.0f) * dt));
	}

	// the level the tone map reads: it multiplies by 2 ^ -level, and the fixed
	// one is -1. A gamma frame's numbers move by the stops over 2.2.
	shift = R_RtLinear() ? eye.ev : eye.ev / 2.2f;
	level = (-1.0f - shift) * 0.05f + 0.5f;
	VectorSet4(texel, level, level, level, 1.0f);
	qglTextureSubImage2DEXT(tr.calcLevelsImage->texnum, GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, texel);

	if (r_autoExposureDebug->integer && now - eye.printMs > 1000)
	{
		eye.printMs = now;
		ri.Printf(PRINT_ALL, "eye adaptation: view %.2f stops (key %.2f), exposure %+.2f stops toward %+.2f\n",
			eye.logAvg, r_autoExposureKey->value, eye.ev,
			Com_Clamp(r_autoExposureMinEV->value, r_autoExposureMaxEV->value, r_autoExposureStrength->value * (r_autoExposureKey->value - eye.logAvg)));
	}
}

// Its buffer goes with the context (RE_Shutdown)
void RB_EyeAdaptShutdown(void)
{
	if (eye.pending)
		qglDeleteSync(eye.fence);
	if (eye.pbo)
		qglDeleteBuffers(1, &eye.pbo);
	Com_Memset(&eye, 0, sizeof(eye));
}

void RB_ToneMap(FBO_t *hdrFbo, ivec4_t hdrBox, FBO_t *ldrFbo, ivec4_t ldrBox, int autoExposure)
{
	ivec4_t srcBox, dstBox;
	vec4_t color;
	static int lastFrameCount = 0;

	if (autoExposure)
	{
		qboolean measured = qfalse;

		if (lastFrameCount == 0 || tr.frameCount < lastFrameCount || tr.frameCount - lastFrameCount > 5)
		{
			// determine average log luminance
			FBO_t *srcFbo, *dstFbo, *tmp;
			int size = 256;

			lastFrameCount = tr.frameCount;

			VectorSet4(dstBox, 0, 0, size, size);

			FBO_Blit(hdrFbo, hdrBox, NULL, tr.textureScratchFbo[0], dstBox, &tr.calclevels4xShader[0], NULL, 0);

			srcFbo = tr.textureScratchFbo[0];
			dstFbo = tr.textureScratchFbo[1];

			// downscale to 1x1 texture
			while (size > 1)
			{
				VectorSet4(srcBox, 0, 0, size, size);
				//size >>= 2;
				size >>= 1;
				VectorSet4(dstBox, 0, 0, size, size);

				if (size == 1)
					dstFbo = tr.targetLevelsFbo;

				//FBO_Blit(targetFbo, srcBox, NULL, tr.textureScratchFbo[nextScratch], dstBox, &tr.calclevels4xShader[1], NULL, 0);
				FBO_FastBlit(srcFbo, srcBox, dstFbo, dstBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);

				tmp = srcFbo;
				srcFbo = dstFbo;
				dstFbo = tmp;
			}
			measured = qtrue;
		}

		// (OPM: ioq3 blended 3% a frame toward it here)
		RB_EyeAdapt(measured);
	}

	// tonemap (OPM: ioq3 took a stop off with its eye adaptation; ours
	// starts from the fixed exposure, RB_EyeAdapt)
	color[0] =
	color[1] =
	color[2] = pow(2, r_cameraExposure->value); //exp2(r_cameraExposure->value);
	color[3] = 1.0f;
	// Added in OPM: r_linearLighting's own exposure (tonemap_fp.glsl)
	if (R_RtLinear())
		VectorScale(color, pow(2, r_linearExposure->value), color);

	if (autoExposure)
		GL_BindToTMU(tr.calcLevelsImage,  TB_LEVELSMAP);
	else
		GL_BindToTMU(tr.fixedLevelsImage, TB_LEVELSMAP);

	// Added in OPM: the grade, done by the same program (r_grade, tr_atmos.c)
	{
		vec4_t lift, gamma, gain, misc;

		R_GradeUniforms(lift, gamma, gain, misc);
		GLSL_SetUniformVec4(&tr.tonemapShader, UNIFORM_GRADELIFT, lift);
		GLSL_SetUniformVec4(&tr.tonemapShader, UNIFORM_GRADEGAMMA, gamma);
		GLSL_SetUniformVec4(&tr.tonemapShader, UNIFORM_GRADEGAIN, gain);
		GLSL_SetUniformVec4(&tr.tonemapShader, UNIFORM_GRADEMISC, misc);
	}

	FBO_Blit(hdrFbo, hdrBox, NULL, ldrFbo, ldrBox, &tr.tonemapShader, color, 0);
}

/*
=============
RB_BokehBlur


Blurs a part of one framebuffer to another.

Framebuffers can be identical. 
=============
*/
void RB_BokehBlur(FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, float blur)
{
//	ivec4_t srcBox, dstBox;
	vec4_t color;
	
	blur *= 10.0f;

	if (blur < 0.004f)
		return;

	if (glRefConfig.framebufferObject)
	{
		// bokeh blur
		if (blur > 0.0f)
		{
			ivec4_t quarterBox;

			quarterBox[0] = 0;
			quarterBox[1] = tr.quarterFbo[0]->height;
			quarterBox[2] = tr.quarterFbo[0]->width;
			quarterBox[3] = -tr.quarterFbo[0]->height;

			// create a quarter texture
			//FBO_Blit(NULL, NULL, NULL, tr.quarterFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit(src, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}

#ifndef HQ_BLUR
		if (blur > 1.0f)
		{
			// create a 1/16th texture
			//FBO_Blit(tr.quarterFbo[0], NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit(tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
#endif

		if (blur > 0.0f && blur <= 1.0f)
		{
			// Crossfade original with quarter texture
			VectorSet4(color, 1, 1, 1, blur);

			FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}
#ifndef HQ_BLUR
		// ok blur, but can see some pixelization
		else if (blur > 1.0f && blur <= 2.0f)
		{
			// crossfade quarter texture with 1/16th texture
			FBO_Blit(tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, NULL, 0);

			VectorSet4(color, 1, 1, 1, blur - 1.0f);

			FBO_Blit(tr.textureScratchFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		}
		else if (blur > 2.0f)
		{
			// blur 1/16th texture then replace
			int i;

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = ((blur - 2.0f) / 2.0f) / 3.0f * (float)(i + 1);

				blurTexScale[0] =
				blurTexScale[1] = subblur;

				color[0] =
				color[1] =
				color[2] = 0.5f;
				color[3] = 1.0f;

				if (i != 0)
					FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
				else
					FBO_Blit(tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, 0);
			}

			FBO_Blit(tr.textureScratchFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0);
		}
#else // higher quality blur, but slower
		else if (blur > 1.0f)
		{
			// blur quarter texture then replace
			int i;

			src = tr.quarterFbo[0];
			dst = tr.quarterFbo[1];

			VectorSet4(color, 0.5f, 0.5f, 0.5f, 1);

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = (blur - 1.0f) / 2.0f * (float)(i + 1);

				blurTexScale[0] =
				blurTexScale[1] = subblur;

				color[0] =
				color[1] =
				color[2] = 1.0f;
				if (i != 0)
					color[3] = 1.0f;
				else
					color[3] = 0.5f;

				FBO_Blit(tr.quarterFbo[0], NULL, blurTexScale, tr.quarterFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
			}

			FBO_Blit(tr.quarterFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0);
		}
#endif
	}
}


static void RB_RadialBlur(FBO_t *srcFbo, FBO_t *dstFbo, int passes, float stretch, float x, float y, float w, float h, float xcenter, float ycenter, float alpha)
{
	ivec4_t srcBox, dstBox;
	int srcWidth, srcHeight;
	vec4_t color;
	const float inc = 1.f / passes;
	const float mul = powf(stretch, inc);
	float scale;

	alpha *= inc;
	VectorSet4(color, alpha, alpha, alpha, 1.0f);

	srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
	srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

	VectorSet4(srcBox, 0, 0, srcWidth, srcHeight);

	VectorSet4(dstBox, x, y, w, h);
	FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0);

	--passes;
	scale = mul;
	while (passes > 0)
	{
		float iscale = 1.f / scale;
		float s0 = xcenter * (1.f - iscale);
		float t0 = (1.0f - ycenter) * (1.f - iscale);

		srcBox[0] = s0 * srcWidth;
		srcBox[1] = t0 * srcHeight;
		srcBox[2] = iscale * srcWidth;
		srcBox[3] = iscale * srcHeight;
			
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

		scale *= mul;
		--passes;
	}
}


/*
=============
RB_FocusBlur

Added in OPM: holding the breath aiming down the sights (the client's cl_breathHold,
0 to 1) blurs what is round the sights: the frame averaged with copies of itself zoomed
a little more each about its middle, the middle sharp and the edges smeared, by up to
r_breathBlur (the last copy's zoom less one).
=============
*/
void RB_FocusBlur(FBO_t *srcFbo, ivec4_t srcBox)
{
	static cvar_t *hold, *lens;
	float amount;

	if (!hold)
		hold = ri.Cvar_Get("cl_breathHold", "0", CVAR_ROM);
	if (!lens)
		lens = ri.Cvar_Get("cl_scopeLens", "0", 0);

	// and through a scope (cgame's cl_scopeLens, 0 to 1) the lens softens toward its rim
	amount = hold->value * r_breathBlur->value + lens->value * r_scopeBlur->value;
	if (!srcFbo || !tr.screenScratchFbo || amount < 0.002f)
		return;

	FBO_FastBlit(srcFbo, srcBox, tr.screenScratchFbo, srcBox, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	RB_RadialBlur(tr.screenScratchFbo, srcFbo, 8, 1.0f + amount, srcBox[0], srcBox[1], srcBox[2], srcBox[3], 0.5f, 0.5f, 1.0f);
}

static qboolean RB_UpdateSunFlareVis(void)
{
	GLuint sampleCount = 0;
	if (!glRefConfig.occlusionQuery)
		return qtrue;

	tr.sunFlareQueryIndex ^= 1;
	if (!tr.sunFlareQueryActive[tr.sunFlareQueryIndex])
		return qtrue;

	/* debug code */
	if (0)
	{
		int iter;
		for (iter=0 ; ; ++iter)
		{
			GLint available = 0;
			qglGetQueryObjectiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT_AVAILABLE, &available);
			if (available)
				break;
		}

		ri.Printf(PRINT_DEVELOPER, "Waited %d iterations\n", iter);
	}
	
	// Note: On desktop OpenGL this is a sample count (glRefConfig.occlusionQueryTarget == GL_SAMPLES_PASSED)
	// but on OpenGL ES this is a boolean (glRefConfig.occlusionQueryTarget == GL_ANY_SAMPLES_PASSED)
	qglGetQueryObjectuiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT, &sampleCount);
	return sampleCount > 0;
}

void RB_SunRays(FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox)
{
	vec4_t color;
	float dot;
	const float cutoff = 0.25f;
	qboolean colorize = qtrue;

//	float w, h, w2, h2;
	mat4_t mvp;
	vec4_t pos, hpos;

	dot = DotProduct(tr.sunDirection, backEnd.viewParms.ori.axis[0]);
	if (dot < cutoff)
		return;

	if (!RB_UpdateSunFlareVis())
		return;

	// From RB_DrawSun()
	{
		float dist;
		mat4_t trans, model;

		Mat4Translation( backEnd.viewParms.ori.origin, trans );
		Mat4Multiply( backEnd.viewParms.world.modelMatrix, trans, model );
		Mat4Multiply(backEnd.viewParms.projectionMatrix, model, mvp);

		dist = backEnd.viewParms.zFar / 1.75;		// div sqrt(3)

		VectorScale( tr.sunDirection, dist, pos );
	}

	// project sun point
	//Mat4Multiply(backEnd.viewParms.projectionMatrix, backEnd.viewParms.world.modelMatrix, mvp);
	Mat4Transform(mvp, pos, hpos);

	// transform to UV coords
	hpos[3] = 0.5f / hpos[3];

	pos[0] = 0.5f + hpos[0] * hpos[3];
	pos[1] = 0.5f + hpos[1] * hpos[3];

	// initialize quarter buffers
	{
		float mul = 1.f;
		ivec4_t rayBox, quarterBox;
		int srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
		int srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

		VectorSet4(color, mul, mul, mul, 1);

		rayBox[0] = srcBox[0] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[1] = srcBox[1] * tr.sunRaysFbo->height / srcHeight;
		rayBox[2] = srcBox[2] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[3] = srcBox[3] * tr.sunRaysFbo->height / srcHeight;

		quarterBox[0] = 0;
		quarterBox[1] = tr.quarterFbo[0]->height;
		quarterBox[2] = tr.quarterFbo[0]->width;
		quarterBox[3] = -tr.quarterFbo[0]->height;

		// first, downsample the framebuffer
		if (colorize)
		{
			FBO_FastBlit(srcFbo, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			FBO_Blit(tr.sunRaysFbo, rayBox, NULL, tr.quarterFbo[0], quarterBox, NULL, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO);
		}
		else
		{
			FBO_FastBlit(tr.sunRaysFbo, rayBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
	}

	// radial blur passes, ping-ponging between the two quarter-size buffers
	{
		const float stretch_add = 2.f/3.f;
		float stretch = 1.f + stretch_add;
		int i;
		for (i=0; i<2; ++i)
		{
			RB_RadialBlur(tr.quarterFbo[i&1], tr.quarterFbo[(~i) & 1], 5, stretch, 0.f, 0.f, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height, pos[0], pos[1], 1.125f);
			stretch += stretch_add;
		}
	}
	
	// add result back on top of the main buffer
	{
		float mul = 1.f;

		VectorSet4(color, mul, mul, mul, 1);

		FBO_Blit(tr.quarterFbo[0], NULL, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}
}

static void RB_BlurAxis(FBO_t *srcFbo, FBO_t *dstFbo, float strength, qboolean horizontal)
{
	float dx, dy;
	float xmul, ymul;
	float weights[3] = {
		0.227027027f,
		0.316216216f,
		0.070270270f,
	};
	float offsets[3] = {
		0.f,
		1.3846153846f,
		3.2307692308f,
	};

	xmul = horizontal;
	ymul = 1.f - xmul;

	xmul *= strength;
	ymul *= strength;

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4(color, weights[0], weights[0], weights[0], 1.0f);
		VectorSet4(srcBox, 0, 0, srcFbo->width, srcFbo->height);
		VectorSet4(dstBox, 0, 0, dstFbo->width, dstFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0);

		VectorSet4(color, weights[1], weights[1], weights[1], 1.0f);
		dx = offsets[1] * xmul;
		dy = offsets[1] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

		VectorSet4(color, weights[2], weights[2], weights[2], 1.0f);
		dx = offsets[2] * xmul;
		dy = offsets[2] * ymul;
		VectorSet4(srcBox, dx, dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		VectorSet4(srcBox, -dx, -dy, srcFbo->width, srcFbo->height);
		FBO_Blit(srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}
}

static void RB_HBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qtrue);
}

static void RB_VBlur(FBO_t *srcFbo, FBO_t *dstFbo, float strength)
{
	RB_BlurAxis(srcFbo, dstFbo, strength, qfalse);
}

void RB_GaussianBlur(FBO_t *srcFbo, FBO_t *dstFbo, float blur)
{
	//float mul = 1.f;
	float factor = Com_Clamp(0.f, 1.f, blur);

	if (factor <= 0.f)
		return;

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4(color, 1, 1, 1, 1);

		// first, downsample the framebuffer
		FBO_FastBlit(srcFbo, NULL, tr.quarterFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		FBO_FastBlit(tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR);

		// set the alpha channel
		qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		FBO_BlitFromTexture(tr.whiteImage, NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, color, GLS_DEPTHTEST_DISABLE);
		qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		// blur the tiny buffer horizontally and vertically
		RB_HBlur(tr.textureScratchFbo[0], tr.textureScratchFbo[1], factor);
		RB_VBlur(tr.textureScratchFbo[1], tr.textureScratchFbo[0], factor);

		// finally, merge back to framebuffer
		VectorSet4(srcBox, 0, 0, tr.textureScratchFbo[0]->width, tr.textureScratchFbo[0]->height);
		VectorSet4(dstBox, 0, 0, glConfig.vidWidth,              glConfig.vidHeight);
		color[3] = factor;
		FBO_Blit(tr.textureScratchFbo[0], srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
	}
}

/*
=============
RB_Bloom

Added in OPM
What is brighter than the scene's white glows into what is round it, as
through a lens: a lamp's bulb, the sun off a window, a muzzle flash. Before
the tone map, from the frame's float image: its bright part halved
(bloom_fp.glsl), halved again down the levels, then widened back up them,
each added onto the next bigger, and the biggest onto the frame, scaled by
r_bloom.
=============
*/
cvar_t *r_bloom;
cvar_t *r_bloomThreshold;
cvar_t *r_bloomKnee;
cvar_t *r_bloomEmissive;
cvar_t *r_sharpen;
cvar_t *r_smaa;

void R_BloomRegisterCvars(void)
{
	r_bloom = ri.Cvar_Get("r_bloom", "0.15", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_bloom, "How much what is brighter than white glows into what is round it (0: no bloom; needs r_hdr and r_postProcess)");
	r_bloomThreshold = ri.Cvar_Get("r_bloomThreshold", "1.2", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_bloomThreshold, "How bright a pixel must be, before the tone map, to glow (1: what the tone map makes white)");
	r_sharpen = ri.Cvar_Get("r_sharpen", "0.3", CVAR_ARCHIVE);
	// Added in OPM: SMAA 1x (tr_smaa.c)
	r_smaa = ri.Cvar_Get("r_smaa", "0", CVAR_ARCHIVE | CVAR_LATCH);
	ri.Cvar_SetDescription(r_smaa, "SMAA anti-aliasing after the tone map (needs r_hdr 1): 0 off, 1 low, 2 medium, 3 high, 4 ultra quality. "
		"Smooths alpha tested fences and foliage and edges inside textures that MSAA leaves; about half the cost of MSAA 2x. "
		"Can be used with or instead of r_ext_framebuffer_multisample. -1 shows the edges found, -2 the blend weights");
	ri.Cvar_SetDescription(r_sharpen, "Sharpens the tone mapped frame where its contrast leaves room, without halos (0 to 1; 0: off; needs r_toneMap)");
	r_bloomEmissive = ri.Cvar_Get("r_bloomEmissive", "2", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_bloomEmissive, "How many times brighter than white the lamps' bulbs, flames and glass are, so that they bloom (1: as white as they were)");
	r_bloomKnee = ri.Cvar_Get("r_bloomKnee", "0.5", CVAR_ARCHIVE);
	ri.Cvar_SetDescription(r_bloomKnee, "How softly the glow starts below r_bloomThreshold");
}

static void RB_BloomPass(image_t *src, const vec4_t srcBox, FBO_t *dst, float mode, float scale, int blend)
{
	shaderProgram_t *sp = &tr.bloomShader;
	vec4_t           quadVerts[4], params, texel;
	vec2_t           texCoords[4];
	const float      x0 = srcBox[0], y0 = srcBox[1], x1 = srcBox[2], y1 = srcBox[3];

	FBO_Bind(dst);
	qglViewport(0, 0, dst->width, dst->height);
	qglScissor(0, 0, dst->width, dst->height);

	VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet2(texCoords[0], x0, y1);
	VectorSet2(texCoords[1], x1, y1);
	VectorSet2(texCoords[2], x1, y0);
	VectorSet2(texCoords[3], x0, y0);

	GLSL_BindProgram(sp);
	GL_BindToTMU(src, TB_COLORMAP);
	VectorSet4(params, mode, r_bloomThreshold->value, r_bloomKnee->value, scale);
	GLSL_SetUniformVec4(sp, UNIFORM_BLOOMPARAMS, params);
	// half a texel in from the edge of what is read, past which is the clamp
	VectorSet4(texel, 1.0f / src->width, 1.0f / src->height, x1 - 0.5f / src->width, y1 - 0.5f / src->height);
	GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, texel);

	GL_State(GLS_DEPTHTEST_DISABLE | blend);
	GL_Cull(CT_TWO_SIDED);
	RB_InstantQuad2(quadVerts, texCoords);
}

void RB_Bloom(FBO_t *fbo, ivec4_t box)
{
	image_t *frame;
	vec4_t   whole = { 0, 0, 1, 1 }, part;
	int      i;

	if (r_bloom->value <= 0 || !tr.bloomFbo[0] || !tr.bloomShader.program || !fbo || !fbo->colorImage[0]
		|| (backEnd.refdef.rdflags & RDF_NOWORLDMODEL) || backEnd.viewParms.isPortal) {
		return;
	}
	frame = fbo->colorImage[0];

	// the view's part of the frame
	VectorSet4(part, box[0] / (float)frame->width, box[1] / (float)frame->height,
		(box[0] + box[2]) / (float)frame->width, (box[1] + box[3]) / (float)frame->height);

	RB_BloomPass(frame, part, tr.bloomFbo[0], 0.0f, 1.0f, 0);
	for (i = 1; i < BLOOM_LEVELS; i++) {
		RB_BloomPass(tr.bloomImage[i - 1], whole, tr.bloomFbo[i], 1.0f, 1.0f, 0);
	}
	for (i = BLOOM_LEVELS - 1; i > 0; i--) {
		RB_BloomPass(tr.bloomImage[i], whole, tr.bloomFbo[i - 1], 2.0f, 1.0f, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}

	// onto the view, widened once more
	{
		shaderProgram_t *sp = &tr.bloomShader;
		vec4_t           quadVerts[4], params, texel;
		vec2_t           texCoords[4];

		FBO_Bind(fbo);
		qglViewport(box[0], box[1], box[2], box[3]);
		qglScissor(box[0], box[1], box[2], box[3]);
		VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
		VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
		VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
		VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
		VectorSet2(texCoords[0], 0, 1);
		VectorSet2(texCoords[1], 1, 1);
		VectorSet2(texCoords[2], 1, 0);
		VectorSet2(texCoords[3], 0, 0);
		GLSL_BindProgram(sp);
		GL_BindToTMU(tr.bloomImage[0], TB_COLORMAP);
		VectorSet4(params, 2.0f, r_bloomThreshold->value, r_bloomKnee->value, r_bloom->value);
		GLSL_SetUniformVec4(sp, UNIFORM_BLOOMPARAMS, params);
		VectorSet4(texel, 1.0f / tr.bloomImage[0]->width, 1.0f / tr.bloomImage[0]->height, 1.0f - 0.5f / tr.bloomImage[0]->width, 1.0f - 0.5f / tr.bloomImage[0]->height);
		GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, texel);
		GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		GL_Cull(CT_TWO_SIDED);
		RB_InstantQuad2(quadVerts, texCoords);
	}
}

/*
=============
RB_Sharpen

Added in OPM
Copies the tone mapped frame back to the view, sharpened (bloom_fp.glsl's
Sharpen, after AMD's contrast adaptive sharpening): the textures' detail
the MSAA resolve and the upscaled textures' filtering soften. Returns qfalse
when it is off, for the plain copy.
=============
*/
qboolean RB_Sharpen(FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox)
{
	shaderProgram_t *sp = &tr.bloomShader;
	vec4_t           quadVerts[4], params, texel;
	vec2_t           texCoords[4];
	image_t         *image;
	float            x0, y0, x1, y1;

	if (r_sharpen->value <= 0 || !sp->program || !src || !dst || !src->colorImage[0]) {
		return qfalse;
	}
	image = src->colorImage[0];
	x0 = srcBox[0] / (float)image->width;
	y0 = srcBox[1] / (float)image->height;
	x1 = (srcBox[0] + srcBox[2]) / (float)image->width;
	y1 = (srcBox[1] + srcBox[3]) / (float)image->height;

	FBO_Bind(dst);
	qglViewport(dstBox[0], dstBox[1], dstBox[2], dstBox[3]);
	qglScissor(dstBox[0], dstBox[1], dstBox[2], dstBox[3]);
	VectorSet4(quadVerts[0], -1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[1], 1.0f, 1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[2], 1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet4(quadVerts[3], -1.0f, -1.0f, 0.0f, 1.0f);
	VectorSet2(texCoords[0], x0, y1);
	VectorSet2(texCoords[1], x1, y1);
	VectorSet2(texCoords[2], x1, y0);
	VectorSet2(texCoords[3], x0, y0);

	GLSL_BindProgram(sp);
	GL_BindToTMU(image, TB_COLORMAP);
	VectorSet4(params, 3.0f, 0, 0, r_sharpen->value);
	GLSL_SetUniformVec4(sp, UNIFORM_BLOOMPARAMS, params);
	VectorSet4(texel, 1.0f / image->width, 1.0f / image->height, x1 - 0.5f / image->width, y1 - 0.5f / image->height);
	GLSL_SetUniformVec4(sp, UNIFORM_BLOOMTEXEL, texel);
	GL_State(GLS_DEPTHTEST_DISABLE);
	GL_Cull(CT_TWO_SIDED);
	RB_InstantQuad2(quadVerts, texCoords);
	return qtrue;
}

/*
=============
RB_SunDepthProbe

Added in OPM: the sun's flare and its glare were hidden only by the world's
solid brushes (SunCheckRay), so they shone on under every tree. Whether the
sun reaches the eye is read from its nearest shadow cascade instead, which has
the leaves: a block of it round a point just sunward of the eye, its share
that is lit how much of the sun shows (none under a crown, some under the
holes in it), on the screen or above it alike. Taken back a frame later
without waiting for it (a pixel buffer and a fence), as RB_EyeAdapt does.
=============
*/
#define SUN_PROBE 8

static struct {
    GLuint   pbo;
    GLsync   fence;
    qboolean pending;
    int      count;   // the texels the reading has
    float    depth;   // the point's own depth in the cascade
    float    open;    // the share of it lit, last read
    float    shown;   // eased toward it
    int      lastMs;
} sunProbe = { 0, 0, qfalse, 0, 0.0f, 1.0f, 1.0f, 0 };

void RB_SunDepthProbe(void)
{
    static cvar_t *sunProbeDebug;
    const float   *mvp = backEnd.refdef.sunShadowMvp[0];
    FBO_t         *fbo = tr.sunShadowFbo[0];
    vec4_t         point, clip;
    int            i, j, x, y, size;

    if (!sunProbeDebug) {
        sunProbeDebug = ri.Cvar_Get("r_sunProbeDebug", "0", 0);
        ri.Cvar_SetDescription(sunProbeDebug, "Print how much of the sun's light reaches the eye past leaves and the like each time it is read, for its flare (RB_SunDepthProbe)");
    }
    if (!s_sun.exists || backEnd.viewParms.isPortal || backEnd.viewParms.isMirror || backEnd.viewParms.isPortalSky
        || !fbo || !glRefConfig.framebufferObject) {
        return;
    }
    // (no sun shadows this frame: nothing to tell, as before)
    if (!r_sunlightMode->integer || !(backEnd.viewParms.flags & VPF_USESUNLIGHT)) {
        sunProbe.open = 1.0f;
        return;
    }

    if (!sunProbe.pbo) {
        qglGenBuffers(1, &sunProbe.pbo);
        qglBindBuffer(GL_PIXEL_PACK_BUFFER, sunProbe.pbo);
        qglBufferData(GL_PIXEL_PACK_BUFFER, SUN_PROBE * SUN_PROBE * sizeof(float), NULL, GL_STREAM_READ);
        qglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    // a reading the card has done
    if (sunProbe.pending && qglClientWaitSync(sunProbe.fence, 0, 0) != GL_TIMEOUT_EXPIRED) {
        const float *d;

        qglDeleteSync(sunProbe.fence);
        sunProbe.pending = qfalse;
        qglBindBuffer(GL_PIXEL_PACK_BUFFER, sunProbe.pbo);
        d = (const float *)qglMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sunProbe.count * sizeof(float), GL_MAP_READ_BIT);
        if (d) {
            int lit = 0;

            for (i = 0; i < sunProbe.count; i++) {
                lit += d[i] >= sunProbe.depth - 0.0005f;
            }
            sunProbe.open = sunProbe.count ? (float)lit / sunProbe.count : 1.0f;
            qglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            if (sunProbeDebug->integer) {
                ri.Printf(PRINT_ALL, "sun probe: %d of %d lit (depth %f, first %f)\n", lit, sunProbe.count,
                          sunProbe.depth, sunProbe.count ? d[0] : 0.0f);
            }
        }
        qglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    if (sunProbe.pending) {
        return;
    }

    // a little toward the sun from the eye: clear of the player's own head
    VectorMA(backEnd.viewParms.ori.origin, 24.0f, s_sun.flaredirection, point);
    point[3] = 1.0f;
    VectorClear4(clip);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            clip[i] += mvp[i + j * 4] * point[j];
        }
    }
    if (clip[3] == 0.0f || fabs(clip[0]) >= fabs(clip[3]) || fabs(clip[1]) >= fabs(clip[3])
        || fabs(clip[2]) >= fabs(clip[3])) {
        sunProbe.open = 1.0f; // (out of the cascade: nothing to tell)
        return;
    }
    size = Q_min(SUN_PROBE, Q_min(fbo->width, fbo->height));
    x    = Com_Clamp(0, fbo->width - size, (int)((clip[0] / clip[3] * 0.5f + 0.5f) * fbo->width) - size / 2);
    y    = Com_Clamp(0, fbo->height - size, (int)((clip[1] / clip[3] * 0.5f + 0.5f) * fbo->height) - size / 2);

    // a new one, not waited for
    {
        FBO_t *was = glState.currentFBO;

        FBO_Bind(fbo);
        qglBindBuffer(GL_PIXEL_PACK_BUFFER, sunProbe.pbo);
        qglReadPixels(x, y, size, size, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        qglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        FBO_Bind(was);
    }
    sunProbe.count   = size * size;
    sunProbe.depth   = clip[2] / clip[3] * 0.5f + 0.5f;
    sunProbe.fence   = qglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    sunProbe.pending = qtrue;
}

/*
=============
R_SunShown

How much of the sun shows past what is drawn (RB_SunDepthProbe), eased a
little so a flare does not flicker as leaves sway across it.
=============
*/
float R_SunShown(void)
{
    const int now = ri.Milliseconds();
    float     dt  = Com_Clamp(0.0f, 0.25f, (now - sunProbe.lastMs) * 0.001f);

    sunProbe.lastMs = now;
    sunProbe.shown += (sunProbe.open - sunProbe.shown) * (1.0f - exp(-12.0f * dt));
    return sunProbe.shown;
}
