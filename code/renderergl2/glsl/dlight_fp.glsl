uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;

varying vec2      var_Tex1;
varying vec4      var_Color;


void main()
{
	vec4 color = texture2D(u_DiffuseMap, var_Tex1);

	float alpha = color.a * var_Color.a;
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
	
	gl_FragColor.rgb = color.rgb * var_Color.rgb;
	gl_FragColor.a = alpha;
#if defined(USE_FROXEL_FOG)
	// Added in OPM: the light in the air between it and the eye
	gl_FragColor.rgb = FroxelFog(gl_FragColor.rgb, gl_FragColor.a);
#endif
}
