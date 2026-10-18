// Added in OPM: the wet beach (tr_water.c).
//
// The tide is going out: the sand it has just left is wet, darker and with the
// sky in it, and there is water standing in its hollows. Each wave that gets
// to the shore runs on up the sand as a thin sheet with foam at its edge, and
// drains back. Under each body lying on it the sand is soaked red. (What
// stands on it or in the water is wet too: that is its own surface's, in
// lightall_fp.glsl, where it knows which way it faces.) Drawn over the opaque view in two passes, from
// where each pixel is in the world (the prepass's depth): one darkens what is
// there (multiplied), the other adds the sky (BEACH_ADD).

uniform sampler2D u_ScreenDepthMap;
uniform sampler2D u_ScreenImageMap; // the view as it was before (BEACH_ADD: what the wet sand mirrors)
uniform mat4      u_ModelViewProjectionMatrix; // the world's

uniform vec3      u_ViewOrigin;
uniform vec4      u_ViewInfo;    // zNear, zFar, and the view's share of the screen across and up
uniform float     u_Time;

uniform vec4      u_GlobalFogColor;
uniform vec2      u_GlobalFogParams;

uniform vec4      u_WaterWaves;  // the way the waves come in (xy)
uniform vec4      u_WaterShore;  // z: whether there is a shore line, w: where
uniform vec4      u_WaterSky;    // the sky
uniform samplerCube u_CubeMap;   // the map's sky box (tr_water.c, R_WaterBuildSky)
uniform vec4      u_CubeMapInfo; // how much of it is mirrored: 0 the flat u_WaterSky
uniform vec4      u_WaterBlood;  // the colour of blood
uniform vec4      u_BeachWet;    // how far inland the sand is wet, the highest it is, puddles (a share), their size
uniform vec4      u_BeachSwash;  // how far up the sand a wave runs, at most
uniform sampler2D u_SpecularMap; // the beach's ground: its height from above, where it is (R_WaterBuildBeach)
uniform vec4      u_BeachGround; // where that grid starts, one over its size (0: the beach is everywhere)
uniform vec4      u_WaterBloodZone; // w: how many u_SandStains
uniform vec4      u_SandStains[WATER_MAX_SPOTS]; // x, y, the ground's height, how far the blood has soaked

varying vec2      var_DepthTex;
varying vec3      var_ViewDir;

const float PI = 3.14159265;

float LinearDepth(float d)
{
	float n = u_ViewInfo.x, f = u_ViewInfo.y;
	return 2.0 * n * f / (f + n - (2.0 * d - 1.0) * (f - n));
}

vec2 Turn(vec2 v, float a)
{
	float c = cos(a), s = sin(a);
	return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

vec3 Noise(vec2 x)
{
	vec2  i  = floor(x);
	vec2  f  = fract(x);
	vec2  u  = f * f * (3.0 - 2.0 * f);
	vec2  du = 6.0 * f * (1.0 - f);
	float a  = Hash(i);
	float b  = Hash(i + vec2(1.0, 0.0));
	float c  = Hash(i + vec2(0.0, 1.0));
	float d  = Hash(i + vec2(1.0, 1.0));
	float k  = a - b - c + d;

	return vec3(a + (b - a) * u.x + (c - a) * u.y + k * u.x * u.y,
	            du * (vec2(b - a, c - a) + k * u.yx));
}

float Clouds(vec2 x)
{
	return Noise(x).x * 0.55 + Noise(Turn(x, 1.1) * 2.03 + 3.7).x * 0.3 + Noise(Turn(x, 2.3) * 4.1 + 9.2).x * 0.15;
}

// On the beach's own ground (R_WaterBuildBeach): its grid has it at this
// height here. The terrain is drawn coarser further off than its heights.
bool OnBeach(vec3 p, float lin)
{
	vec2 at = (p.xy - u_BeachGround.xy) * u_BeachGround.zw;
	if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0)
		return false;
	return abs(p.z - texture2D(u_SpecularMap, at).r) <= 6.0 + lin * 0.006;
}

