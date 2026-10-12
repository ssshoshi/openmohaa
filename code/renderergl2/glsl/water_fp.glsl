// Added in OPM: water (tr_water.c).
//
// What is seen of water is the light it sends back from the sky (more of it
// the flatter one looks across it) and what comes up through it from the
// bottom, less what the water takes on the way and plus what it scatters back
// in its own colour. The bottom is the view as it was drawn before its water,
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
uniform vec4      u_WaterBloodZone; // all the blood to, and none past: out from the shore line, else in depth; how many u_WaterSpots
uniform vec4      u_WaterSpots[WATER_MAX_SPOTS]; // blood round bodies in the water: x, y, how far it has spread, how thick

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
	vec3 N     = normalize(var_Normal + vec3(-slope, 0.0));
	if (below)
		N = -N;

	// What lies under it: how deep, and how much water the eye looks through
	float fragLin   = LinearDepth(gl_FragCoord.z);
	float behindLin = hasDepth ? LinearDepth(texture2D(u_ScreenDepthMap, screen).r) : fragLin + 4096.0;
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
	vec3  own     = mix(u_WaterColor.rgb, u_WaterBlood.rgb, clamp(blood * 0.9, 0.0, 1.0));

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

	// The sky in it, more of it the flatter one looks across
	vec3  R      = reflect(-V, N);
	float up     = clamp(R.z, 0.0, 1.0);
	vec3  sky    = mix(u_WaterSky.rgb, u_WaterSky.rgb * vec3(0.78, 0.85, 0.94), sqrt(up));
	float cosV   = clamp(dot(N, V), 0.0, 1.0);
	float fresnel = below ? 0.0 : 0.02 + 0.98 * pow(1.0 - cosV, 5.0);
	fresnel = min(fresnel, 0.85);

	vec3 L      = normalize(u_PrimaryLightOrigin.xyz);
	float glint = max(dot(R, L), 0.0);
	vec3 sun    = u_PrimaryLightColor * (pow(glint, 900.0) * 2.5 + pow(glint, 70.0) * 0.2) * u_WaterLook.z;

	vec3 color = mix(under, sky, fresnel) + (below ? vec3(0.0) : sun);
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
	vec3 foamColor = mix(vec3(0.82, 0.84, 0.84), vec3(0.62, 0.30, 0.27), clamp(blood * 1.4, 0.0, 1.0));
	// lit as the sky lights it: grey under a grey one
	foamColor *= clamp(dot(u_WaterSky.rgb, vec3(0.3)) + 0.1, 0.3, 1.0);
	color = mix(color, foamColor, foam);
	alpha = max(alpha, foam);

	// Soft where it meets the shore, rather than cut along it
	if (hasDepth && !below)
		alpha *= smoothstep(0.0, 3.0, depth);

	float fogFactor = clamp((dist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0);
	color = mix(color, u_GlobalFogColor.rgb, fogFactor * u_GlobalFogColor.a);

	gl_FragColor = vec4(color, alpha);
}
