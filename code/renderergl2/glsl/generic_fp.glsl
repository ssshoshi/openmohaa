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


void main()
{
	vec4 color  = texture2D(u_DiffuseMap, var_DiffuseTex);

	float alpha = color.a * var_Color.a;
	vec3  rgb   = color.rgb * var_Color.rgb;

#if defined(USE_TCGEN)
	// As GL1's texture environment does it: the first texture takes the
	// vertex colour, then the second is modulated or added on top. Adding
	// still multiplies alpha.
	if (u_Texture1Env != 0)
	{
		vec4 color2 = texture2D(u_LightMap, var_Tex2);

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
	
	gl_FragColor.rgb = rgb;
	gl_FragColor.a = alpha;

	// MOH:AA global distance fog, linear between the two params. When it is off
	// the renderer sets the range scale to zero, so the factor stays at zero.
	float fogFactor = clamp((var_FogDist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0);
	gl_FragColor.rgb = mix(gl_FragColor.rgb, u_GlobalFogColor.rgb, fogFactor * u_GlobalFogColor.a);
}
