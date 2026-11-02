// Mirrors lightall_vp.glsl: only normal and parallax mapping read the tangent
// frame, so without a normal map neither the basis nor its varyings exist.
#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT) && defined(USE_NORMALMAP)
  #define USE_TANGENT_FRAME
#endif

uniform sampler2D u_DiffuseMap;

#if defined(USE_LIGHTMAP)
uniform sampler2D u_LightMap;
#endif

#if defined(USE_NORMALMAP)
uniform sampler2D u_NormalMap;
#endif

#if defined(USE_DELUXEMAP)
uniform sampler2D u_DeluxeMap;
#endif

#if defined(USE_SPECULARMAP)
uniform sampler2D u_SpecularMap;
#endif

#if defined(USE_SHADOWMAP)
uniform sampler2D u_ShadowMap;
#endif

#if defined(USE_CUBEMAP)
uniform samplerCube u_CubeMap;
#endif

#if defined(USE_NORMALMAP) || defined(USE_DELUXEMAP) || defined(USE_SPECULARMAP) || defined(USE_CUBEMAP)
// y = deluxe, w = cube
uniform vec4      u_EnableTextures; 
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
uniform vec3  u_PrimaryLightColor;
uniform vec3  u_PrimaryLightAmbient;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
uniform vec4      u_NormalScale;
uniform vec4      u_SpecularScale;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
#if defined(USE_CUBEMAP)
uniform vec4      u_CubeMapInfo;
#endif
#endif

uniform int       u_AlphaTest;

// Added in OPM: wet (tr_water.c). What has been in the sea or under the
// spray, up to the mark the tide reached, is wet, after Lagarde's wet
// surfaces in Remember Me: not a layer over the lit surface but the surface
// changed before it is lit. Water filling a porous surface's pores darkens
// it and deepens its colour (stone, wood, cloth; paint and metal hardly);
// wet, any surface is smoother, and a film of water over it smoother still:
// it flattens the bumps of its normal map, reflects as water does, and gives
// back the sun as a small hard highlight and the sky most at a glancing look.
// The film dries first: under the mark is a band that is dark but no longer
// shines.
uniform vec4      u_Wet;    // how wet, the mark's height; the rain: how wet, what is drawn (1 the world, 2 the view's hands and weapon)
uniform vec4      u_WetSky; // the sky it gives back
uniform vec4      u_WetSun; // towards the sun, and how bright it is
uniform vec3      u_ViewOrigin;
varying vec3      var_WetPos;
varying vec3      var_WetNormal;
// Added in OPM: the rain (tr_water.c's R_RainBuildCover): the highest of the
// world at each place, from above; the grid's corner and one over its size;
// the time, its texel, how much ground holds puddles, how much beads show
uniform sampler2D u_RainCoverMap;
uniform vec4      u_RainCover;
uniform vec4      u_RainParams;
// the world the puddles mirror, drawn from under them (R_WaterReflectView):
// whether there is it, its height, its share of the image across and up
uniform sampler2D u_RainReflectMap;
uniform sampler2D u_RainReflectDepthMap; // its depth
uniform vec4      u_RainReflect;
uniform mat4      u_RainReflectMvp;
// a lightning flash: its light, how strong (R_RainFrame)
uniform vec4      u_RainFlash;

float WetNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
	float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
	float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Out in the rain: nothing over it. A wall reads the grid a little way out
// from its face, where the rain falls past it; the ground at its own place,
// where the grid holds its own height (or a little more, on a slope).
float RainExposedAt(vec3 p, vec2 at)
{
	vec2 uv = (at - u_RainCover.xy) * u_RainCover.zw;
	if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
		return 1.0;
	float top  = texture2D(u_RainCoverMap, uv).r;
	float give = u_RainParams.y + 6.0;
	return smoothstep(top - give - 10.0, top - give, p.z);
}

// A wall the wind blows the rain onto: open sky anywhere a little way out
// from it, past the eaves over it, wets all of it, from the foot to the
// roof (an eave reaching out a few units kept a whole house front dry). A
// room's walls, a deep porch's, have a roof all the way out and stay dry.
float RainExposed(vec3 p, vec3 n)
{
	float open = RainExposedAt(p, p.xy + n.xy * u_RainParams.y * 1.6);
	float side = length(n.xy);
	if (side > 0.5 && open < 1.0)
	{
		vec2 out2 = n.xy / side;
		open = max(open, RainExposedAt(p, p.xy + out2 * 24.0));
		open = max(open, RainExposedAt(p, p.xy + out2 * 48.0));
		open = max(open, RainExposedAt(p, p.xy + out2 * 80.0));
	}
	// the ground at a wall's foot reads the wall's own top in the grid (a
	// dry line along every wall); the wind blows the rain in a little way
	// under an edge too
	else if (side <= 0.5 && open < 1.0)
	{
		float r = u_RainParams.y * 1.5;
		open = max(open, RainExposedAt(p, p.xy + vec2( r, 0.0)));
		open = max(open, RainExposedAt(p, p.xy + vec2(-r, 0.0)));
		open = max(open, RainExposedAt(p, p.xy + vec2(0.0,  r)));
		open = max(open, RainExposedAt(p, p.xy + vec2(0.0, -r)));
	}
	return open;
}

// How wet it is here (x: the water in it; y: the film on it; z: a puddle
// standing on it) and which way the surface itself faces
// the face's own normal, from how it lies on the screen, to the eye
// (WetAmount): which way is down it, whatever the vertices' normals say
vec3 wetGeoN = vec3(0.0, 0.0, 1.0);

vec3 WetAmount(out vec3 surfN)
{
	wetGeoN = normalize(cross(dFdx(var_WetPos), dFdy(var_WetPos)));
	if (dot(wetGeoN, u_ViewOrigin - var_WetPos) < 0.0)
		wetGeoN = -wetGeoN;
	// faces' winding as the rest of this file has it: the front is the back
	surfN = normalize(!gl_FrontFacing ? var_WetNormal : -var_WetNormal);
	// a draw that hands over no normals (the terrain, unless the shader
	// asks for them): the face's own
	if (dot(var_WetNormal, var_WetNormal) < 1e-4)
		surfN = wetGeoN;
	vec3 wet = vec3(0.0);

	// the beach: up to the tide's mark, which is not a ruled line; the film
	// a little below it
	if (u_Wet.x > 0.0)
	{
		float mark = u_Wet.y + (WetNoise(var_WetPos.xy / 70.0 + var_WetPos.z / 50.0) - 0.5) * 16.0;
		float soak = u_Wet.x * (1.0 - smoothstep(mark - 8.0, mark + 2.0, var_WetPos.z));
		float film = u_Wet.x * (1.0 - smoothstep(mark - 22.0, mark - 8.0, var_WetPos.z));

		// the ground is the wet beach's (beach_fp.glsl): only what stands up
		float up = 1.0 - 0.85 * smoothstep(0.75, 0.95, surfN.z);

		// the film runs down what stands up in streaks, here thick, there thin
		float runs = smoothstep(0.25, 0.85, WetNoise(vec2(dot(var_WetPos.xy, vec2(0.11, 0.08)), var_WetPos.z * 0.03)));
		film *= mix(1.0, mix(0.45, 1.0, runs), 1.0 - abs(surfN.z));

		wet.xy = vec2(soak, film) * up;
	}

	// the rain: what is out under the sky soaked through, with a film on it
	// thick where it faces up and in runs down what stands up; nothing on
	// the undersides it never reaches; puddles on the open ground
	// (none on a cut-out, a tree line, a hedge, a fence: R_WaterSetWet)
	if (u_Wet.z > 0.0)
	{
		float open  = RainExposed(var_WetPos, surfN) * u_Wet.z;
		float under = smoothstep(-0.75, -0.3, surfN.z);
		float runs  = smoothstep(0.2, 0.8, WetNoise(vec2(dot(var_WetPos.xy, vec2(0.11, 0.08)), var_WetPos.z * 0.03)));
		float film  = mix(mix(0.3, 0.75, runs), 0.8, smoothstep(0.1, 0.7, surfN.z));

		wet.x = max(wet.x, open * under);
		wet.y = max(wet.y, open * under * film);
		if (abs(mod(u_Wet.w, 10.0) - 1.0) < 0.5 && surfN.z > 0.9)
		{
			float n   = WetNoise(var_WetPos.xy / 110.0) * 0.6 + WetNoise(var_WetPos.xy / 27.0) * 0.3 + WetNoise(var_WetPos.xy / 7.0) * 0.1;
			float lim = 1.0 - 0.75 * u_RainParams.z;
			wet.z = open * smoothstep(lim, lim + 0.08, n) * smoothstep(0.9, 0.97, surfN.z) * step(0.001, u_RainParams.z);
		}
	}
	return wet;
}

