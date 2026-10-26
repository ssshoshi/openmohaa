// Added in OPM: water (tr_water.c).
//
// What is seen of water is the light it sends back from the sky and what
// stands over it (more of it the flatter one looks across it) and what comes
// up through it from the bottom, less what the water takes on the way and
// plus what it scatters back in its own colour. The bottom is the view as it was drawn before its water,
// moved a little by the waves; how much water lies over it comes from the
// depth the prepass left. Where it is shallow the surf breaks and foams.
// There is blood in it where the map says (out from its shore line, or to a
// depth), in clouds, and round every body in it (u_WaterSpots).
//
// A unit is an inch. The first wave train is the one the cgame pushes floating
// bodies with (cg_water.cpp), so its crests and the surf they make here come
// in with the bodies: change the two together.

uniform sampler2D u_DiffuseMap;      // the water's own texture: ripples, foam, blood clouds
uniform sampler2D u_ScreenImageMap;  // the view before its water
uniform sampler2D u_ScreenDepthMap;  // the view's depth after the prepass
uniform sampler2D u_SpecularMap;     // its ripples round the view (glsl/ripple_fp.glsl): height, before, foam
uniform samplerCube u_CubeMap;       // the map's sky box (tr_water.c, R_WaterBuildSky)
uniform sampler2D u_WaterReflectMap; // the world from under the surface (tr_water.c, R_WaterReflectView)
uniform vec4      u_WaterReflect;    // whether there is it, -, its share of the image across and up
uniform vec4      u_WaterReflectPlane; // the plane of the water it was seen from: normal, distance
uniform mat4      u_WaterReflectMvp; // how it was seen
uniform vec4      u_CubeMapInfo;     // how much of it is mirrored: 0 the flat u_WaterSky

uniform vec3      u_ViewOrigin;
uniform vec4      u_ViewInfo;        // zNear, zFar, whether there is the depth, and the view
uniform float     u_Time;

uniform vec4      u_PrimaryLightOrigin; // towards the sun
uniform vec3      u_PrimaryLightColor;

uniform vec4      u_GlobalFogColor;
uniform vec2      u_GlobalFogParams;

uniform vec4      u_WaterColor; // its own colour, and how far one sees into it
uniform vec4      u_WaterBlood; // the colour of blood in it, and how much there is
uniform vec4      u_WaterWaves; // the way the waves come in (xy), their length and period
uniform vec4      u_WaterShore; // wave height, the depth the surf starts at, whether there is a shore line, where
uniform vec4      u_WaterSky;   // the sky it mirrors, and how choppy it is
uniform vec4      u_WaterLook;  // foam, refraction, sun glints, how much the blood lies in clouds
uniform vec4      u_WaterOcean; // the open sea's waves (OceanWaves): one over their scale, height, drag, octaves (0: the chop)
uniform vec4      u_WaterBloodZone; // all the blood to, and none past: out from the shore line, else in depth; how many u_WaterSpots
uniform vec4      u_WaterSpots[WATER_MAX_SPOTS]; // blood round bodies in the water: x, y, how far it has spread, how thick
uniform mat4      u_WorldViewProjection; // the world's, to follow what it mirrors through the view
uniform vec4      u_WaterView;           // the view's box on the screen: where it starts, where it ends
uniform vec4      u_WaterRipples;        // where the ripples' grid starts, one over how wide it is, whether there is one

// Added in OPM: r_linearLighting, the colours as linear light (GLSL_BindProgram)
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

varying vec3      var_Position;
varying vec3      var_Normal;

const float PI = 3.14159265;
const float GRAVITY = 386.0; // inches a second a second

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

// A train of waves going dir, as deep water carries them: its slope
vec2 WaveSlope(vec2 p, vec2 dir, float len, float height, float t, float offset)
{
	float k = 2.0 * PI / len;
	float w = sqrt(GRAVITY * k);
	return dir * (height * k * cos(k * dot(dir, p) - w * t + offset));
}

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Value noise between 0 and 1, and its slope (after Inigo Quilez)
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

// Three octaves of it, each turned from the last
float Clouds(vec2 x)
{
	return Noise(x).x * 0.55 + Noise(Turn(x, 1.1) * 2.03 + 3.7).x * 0.3 + Noise(Turn(x, 2.3) * 4.1 + 9.2).x * 0.15;
}

