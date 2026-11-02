// Added in OPM: what is on the lens (tr_lens.c).
//
// The view as if through a camera's lens that things land on (the cgame says
// what and where, cg_lens.cpp): drops of water, each a little lens of its own
// that turns what is behind it over and leaves a wet trail as it slides down;
// blood, ragged and thick, running; and dirt, specks of it. Drawn over the
// finished frame, before the 2D.

uniform sampler2D u_ScreenImageMap; // the frame

uniform vec4 u_LensInfo;                     // how many, the view's width over its height, how far it is across a texel of the copy
uniform vec4 u_LensSpots[WATER_MAX_SPOTS];   // x, y (a share of the view), how big (a share of its height), how much of it is left
uniform vec4 u_LensLook[WATER_MAX_SPOTS];    // what (0 water, 1 blood, 2 dirt), a seed, how far it has run down, 0
uniform vec4 u_LensRain;                     // how wet the glass is from the rain (0 to 1), the time in seconds

varying vec2 var_Tex;
varying vec2 var_View;

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float Noise(vec2 x)
{
	vec2 i = floor(x);
	vec2 f = fract(x);
	vec2 u = f * f * (3.0 - 2.0 * f);

	return mix(mix(Hash(i), Hash(i + vec2(1.0, 0.0)), u.x), mix(Hash(i + vec2(0.0, 1.0)), Hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float Clouds(vec2 x)
{
	return Noise(x) * 0.6 + Noise(x * 2.1 + 3.7) * 0.28 + Noise(x * 4.3 + 9.1) * 0.12;
}

// The rain on the glass. Water lies on it in blobs that run together where
// they meet (each a soft mound, the mounds added, and water where the sum is
// enough): beads that land and sit, then dry away; and drops that run down
// in fits along a wandering path, a line of small beads behind them, in two
// sizes. Few at a time: a drop now and then looking ahead, a couple of
// dozen looking up. What is behind is bent by the slope of the water.

vec3 GlassHash(vec2 id)
{
	return fract(sin(vec3(dot(id, vec2(127.1, 311.7)), dot(id, vec2(269.5, 183.3)), dot(id, vec2(419.2, 371.9)))) * 43758.5453);
}

// a soft mound of water, 1 in its middle, none at r
float GlassBlob(float len, float r)
{
	return smoothstep(r, 0.0, len);
}

// beads sitting on the glass: one to a cell, in a few of them (the wetter
// the more), landing at once and drying away slowly
float GlassBeads(vec2 p, float t, float wet)
{
	vec2  q  = p * 12.0;
	vec2  id = floor(q);
	vec3  n  = GlassHash(id + 7.3);
	// kept inside its cell (cut by its edge, it drew a box)
	float r  = 0.12 + 0.2 * n.z;
	vec2  at = (n.xy - 0.5) * (0.9 - 2.0 * r);
	float c  = fract(t * 0.05 + n.z * 3.1);
	float life = smoothstep(0.0, 0.02, c) * smoothstep(1.0, 0.4, c);
	float here = step(fract(n.z * 13.1), 0.12 * wet);

	return GlassBlob(length(fract(q) - 0.5 - at), r) * life * here;
}

// where a running drop is across its column at a height in it
float GlassPath(float y, vec3 n)
{
	return 0.5 + (n.x - 0.5) * 0.2 + sin(y * 7.0 + n.y * 6.283) * 0.1 * (n.z - 0.35);
}

// drops running down: in a few of the cells of a column (the wetter the
// more), starting high in it, going down in fits (it holds, then slips),
// the beads of its trail above it, smaller and fewer the further back
float GlassRunning(vec2 p, float t, float seed, float wet)
{
	const vec2 cell = vec2(1.0 / 9.0, 0.6); // in view heights at this scale

	float col = floor(p.x / cell.x);
	vec3  cn  = GlassHash(vec2(col, seed));
	vec2  q   = vec2(p.x / cell.x, p.y / cell.y + cn.x);
	vec2  id  = floor(q);
	vec2  st  = fract(q);
	vec3  n   = GlassHash(id + seed * 3.7);

	float life = fract(t * (0.09 + 0.08 * n.y) + n.z);
	float fits = 5.0;
	float gone = (floor(life * fits) + smoothstep(0.55, 1.0, fract(life * fits))) / fits;
	float y    = 0.85 - 0.72 * gone;
	float show = smoothstep(0.0, 0.05, life) * smoothstep(1.0, 0.92, life) * step(n.y, 0.22 * wet);

	if (show <= 0.0)
		return 0.0;

	// the drop: longer than wide, fuller below, where the water gathers
	float r  = 0.02 * (0.75 + 0.5 * n.x);
	vec2  d  = vec2((st.x - GlassPath(y, n)) * cell.x, (st.y - y) * cell.y);
	d.y     *= d.y > 0.0 ? 1.0 : 0.75;
	float m  = GlassBlob(length(d * vec2(1.0, 0.8)), r);

	// its trail: beads left where it went, above it
	float back = (st.y - y) * cell.y;
	if (back > 0.0)
	{
		float gap   = r * 1.4;
		float k     = back / gap;
		float along = (fract(k) - 0.5) * gap;
		float dx    = (st.x - GlassPath(st.y, n)) * cell.x;
		float rb    = r * (0.55 - 0.3 * smoothstep(0.0, 0.25, back));
		float fade  = smoothstep(0.3, 0.0, back);
		m += GlassBlob(length(vec2(dx, along)), rb) * fade;
	}
	return m * show;
}

// the water on the glass here, 0 to 1
float GlassWater(vec2 p, float t, float wet)
{
	float h = GlassBeads(p, t, wet)
	        + GlassRunning(p, t, 1.0, wet)
	        + GlassRunning(p * 1.6 + vec2(3.1, 1.7), t * 1.1, 2.0, wet * 0.6);
	return smoothstep(0.3, 1.0, h);
}

void main()
{
	float aspect = u_LensInfo.y;
	vec2  bent   = vec2(0.0); // how far what is seen is moved, in view heights
	float shade  = 0.0;       // the dark rim of a drop
	float glint  = 0.0;       // the sky's light caught on one
	float blood  = 0.0;
	float dirt   = 0.0;

	// the rain on the glass: what is behind bent by how the water lies on
	// it, darker where it is steep (a drop's rim), lit where it faces up
	if (u_LensRain.x > 0.002)
	{
		vec2  p  = (var_View - 0.5) * vec2(aspect, 1.0);
		float t  = u_LensRain.y;
		float e  = 0.001;
		float h  = GlassWater(p, t, u_LensRain.x);
		vec2  dh = vec2(GlassWater(p + vec2(e, 0.0), t, u_LensRain.x) - h, GlassWater(p + vec2(0.0, e), t, u_LensRain.x) - h);

		// what is behind seen through the water, moved by its slope; a
		// little darker where it is steep (a drop's rim), the sky caught
		// high on it
		bent  += dh * 1.6;
		shade  = max(shade, clamp(length(dh) * 1.2, 0.0, 0.15));
		glint  = max(glint, clamp(dot(dh, vec2(-0.6, 0.8)) * 2.0, 0.0, 0.3));
	}

	int count = int(u_LensInfo.x + 0.5);
	for (int i = 0; i < WATER_MAX_SPOTS; i++)
	{
		if (i >= count)
			break;

		vec4  s = u_LensSpots[i];
		vec4  k = u_LensLook[i];
		vec2  d = (var_View - s.xy) * vec2(aspect, 1.0);
		float r = max(s.z, 0.001);

		// nothing of it here: well away from it and from its run
		if (d.y > r * 2.0 || d.y < -(k.z + r * 2.0) || abs(d.x) > r * 2.0)
			continue;

		if (k.x < 0.5)
		{
			// a drop: round, not quite, and a lens: what is behind it is
			// turned over in it, darker at its rim, with a glint
			float edge = 1.0 + (Noise(normalize(d + 0.0001) * 2.0 + k.y * 17.0) - 0.5) * 0.3;
			float q    = length(d) / (r * edge);
			if (q < 1.0)
			{
				float h = sqrt(1.0 - q * q);
				bent  += -d * 2.4 * h * s.w;
				shade  = max(shade, smoothstep(0.82, 1.0, q) * 0.16 * s.w);
				glint  = max(glint, pow(max(0.0, 1.0 - length(d / r - vec2(-0.32, 0.38)) * 4.0), 3.0) * 0.8 * s.w);
			}
			// the wet it has left above it, sliding down
			if (d.y > 0.0 && d.y < k.z)
			{
				float w = r * 0.3 * (1.0 - 0.6 * d.y / k.z);
				float x = d.x + (Noise(vec2(d.y / r * 1.5, k.y * 7.0)) - 0.5) * r * 0.4;
				float a = 1.0 - smoothstep(w * 0.5, w, abs(x));
				bent += vec2(-x / max(w, 0.0001) * w * 0.25, 0.0) * a * s.w;
				shade = max(shade, a * 0.08 * s.w);
			}
		}
		else if (k.x < 1.5)
		{
			// blood: a ragged splat, specks thrown round it, running down;
			// thick and dark in the middle, thin and red at its edges, and
			// wet: what is behind it bent a little, the light caught on it
			float q = length(d) / (r * (1.0 + (Clouds(d / r * 2.5 + k.y * 11.0) - 0.5) * 1.1));
			float a = (1.0 - smoothstep(0.65, 1.0, q)) * (0.55 + 0.45 * (1.0 - q));
			if (q < 1.0)
			{
				bent  += -d * 0.7 * (1.0 - q) * s.w;
				glint  = max(glint, pow(max(0.0, 1.0 - length(d / r - vec2(-0.25, 0.3)) * 3.5), 4.0) * 0.35 * s.w);
			}
			float specks = smoothstep(0.78, 0.86, Noise(d / r * 8.0 + k.y * 5.0)) * (1.0 - smoothstep(1.0, 2.0, length(d) / r));
			if (d.y < 0.0 && -d.y < k.z)
			{
				float w = r * 0.28 * (1.0 + d.y / k.z * 0.7);
				float x = d.x + (Noise(vec2(d.y / r * 2.0, k.y * 3.0)) - 0.5) * r * 0.5;
				a = max(a, (1.0 - smoothstep(w * 0.5, w, abs(x))) * 0.95);
			}
			blood = max(blood, max(a, specks) * s.w);
		}
		else
		{
			// dirt: clumps of it and grit round them, hard edged, more of
			// it in the middle
			float m      = 1.0 - smoothstep(0.3, 1.0, length(d) / r);
			float clumps = smoothstep(0.66, 0.7, Clouds(d / r * 3.0 + k.y * 13.0));
			float grains = smoothstep(0.86, 0.9, Noise(d / r * 26.0 + k.y * 3.0)) * (1.0 - smoothstep(0.6, 1.3, length(d) / r));
			dirt = max(dirt, max(clumps * m, grains) * s.w);
		}
	}

	vec2 move = bent * vec2(1.0 / aspect, 1.0);
	vec3 c    = texture2D(u_ScreenImageMap, var_Tex + move * u_LensInfo.zw).rgb;

	c *= 1.0 - shade;
	c += vec3(glint * 0.5);

	// blood: what is behind seen through it red where it is thin, near black
	// where it is thick
	c = mix(c, c * vec3(0.6, 0.05, 0.04), clamp(blood * 1.8, 0.0, 1.0));
	c = mix(c, vec3(0.2, 0.012, 0.01), smoothstep(0.45, 1.0, blood));
	// dirt: brown and grey, lit a little by the day
	vec3 soil = mix(vec3(0.16, 0.12, 0.08), vec3(0.22, 0.2, 0.17), Noise(var_View * 90.0)) * (0.75 + 0.4 * Noise(var_View * 500.0));
	c = mix(c, soil, clamp(dirt, 0.0, 1.0) * 0.95);

	gl_FragColor = vec4(c, 1.0);
}
