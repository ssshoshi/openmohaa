// Added in OPM: the bloom (tr_postprocess.c, RB_Bloom).
//
// What is brighter than the scene's own white glows into what is round it,
// as it does through a lens: the frame's brightest is taken to half size (each
// tap weighted down by how bright it is, after Karis, so that one hot pixel
// does not flicker a blob of glow on and off as it moves) and
// halved over and over, each level a softer blur of the one before, then the
// levels are added back up, each widened onto the next bigger, and the whole
// onto the frame before its tone map. The halving is the 13 tap filter of the
// "next generation post processing" talk (Jimenez 2014), the widening a tent.

uniform sampler2D u_ScreenImageMap; // the level read

uniform vec4 u_BloomParams; // x 0 the frame's bright part halved, 1 halved, 2 widened; y the brightness it starts at, z how softly; w what it gives, scaled
uniform vec4 u_BloomTexel;  // xy a texel of what is read, zw the most of it read (its corner, past which is another view's)

varying vec2 var_Tex;

vec3 Fetch(vec2 uv)
{
	return texture2D(u_ScreenImageMap, clamp(uv, vec2(0.0), u_BloomTexel.zw)).rgb;
}

float Luma(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 Down(vec2 uv)
{
	vec2 t = u_BloomTexel.xy;
	vec3 a = Fetch(uv + t * vec2(-2.0, -2.0));
	vec3 b = Fetch(uv + t * vec2( 0.0, -2.0));
	vec3 c = Fetch(uv + t * vec2( 2.0, -2.0));
	vec3 d = Fetch(uv + t * vec2(-2.0,  0.0));
	vec3 e = Fetch(uv);
	vec3 f = Fetch(uv + t * vec2( 2.0,  0.0));
	vec3 g = Fetch(uv + t * vec2(-2.0,  2.0));
	vec3 h = Fetch(uv + t * vec2( 0.0,  2.0));
	vec3 i = Fetch(uv + t * vec2( 2.0,  2.0));
	vec3 j = Fetch(uv + t * vec2(-1.0, -1.0));
	vec3 k = Fetch(uv + t * vec2( 1.0, -1.0));
	vec3 l = Fetch(uv + t * vec2(-1.0,  1.0));
	vec3 m = Fetch(uv + t * vec2( 1.0,  1.0));

	return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}

// The first halving, from the whole frame: four filtered taps, each the mean
// of four texels, the 4x4 round the pixel (a quarter of the 13 taps' reads,
// at the size where they cost the most), Karis weighted
vec3 First(vec2 uv)
{
	vec2 t = u_BloomTexel.xy;
	vec3 a = Fetch(uv + t * vec2(-1.0, -1.0));
	vec3 b = Fetch(uv + t * vec2( 1.0, -1.0));
	vec3 c = Fetch(uv + t * vec2(-1.0,  1.0));
	vec3 d = Fetch(uv + t * vec2( 1.0,  1.0));
	float wa = 1.0 / (1.0 + Luma(a)), wb = 1.0 / (1.0 + Luma(b)), wc = 1.0 / (1.0 + Luma(c)), wd = 1.0 / (1.0 + Luma(d));

	return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

// what is over the threshold, with a soft knee into it
vec3 Bright(vec3 c)
{
	float br   = max(c.r, max(c.g, c.b));
	float knee = max(u_BloomParams.z, 0.0001);
	float soft = clamp(br - u_BloomParams.y + knee, 0.0, 2.0 * knee);

	soft = soft * soft / (4.0 * knee);
	return c * (max(soft, br - u_BloomParams.y) / max(br, 0.0001));
}

// the 3x3 tent, [1 2 1] each way: the same as four filtered taps half a
// texel out on the diagonals
vec3 Up(vec2 uv)
{
	vec2 t = u_BloomTexel.xy * 0.5;

	return (Fetch(uv + vec2(-t.x, -t.y)) + Fetch(uv + vec2(t.x, -t.y)) + Fetch(uv + vec2(-t.x, t.y)) + Fetch(uv + vec2(t.x, t.y))) * 0.25;
}

void main()
{
	vec3 c;

	if (u_BloomParams.x < 0.5)
		c = Bright(First(var_Tex));
	else if (u_BloomParams.x < 1.5)
		c = Down(var_Tex);
	else
		c = Up(var_Tex) * u_BloomParams.w;

	gl_FragColor = vec4(c, 1.0);
}