// The open sea (after afl_ext's "Very fast procedural ocean", MIT): waves of
// exp(sin) shape, peaked crests and long flat troughs, each octave higher,
// faster and weaker than the last and pushed along by the slope of those
// before it (the drag), so they gather into ragged crests rather than lines.
// Their ways spread round the way the waves come in, and they go that way, to
// the shore. p is in the sea's scale (u_WaterOcean.x); back the height, 0
// to 1, and its slope, each wave's own (the drag's bend left out).
vec3 OceanWaves(vec2 p, vec2 dir, float t, int octaves)
{
	vec2  grad   = vec2(0.0);
	float shift  = length(p) * 0.1; // no two octaves in phase everywhere
	float iter   = 0.0;
	float freq   = 1.0;
	float speed  = 2.0;
	float weight = 1.0;
	float sum = 0.0, weights = 0.0;
	for (int i = 0; i < 40; i++)
	{
		if (i >= octaves)
			break;
		vec2  d    = Turn(dir, (fract(sin(iter) * 43758.5453) * 2.0 - 1.0) * 0.9);
		float x    = dot(d, p) * freq - t * speed + shift;
		float wave = exp(sin(x) - 1.0);
		float dx   = wave * cos(x);
		p       += d * (-dx) * weight * u_WaterOcean.z;
		grad    += d * (dx * freq * weight);
		sum     += wave * weight;
		weights += weight;
		weight   = mix(weight, 0.0, 0.2);
		freq    *= 1.18;
		speed   *= 1.07;
		iter    += 1232.399963;
	}
	return vec3(sum, grad) / weights;
}

// The chop on the swell: noise as a height, in units, and its slope. Two
// layers drift across each other, which reads as small waves running.
vec2 Chop(vec2 p, vec2 dir, float t)
{
	vec2 slope = vec2(0.0);
	vec2 a     = dir * t * 14.0;
	vec2 b     = Turn(dir, 1.9) * t * 9.0;

	slope += Noise(p / 160.0 + a / 160.0).yz * (3.0 / 160.0);
	slope += Noise(Turn(p, 1.3) / 70.0 + b / 70.0).yz * (1.4 / 70.0);
	slope += Noise(Turn(p, 2.6) / 30.0 - a / 30.0).yz * (0.6 / 30.0);
	slope += Noise(Turn(p, 4.0) / 13.0 + b / 13.0).yz * (0.25 / 13.0);
	return slope;
}