void main()
{
	float d = texture2D(u_ScreenDepthMap, var_DepthTex).r;
	if (d >= 0.99999 || u_WaterShore.z < 0.5)
		discard;
	// at the prepass's depth, tested against the main pass's: what the main
	// pass drew in front and the prepass left out (a model far off,
	// r_prepassFarModels) keeps its own pixels
	gl_FragDepth = d;

	float lin  = LinearDepth(d);
	vec3  vd   = var_ViewDir / u_ViewInfo.y;
	vec3  ddx  = dFdx(var_ViewDir) / u_ViewInfo.y;
	vec3  ddy  = dFdy(var_ViewDir) / u_ViewInfo.y;
	vec3  pos  = u_ViewOrigin + vd * lin;
	vec2  texel = r_FBufScale;

	// Which way it faces, from the pixels beside it on its own side: at an
	// edge (a model before the sand) the depth jumps, and across it the
	// normal was anything, a bright line round everything on the beach.
	// The side whose depth goes on most smoothly.
	float dx0  = texture2D(u_ScreenDepthMap, var_DepthTex - vec2(texel.x, 0.0)).r;
	float dx1  = texture2D(u_ScreenDepthMap, var_DepthTex + vec2(texel.x, 0.0)).r;
	float dy0  = texture2D(u_ScreenDepthMap, var_DepthTex - vec2(0.0, texel.y)).r;
	float dy1  = texture2D(u_ScreenDepthMap, var_DepthTex + vec2(0.0, texel.y)).r;
	float lx0  = LinearDepth(dx0), lx1 = LinearDepth(dx1);
	float ly0  = LinearDepth(dy0), ly1 = LinearDepth(dy1);
	vec3  sx   = abs(lx1 - lin) < abs(lin - lx0) ? (vd + ddx) * lx1 - vd * lin : vd * lin - (vd - ddx) * lx0;
	vec3  sy   = abs(ly1 - lin) < abs(lin - ly0) ? (vd + ddy) * ly1 - vd * lin : vd * lin - (vd - ddy) * ly0;
	vec3  N    = normalize(cross(sx, sy));
	if (N.z < 0.0)
		N = -N;

	// Only the beach's own ground: not the sand under the water (another
	// shader, with the water's reflection over it already), nor what stands
	// on the beach, a body or a boat, which the swash ran over. A pixel off
	// it at a model's edge, which the prepass covered a pixel further than
	// the view did, takes the ground beside it and its depth: where the view
	// has the model there, it is in front and keeps the pixel, and where the
	// view has the ground it is wet as the ground round it (left dry, it was
	// a light line round everything on the wet sand)
	if (u_BeachGround.z > 0.0 && !OnBeach(pos, lin))
	{
		vec3 p;

		if (OnBeach(p = u_ViewOrigin + (vd - ddx) * lx0, lx0))
			d = dx0;
		else if (OnBeach(p = u_ViewOrigin + (vd + ddx) * lx1, lx1))
			d = dx1;
		else if (OnBeach(p = u_ViewOrigin + (vd - ddy) * ly0, ly0))
			d = dy0;
		else if (OnBeach(p = u_ViewOrigin + (vd + ddy) * ly1, ly1))
			d = dy1;
		else
			discard;
		pos = p;
		lin = LinearDepth(d);
		gl_FragDepth = d;
		N = vec3(0.0, 0.0, 1.0);
	}
	float onGround = smoothstep(0.55, 0.8, N.z); // ground, not the sides of things

	vec2  dir    = normalize(u_WaterWaves.xy);
	float inland = dot(dir, pos.xy) - u_WaterShore.w;
	float reach  = max(u_BeachWet.x, 1.0);

	// Wet from below the waterline to where the tide got to, along a ragged line
	float line = (Clouds(pos.xy / 260.0) - 0.5) * reach * 0.4;
	float wet  = (1.0 - smoothstep(reach * 0.6, reach, inland + line)) * onGround;
	wet *= 1.0 - smoothstep(u_BeachWet.y - 8.0, u_BeachWet.y, pos.z);

	// Water standing in its hollows, on the flattest of it: runnels along
	// the shore, as the tide leaves them
	float puddle = 0.0;
	if (u_BeachWet.z > 0.0)
	{
		vec2  along  = vec2(dot(pos.xy, vec2(dir.y, -dir.x)) * 0.45, dot(pos.xy, dir));
		float hollow = Clouds(along / max(u_BeachWet.w, 1.0) + 3.1);
		float level  = 0.74 - u_BeachWet.z * 0.3;
		puddle = smoothstep(level, level + 0.025, hollow) * wet * smoothstep(0.92, 0.98, N.z);
		// above the waterline alone: under the water they were a second
		// reflection under the water's own
		puddle *= smoothstep(0.0, 40.0, inland);
	}

	// The swash: each wave runs up the sand in a thin sheet, fast, and
	// drains back slowly. How far it gets is different from wave to wave and
	// along the shore, and the waves get there a little apart along it, so
	// its edge is never one line. Foam rides the edge as it comes up.
	float sheet = 0.0, swashFoam = 0.0;
	if (u_BeachSwash.x > 0.0 && inland > -60.0 && inland < u_BeachSwash.x * 1.4)
	{
		float across = dot(pos.xy, vec2(dir.y, -dir.x));
		// The waves come in square to the shore, so a wave's swash runs up all
		// along it at once, ragged: a big lag along the shore swept each wave
		// in from both sides to the middle, its bright front with it
		float cycle  = u_Time / max(u_WaterWaves.w, 0.5) + (Clouds(vec2(across / 2400.0, 1.7)) - 0.5) * 0.3
		             + (Clouds(vec2(across / 260.0, 4.3)) - 0.5) * 0.12;
		float wave   = floor(cycle);
		float f      = fract(cycle);
		float run    = u_BeachSwash.x * (0.3 + 0.85 * Clouds(vec2(across / 380.0 + wave * 3.7, wave * 1.3)));
		float rising = f < 0.3 ? 1.0 : 0.0;
		float ahead  = f < 0.3 ? 1.0 - (1.0 - f / 0.3) * (1.0 - f / 0.3) : 1.0 - smoothstep(0.0, 1.0, (f - 0.3) / 0.7);
		// tongues of it run on further than the rest
		float front  = mix(-40.0, run, ahead) + (Clouds(vec2(across / 55.0, wave * 2.1)) - 0.5) * run * 0.35;

		sheet = 1.0 - smoothstep(front - 8.0, front, inland);
		// thinner, and gone glassy, as it drains
		sheet *= mix(0.65, 1.0, rising) * onGround;

		float lace  = Clouds(pos.xy / 13.0 + dir * u_Time * 0.6) * 0.6 + Clouds(Turn(pos.xy, 0.8) / 34.0) * 0.4;
		float band  = smoothstep(front - 34.0, front - 3.0, inland) * sheet;
		float trail = sheet * 0.35 * (1.0 - ahead);
		swashFoam   = clamp((band * mix(0.25, 1.0, rising) + trail) * smoothstep(0.4, 0.62, lace) * 1.6, 0.0, 0.85);
	}

	// Soaked red under the bodies, ragged and darkest under them
	float stain = 0.0;
	int stains = int(u_WaterBloodZone.w + 0.5);
	for (int i = 0; i < WATER_MAX_SPOTS; i++)
	{
		if (i >= stains)
			break;

		vec4 st = u_SandStains[i];
		vec2 q  = (pos.xy - st.xy) / max(st.w, 1.0);
		if (dot(q, q) < 2.5 && pos.z < st.z + 6.0)
		{
			vec2  seed  = vec2(float(i) * 5.71, float(i) * 2.93);
			float warp  = Clouds(q * 1.6 + seed);
			float r     = length(q + vec2(warp - 0.5, Clouds(q * 1.6 + seed.yx + 4.3) - 0.5) * 0.8);
			float soak  = smoothstep(0.25, 0.75, Clouds(q * 4.0 + seed * 1.3));
			stain = max(stain, (1.0 - smoothstep(0.2, 1.0, r)) * mix(0.55, 1.0, soak));
		}
	}
	stain *= onGround;

	// Under the water all of it fades out, before the swash's reach and the
	// beach's ground end: the water is over it there, and where either ended
	// it did so in a straight line along the shore
	float sea = smoothstep(-60.0, -10.0, inland);
	wet       *= sea;
	sheet     *= sea;
	swashFoam *= sea;
	stain     *= sea;

	if (wet + stain + sheet <= 0.002)
		discard;

	float dist      = distance(pos, u_ViewOrigin);
	float fogFactor = clamp((dist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0) * u_GlobalFogColor.a;
	vec3  blood     = mix(vec3(0.5, 0.14, 0.11), u_WaterBlood.rgb * 2.2, 0.35);

#if defined(BEACH_ADD)
	// the sky in it: a mirror where water stands, stirred by the wind
	vec2  ripple = Noise(pos.xy / 9.0 + vec2(u_Time * 0.7, u_Time * -0.4)).yz * 0.06
	             + Noise(Turn(pos.xy, 1.7) / 4.0 + vec2(u_Time * -0.5, u_Time * 0.9)).yz * 0.03;
	vec3  n      = normalize(N + vec3(-ripple * clamp(puddle + sheet, 0.0, 1.0), 0.0));
	vec3  V      = normalize(u_ViewOrigin - pos);
	float facing = clamp(dot(n, V), 0.0, 1.0);
	float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
	// only the water standing in the hollows gives the sky back: the wet sand
	// and the swash's thin sheet are dull
	float shine  = puddle * mix(0.08, 0.7, fresnel);
	shine *= 1.0 - swashFoam;

	// What it mirrors: the ray off it, followed through the view's depth
	// until it goes behind something, which is then what is seen in it;
	// else the sky, the map's box where the view has drawn it
	vec3 R      = reflect(-V, n);
	vec3 sky    = u_WaterSky.rgb;
	if (u_CubeMapInfo.x > 0.0)
		sky = mix(sky, textureCubeLod(u_CubeMap, R, 1.0).rgb, u_CubeMapInfo.x);
	vec3 mirror = sky;
	if (shine > 0.01)
	{
		float stepLen = 6.0;
		vec3  q       = pos + n * 1.0;
		for (int i = 0; i < 24; i++)
		{
			q += R * stepLen;
			stepLen *= 1.2;

			vec4 clip = u_ModelViewProjectionMatrix * vec4(q, 1.0);
			if (clip.w <= 0.0)
				break;
			vec2 uv = (clip.xy / clip.w * 0.5 + 0.5) * u_ViewInfo.zw;
			if (uv.x < 0.0 || uv.y < 0.0 || uv.x > u_ViewInfo.z || uv.y > u_ViewInfo.w)
				break;

			float sceneLin = LinearDepth(texture2D(u_ScreenDepthMap, uv).r);
			float behind   = clip.w - sceneLin;
			if (behind > 0.0 && behind < stepLen * 2.0 + 8.0)
			{
				// fading towards the edge of the view, where it would run out
				vec2 edge = min(uv, u_ViewInfo.zw - uv) / u_ViewInfo.zw;
				mirror = mix(sky, texture2D(u_ScreenImageMap, uv).rgb, smoothstep(0.0, 0.08, min(edge.x, edge.y)));
				break;
			}
		}
	}
	vec3 sheen = mirror * shine;
	sheen *= mix(vec3(1.0), blood * 1.4, clamp(stain * (0.5 + puddle), 0.0, 1.0));

	// the foam, lit as the sky lights it and pink with the blood in the surf
	vec3 foamColor = mix(vec3(0.82, 0.84, 0.84), vec3(0.66, 0.42, 0.39), clamp(u_WaterBlood.a * 0.8, 0.0, 1.0));
	foamColor *= clamp(dot(u_WaterSky.rgb, vec3(0.3)) + 0.1, 0.3, 1.0);
	sheen += foamColor * swashFoam;
	gl_FragColor = vec4(sheen * (1.0 - fogFactor), 1.0);
#else
	// darker for the water in it, darker still under standing water, and
	// red-brown where the blood has soaked it
	vec3 mul = vec3(mix(1.0, 0.6, wet));
	mul *= mix(1.0, 0.55, max(puddle, sheet));
	mul *= mix(vec3(1.0), blood, sheet * clamp(u_WaterBlood.a, 0.0, 1.0) * 0.5); // the surf's blood in it
	mul *= 1.0 - swashFoam; // the foam is what is added
	mul *= mix(vec3(1.0), blood, stain);
	gl_FragColor = vec4(mix(mul, vec3(1.0), fogFactor), 1.0);
#endif
}
