uniform sampler2D u_DiffuseMap;

#if defined(USE_TCGEN)
// MOH:AA's nextBundle: 1 modulates the second texture in, 2 adds it
uniform sampler2D u_LightMap;
uniform int       u_Texture1Env;
varying vec2      var_Tex2;
#endif

uniform int       u_AlphaTest;

uniform vec4      u_GlobalFogColor;
uniform vec2      u_GlobalFogParams;

varying vec2      var_DiffuseTex;

varying vec4      var_Color;
varying float     var_FogDist;

#if defined(USE_RTLIGHT)
varying vec3      var_RtPos;
varying vec3      var_RtNormal;
#endif
#if defined(USE_RTLIGHT_PARTICLES)
varying vec3      var_RtParticleLight;
#endif

// Added in OPM: soft particles. u_SoftParticle is the distance to fade over
// (0: none), 1 to fade the colour (added on top) rather than the alpha, and the
// view's near and far planes; u_ScreenDepthMap the view's depth after the prepass
#if defined(USE_SOFTPARTICLES)
uniform sampler2D u_ScreenDepthMap;
uniform vec4      u_SoftParticle;

float SoftLinearDepth(float d)
{
	float n = u_SoftParticle.z, f = u_SoftParticle.w;
	return 2.0 * n * f / (f + n - (2.0 * d - 1.0) * (f - n));
}
#endif


void main()
{
	vec4 color  = texture2D(u_DiffuseMap, var_DiffuseTex);

	float alpha = color.a * var_Color.a;
	vec3  rgb   = color.rgb * var_Color.rgb;

#if defined(USE_RTLIGHT)
	// Added in OPM: the realtime lights (see RT_STAGE_* in tr_local.h)
	vec3 rtLight = vec3(0.0);
  #if defined(USE_RTLIGHT_PARTICLES)
	// dust and smoke were lit at their corners
	if (u_RtStage == RT_STAGE_PARTICLE || u_RtStage == RT_STAGE_PARTICLE_LIT)
		rtLight = var_RtParticleLight;
	else
  #endif
	if (u_RtStage != RT_STAGE_NONE)
		rtLight = RtLight(var_RtPos, normalize(gl_FrontFacing ? var_RtNormal : -var_RtNormal),
			u_RtStage != RT_STAGE_LIT && u_RtParams.y < 1.5);

	if (u_RtStage == RT_STAGE_LIGHTING)
	{
		// the texture is the lighting: all of it live, the ambient stands in
		if (u_RtParams.y > 1.5)
			color.rgb = u_RtAmbient.rgb;
		rgb = RtDelta(color.rgb, rtLight) * var_Color.rgb;
	}
	else if (u_RtStage == RT_STAGE_LIT || u_RtStage == RT_STAGE_PARTICLE_LIT)
	{
		// lit by its colour, worked out at run time without the lights
		rgb = color.rgb * (var_Color.rgb + rtLight);
	}
	else if (u_RtStage == RT_STAGE_LIT_BAKED || u_RtStage == RT_STAGE_PARTICLE)
	{
		// lit by a colour the map baked: all of it live, the ambient stands in
		vec3 base = u_RtParams.y > 1.5 ? u_RtAmbient.rgb : var_Color.rgb;
		rgb = color.rgb * RtDelta(base, rtLight);
	}
#endif

#if defined(USE_TCGEN)
	// As GL1's texture environment does it: the first texture takes the
	// vertex colour, then the second is modulated or added on top. Adding
	// still multiplies alpha.
	if (u_Texture1Env != 0)
	{
		vec4 color2 = texture2D(u_LightMap, var_Tex2);

  #if defined(USE_RTLIGHT)
		// the second texture is the lighting
		if (u_RtStage == RT_STAGE_LIGHTING2)
		{
			if (u_RtParams.y > 1.5)
				color2.rgb = u_RtAmbient.rgb;
			color2.rgb = RtDelta(color2.rgb, RtLight(var_RtPos, normalize(gl_FrontFacing ? var_RtNormal : -var_RtNormal), u_RtParams.y < 1.5));
		}
  #endif

		if (u_Texture1Env == 2)
			rgb += color2.rgb;
		else
			rgb *= color2.rgb;

		alpha *= color2.a;
	}
#endif

	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}
	
#if defined(USE_SOFTPARTICLES)
	if (u_SoftParticle.x > 0.0)
	{
		float behind = SoftLinearDepth(texture2D(u_ScreenDepthMap, gl_FragCoord.xy * r_FBufScale).r);
		float fade   = clamp((behind - SoftLinearDepth(gl_FragCoord.z)) / u_SoftParticle.x, 0.0, 1.0);
		if (u_SoftParticle.y > 0.5)
			rgb *= fade;
		else
			alpha *= fade;
	}
#endif

	gl_FragColor.rgb = rgb;
	gl_FragColor.a = alpha;

	// MOH:AA global distance fog, linear between the two params. When it is off
	// the renderer sets the range scale to zero, so the factor stays at zero.
	float fogFactor = clamp((var_FogDist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0);
	gl_FragColor.rgb = mix(gl_FragColor.rgb, u_GlobalFogColor.rgb, fogFactor * u_GlobalFogColor.a);
}
