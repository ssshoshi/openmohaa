// Added in OPM: the depth what is blended over r_taa's blend is tested
// against (tr_taa.c, RB_TaaOpaqueDone).
//
// The blend is of frames each drawn from a camera nudged a fraction of a
// pixel; the depth is this frame's alone, nudged. Water, glass and smoke drawn
// after, without the nudge, over the blend, were cut by that depth: along the
// edge of everything standing in front of them they reached a pixel over its
// blended edge... the other way: cut short of it by the nudged depth, they
// left a pixel round everything standing in front of them undrawn, and what
// showed there was the blend of what is behind them (under water the void,
// cleared to the fog: a thin bright line round a rifle over water). Each pixel
// takes the farthest depth round it instead, so they reach in under the
// blended edge.

uniform sampler2D u_ScreenDepthMap; // this frame's depth (the soft particles' copy)
uniform vec4      u_BloomTexel;     // one over the image's width and height, its width and height

varying vec2 var_Tex;
varying vec2 var_View;

void main()
{
	float far = 0.0;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			far = max(far, texture2D(u_ScreenDepthMap, var_Tex + vec2(float(x), float(y)) * u_BloomTexel.xy).r);
		}
	}
	gl_FragDepth = far;
	gl_FragColor = vec4(0.0);
}