float WetHash(float x)
{
	return fract(sin(x * 91.3458) * 47453.5453);
}

// Added in OPM: the rain running off what it falls on: down what stands up
// in rivulets, each a bead sliding down with its wet trail over it, and down
// a slope (a roof, a bank) in a sheet that ripples as it goes. How much of
// the film it is here, and the tilt it gives the face (dN, along it)
float RainRuns(vec3 p, vec3 n, float t, float roof, out vec3 dN)
{
	dN = vec3(0.0);
	if (n.z > 0.97 || n.z < -0.25)
		return 0.0;

	// down the face, and across it
	vec3  down   = normalize(vec3(0.0, 0.0, -1.0) + n * n.z);
	vec3  across = normalize(cross(n, down));
	float u = dot(p, across), v = dot(p, down);
	float run = 0.0;

	// rivulets, on what stands up: straight down, in columns along the
	// world's axis nearest the face. Not along the face's normal: smoothed
	// across a wall, it turns a little from place to place, and so far out
	// from the world's middle that turned the columns into slants
	float steep = smoothstep(0.8, 0.4, n.z);
	if (steep > 0.0)
	{
		vec3  rAcross = abs(n.x) < abs(n.y) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
		vec3  rDown   = vec3(0.0, 0.0, -1.0);
		float ru = dot(p, rAcross), rv = -p.z;
		for (int k = 0; k < 2; k++)
		{
			float w    = k == 0 ? 7.0 : 11.0;
			float uk   = ru + float(k) * 3.7;
			float c    = floor(uk / w) + float(k) * 173.0;
			float h    = WetHash(c);
			if (h > 0.6)
				continue;
			float x     = fract(uk / w) - mix(0.25, 0.75, WetHash(c + 0.31));
			float speed = mix(18.0, 46.0, WetHash(c + 0.57));
			float len   = mix(40.0, 110.0, WetHash(c + 0.83));
			// where along its run, the bead at 1, its trail above it fading
			float s     = fract((rv - t * speed) / len + h * 7.0);
			float trail = smoothstep(0.25, 0.97, s) * (1.0 - smoothstep(0.985, 1.0, s));
			float head  = smoothstep(0.93, 0.975, s) * (1.0 - smoothstep(0.985, 1.0, s));
			// a little wider at the bead; wandering side to side as it goes
			x += (WetNoise(vec2(c, rv / 9.0)) - 0.5) * 0.1;
			float width = mix(0.06, 0.1, WetHash(c + 0.13)) * (1.0 + head * 0.4);
			float prof  = 1.0 - smoothstep(0.0, width, abs(x));
			float m     = prof * max(trail * 0.75, head);
			run = max(run, m * steep);
			// rounded across, and the bead's dome down it
			dN += rAcross * clamp(x / width, -1.0, 1.0) * m * steep * 0.35;
			dN += rDown * head * prof * steep * 0.2;
		}
	}

	// a sheet down a slope, in bands where more of it runs
	{
		// (on a roof only, r_rainRoofShaders: on a bank, a wall's cap, the
		// ground, it is just wet)
		float sheet = smoothstep(0.97, 0.88, n.z) * smoothstep(0.45, 0.6, n.z) * roof;
		vec2  q  = vec2(u / 7.0, (v - t * 40.0) / 14.0);
		float e  = 0.08;
		float n0 = WetNoise(q) * 0.65 + WetNoise(q * 2.7 + 4.1) * 0.35;
		float nx = WetNoise(q + vec2(e, 0.0)) * 0.65 + WetNoise((q + vec2(e, 0.0)) * 2.7 + 4.1) * 0.35;
		float ny = WetNoise(q + vec2(0.0, e)) * 0.65 + WetNoise((q + vec2(0.0, e)) * 2.7 + 4.1) * 0.35;
		dN  += (across * (n0 - nx) + down * (n0 - ny)) / e * 0.06 * sheet;
		run  = max(run, sheet * smoothstep(0.35, 0.75, n0));
	}
	return run;
}

// The rain falling in a puddle: rings spreading from where each drop lands,
// the tilt they give the water's face, along the ground
vec2 RainRipples(vec2 p, float t)
{
	vec2 sum = vec2(0.0);
	for (int k = 0; k < 2; k++)
	{
		vec2 q    = p / 16.0 + float(k) * vec2(0.37, 0.71);
		vec2 cell = floor(q);
		for (int j = -1; j <= 1; j++)
		for (int i = -1; i <= 1; i++)
		{
			vec2  c      = cell + vec2(float(i), float(j));
			float h      = fract(sin(dot(c, vec2(12.9898, 78.233)) + float(k) * 1.7) * 43758.5453);
			vec2  centre = c + vec2(fract(h * 13.7), fract(h * 7.3));
			float phase  = fract(t * (0.8 + 0.4 * fract(h * 3.1)) + h);
			vec2  d      = q - centre;
			float r      = length(d);
			float w      = r - phase * 0.9;
			float amp    = (1.0 - phase) * (1.0 - phase) * (1.0 - smoothstep(0.0, 0.1, abs(w)));
			sum += amp * sin(w * 60.0) * d / max(r, 0.001);
		}
	}
	return sum * 0.25;
}

// Drops beaded on the hands and the weapon in view, held on its surface (in
// its texture's frame): whether one is here, and the slope of its dome
float RainBeads(vec2 uv, out vec2 slope)
{
	float bead = 0.0;
	slope = vec2(0.0);
	for (int k = 0; k < 2; k++)
	{
		vec2  q = uv * (k == 0 ? 70.0 : 150.0) + float(k) * 0.5;
		vec2  c = floor(q);
		vec2  f = fract(q) - 0.5;
		float h = fract(sin(dot(c, vec2(41.3, 289.1)) + float(k) * 3.3) * 43758.5453);
		if (h > (k == 0 ? 0.22 : 0.35))
			continue;
		vec2  o   = (vec2(fract(h * 17.3), fract(h * 31.7)) - 0.5) * 0.45;
		float rad = mix(0.14, 0.32, fract(h * 7.1));
		vec2  d   = (f - o) / rad;
		float r2  = dot(d, d);
		if (r2 < 1.0)
		{
			float b = 1.0 - smoothstep(0.75, 1.0, r2);
			if (b > bead)
			{
				bead  = b;
				slope = d * 0.9;
			}
		}
	}
	return bead;
}

