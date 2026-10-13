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

void main()
{
	float aspect = u_LensInfo.y;
	vec2  bent   = vec2(0.0); // how far what is seen is moved, in view heights
	float shade  = 0.0;       // the dark rim of a drop
	float glint  = 0.0;       // the sky's light caught on one
	float blood  = 0.0;
	float dirt   = 0.0;

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
