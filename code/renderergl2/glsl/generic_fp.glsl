uniform sampler2D u_DiffuseMap;

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

	// MOH:AA global distance fog, linear between the two params. When it is off
	// the renderer sets the range scale to zero, so the factor stays at zero.
	float fogFactor = clamp((var_FogDist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0);
	gl_FragColor.rgb = mix(gl_FragColor.rgb, u_GlobalFogColor.rgb, fogFactor * u_GlobalFogColor.a);
}