// How porous a surface is, by how rough and how metallic it looks: rough
// and dull drinks the water, smooth or metallic sheds it
float WetPorosity(float roughness, vec3 specular)
{
	float metal = smoothstep(0.08, 0.4, max(max(specular.r, specular.g), specular.b));
	return smoothstep(0.25, 0.75, roughness) * (1.0 - metal);
}

// The water in it: darker, deeper in colour; smoother. Lagarde's factors.
void WetMaterial(vec2 wet, float porosity, inout vec3 diffuse, inout float roughness)
{
	float factor = mix(1.0, 0.45, porosity);
	float lum    = dot(diffuse, vec3(0.2125, 0.7154, 0.0721));

	diffuse   = mix(vec3(lum), diffuse, 1.0 + 0.25 * wet.x * porosity);
	diffuse   = max(diffuse, vec3(0.0)) * mix(1.0, factor, wet.x);
	roughness = roughness * mix(1.0, factor, 0.5 * wet.x);
	// the film: water's own smoothness
	roughness = mix(roughness, 0.12, wet.y);
}

// What the film gives back with no cubemap: the sky above the horizon, the
// dark beach and sea below it, with the fresnel of a surface this rough and
// as much of the sky as the surface's light says it sees
vec3 WetSky(vec3 N, vec3 E, float roughness, vec3 specular, vec3 light)
{
	float NE  = clamp(dot(N, E), 0.0, 1.0);
	vec3  R   = reflect(-E, N);
	vec3  sky = u_WetSky.rgb * mix(0.12, 1.0, smoothstep(-0.15, 0.35, R.z));
	vec3  F   = specular + (max(vec3(1.0 - roughness), specular) - specular) * pow(1.0 - NE, 5.0);
	float sees = clamp(dot(light, vec3(0.2125, 0.7154, 0.0721)) * 1.6, 0.25, 1.0);
	// seen edge on, the film's own ripples hide what it gives back: at a
	// model's outline, where its normal turns from the eye, the most of it
	// was a bright line a pixel wide (a Higgins boat's side)
	float edge = smoothstep(0.0, 0.25, dot(N, E));

	return sky * F * sees * (1.0 - roughness) * edge;
}

#if defined(USE_SSR)
// Added in OPM: reflections of what is on the screen (r_ssr). The reflected
// ray marched through this frame's depth (its prepass), in steps that grow
// with the distance, and the last frame's colour, before its glow and tone
// map, where it hits; R_SsrSetUniforms. The cubemap's, or the wet film's
// sky, stands in where the ray finds nothing on the screen.
uniform sampler2D u_SsrColorMap;
uniform sampler2D u_SsrDepthMap;
uniform mat4      u_SsrViewProj;
uniform mat4      u_SsrPrevViewProj;
uniform vec4      u_SsrParams; // strength (0: off), how far, near, far

#define SSR_STEPS    16
#define SSR_MAX_STEPS 40
#define SSR_REFINE   5
// rougher than this, the reflection is too blurred to be one
#define SSR_ROUGHEST 0.45

float SsrSceneDepth(vec2 uv)
{
	float z = texture2D(u_SsrDepthMap, uv).r * 2.0 - 1.0;
	float n = u_SsrParams.z, f = u_SsrParams.w;
	return 2.0 * n * f / (f + n - z * (f - n));
}

// x y z the colour, w how much of it (0: none found); in steps steps (at
// most SSR_MAX_STEPS). What the ray meets or passes behind before it has
// risen rise units off the surface is not seen: still water hides the
// ground around it, the bumps a puddle lies among
// how far along its ray the last SsrTrace met something (Added in OPM: the
// puddles take what is near from the screen, what is far from their mirror)
float ssrHitT = 0.0;

vec4 SsrTrace(vec3 P, vec3 N, vec3 E, float roughness, float steps, float rise)
{
	if (u_SsrParams.x <= 0.0 || roughness >= SSR_ROUGHEST)
		return vec4(0.0);

	vec3  R      = reflect(-E, N);
	float reach  = u_SsrParams.y;
	// steps staggered pixel to pixel; a puddle's (rise > 0), in its many
	// steps, the same for all: staggered, its hits and misses side by side
	// were a grain over it
	float jitter = rise > 0.0 ? 0.5 : fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float prevT  = 0.0, t = 0.0;
	bool  hit    = false;
	vec4  c;

	for (int i = 0; i < SSR_MAX_STEPS; i++)
	{
		if (float(i) >= steps)
			break;
		float s = (float(i) + jitter) / steps;
		t = 2.0 + reach * s * s;
		c = u_SsrViewProj * vec4(P + R * t, 1.0);
		if (c.w <= 1.0)
			break;
		vec2 uv = c.xy / c.w * 0.5 + 0.5;
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
			break;
		float behind = c.w - SsrSceneDepth(uv);
		if (behind > 0.0 && dot(R, N) * t >= rise)
		{
			// behind what is seen there, not by much: the ray meets it.
			// Further behind, it passed behind something: what it would
			// meet is hidden from the screen
			if (behind < max(12.0, 1.5 * (t - prevT)))
				hit = true;
			break;
		}
		prevT = t;
	}
	if (!hit)
		return vec4(0.0);

	// where between the last two steps it meets it
	{
		float a = prevT, b = t;
		for (int j = 0; j < SSR_REFINE; j++)
		{
			float m  = 0.5 * (a + b);
			vec4  cm = u_SsrViewProj * vec4(P + R * m, 1.0);
			if (cm.w - SsrSceneDepth(cm.xy / cm.w * 0.5 + 0.5) > 0.0)
				b = m;
			else
				a = m;
		}
		t = b;
	}
	ssrHitT = t;

	// there in the last frame
	vec4 pc = u_SsrPrevViewProj * vec4(P + R * t, 1.0);
	if (pc.w <= 1.0)
		return vec4(0.0);
	vec2 puv = pc.xy / pc.w * 0.5 + 0.5;
	vec2 edge = min(puv, vec2(1.0) - puv);
	float w = smoothstep(0.0, 0.08, min(edge.x, edge.y))
		* (1.0 - smoothstep(0.6 * reach, reach, t))
		* (1.0 - smoothstep(0.5 * SSR_ROUGHEST, SSR_ROUGHEST, roughness));
	if (w <= 0.0)
		return vec4(0.0);
	return vec4(texture2D(u_SsrColorMap, puv).rgb, w * min(u_SsrParams.x, 1.0));
}
#endif

#if defined(USE_RTLIGHT)
varying vec3      var_RtPos;
varying vec3      var_RtNormal;
#endif

uniform vec4      u_GlobalFogColor;
uniform vec2      u_GlobalFogParams;

// Added in OPM: r_linearLighting. The colours read as linear light (gamma
// 2.2, as the display showed them), added up as light; the tone map shows the
// frame (GLSL_BindProgram sets it, for the world's view alone)
uniform float     u_Linear;

vec3 Lin(vec3 c)
{
	return u_Linear > 0.5 ? pow(max(c, vec3(0.0)), vec3(2.2)) : c;
}

// The map's distance fog, mixed as it was shown (the mix of a colour shown and
// the fog's): mixed as light, a dark wall took far more of a grey fog
vec3 Fog(vec3 c, vec3 fog, float f)
{
	if (u_Linear > 0.5)
		return Lin(mix(pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)), fog, f));
	return mix(c, fog, f);
}

varying vec4      var_TexCoords;
varying float     var_FogDist;

varying vec4      var_Color;
#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4      var_ColorAmbient;
#endif

#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4   var_Normal;
  #if defined(USE_TANGENT_FRAME)
