// Added in OPM: temporal anti-aliasing (tr_taa.c, r_taa).
//
// Each frame is drawn from a camera nudged by a different fraction of a
// pixel, so over a few frames every pixel has been looked at from several
// places inside it. This pass keeps a running blend of those frames: where
// this pixel was in the last frame is worked out from its depth and the two
// cameras, the blend so far is read there, held to the colours round the
// pixel in this frame (so what has moved, or was behind what moved, is not
// smeared over it), and a share of this frame is added to it. It runs on the
// frame as lit, before its glow and tone map; the blend is done on the
// colours squeezed below one (c / (1 + c)) so that one bright pixel does not
// outweigh the rest and flicker.
//
// Only the camera's own movement is followed. Things that move by
// themselves (soldiers, doors) are held by the colours round them instead,
// which leaves them a little softer while they move.

uniform sampler2D u_ScreenImageMap; // this frame, before its glow and tone map
uniform sampler2D u_ScreenDepthMap; // its depth, from the prepass
uniform sampler2D u_TaaHistoryMap;  // the blend of the frames before

uniform mat4 u_TaaReproject; // from this view's clip space (without the nudge) to the last frame's
uniform vec4 u_TaaParams;    // the share of this frame, whether there is a blend to read (1) or not (0), the depth under which it is the view weapon, what to show (0 the frame, 1 how far each pixel moved)
uniform vec4 u_TaaBox;       // where the view is in the images, x, y, width, height (0 to 1)
uniform vec4 u_BloomTexel;   // one over the images' width and height, their width and height

varying vec2 var_Tex;
varying vec2 var_View;

float MaxOf(vec3 c)
{
	return max(c.r, max(c.g, c.b));
}

// below one, and back
vec3 Squeeze(vec3 c)
{
	return c / (1.0 + MaxOf(c));
}

vec3 Unsqueeze(vec3 c)
{
	return c / max(1.0 - MaxOf(c), 0.001);
}

vec3 ToYCoCg(vec3 c)
{
	return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 FromYCoCg(vec3 c)
{
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// a finite colour (what is not a number or past half float is black)
vec3 Finite(vec3 c)
{
	float s = c.r + c.g + c.b;

	return (s >= 0.0 && s < 65000.0) ? max(c, vec3(0.0)) : vec3(0.0);
}

vec3 Current(vec2 uv)
{
	return Finite(texture2D(u_ScreenImageMap, uv).rgb);
}

// the blend so far at uv, read with a Catmull-Rom filter from five bilinear
// reads (the corners left out): sharper than one bilinear read, which would
// blur the blend a little more each frame
vec3 History(vec2 uv)
{
	vec2 size  = u_BloomTexel.zw;
	vec2 texel = u_BloomTexel.xy;
	vec2 pos   = uv * size;
	vec2 c     = floor(pos - 0.5) + 0.5;
	vec2 f     = pos - c;
	vec2 w0    = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1    = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2    = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3    = f * f * (-0.5 + 0.5 * f);
	vec2 w12   = w1 + w2;
	vec2 tc0   = (c - 1.0) * texel;
	vec2 tc3   = (c + 2.0) * texel;
	vec2 tc12  = (c + w2 / w12) * texel;
	float a    = w12.x * w0.y;
	float b    = w0.x * w12.y;
	float m    = w12.x * w12.y;
	float d    = w3.x * w12.y;
	float e    = w12.x * w3.y;
	vec3 sum;

	sum  = texture2D(u_TaaHistoryMap, vec2(tc12.x, tc0.y)).rgb * a;
	sum += texture2D(u_TaaHistoryMap, vec2(tc0.x, tc12.y)).rgb * b;
	sum += texture2D(u_TaaHistoryMap, tc12).rgb * m;
	sum += texture2D(u_TaaHistoryMap, vec2(tc3.x, tc12.y)).rgb * d;
	sum += texture2D(u_TaaHistoryMap, vec2(tc12.x, tc3.y)).rgb * e;

	return Finite(sum / (a + b + m + d + e));
}

// q pulled along the line to the box's middle until it is in the box
vec3 ClipToBox(vec3 lo, vec3 hi, vec3 q)
{
	vec3 mid    = 0.5 * (hi + lo);
	vec3 extent = 0.5 * (hi - lo) + vec3(0.00001);
	vec3 v      = q - mid;
	vec3 r      = abs(v / extent);
	float most  = max(r.x, max(r.y, r.z));

	return most > 1.0 ? mid + v / most : q;
}

void main()
{
	vec2 texel = u_BloomTexel.xy;
	vec3 sum   = vec3(0.0);
	vec3 sum2  = vec3(0.0);
	vec3 lo    = vec3(1.0e9);
	vec3 hi    = vec3(-1.0e9);
	vec3 cur   = vec3(0.0);
	float near = 1.0;
	int x, y;

	// the colours round the pixel, and the nearest depth among them (so an
	// edge follows what is in front, not what is behind it)
	for (y = -1; y <= 1; y++) {
		for (x = -1; x <= 1; x++) {
			vec2 uv = var_Tex + vec2(float(x), float(y)) * texel;
			vec3 c  = ToYCoCg(Squeeze(Current(uv)));

			sum  += c;
			sum2 += c * c;
			lo    = min(lo, c);
			hi    = max(hi, c);
			if (x == 0 && y == 0) {
				cur = c;
			}
			near = min(near, texture2D(u_ScreenDepthMap, uv).r);
		}
	}

	{
		vec3 mean  = sum / 9.0;
		vec3 sigma = sqrt(max(sum2 / 9.0 - mean * mean, vec3(0.0)));

		// the box the colours are in, made no bigger than they spread
		lo = max(lo, mean - 1.25 * sigma);
		hi = min(hi, mean + 1.25 * sigma);
	}

	// where the pixel was in the last frame: the view weapon goes with the
	// camera, so it is where it is
	vec2 was = var_View;
	if (near >= u_TaaParams.z) {
		vec4 p = u_TaaReproject * vec4(var_View * 2.0 - 1.0, near * 2.0 - 1.0, 1.0);

		was = p.w > 0.0 ? p.xy / p.w * 0.5 + 0.5 : vec2(-1.0);
	}

	float share = u_TaaParams.x;
	vec3 res    = cur;
	if (u_TaaParams.y > 0.5 && all(greaterThanEqual(was, vec2(0.0))) && all(lessThanEqual(was, vec2(1.0)))) {
		vec3 hist = ToYCoCg(Squeeze(History(u_TaaBox.xy + was * u_TaaBox.zw)));

		hist = ClipToBox(lo, hi, hist);
		res  = mix(hist, cur, share);
	}

	vec3 rgb = Finite(Unsqueeze(FromYCoCg(res)));

	if (u_TaaParams.w > 0.5) {
		// how far each pixel moved, in pixels (red across, green up), blue
		// where there was nothing to read
		vec2 moved = abs(was - var_View) * u_TaaBox.zw * u_BloomTexel.zw;
		rgb = vec3(moved * 0.1, u_TaaParams.y > 0.5 && was.x >= 0.0 ? 0.0 : 1.0);
	}

	gl_FragColor = vec4(rgb, 1.0);
}