void main()
{
	vec2  screen  = gl_FragCoord.xy * r_FBufScale;
	vec3  toFrag  = var_Position - u_ViewOrigin;
	float dist    = length(toFrag);
	vec3  V       = -toFrag / max(dist, 0.001);
	float t       = u_Time;
	vec2  p       = var_Position.xy;
	bool  below   = dot(V, var_Normal) < 0.0;
	bool  hasDepth = u_ViewInfo.z > 0.5;
	bool  hasView  = u_ViewInfo.w > 0.5;

	// The waves. The first train is the cgame's (cg_water.cpp).
	vec2  dir     = normalize(u_WaterWaves.xy);
	float waveLen = u_WaterWaves.z;
	float height  = u_WaterShore.x;
	float k0      = 2.0 * PI / waveLen;
	float phase   = k0 * dot(dir, p) - 2.0 * PI * t / u_WaterWaves.w;

	vec2 swell = dir * (height * k0 * cos(phase));
	swell += WaveSlope(p, Turn(dir, 0.45),  waveLen * 0.61, height * 0.50, t, 1.3);
	swell += WaveSlope(p, Turn(dir, -0.6),  waveLen * 0.37, height * 0.30, t, 4.1);
	swell += WaveSlope(p, Turn(dir, 1.2),   waveLen * 0.23, height * 0.18, t, 2.7);
	swell += WaveSlope(p, Turn(dir, -1.45), waveLen * 0.13, height * 0.10, t, 5.2);

	// The chop on them, fading with distance where it would only sparkle
	float near  = 1.0 / (1.0 + dist / 1400.0);
	vec2  slope = swell / (1.0 + dist / 6000.0) + Chop(p, dir, t) * (u_WaterSky.a * 2.5) * near;
	// or the open sea's waves over the swell, in place of the chop: fewer of
	// them further off, and flatter, where they would only sparkle
	float crest = 0.5;
	if (u_WaterOcean.x > 0.0)
	{
		float scale  = u_WaterOcean.x;
		vec2  q      = p * scale;
		float far    = dist * scale;
		int   octs   = int(mix(u_WaterOcean.w, 6.0, clamp(far / 60.0, 0.0, 1.0)));
		vec3  wave   = OceanWaves(q, dir, t, octs);
		float h      = wave.x;
		vec2  grad   = wave.yz * u_WaterOcean.y * scale;
		grad *= 1.0 - 0.8 * min(1.0, sqrt(far * 0.01) * 1.1);
		slope = swell / (1.0 + dist / 6000.0) + grad;
		crest = h;
	}

	// and the ripples and wakes of what goes through it, and their foam
	float rippleFoam = 0.0;
	if (u_WaterRipples.w > 0.5)
	{
		vec2 at = (p - u_WaterRipples.xy) * u_WaterRipples.z;
		if (at.x > 0.0 && at.y > 0.0 && at.x < 1.0 && at.y < 1.0)
		{
			float texel = 1.0 / float(WATER_RIPPLE_SIZE);
			float dx    = 1.0 / (u_WaterRipples.z * float(WATER_RIPPLE_SIZE)); // units a texel
			vec4  here  = texture2D(u_SpecularMap, at);
			float east  = texture2D(u_SpecularMap, at + vec2(texel, 0.0)).r;
			float west  = texture2D(u_SpecularMap, at - vec2(texel, 0.0)).r;
			float north = texture2D(u_SpecularMap, at + vec2(0.0, texel)).r;
			float south = texture2D(u_SpecularMap, at - vec2(0.0, texel)).r;
			// fading out toward the grid's edge, where they are cut off
			vec2  inside = min(at, 1.0 - at);
			float keep   = smoothstep(0.0, 0.12, min(inside.x, inside.y));
			slope += vec2(east - west, north - south) / (2.0 * dx) * keep;
			rippleFoam = here.b * keep;
		}
	}
	vec3 N     = normalize(var_Normal + vec3(-slope, 0.0));
	if (below)
		N = -N;

	// What lies under it: how deep, and how much water the eye looks through
	float fragLin   = LinearDepth(gl_FragCoord.z);
	float behindLin = hasDepth ? LinearDepth(texture2D(u_ScreenDepthMap, screen).r) : fragLin + 4096.0;
	// In front of the water, which it cannot be where the water is drawn: a
	// model's edge, which the prepass covered a pixel further than the view
	// did. Read as no water at all, it was surf, a bright dotted line round
	// everything standing in front of water. The farthest beside it instead.
	if (hasDepth && behindLin < fragLin)
	{
		behindLin = max(max(LinearDepth(texture2D(u_ScreenDepthMap, screen + vec2(r_FBufScale.x, 0.0)).r),
		                    LinearDepth(texture2D(u_ScreenDepthMap, screen - vec2(r_FBufScale.x, 0.0)).r)),
		                max(LinearDepth(texture2D(u_ScreenDepthMap, screen + vec2(0.0, r_FBufScale.y)).r),
		                    LinearDepth(texture2D(u_ScreenDepthMap, screen - vec2(0.0, r_FBufScale.y)).r)));
		if (behindLin < fragLin)
			behindLin = fragLin + 64.0;
	}
	vec3  bottom    = u_ViewOrigin + toFrag * (behindLin / fragLin);
	float depth     = below ? 4096.0 : max(var_Position.z - bottom.z, 0.0);

	// Blood, in clouds that drift and roll with the swell
	float blood = 0.0;
	if (u_WaterBlood.a > 0.0)
	{
		vec2  bp    = p / 420.0 + dir * sin(phase) * 0.02 + vec2(t * 0.004, t * -0.0027);
		float cloud = smoothstep(0.35, 0.7, Clouds(bp));
		// Out from the shore, where the map says where it is. Measured by the
		// depth, anything standing in the water would have blood round it.
		float offshore = u_WaterShore.z > 0.5 ? u_WaterShore.w - dot(dir, p) : depth;
		blood = u_WaterBlood.a * (1.0 - smoothstep(u_WaterBloodZone.x, u_WaterBloodZone.y, offshore));
		blood *= mix(1.0, cloud, u_WaterLook.w);
	}

	// and round each body in it, a cloud of its own: thickest at the body,
	// ragged at the edge and wispy in itself, its whorls growing as it
	// spreads, as blood in water does
	int spots = int(u_WaterBloodZone.z + 0.5);
	for (int i = 0; i < WATER_MAX_SPOTS; i++)
	{
		if (i >= spots)
			break;

		vec4 spot = u_WaterSpots[i];
		vec2 q    = (p - spot.xy) / max(spot.z, 1.0);
		if (dot(q, q) < 2.0)
		{
			vec2  seed  = vec2(float(i) * 7.31, float(i) * 3.17);
			float warp  = Clouds(q * 1.8 + seed + vec2(t * 0.011, -t * 0.008));
			float reach = length(q + vec2(warp - 0.5, Clouds(q * 1.8 + seed.yx + 5.1) - 0.5) * 0.7);
			float wisps = smoothstep(0.3, 0.75, Clouds(q * 3.4 + seed * 1.7 + vec2(-t * 0.017, t * 0.013)));
			float body  = 1.0 - smoothstep(0.15, 0.95, reach);
			blood = max(blood, spot.w * body * mix(0.45, 1.0, wisps));
		}
	}
	blood = clamp(blood, 0.0, 1.0);

	// The bottom, seen through the waves. Something standing out of the water
	// there is not bent: the bottom straight behind is taken instead.
	vec2  bent   = N.xy * u_WaterLook.y * clamp((behindLin - fragLin) / 48.0, 0.0, 1.0) / (1.0 + fragLin / 700.0);
	vec2  uv     = screen + bent;
	float seen   = hasDepth ? LinearDepth(texture2D(u_ScreenDepthMap, uv).r) : behindLin;
	if (seen < fragLin)
	{
		uv   = screen;
		seen = behindLin;
	}
	float through = below ? 96.0 : dist * max(seen / fragLin - 1.0, 0.0);

	// The water takes red first, then green; blood takes green and blue
	float clarity = max(u_WaterColor.a, 1.0);
	vec3  absorb  = vec3(1.0, 0.55, 0.45) * (2.3 / clarity) + blood * vec3(0.3, 1.6, 1.8) / clarity;
	vec3  T       = exp(-absorb * through);
	vec3  own     = mix(Lin(u_WaterColor.rgb), Lin(u_WaterBlood.rgb), clamp(blood * 0.9, 0.0, 1.0));
	// the light the sea scatters back from inside it: more through the thin
	// water of a crest than in a trough
	if (u_WaterOcean.x > 0.0)
		own *= 0.55 + 1.3 * crest;

	vec3 under;
	float alpha = 1.0;
	if (hasView)
	{
		under = texture2D(u_ScreenImageMap, uv).rgb * T + own * (1.0 - T);
	}
	else
	{
		under = own;
		alpha = 1.0 - dot(T, vec3(0.333));
	}

	// What it mirrors, more of it the flatter one looks across: the ray off
	// the waves followed through the view's depth until it goes behind
	// something, which is then what is seen in it (as on the wet sand,
	// beach_fp.glsl); else the sky, the map's box where the view has drawn
	// it. The chop only breaks it up a little.
	vec3  R      = reflect(-V, N);
	float up     = clamp(R.z, 0.0, 1.0);
	vec3  sky    = Lin(mix(u_WaterSky.rgb, u_WaterSky.rgb * vec3(0.78, 0.85, 0.94), sqrt(up)));
	if (u_CubeMapInfo.x > 0.0)
		sky = mix(sky, Lin(textureCubeLod(u_CubeMap, R, 1.0).rgb), u_CubeMapInfo.x);
	float cosV   = clamp(dot(N, V), 0.0, 1.0);
	float fresnel = below ? 0.0 : 0.03 + 0.97 * pow(1.0 - cosV, 4.0);
	fresnel = min(fresnel, 0.9);

	vec3 mirror = sky;
	if (u_WaterReflect.x > 0.5 && !below && abs(dot(var_Position, u_WaterReflectPlane.xyz) - u_WaterReflectPlane.w) < 4.0)
	{
		// the view from under the surface: the spot of it over this one,
		// moved as the waves turn the surface
		vec4 clip = u_WaterReflectMvp * vec4(var_Position + vec3(-slope * 80.0, 0.0), 1.0);
		vec2 at   = clamp(clip.xy / max(clip.w, 0.001) * 0.5 + 0.5, 0.001, 0.999);
		mirror    = texture2D(u_WaterReflectMap, at * u_WaterReflect.zw).rgb;
	}
	else if (hasDepth && hasView && fresnel > 0.05)
	{
		vec3  Rm      = reflect(-V, normalize(var_Normal + vec3(-slope * 0.6, 0.0)));
		float stepLen = 8.0;
		vec3  q       = var_Position + Rm * 2.0;
		for (int i = 0; i < 24; i++)
		{
			q += Rm * stepLen;
			stepLen *= 1.2;

			vec4 clip = u_WorldViewProjection * vec4(q, 1.0);
			if (clip.w <= 0.0)
				break;
			vec2 at   = clip.xy / clip.w * 0.5 + 0.5;
			if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0)
				break;
			vec2 tc   = mix(u_WaterView.xy, u_WaterView.zw, at);

			float behind = clip.w - LinearDepth(texture2D(u_ScreenDepthMap, tc).r);
			if (behind > 0.0 && behind < stepLen * 2.0 + 8.0)
			{
				// fading towards the edge of the view, where it would run out
				vec2 edge = min(at, 1.0 - at);
				mirror = mix(sky, texture2D(u_ScreenImageMap, tc).rgb, smoothstep(0.0, 0.08, min(edge.x, edge.y)));
				break;
			}
		}
	}

	vec3 L      = normalize(u_PrimaryLightOrigin.xyz);
	float glint = max(dot(R, L), 0.0);
	vec3 sun    = Lin(u_PrimaryLightColor) * (pow(glint, 900.0) * 2.5 + pow(glint, 70.0) * 0.2) * u_WaterLook.z;

	vec3 color = mix(under, mirror, fresnel) + (below ? vec3(0.0) : sun);
	alpha = mix(alpha, 1.0, fresnel);

	// The surf: crests of the first train breaking as the bottom comes up,
	// and a wash where the water runs out on the sand
	float foam = 0.0;
	if (hasDepth && !below)
	{
		float surfDepth = max(u_WaterShore.y, 1.0);
		float crest     = pow(max(sin(phase), 0.0), 3.0);
		float surf      = 1.0 - smoothstep(surfDepth * 0.2, surfDepth, depth);
		float wash      = 1.0 - smoothstep(0.0, surfDepth * 0.22 * (1.0 + 0.6 * sin(phase)), depth);

		// Where the map says where the shore is, only near it: else every
		// post standing in the water out at sea would have surf round it
		if (u_WaterShore.z > 0.5)
		{
			float nearShore = 1.0 - smoothstep(surfDepth * 8.0, surfDepth * 18.0, u_WaterShore.w - dot(dir, p));
			surf *= nearShore;
			wash *= nearShore;
		}
		float speckle   = dot(texture2D(u_DiffuseMap, p / 96.0 - dir * t * 0.03).rgb, vec3(0.333));
		// lace: streaks of it with the water showing through between
		float lace      = Clouds(p / 46.0 - dir * t * 0.35) * 0.65 + Clouds(Turn(p, 0.9) / 17.0 + dir * t * 0.2) * 0.35;
		float pattern   = smoothstep(0.42, 0.62, lace) * mix(0.75, 1.2, smoothstep(0.25, 0.5, speckle));
		foam = clamp((surf * crest * 1.3 + wash * 0.85) * pattern * 1.5, 0.0, 0.8) * u_WaterLook.x;
	}
	// what has gone through it churns it white: a boat's wash, a splash
	if (!below && rippleFoam > 0.0)
	{
		// in lace and streaks, thinning to nothing as it fades
		float churn = Clouds(p / 11.0 + vec2(t * 0.05, -t * 0.04)) * 0.6 + Clouds(Turn(p, 1.3) / 29.0) * 0.4;
		float lace  = smoothstep(0.62 - 0.3 * rippleFoam, 0.8 - 0.3 * rippleFoam, churn);
		foam = max(foam, lace * min(rippleFoam * 1.5, 0.8) * u_WaterLook.x);
	}
	vec3 foamColor = mix(vec3(0.82, 0.84, 0.84), vec3(0.62, 0.30, 0.27), clamp(blood * 1.4, 0.0, 1.0));
	// lit as the sky lights it: grey under a grey one
	foamColor *= clamp(dot(u_WaterSky.rgb, vec3(0.3)) + 0.1, 0.3, 1.0);
	color = mix(color, Lin(foamColor), foam);
	alpha = max(alpha, foam);

	// Soft where it meets the shore, and not along a ruled line: how far up
	// the sand it gets varies along it, and the swash runs on past it
	// (beach_fp.glsl)
	if (hasDepth && !below)
	{
		float edge = 2.5 * Clouds(vec2(dot(p, vec2(dir.y, -dir.x)) / 240.0, t * 0.03));
		alpha *= smoothstep(edge, edge + 3.0, depth);
	}

	float fogFactor = clamp((dist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0);
	color = Fog(color, u_GlobalFogColor.rgb, fogFactor * u_GlobalFogColor.a);

	gl_FragColor = vec4(color, alpha);
}