varying vec4   var_Tangent;
varying vec4   var_Bitangent;
  #else
varying vec3   var_ViewDir;
  #endif
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
varying vec4      var_LightDir;
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
varying vec4      var_PrimaryLightDir;
#endif


#define EPSILON 0.00000001

#if defined(USE_PARALLAXMAP)
float SampleDepth(sampler2D normalMap, vec2 t)
{
  #if defined(SWIZZLE_NORMALMAP)
	return 1.0 - texture2D(normalMap, t).r;
  #else
	return 1.0 - texture2D(normalMap, t).a;
  #endif
}

// Added in OPM: the depth read with the pixel's own gradients, so that a
// search that stops at its hit (non-uniform flow, where implicit derivatives
// are undefined) still reads the mip the pixel would
#if __VERSION__ >= 130
#define PARALLAX_GRAD
float SampleDepthGrad(sampler2D normalMap, vec2 t, vec2 gx, vec2 gy)
{
  #if defined(SWIZZLE_NORMALMAP)
	return 1.0 - textureGrad(normalMap, t, gx, gy).r;
  #else
	return 1.0 - textureGrad(normalMap, t, gx, gy).a;
  #endif
}
#else
#define SampleDepthGrad(m, t, gx, gy) SampleDepth(m, t)
#endif

// (OPM: steps, as many as the relief spans pixels, 16 at most; the search
// stops at the first point inside, which was all it kept)
float RayIntersectDisplaceMap(vec2 dp, vec2 ds, sampler2D normalMap, vec2 gx, vec2 gy, float steps)
{
	const int linearSearchSteps = 16;
	const int binarySearchSteps = 6;

	// current size of search window
	float size = 1.0 / steps;

	// adjust position if offset above surface
	dp -= ds * r_parallaxMapOffset;

	// current depth position
	float depth = 0.0;

	// best match found (starts with last position 1.0)
	float bestDepth = 1.0;

	// texture depth at best depth
	float texDepth = 0.0;

	float prevT = SampleDepthGrad(normalMap, dp, gx, gy);
	float prevTexDepth = prevT;

	// search front to back for first point inside object
	for(int i = 0; i < linearSearchSteps - 1; ++i)
	{
		if (float(i) >= steps - 1.0)
			break;

		depth += size;
		
		float t = SampleDepthGrad(normalMap, dp + ds * depth, gx, gy);
		
		if(depth >= t)
		{
			bestDepth = depth;	// store best depth
			texDepth = t;
			prevTexDepth = prevT;
#if defined(PARALLAX_GRAD)
			break;
#endif
		}
		prevT = t;
	}

	depth = bestDepth;

#if !defined (USE_RELIEFMAP)
	float div = 1.0 / (1.0 + (prevTexDepth - texDepth) * steps);
	bestDepth -= (depth - size - prevTexDepth) * div;
#else
	// recurse around first point (depth) for closest match
	for(int i = 0; i < binarySearchSteps; ++i)
	{
		size *= 0.5;

		float t = SampleDepthGrad(normalMap, dp + ds * depth, gx, gy);
		
		if(depth >= t)
		{
			bestDepth = depth;
			depth -= 2.0 * size;
		}

		depth += size;
	}
#endif

	return bestDepth - r_parallaxMapOffset;
}

// (OPM: gx, gy, the pixel's own texture gradients, as for the search: the ray
// stops at its first hit and is only cast where parallax is on, both per
// pixel, so the plain reads it made had undefined derivatives and could fetch
// any mip, the noise the search had before it read with gradients)
float LightRay(vec2 dp, vec2 ds, sampler2D normalMap, vec2 gx, vec2 gy)
{
	const int linearSearchSteps = 16;

	// current size of search window
	float size = 1.0 / float(linearSearchSteps);

	// current height from initial texel depth
	float height = 0.0;

	float startDepth = SampleDepthGrad(normalMap, dp, gx, gy);

	// find a collision or escape
	for(int i = 0; i < linearSearchSteps - 1; ++i)
	{
		height += size;

		if (startDepth < height)
			return 1.0;
		
		float t = SampleDepthGrad(normalMap, dp + ds * height, gx, gy);

		if (startDepth > t + height)
			return 0.0;
	}

	return 1.0;
}

// Added in OPM: LightRay for a light in tangent space. No direction (a model
// lit per vertex) or a light at or below the surface's plane casts no relief
// shadow: dividing by z there made the ray NaN, and whatever that fetch read
// put black blotches in every crevice. The shadow fades in over the first few
// degrees above the plane, where the ray would stretch across the texture.
float ParallaxShadow(vec3 lightDir, float scale, vec2 dp, sampler2D normalMap, vec2 gx, vec2 gy)
{
	float fade = smoothstep(0.0, 0.1, lightDir.z);
	if (fade <= 0.0)
		return 1.0;

	return mix(1.0, LightRay(dp, lightDir.xy * (scale / lightDir.z), normalMap, gx, gy), fade);
}
#endif

vec3 CalcDiffuse(vec3 diffuseAlbedo, float NH, float EH, float roughness)
{
#if defined(USE_BURLEY)
	// modified from https://disney-animation.s3.amazonaws.com/library/s2012_pbs_disney_brdf_notes_v2.pdf
	float fd90 = -0.5 + EH * EH * roughness;
	float burley = 1.0 + fd90 * 0.04 / NH;
	burley *= burley;
	return diffuseAlbedo * burley;
#else
	return diffuseAlbedo;
#endif
}

vec3 EnvironmentBRDF(float roughness, float NE, vec3 specular)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float v = 1.0 - max(roughness, NE);
	v *= v * v;
	return vec3(v) + specular;
}

vec3 CalcSpecular(vec3 specular, float NH, float EH, float roughness)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float rr = roughness*roughness;
	float rrrr = rr*rr;
	float d = (NH * NH) * (rrrr - 1.0) + 1.0;
	float v = (EH * EH) * (roughness + 0.5) + EPSILON;
	return specular * (rrrr / (4.0 * d * d * v));
}

// Added in OPM: the sun's glint on what is wet (u_WetSun: towards it, how
// bright). Behind the rain's clouds the way to it is longer than 1 by how
// overcast it is (R_WaterSetWet), and the glint is the clouds': wide and
// soft, no bright sun in it
vec3 WetSunGlint(vec3 N, vec3 E, vec3 specular, float roughness)
{
	float len = length(u_WetSun.xyz);
	if (u_WetSun.w <= 0.0 || len < 0.001)
		return vec3(0.0);
	vec3  L  = u_WetSun.xyz / len;
	float oc = max(len - 1.0, 0.0);
	vec3  H  = normalize(L + E);
	roughness = mix(roughness, max(roughness, 0.55), clamp(oc * 2.0, 0.0, 1.0));
	return u_WetSun.w * clamp(dot(N, L), 0.0, 1.0)
		* CalcSpecular(specular, clamp(dot(N, H), 0.0, 1.0), clamp(dot(E, H), 0.0, 1.0), roughness);
}


float CalcLightAttenuation(float point, float normDist)
{
	// zero light at 1.0, approximating q3 style
	// also don't attenuate directional light
	float attenuation = (0.5 * normDist - 1.5) * point + 1.0;

	// clamp attenuation
	#if defined(NO_LIGHT_CLAMP)
	attenuation = max(attenuation, 0.0);
	#else
	attenuation = clamp(attenuation, 0.0, 1.0);
	#endif

	return attenuation;
}

