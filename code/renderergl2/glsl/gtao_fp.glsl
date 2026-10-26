// Added in OPM: ambient occlusion for the light that comes from all round
// (r_gtao, r_realtimeLighting 2): the sky, the bounce, the probes and the
// ambient. Horizon based, as HBAO and GTAO are: in four ways round a point,
// across the screen, how far what is near it rises above its surface, within
// r_gtaoRadius; the lights and the sun have shadows of their own and are not
// darkened by it (rt_ao in tr_glsl.c's rt_glsl). Worked out at half the view's
// size and smoothed by the depth aware blur after it.
uniform sampler2D u_ScreenDepthMap;

uniform vec4   u_ViewInfo;  // zfar / znear, zfar, 1 / the depth's width, 1 / its height
uniform vec3   u_ViewForward;
uniform vec3   u_ViewLeft;
uniform vec3   u_ViewUp;
uniform vec4   u_AOParams;  // radius, strength, -, -

varying vec2   var_DepthTex;
varying vec3   var_ViewDir;
varying vec2   var_Ndc;

#define AO_DIRS  4
#define AO_STEPS 3

float LinearDepth(vec2 tex)
{
	float z = textureLod(u_ScreenDepthMap, tex, 0.0).r;
	return 1.0 / mix(u_ViewInfo.x, 1.0, z);
}

// from a smaller copy of the depth, the further off it is read (McGuire's
// SAO): eight texels off and nearer from the whole one
float LinearDepthAt(vec2 tex, float texels)
{
	float lod = clamp(floor(log2(max(texels, 1.0))) - 3.0, 0.0, 4.0);
	float z = textureLod(u_ScreenDepthMap, tex, lod).r;
	return 1.0 / mix(u_ViewInfo.x, 1.0, z);
}

// where on the screen, as far off as the depth says: the point, from the eye
vec3 PointAt(vec2 ndc, float depth)
{
	return (u_ViewForward + u_ViewLeft * -ndc.x + u_ViewUp * ndc.y) * depth;
}

void main()
{
	vec2  texPerNdc = vec2(dFdx(var_DepthTex.x) / dFdx(var_Ndc.x), dFdy(var_DepthTex.y) / dFdy(var_Ndc.y));
	float depth = LinearDepth(var_DepthTex);

	if (depth >= 0.999)
	{
		gl_FragColor = vec4(1.0);
		return;
	}

	vec3 P = var_ViewDir * depth;

	// its surface's way, from the neighbours on whichever side is nearer in
	// depth (at an edge the other side is something else)
	vec2 dx = vec2(u_ViewInfo.z, 0.0), dy = vec2(0.0, u_ViewInfo.w);
	vec3 pr = PointAt(var_Ndc + dx / texPerNdc, LinearDepth(var_DepthTex + dx));
	vec3 pl = PointAt(var_Ndc - dx / texPerNdc, LinearDepth(var_DepthTex - dx));
	vec3 pu = PointAt(var_Ndc + dy / texPerNdc, LinearDepth(var_DepthTex + dy));
	vec3 pd = PointAt(var_Ndc - dy / texPerNdc, LinearDepth(var_DepthTex - dy));
	vec3 ex = abs(pr.x - P.x) + abs(pr.y - P.y) + abs(pr.z - P.z) < abs(pl.x - P.x) + abs(pl.y - P.y) + abs(pl.z - P.z) ? pr - P : P - pl;
	vec3 ey = abs(pu.x - P.x) + abs(pu.y - P.y) + abs(pu.z - P.z) < abs(pd.x - P.x) + abs(pd.y - P.y) + abs(pd.z - P.z) ? pu - P : P - pd;
	vec3 N  = normalize(cross(ex, ey));
	if (dot(N, P) > 0.0)
		N = -N;

	// the radius across the screen, at this depth
	float radius   = u_AOParams.x;
	float units    = depth * u_ViewInfo.y;
	vec2  ndcRange = radius / units * vec2(length(u_ViewForward) / length(u_ViewLeft), length(u_ViewForward) / length(u_ViewUp));
	ndcRange = min(ndcRange, vec2(0.25));

	// the ways turned for each pixel of a 4 x 4 block, which the blur evens out
	float turn = (mod(floor(gl_FragCoord.x), 4.0) + 4.0 * mod(floor(gl_FragCoord.y), 4.0)) / 16.0;
	float occlusion = 0.0;

	for (int d = 0; d < AO_DIRS; d++)
	{
		float a   = (float(d) + turn) * (6.2831853 / float(AO_DIRS));
		vec2  way = vec2(cos(a), sin(a));
		float most = 0.0;

		for (int s = 1; s <= AO_STEPS; s++)
		{
			float t   = (float(s) - 0.5 + 0.5 * turn) / float(AO_STEPS);
			vec2  ndc = var_Ndc + way * ndcRange * t;
			vec2  off = way * ndcRange * t * texPerNdc;
			vec3  Q   = PointAt(ndc, LinearDepthAt(var_DepthTex + off, length(off / u_ViewInfo.zw)));
			vec3  v   = Q - P;
			float dist = length(v);

			if (dist > 0.001 && dist < radius)
			{
				float rise = dot(N, v) / dist - 0.15;
				float fade = 1.0 - (dist * dist) / (radius * radius);
				most = max(most, rise * fade);
			}
		}
		occlusion += max(most, 0.0);
	}

	float ao = clamp(1.0 - u_AOParams.y * occlusion / float(AO_DIRS), 0.0, 1.0);
	gl_FragColor = vec4(ao, ao, ao, 1.0);
}
