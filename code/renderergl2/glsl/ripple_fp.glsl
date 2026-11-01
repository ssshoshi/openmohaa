// Added in OPM: the water's ripples (tr_water.c).
//
// The water round the view is a grid of heights, one step of the wave
// equation a sixtieth of a second: each point pulled toward its neighbours,
// so what disturbs it spreads out in rings, and something going through it
// faster than they spread leaves them behind it in a V. What disturbs it is
// the cgame's (cg_water.cpp): men wading, boats, bullets, things falling in;
// each a dip pressed into it this step, and foam that fades on its own.
//
// The grid goes with the view: when it has moved on, what was there is read
// from where it is now.

uniform sampler2D u_DiffuseMap; // the grid: height now, height a step ago, foam

uniform vec4 u_RippleSim;   // a texel (a share of the grid), how far the grid moved (texels), how fast ripples go ((c dt / dx)^2)
uniform vec4 u_RippleDecay; // what is left of their motion a step, and of the height, and of the foam; how many u_RippleSources
uniform vec4 u_RippleSources[WATER_MAX_SPOTS]; // x, y (a share of the grid), how wide (the same), how deep a dip this step
uniform vec4 u_RippleFoam[WATER_MAX_SPOTS];    // how much foam it leaves

varying vec2 var_Tex;

vec4 Grid(vec2 uv)
{
	// nothing outside it: the water there is still
	if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
		return vec4(0.0);
	return texture2D(u_DiffuseMap, uv);
}

void main()
{
	float texel = u_RippleSim.x;
	vec2  uv    = var_Tex + u_RippleSim.yz * texel;
	vec4  here  = Grid(uv);

	float around = Grid(uv + vec2(texel, 0.0)).r + Grid(uv - vec2(texel, 0.0)).r
	             + Grid(uv + vec2(0.0, texel)).r + Grid(uv - vec2(0.0, texel)).r;
	float h      = here.r;
	float moving = (h - here.g) * u_RippleDecay.x;
	float next   = (h + moving + u_RippleSim.w * (around - 4.0 * h)) * u_RippleDecay.y;
	float foam   = here.b * u_RippleDecay.z;

	int sources = int(u_RippleDecay.w + 0.5);
	for (int i = 0; i < WATER_MAX_SPOTS; i++)
	{
		if (i >= sources)
			break;

		vec4  s = u_RippleSources[i];
		vec2  d = (var_Tex - s.xy) / max(s.z, texel);
		float r = dot(d, d);
		if (r < 9.0)
		{
			float g = exp(-r);
			next -= s.w * g;
			foam += u_RippleFoam[i].x * g;
		}
	}

	// still toward the grid's edge, so nothing comes back off it
	vec2 edge = min(var_Tex, 1.0 - var_Tex);
	next *= smoothstep(0.0, 0.04, min(edge.x, edge.y));

	gl_FragColor = vec4(next, h, clamp(foam, 0.0, 1.0), 1.0);
}