#if defined(USE_BOX_CUBEMAP_PARALLAX)
vec4 hitCube(vec3 ray, vec3 pos, vec3 invSize, float lod, samplerCube tex)
{
	// find any hits on cubemap faces facing the camera
	vec3 scale = (sign(ray) - pos) / ray;

	// find the nearest hit
	float minScale = min(min(scale.x, scale.y), scale.z);

	// if the nearest hit is behind the camera, ignore
	// should not be necessary as long as pos is inside the cube
	//if (minScale < 0.0)
		//return vec4(0.0);

	// calculate the hit position, that's our texture coordinates
	vec3 tc = pos + ray * minScale;

	// if the texture coordinates are outside the cube, ignore
	// necessary since we're not fading out outside the cube
	if (any(greaterThan(abs(tc), vec3(1.00001))))
		return vec4(0.0);

	// fade out when approaching the cubemap edges
	//vec3 fade3 = abs(pos);
	//float fade = max(max(fade3.x, fade3.y), fade3.z);
	//fade = clamp(1.0 - fade, 0.0, 1.0);
			
	//return vec4(textureCubeLod(tex, tc, lod).rgb * fade, fade);
	return vec4(textureCubeLod(tex, tc, lod).rgb, 1.0);
}
#endif

void main()
{
	vec3 viewDir, lightColor, ambientColor, reflectance;
	vec3 L, N, E, H;
	float NL, NH, NE, EH, attenuation;

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
  #if defined(USE_GROUNDCOVER)
	// Added in OPM: a tuft is lit from above as the ground it stands in is,
	// whichever side of it shows (turned over, the tufts seen from behind
	// went black in clumps)
	vec3 surfNormal = var_Normal.xyz;
  #else
	vec3 surfNormal = (!gl_FrontFacing ? var_Normal : -var_Normal).xyz;
  #endif
  #if defined(USE_TANGENT_FRAME)
	mat3 tangentToWorld = mat3(var_Tangent.xyz, var_Bitangent.xyz, surfNormal);
	viewDir = vec3(var_Normal.w, var_Tangent.w, var_Bitangent.w);
  #else
	viewDir = var_ViewDir;
  #endif
	E = normalize(viewDir);
#endif

	lightColor = Lin(var_Color.rgb);

#if defined(USE_RTLIGHT) && defined(USE_LIGHT_VERTEX)
	// all of it live: the baked vertex lighting gives way to the ambient
	if (u_RtParams.y > 1.5)
		lightColor = u_RtAmbient.rgb * RtAmbientOcclusion();
#endif

#if defined(USE_LIGHTMAP)
	vec4 lightmapColor = texture2D(u_LightMap, var_TexCoords.zw);
  #if defined(RGBM_LIGHTMAP)
	lightmapColor.rgb *= lightmapColor.a;
  #endif
  #if defined(USE_PBR) && !defined(USE_FAST_LIGHT)
	lightmapColor.rgb *= lightmapColor.rgb;
  #endif
	lightColor *= Lin(lightmapColor.rgb);

  #if defined(USE_RTLIGHT)
	// all of it live: the lightmap gives way to the ambient
	if (u_RtParams.y > 1.5)
		lightColor = Lin(var_Color.rgb) * u_RtAmbient.rgb * RtAmbientOcclusion();
  #endif
#endif

	vec2 texCoords = var_TexCoords.xy;

#if defined(USE_PARALLAXMAP)
	vec3 offsetDir = E * tangentToWorld;

	// Added in OPM: the offset grows as 1/z toward grazing views, without
	// bound, and slid the texture across the surface as the view moved. Cap
	// it at 4x the depth, and fade it out where the surface is seen edge on.
	offsetDir.xy *= -u_NormalScale.a * smoothstep(0.0, 0.25, offsetDir.z) / max(offsetDir.z, 0.25);

	// Added in OPM: how far the deepest of it moves on the screen. Under half
	// a pixel (a wall across the room) it cannot be seen, and is not searched
	// for, nor its shadows; above, a step a pixel, 4 to 16.
	vec2  parallaxGx = dFdx(texCoords), parallaxGy = dFdy(texCoords);
	float reliefPixels = length(offsetDir.xy) / max(max(length(parallaxGx), length(parallaxGy)), 1e-7);
	bool  parallaxOn = reliefPixels >= 0.5;

	if (parallaxOn)
		texCoords += offsetDir.xy * RayIntersectDisplaceMap(texCoords, offsetDir.xy, u_NormalMap, parallaxGx, parallaxGy, clamp(ceil(reliefPixels), 4.0, 16.0));
#endif

	vec4 diffuse = texture2D(u_DiffuseMap, texCoords);
	
	float alpha = diffuse.a * var_Color.a;
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
	// Added in OPM: alpha to coverage (RB_AlphaToCoverage): the same cut, as a
	// ramp a pixel wide that the samples it covers smooth
	else if (u_AlphaTest == 4)
	{
		alpha = clamp((alpha - 0.5) / max(fwidth(alpha), 0.0001) + 0.5, 0.0, 1.0);
		if (alpha <= 0.0)
			discard;
	}

	// (r_linearLighting: the texture's colour as light; its alpha as it is)
	diffuse.rgb = Lin(diffuse.rgb);

	// Added in OPM: how wet (see WetAmount), and the light it had baked, for
	// how much sky it sees
	vec3 wetSurfN;
	vec3 wet       = WetAmount(wetSurfN);
	vec3 wetLight  = lightColor;
	// Added in OPM: the tilt the rain running down it gives it (RainRuns)
	vec3 wetRunN   = vec3(0.0);
	float wetRun   = 0.0;
	// Added in OPM: the reflection of what is on the screen (SsrTrace)
	vec4 ssr       = vec4(0.0);

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	L = var_LightDir.xyz;
  #if defined(USE_DELUXEMAP)
	L += (texture2D(u_DeluxeMap, var_TexCoords.zw).xyz - vec3(0.5)) * u_EnableTextures.y;
  #endif
	float sqrLightDist = dot(L, L);
	// Added in OPM: no direction at all (a model lit per vertex) is no light
	// from any way: all of it ambient, as it was where the division made it
	// NaN and the driver's clamps turned that to nothing
	L = sqrLightDist > 0.0 ? L * inversesqrt(sqrLightDist) : vec3(0.0);

  #if defined(USE_LIGHT_VECTOR)
	attenuation  = CalcLightAttenuation(float(var_LightDir.w > 0.0), var_LightDir.w / sqrLightDist);
  #else
	attenuation  = 1.0;
  #endif

  #if defined(USE_NORMALMAP)
    #if defined(SWIZZLE_NORMALMAP)
	N.xy = texture2D(u_NormalMap, texCoords).ag - vec2(0.5);
    #else
	N.xy = texture2D(u_NormalMap, texCoords).rg - vec2(0.5);
    #endif
	N.xy *= u_NormalScale.xy;
	N.z = sqrt(clamp((0.25 - N.x * N.x) - N.y * N.y, 0.0, 1.0));
	N = tangentToWorld * N;
  #else
	N = surfNormal;
  #endif

	N = normalize(N);

  #if defined(USE_SHADOWMAP) 
	vec2 shadowTex = gl_FragCoord.xy * r_FBufScale;
	float shadowValue = texture2D(u_ShadowMap, shadowTex).r;

	// surfaces not facing the light are always shadowed
	shadowValue *= clamp(dot(N, var_PrimaryLightDir.xyz), 0.0, 1.0);

    #if defined(SHADOWMAP_MODULATE)
	lightColor *= shadowValue * (1.0 - u_PrimaryLightAmbient.r) + u_PrimaryLightAmbient.r;
    #endif
  #endif

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	float parallaxShadow = parallaxOn ? ParallaxShadow(L * tangentToWorld, u_NormalScale.a, texCoords, u_NormalMap, parallaxGx, parallaxGy) : 1.0;
  #endif


  #if !defined(USE_LIGHT_VECTOR)
	ambientColor = lightColor;
	float surfNL = clamp(dot(surfNormal, L), 0.0, 1.0);

	// reserve 25% ambient to avoid black areas on normalmaps
	lightColor *= 0.75;

	// Scale the incoming light to compensate for the baked-in light angle
	// attenuation.
	lightColor /= max(surfNL, 0.25);

	// Recover any unused light as ambient, in case attenuation is over 4x or
	// light is below the surface
	ambientColor = max(ambientColor - lightColor * surfNL, vec3(0.0));
  #else
	ambientColor = Lin(var_ColorAmbient.rgb);
  #endif

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	// Added in OPM: the relief shadows only the direct light, after the
	// ambient is split off; shadowing it all left lightmapped crevices black
	lightColor *= parallaxShadow;
  #endif

	NL = clamp(dot(N, L), 0.0, 1.0);
	NE = clamp(dot(N, E), 0.0, 1.0);
	H = normalize(L + E);
	EH = clamp(dot(E, H), 0.0, 1.0);
	NH = clamp(dot(N, H), 0.0, 1.0);

  #if defined(USE_SPECULARMAP)
	vec4 specular = texture2D(u_SpecularMap, texCoords);
  #else
	vec4 specular = vec4(1.0);
  #endif
	specular *= u_SpecularScale;

  #if defined(USE_PBR)
	diffuse.rgb *= diffuse.rgb;
  #endif

  #if defined(USE_PBR)
	// diffuse rgb is base color
	// specular red is gloss
	// specular green is metallicness
	float gloss = specular.r;
	float metal = specular.g;
	specular.rgb = metal * diffuse.rgb + vec3(0.04 - 0.04 * metal);
	diffuse.rgb *= 1.0 - metal;
  #else
	// diffuse rgb is diffuse
	// specular rgb is specular reflectance at normal incidence
	// specular alpha is gloss
	float gloss = specular.a;

	// adjust diffuse by specular reflectance, to maintain energy conservation
	diffuse.rgb *= vec3(1.0) - specular.rgb;
  #endif

  #if defined(GLOSS_IS_GLOSS)
	float roughness = exp2(-3.0 * gloss);
  #elif defined(GLOSS_IS_SMOOTHNESS)
	float roughness = 1.0 - gloss;
  #elif defined(GLOSS_IS_ROUGHNESS)
	float roughness = gloss;
  #elif defined(GLOSS_IS_SHININESS)
	float roughness = pow(2.0 / (8190.0 * gloss + 2.0), 0.25);
  #endif

	// Added in OPM: wet, the material before it is lit; the film flattens
	// the normal map's bumps
	if (wet.x > 0.0)
	{
		// the rain running down it (on the world's own surfaces)
		// (none on a cut-out: a tree line, a hedge, a fence; R_WaterSetWet)
		if (abs(mod(u_Wet.w, 10.0) - 1.0) < 0.5 && u_Wet.z > 0.0)
		{
			vec3  runN;
			float run = RainRuns(var_WetPos, wetSurfN, u_RainParams.x, step(0.1, mod(u_Wet.w, 10.0) - 1.0), runN) * wet.x;
			if (run > 0.0)
			{
				wet.y   = max(wet.y, run);
				wetRunN = runN * wet.x;
				wetRun  = run;
			}
		}

		// what drinks the water holds less of it on top
		float porosity = WetPorosity(roughness, specular.rgb);
		wet.y *= mix(1.0, 0.5, porosity);
		WetMaterial(wet.xy, porosity, diffuse.rgb, roughness);
		// the water running down it: darker, and glossy over what is wet
		diffuse.rgb *= mix(1.0, 0.9, wetRun);
		roughness    = mix(roughness, 0.06, wetRun);
		specular.rgb = mix(specular.rgb, max(specular.rgb, vec3(0.02)), wet.y);
		N  = normalize(mix(N, surfNormal, 0.8 * wet.y) + wetRunN);

		// a puddle: still water over it, darker under, rung by the rain
		if (wet.z > 0.0)
		{
			vec2 ring = RainRipples(var_WetPos.xy, u_RainParams.x);
			diffuse.rgb *= mix(1.0, 0.4, wet.z);
			roughness    = mix(roughness, 0.08, wet.z);
			specular.rgb = mix(specular.rgb, vec3(0.02), wet.z);
			N = normalize(mix(N, normalize(wetSurfN + vec3(ring, 0.0)), wet.z));
		}

		// beads on the hands and the weapon in view
		if (abs(mod(u_Wet.w, 10.0) - 2.0) < 0.5 && u_RainParams.w > 0.0)
		{
			vec2  slope;
			float bead = RainBeads(var_TexCoords.xy, slope) * wet.x * u_RainParams.w;

			if (bead > 0.0)
			{
				// the texture's frame on the surface, from how both change
				// across the screen
				vec3  dp1 = dFdx(var_WetPos), dp2 = dFdy(var_WetPos);
				vec2  du1 = dFdx(var_TexCoords.xy), du2 = dFdy(var_TexCoords.xy);
				vec3  p2  = cross(dp2, wetSurfN), p1 = cross(wetSurfN, dp1);
				vec3  T   = p2 * du1.x + p1 * du2.x;
				vec3  B   = p2 * du1.y + p1 * du2.y;
				float inv = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));

				diffuse.rgb *= mix(1.0, 0.9, bead);
				roughness    = mix(roughness, 0.16, bead);
				specular.rgb = mix(specular.rgb, max(specular.rgb, vec3(0.03)), bead);
				N = normalize(mix(N, normalize(N - (T * slope.x + B * slope.y) * inv), bead));
			}
		}

		NL = clamp(dot(N, L), 0.0, 1.0);
		NE = clamp(dot(N, E), 0.0, 1.0);
		NH = clamp(dot(N, H), 0.0, 1.0);
	}

  #if defined(USE_SSR)
	ssr = SsrTrace(var_WetPos, N, E, roughness, float(SSR_STEPS), 0.0);
  #endif

	reflectance  = CalcDiffuse(diffuse.rgb, NH, EH, roughness);

  #if defined(r_deluxeSpecular)
    #if defined(USE_LIGHT_VECTOR)
	reflectance += CalcSpecular(specular.rgb, NH, EH, roughness) * r_deluxeSpecular;
    #else
	reflectance += CalcSpecular(specular.rgb, NH, EH, pow(roughness, r_deluxeSpecular));
    #endif
  #endif

	gl_FragColor.rgb  = lightColor   * reflectance * (attenuation * NL);
	gl_FragColor.rgb += ambientColor * diffuse.rgb;

  #if defined(USE_CUBEMAP)
	reflectance = EnvironmentBRDF(roughness, NE, specular.rgb);

	vec3 R = reflect(E, N);

	// parallax corrected cubemap (cheaper trick)
	// from http://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/
	vec3 parallax = u_CubeMapInfo.xyz + u_CubeMapInfo.w * viewDir;

  #if defined(USE_BOX_CUBEMAP_PARALLAX)
	vec3 cubeLightColor = hitCube(R * u_CubeMapInfo.w, parallax, u_CubeMapInfo.www, ROUGHNESS_MIPS * roughness, u_CubeMap).rgb * u_EnableTextures.w;
  #else
	vec3 cubeLightColor = textureCubeLod(u_CubeMap, R + parallax, ROUGHNESS_MIPS * roughness).rgb * u_EnableTextures.w;
  #endif

	// normalize cubemap based on last roughness mip (~diffuse)
	// multiplying cubemap values by lighting below depends on either this or the cubemap being normalized at generation
	//vec3 cubeLightDiffuse = max(textureCubeLod(u_CubeMap, N, ROUGHNESS_MIPS).rgb, 0.5 / 255.0);
	//cubeLightColor /= dot(cubeLightDiffuse, vec3(0.2125, 0.7154, 0.0721));

    #if defined(USE_PBR)
	cubeLightColor *= cubeLightColor;
    #else
	// Added in OPM: a cubemap holds what was shown, not light (r_linearLighting)
	cubeLightColor = Lin(cubeLightColor);
    #endif

	// multiply cubemap values by lighting
	// not technically correct, but helps make reflections look less unnatural
	//cubeLightColor *= lightColor * (attenuation * NL) + ambientColor;

    #if defined(USE_SSR)
	cubeLightColor = mix(cubeLightColor, ssr.rgb, ssr.a);
    #endif

	gl_FragColor.rgb += cubeLightColor * reflectance;
  #elif defined(USE_SSR)
	// Added in OPM: with no cubemap, the reflection where one was found
	gl_FragColor.rgb += ssr.rgb * ssr.a * EnvironmentBRDF(roughness, NE, specular.rgb);
  #endif

  #if defined(USE_PRIMARY_LIGHT) || defined(SHADOWMAP_MODULATE)
	vec3 L2, H2;
	float NL2, EH2, NH2;

	L2 = var_PrimaryLightDir.xyz;

	// enable when point lights are supported as primary lights
	//sqrLightDist = dot(L2, L2);
	//L2 /= sqrt(sqrLightDist);

	NL2 = clamp(dot(N, L2), 0.0, 1.0);
	H2 = normalize(L2 + E);
	EH2 = clamp(dot(E, H2), 0.0, 1.0);
	NH2 = clamp(dot(N, H2), 0.0, 1.0);

	reflectance  = CalcSpecular(specular.rgb, NH2, EH2, roughness);

	// bit of a hack, with modulated shadowmaps, ignore diffuse
    #if !defined(SHADOWMAP_MODULATE)
	reflectance += CalcDiffuse(diffuse.rgb, NH2, EH2, roughness);
    #endif

	lightColor = Lin(u_PrimaryLightColor);

    #if defined(USE_SHADOWMAP)
	lightColor *= shadowValue;
    #endif

	// enable when point lights are supported as primary lights
	//lightColor *= CalcLightAttenuation(float(u_PrimaryLightDir.w > 0.0), u_PrimaryLightDir.w / sqrLightDist);

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	if (parallaxOn)
		lightColor *= ParallaxShadow(L2 * tangentToWorld, u_NormalScale.a, texCoords, u_NormalMap, parallaxGx, parallaxGy);
  #endif

	gl_FragColor.rgb += lightColor * reflectance * NL2;
  #endif

  #if defined(USE_PBR)
	gl_FragColor.rgb = sqrt(gl_FragColor.rgb);
  #endif

	// Added in OPM: what the wet film gives back (a cubemap's reflection is
	// the surface's own), and the sun where no primary light pass draws it
	if (wet.y > 0.0)
	{
    #if !defined(USE_CUBEMAP)
		gl_FragColor.rgb += wet.y * (1.0 - ssr.a) * WetSky(N, E, roughness, specular.rgb, wetLight);
    #endif
    #if !defined(USE_PRIMARY_LIGHT) && !defined(SHADOWMAP_MODULATE)
		gl_FragColor.rgb += wet.y * WetSunGlint(N, E, specular.rgb, roughness);
    #endif
	}

