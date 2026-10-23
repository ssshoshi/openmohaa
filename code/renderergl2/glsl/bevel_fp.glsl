// Added in OPM: the edges' bevel (RB_EdgeBevel).
//
// A brush's faces are flat, each with the one normal, so where two meet the
// light jumps from one's to the other's along a line drawn with a ruler. A
// real corner is worn round, and catches the light across it. This turns
// each pixel's normal toward the faces within u_ViewInfo.z units of it: half
// way at the edge itself, none that far from it. The lighting (lightall_fp.glsl)
// adds that turn to its own normal, where it is the surface this saw.
//
// Two passes over the prepass's depth. BEVEL_NORMALS: each pixel's normal,
// from the pixels beside it, and how far it is. Then the bevel: the normals
// round it, out to that many units, averaged, less its own; and how far it
// is, for the lighting to tell it is the same surface.

uniform sampler2D u_ScreenDepthMap; // the prepass's depth; for the bevel, the normals
uniform vec4      u_ViewInfo;       // zNear, zFar, the bevel's width in units, pixels a unit at a unit away
uniform vec4      u_EdgeBevel;      // one over the image's size, how much of the turn

varying vec2      var_DepthTex;
varying vec3      var_ViewDir;

float LinearDepth(float d)
{
	float n = u_ViewInfo.x, f = u_ViewInfo.y;
	return 2.0 * n * f / (f + n - (2.0 * d - 1.0) * (f - n));
}

#if defined(BEVEL_NORMALS)

void main()
{
	// (before any pixel leaves: across one that has, they are undefined)
	vec3  vd    = var_ViewDir / u_ViewInfo.y;
	vec3  ddx   = dFdx(var_ViewDir) / u_ViewInfo.y;
	vec3  ddy   = dFdy(var_ViewDir) / u_ViewInfo.y;

	float d = texture2D(u_ScreenDepthMap, var_DepthTex).r;
	if (d >= 0.99999)
	{
		// the sky: nothing
		gl_FragColor = vec4(0.0);
		return;
	}

	float lin   = LinearDepth(d);
	vec2  texel = u_EdgeBevel.xy;

	// from the side whose depth goes on most smoothly: across an edge the
	// normal would be neither face's
	float lx0 = LinearDepth(texture2D(u_ScreenDepthMap, var_DepthTex - vec2(texel.x, 0.0)).r);
	float lx1 = LinearDepth(texture2D(u_ScreenDepthMap, var_DepthTex + vec2(texel.x, 0.0)).r);
	float ly0 = LinearDepth(texture2D(u_ScreenDepthMap, var_DepthTex - vec2(0.0, texel.y)).r);
	float ly1 = LinearDepth(texture2D(u_ScreenDepthMap, var_DepthTex + vec2(0.0, texel.y)).r);
	vec3  sx  = abs(lx1 - lin) < abs(lin - lx0) ? (vd + ddx) * lx1 - vd * lin : vd * lin - (vd - ddx) * lx0;
	vec3  sy  = abs(ly1 - lin) < abs(lin - ly0) ? (vd + ddy) * ly1 - vd * lin : vd * lin - (vd - ddy) * ly0;
	vec3  N   = cross(sx, sy);
	float NN  = dot(N, N);

	// no slope to tell (a pixel on its own between two jumps): no normal,
	// which the bevel round it leaves out; a NaN here was a NaN in the
	// view, and the exposure it is averaged into went black for good
	if (!(NN > 1e-20))
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, lin);
		return;
	}
	N *= inversesqrt(NN);

	// facing the view, as the lighting turns its own
	if (dot(N, vd) > 0.0)
		N = -N;

	gl_FragColor = vec4(N, lin);
}

#else

// two rings of six, the inner turned half a step
const vec2 bevelTaps[12] = vec2[12](
	vec2( 1.0,  0.0),   vec2( 0.5,  0.866), vec2(-0.5,  0.866),
	vec2(-1.0,  0.0),   vec2(-0.5, -0.866), vec2( 0.5, -0.866),
	vec2( 0.433, 0.25), vec2( 0.0,  0.5),   vec2(-0.433, 0.25),
	vec2(-0.433,-0.25), vec2( 0.0, -0.5),   vec2( 0.433,-0.25));

void main()
{
	vec3  vd    = var_ViewDir / u_ViewInfo.y;
	vec3  ddx   = dFdx(var_ViewDir) / u_ViewInfo.y;
	vec3  ddy   = dFdy(var_ViewDir) / u_ViewInfo.y;

	vec4 c = texture2D(u_ScreenDepthMap, var_DepthTex);
	float lin = c.w;

	// the width on the screen; under a pixel, nothing to see
	float pixels = u_ViewInfo.z * u_ViewInfo.w / max(lin, 1.0);
	if (lin <= 0.0 || pixels < 0.75 || dot(c.xyz, c.xyz) < 0.5)
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, lin);
		return;
	}
	pixels = min(pixels, 32.0);

	vec3  P     = vd * lin;
	vec3  N     = c.xyz;
	float reach = u_ViewInfo.z;
	vec3  sum   = N;

	for (int i = 0; i < 12; i++)
	{
		vec2 o = bevelTaps[i] * pixels;
		vec4 s = texture2D(u_ScreenDepthMap, var_DepthTex + o * u_EdgeBevel.xy);

		if (s.w <= 0.0)
			continue;

		// where it is: further than the bevel's width (seen at a slant, or
		// something else in front or behind) it is not this corner's
		vec3  Q = (vd + ddx * o.x + ddy * o.y) * s.w;
		float w = 1.0 - smoothstep(reach, reach * 1.5, length(Q - P));

		sum += s.xyz * w;
	}

	// the faces round it turned right away from it (a blade seen edge on)
	float ss = dot(sum, sum);
	if (!(ss > 1e-4))
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, lin);
		return;
	}

	gl_FragColor = vec4((sum * inversesqrt(ss) - N) * u_EdgeBevel.z, lin);
}

#endif
