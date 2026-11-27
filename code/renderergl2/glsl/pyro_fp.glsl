// Added in OPM: a simulated explosion (tr_pyro.c): each pixel marches through
// the volume's box, from where it enters to where it leaves or to what is
// behind it, whichever is nearer. The smoke takes light out and scatters
// the map's light toward the eye (shadowed by the smoke between it and the
// light), and the flame glows, and lights the smoke round it.
#if defined(PYRO_COMPOSITE)
// the volumes, marched at half the view's size, over it
uniform sampler2D u_ScreenImageMap;
uniform vec4      u_PyroDepth;  // one over twice the half size image's size

void main()
{
	gl_FragColor = texture2D(u_ScreenImageMap, gl_FragCoord.xy * u_PyroDepth.xy);
}
#else
uniform sampler2D u_ScreenDepthMap;
uniform sampler3D u_PyroMap;     // RG: sqrt of density, flame; the frames side by side

uniform vec3 u_ViewOrigin;
uniform vec3 u_ViewForward;      // unit
uniform vec3 u_ViewLeft;         // as far as the view's edge is a unit forward
uniform vec3 u_ViewUp;
uniform vec4 u_ViewInfo;         // zfar / znear, zfar
uniform vec4 u_PyroBox;          // lowest corner, extinction per unit
uniform vec4 u_PyroSize;         // size, how much is left (fade)
uniform vec4 u_PyroFrames;       // the two frames' tiles
uniform vec4 u_PyroParams;       // first to second, tiles across, up, steps
uniform vec4 u_PyroCells;        // cells across, up, deep; draw the box
uniform vec4 u_PyroAmbient;      // light all round, the fire's brightness
uniform vec4 u_PyroSun;          // the directed light, the smoke's albedo
uniform vec4 u_PyroDir;          // toward it
uniform vec4 u_PyroView;         // viewport corner, one over its size
uniform vec4 u_PyroDepth;        // one over the depth image's size; a march pixel in depth pixels (2: half size)

// density (squared back) and flame at q, 0-1 in the box, between the frames
vec2 Cell(vec3 q)
{
	vec3 c  = clamp(q, 0.5 / u_PyroCells.xyz, 1.0 - 0.5 / u_PyroCells.xyz);
	vec2 at = vec2(u_PyroParams.y, u_PyroParams.z);
	vec2 a  = texture(u_PyroMap, vec3((u_PyroFrames.xy + c.xy) / at, c.z)).rg;
	vec2 b  = texture(u_PyroMap, vec3((u_PyroFrames.zw + c.xy) / at, c.z)).rg;
	vec2 s  = mix(a, b, u_PyroParams.x);
	// outside the box, nothing
	float inside = step(0.0, q.x) * step(q.x, 1.0) * step(0.0, q.y) * step(q.y, 1.0) * step(0.0, q.z) * step(q.z, 1.0);
	return vec2(s.x * s.x, s.y) * inside;
}

// the flame's colour as it cools: white-yellow, orange, deep red
vec3 FlameColor(float f)
{
	vec3 c = mix(vec3(0.45, 0.04, 0.005), vec3(1.0, 0.3, 0.04), smoothstep(0.05, 0.45, f));
	return mix(c, vec3(1.0, 0.75, 0.4), smoothstep(0.55, 0.95, f));
}