#else

	// Added in OPM: wet, with no material maps: a porous dull surface
	if (wet.x > 0.0)
	{
		float wetRough = 0.6;
		vec3  Ew       = normalize(u_ViewOrigin - var_WetPos);
		vec3  F0       = vec3(0.02);

		wet.y *= 0.65;
		vec3 wetN = wetSurfN;
		// the rain running down it (RainRuns)
		// (none on a cut-out: a tree line, a hedge, a fence; R_WaterSetWet)
		if (abs(mod(u_Wet.w, 10.0) - 1.0) < 0.5 && u_Wet.z > 0.0)
		{
			vec3  runN;
			float run = RainRuns(var_WetPos, wetSurfN, u_RainParams.x, step(0.1, mod(u_Wet.w, 10.0) - 1.0), runN) * wet.x;
			if (run > 0.0)
			{
				wet.y    = max(wet.y, run);
				wetRough = mix(wetRough, 0.1, run);
				diffuse.rgb *= mix(1.0, 0.9, run);
				wetN     = normalize(wetN + runN * wet.x);
			}
		}
		WetMaterial(wet.xy, 0.7, diffuse.rgb, wetRough);
		// a puddle: still water, darker under, rung by the rain
		if (wet.z > 0.0)
		{
			wet.y    = max(wet.y, wet.z);
			wetRough = mix(wetRough, 0.08, wet.z);
			diffuse.rgb *= mix(1.0, 0.4, wet.z);
			wetN = normalize(wetSurfN + vec3(RainRipples(var_WetPos.xy, u_RainParams.x), 0.0) * wet.z);
		}
		gl_FragColor.rgb  = diffuse.rgb * lightColor;
		gl_FragColor.rgb += wet.y * WetSky(wetN, Ew, wetRough, F0, wetLight);
		gl_FragColor.rgb += wet.y * WetSunGlint(wetN, Ew, F0, wetRough);
	}
	else
		gl_FragColor.rgb = diffuse.rgb * lightColor;

#endif

	// Added in OPM: a puddle mirrors what is over it, the more the lower it
	// is seen, its face tilted by the rings of the rain falling in it: the
	// world on the screen where the reflected ray finds it (SsrTrace), the
	// sky where it finds nothing
	float wetHit = 0.0;
	vec3  wetRefl = vec3(0.0);
	if (wet.z > 0.0)
	{
		vec3  Ep   = normalize(u_ViewOrigin - var_WetPos);
		vec2  ring = RainRipples(var_WetPos.xy, u_RainParams.x);
		vec3  Np   = normalize(wetSurfN + vec3(ring, 0.0));
		float F    = 0.03 + 0.97 * pow(1.0 - clamp(dot(Np, Ep), 0.0, 1.0), 5.0);
		vec3  R    = reflect(-Ep, Np);
		vec3  refl = u_WetSky.rgb * mix(0.55, 1.15, smoothstep(0.0, 0.6, R.z));
		float most = 0.75;

		// the water lies level at the top of the ground under it (the
		// cover's grid, which holds the highest of it there), not on its
		// bumps: what it gives back is worked out from there, where the
		// eye's ray through this pixel meets it. From the ground itself the
		// ray ran into the bumps before a man's boots, his reflection a gap
		// below them
		vec3 Pw = var_WetPos;
		{
			vec2  uv = (var_WetPos.xy - u_RainCover.xy) * u_RainCover.zw;
			float hw = texture2D(u_RainCoverMap, clamp(uv, 0.0, 1.0)).r;
			hw = clamp(hw, var_WetPos.z, var_WetPos.z + 6.0);
			// a puddle is one level all over: near the mirror's, the
			// mirror's (the grid's, from cell to cell, made patches of it
			// that looked the mirror up at other heights, a grey cloud over
			// the puddle that moved as the eye did)
			if (u_RainReflect.x > 0.5 && abs(hw - u_RainReflect.y) < 10.0 && var_WetPos.z <= u_RainReflect.y + 2.0)
				hw = u_RainReflect.y;
			if (u_ViewOrigin.z - hw > 1.0)
				Pw = u_ViewOrigin + (var_WetPos - u_ViewOrigin) * ((u_ViewOrigin.z - hw) / (u_ViewOrigin.z - var_WetPos.z));
		}

  #if defined(USE_SSR)
		// what is on the screen: right at any height, so a man's reflection
		// meets his feet
		{
			// the water lies level whatever the ground under it; only the
			// rings tilt it, a little
			vec3 Nw    = normalize(vec3(ring * 0.1, 1.0));
			vec4 world = SsrTrace(Pw, Nw, Ep, 0.08, 32.0, 3.0);
			// what is near only, where there is a mirror: what is far is the
			// mirror's, which has its own air; the screen's has the air to
			// the eye in it already, greyer, and faded in and out of the
			// mirror's it was a grey cloud over the puddle as the eye moved
			if (u_RainReflect.x > 0.5)
				world.a *= 1.0 - smoothstep(200.0, 450.0, ssrHitT);
			refl   = mix(refl, world.rgb, world.a);
			wetHit = world.a;
		}
  #endif

		// where the screen has nothing (off it, far off): the world drawn
		// mirrored at the height of the puddle nearest the eye
		// (R_RainPuddleHeight), off the screen too. A lower one looks it up
		// where the eye's ray through it crosses that height, which gives
		// back the same way: far off where it should be, what is near a
		// little out (the screen has that). One higher than it would look
		// under the ground: it looks the way its ray goes, far off, where
		// the mirror has nothing lower than it that way
		if (wetHit < 0.99 && u_RainReflect.x > 0.5 && u_ViewOrigin.z - var_WetPos.z > 1.0)
		{
			float dh = Pw.z - u_RainReflect.y;
			vec4  clip;
			float near;
			if (dh <= 3.0)
			{
				near = 1.0 - smoothstep(96.0, 160.0, -dh);
				vec3 Q = u_ViewOrigin + (var_WetPos - u_ViewOrigin)
					* ((u_ViewOrigin.z - u_RainReflect.y) / (u_ViewOrigin.z - var_WetPos.z));
				clip = u_RainReflectMvp * vec4(Q + vec3(ring * 10.0, 0.0), 1.0);
			}
			else
			{
				// higher than the mirror: what lies the way its reflected ray
				// goes, far off (what is near is the screen's), unless what
				// the mirror drew that way is lower than this puddle: the
				// ground or rubble it lies on, seen from under it
				vec3 Rw = reflect(-Ep, normalize(vec3(ring * 0.1, 1.0)));
				vec3 X  = Pw + Rw * 4000.0;
				clip = u_RainReflectMvp * vec4(X, 1.0);
				near = 0.0;
				if (clip.w > 0.1)
				{
					vec3  Em = vec3(u_ViewOrigin.xy, 2.0 * u_RainReflect.y - u_ViewOrigin.z);
					float k  = (Pw.z - Em.z) / max(X.z - Em.z, 0.001);
					vec4  cy = u_RainReflectMvp * vec4(Em + (X - Em) * clamp(k, 0.0, 1.0), 1.0);
					vec2  at = clamp(clip.xy / clip.w * 0.5 + 0.5, 0.001, 0.999);
					float ds = texture2D(u_RainReflectDepthMap, at * u_RainReflect.zw).r;
					float dy = cy.w > 0.0 ? cy.z / cy.w * 0.5 + 0.5 : 0.0;
					near = ds > dy ? 1.0 : 0.0;
				}
			}
			if (near > 0.0)
			{
				vec2 at   = clamp(clip.xy / max(clip.w, 0.001) * 0.5 + 0.5, 0.001, 0.999);
				float take = near * (1.0 - wetHit);
				refl   = mix(refl, texture2D(u_RainReflectMap, at * u_RainReflect.zw).rgb, take);
				wetHit += take;
			}
		}

		// still water seen low gives back nearly all of what it mirrors
		most = mix(most, 0.92, wetHit);

		wetRefl = refl;
		gl_FragColor.rgb = mix(gl_FragColor.rgb, refl, clamp(0.04 + F * 1.4, 0.0, most) * wet.z);
	}

	// Added in OPM: a lightning flash lights what is out under the sky, from
	// above more than from the side; what has a roof over it stays dark
	if (u_RainFlash.a > 0.0 && u_Wet.z > 0.0)
	{
		float open = RainExposed(var_WetPos, wetSurfN);
		gl_FragColor.rgb += diffuse.rgb * u_RainFlash.rgb * u_RainFlash.a * open
			* mix(0.3, 1.0, clamp(wetSurfN.z * 0.5 + 0.5, 0.0, 1.0));
	}

	// Added in OPM: r_rainDebug: red how wet, green a puddle, blue where a
	// puddle mirrors the world
	// (r_rainDebug 2: what the puddles mirror, alone)
	if (u_Wet.w > 19.5)
		gl_FragColor.rgb = wet.z > 0.0 ? wetRefl : gl_FragColor.rgb * 0.25;
	else if (u_Wet.w > 9.5)
		gl_FragColor.rgb = vec3(wet.x, wet.z, wetHit);

	gl_FragColor.a = alpha;