float Ign(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
	vec2  frag = gl_FragCoord.xy;
	vec2  ndc  = (frag - u_PyroView.xy) * u_PyroView.zw * 2.0 - 1.0;
	vec3  dir  = u_ViewForward - u_ViewLeft * ndc.x + u_ViewUp * ndc.y;
	float dl   = length(dir);
	dir /= dl;

	// what is behind it, along the ray
	float z     = texture2D(u_ScreenDepthMap, frag * u_PyroDepth.z * u_PyroDepth.xy).r;
	float scene = u_ViewInfo.y / mix(u_ViewInfo.x, 1.0, z) * dl;

	vec3  bmin = u_PyroBox.xyz;
	vec3  bmax = u_PyroBox.xyz + u_PyroSize.xyz;
	vec3  inv  = 1.0 / (dir + vec3(equal(dir, vec3(0.0))) * 1e-6);
	vec3  t0   = (bmin - u_ViewOrigin) * inv;
	vec3  t1   = (bmax - u_ViewOrigin) * inv;
	vec3  tlo  = min(t0, t1), thi = max(t0, t1);
	float tn   = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
	float tf   = min(min(thi.x, thi.y), min(thi.z, scene));

	if (u_PyroCells.w > 0.5 && tf > tn) {
		gl_FragColor = vec4(0.05, 0.0, 0.08, 0.08);
		return;
	}
	if (tf <= tn)
		discard;

	float cell   = u_PyroSize.x / u_PyroCells.x;          // a cell, units
	float ds     = max(u_PyroSize.x / u_PyroParams.w, (tf - tn) / 256.0);
	float sigmaK = u_PyroBox.w * u_PyroSize.w;            // extinction where a cell holds 1
	// the flame's light per unit: a flame a sixth of the box through adds up to u_PyroAmbient.w
	float fire   = u_PyroAmbient.w * u_PyroSize.w / (u_PyroSize.x * 0.15);
	vec3  L      = normalize(u_PyroDir.xyz);
	float cosT   = dot(L, dir);
	// Henyey-Greenstein, a little forward
	float g      = 0.2;
	float phase  = (1.0 - g * g) / (4.0 * 3.14159 * pow(1.0 + g * g - 2.0 * g * cosT, 1.5)) * 4.0 * 3.14159;
	vec3  albedo = u_PyroSun.w * vec3(1.0, 0.95, 0.9);

	vec3  sum = vec3(0.0);
	float T   = 1.0;
	// how deep what is seen of it is, forward of the eye, weighted by how much
	// of each step shows: the air in front of it is taken there
	float seenDepth = 0.0, seen = 0.0;
	float t   = tn + ds * Ign(frag);

	for (int i = 0; i < 256; i++) {
		if (t >= tf || T < 0.01)
			break;
		vec3  p = u_ViewOrigin + dir * t;
		vec2  s = Cell((p - bmin) / u_PyroSize.xyz);
		// where it burns the fuel is flame, not soot yet: less to hide the fire behind
		float sigma = s.x * sigmaK * (1.0 - 0.75 * smoothstep(0.05, 0.4, s.y));

		if (sigma > 1e-5 || s.y > 0.004) {
			// toward the light: what smoke is in the way, and the fire there
			float occl = 0.0, glow = s.y;
			float stride = cell * 1.5;
			vec3  lp = p;
			for (int j = 0; j < 5; j++) {
				lp += L * stride;
				vec2 ls = Cell((lp - bmin) / u_PyroSize.xyz);
				occl += ls.x * sigmaK * stride;
				glow = max(glow, ls.y * (1.0 - 0.15 * float(j)));
				stride *= 1.7;
			}
			// up, for the sky's share
			vec2 up = Cell((p + vec3(0.0, 0.0, cell * 4.0) - bmin) / u_PyroSize.xyz);
			float sky = exp(-up.x * sigmaK * cell * 8.0);

			vec3 light = u_PyroSun.rgb * exp(-occl) * phase
			           + u_PyroAmbient.rgb * (0.3 + 0.7 * sky)
			           + FlameColor(glow) * glow * glow * u_PyroAmbient.w * 0.35;
			vec3 emit  = FlameColor(s.y) * pow(s.y, 1.4) * fire;

			float Ts   = exp(-sigma * ds);
			float span = sigma > 1e-5 ? (1.0 - Ts) / sigma : ds;
			sum += T * (albedo * sigma * light + emit) * span;
			float w = T * (1.0 - Ts) + T * dot(emit, vec3(0.3)) * span;
			seenDepth += w * t / dl;
			seen      += w;
			T   *= Ts;
		}
		t += ds;
	}

#if defined(USE_FROXEL_FOG)
	if (seen > 0.0)
		sum = FroxelFogAt(sum, 1.0 - T, frag * u_PyroDepth.z, seenDepth / seen);
#endif
	gl_FragColor = vec4(sum, 1.0 - T);
}
#endif