#if defined(USE_RTLIGHT)
	// Added in OPM: the realtime lights, on the surface's own colour; not on
	// a glow added on top, nor on dust and smoke without a light of their own
	// (R_RtStageKind), every pixel of a muzzle flash through every light
	if (u_RtStage != RT_STAGE_NONE)
	{
  #if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
		vec3 rtNormal = N;
		// the surface's own, which the baked lamps were worked out for, on
		// the side the normal map's is
		rt_geomN = normalize(var_RtNormal);
		if (dot(rt_geomN, N) < 0.0) rt_geomN = -rt_geomN;

		// the lights' and the sun's shine, with all of the light live
		rt_specOn    = u_RtParams.y > 1.5 && u_RtSurface.z > 0.0;
		rt_specView  = E;
		rt_specRough = roughness;
  #else
    #if defined(USE_GROUNDCOVER)
		vec3 rtNormal = normalize(var_RtNormal);
    #else
		vec3 rtNormal = normalize(!gl_FrontFacing ? var_RtNormal : -var_RtNormal);
    #endif
  #endif
  #if defined(USE_LIGHTMAP)
		rt_skyLM = var_TexCoords.zw;
  #endif
  #if defined(USE_LIGHTMAP) || defined(USE_LIGHT_VERTEX)
		// the map's own lighting: with the direct light live alone
		// (r_realtimeLighting 1), what the lights change of it
		gl_FragColor.rgb = RtDelta(gl_FragColor.rgb, diffuse.rgb * RtLight(var_RtPos, rtNormal, u_RtParams.y < 1.5));
  #else
		// lit at run time: the lights, when all of it is live
		if (u_RtParams.y > 1.5)
			gl_FragColor.rgb += diffuse.rgb * RtLight(var_RtPos, rtNormal, false);
  #endif
  #if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
		gl_FragColor.rgb += rt_spec * specular.rgb;
  #endif
	}
#endif

	// MOH:AA global distance fog, matching the linear falloff the GL1 renderer
	// gets from fixed function GL_FOG.
	float fogFactor = clamp((var_FogDist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0);
	gl_FragColor.rgb = Fog(gl_FragColor.rgb, u_GlobalFogColor.rgb, fogFactor * u_GlobalFogColor.a);

#if defined(USE_FROXEL_FOG)
	// Added in OPM: the light in the air between it and the eye
	gl_FragColor.rgb = FroxelFog(gl_FragColor.rgb, gl_FragColor.a);
#endif
}
