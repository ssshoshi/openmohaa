// Added in OPM: the light in the air (r_volumetric, r_realtimeLighting 2):
// the sun through windows and between trees, the lamps' glow round them,
// each with its shadows. Worked out at a quarter of the view's size across,
// along the ray from the eye to what it sees, then smoothed by the depth and
// added to the view.
//
// The sun: steps along the ray, each in the finest of its shadow cascades
// that holds it. A lamp: only the part of the ray inside its reach, its own
// steps spread by the angle at the lamp, in its own shadow (one tap of it). Forward scattering (Henyey-Greenstein),
// so the air towards a light is brighter than the air beside it.
uniform sampler2D u_ScreenDepthMap;

uniform sampler2DShadow u_ShadowMap;
uniform sampler2DShadow u_ShadowMap2;
uniform sampler2DShadow u_ShadowMap3;
uniform sampler2DShadow u_ShadowMap4;
uniform mat4   u_ShadowMvp;
uniform mat4   u_ShadowMvp2;
uniform mat4   u_ShadowMvp3;
uniform mat4   u_ShadowMvp4;

uniform vec3   u_ViewOrigin;
uniform vec4   u_ViewInfo;   // zfar / znear, zfar
uniform vec4   u_VolParams;  // density, anisotropy, the sun's share, the lamps'
uniform vec4   u_VolRange;   // how far the sun's steps go, the sun's cascades (1: there), -, -

varying vec2   var_DepthTex;
varying vec3   var_ViewDir;

#define SUN_STEPS  16
#define LAMP_STEPS 4
// too little to see in the air, after the tone map
#define VOL_CUTOFF 0.002

float Phase(float cosTheta)
{
	float g  = u_VolParams.y;
	float gg = g * g;
	return (1.0 - gg) / (12.566371 * pow(max(1.0 + gg - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// the finest cascade that holds P
float SunShadow(vec3 P)
{
	vec4 p = u_ShadowMvp * vec4(P, 1.0);
	if (all(lessThan(abs(p.xyz), vec3(abs(p.w)))))
		return shadow2D(u_ShadowMap, vec3(p.xy * (0.5 / p.w) + 0.5, p.z * (0.5 / p.w) + 0.5));
	p = u_ShadowMvp2 * vec4(P, 1.0);
	if (all(lessThan(abs(p.xyz), vec3(abs(p.w)))))
		return shadow2D(u_ShadowMap2, vec3(p.xy * (0.5 / p.w) + 0.5, p.z * (0.5 / p.w) + 0.5));
	p = u_ShadowMvp3 * vec4(P, 1.0);
	if (all(lessThan(abs(p.xyz), vec3(abs(p.w)))))
		return shadow2D(u_ShadowMap3, vec3(p.xy * (0.5 / p.w) + 0.5, p.z * (0.5 / p.w) + 0.5));
	p = u_ShadowMvp4 * vec4(P, 1.0);
	if (all(lessThan(abs(p.xyz), vec3(abs(p.w)))))
		return shadow2D(u_ShadowMap4, vec3(p.xy * (0.5 / p.w) + 0.5, p.z * (0.5 / p.w) + 0.5));
	return 1.0;
}

// light i's shadow at v from it, as RtShadow has it, one tap of its four:
// the blur smooths the air, and the steps were most of the cost
float LampShadow(vec4 info, vec3 v)
{
	vec3  a = abs(v);
	float face, fwd, left, up;
	if (a.x >= a.y && a.x >= a.z) {
		if (v.x > 0.0) { face = 0.0; fwd =  v.x; left =  v.y; up = v.z; }
		else           { face = 1.0; fwd = -v.x; left = -v.y; up = v.z; }
	} else if (a.y >= a.z) {
		if (v.y > 0.0) { face = 2.0; fwd =  v.y; left = -v.x; up = v.z; }
		else           { face = 3.0; fwd = -v.y; left =  v.x; up = v.z; }
	} else {
		if (v.z > 0.0) { face = 4.0; fwd =  v.z; left = -v.y; up = v.x; }
		else           { face = 5.0; fwd = -v.z; left =  v.y; up = v.x; }
	}
	vec2  faceUV = clamp(vec2(-left, up) / fwd * 0.5 + 0.5, vec2(0.01), vec2(0.99));
	float n = RT_ZNEAR, f = info.w;
	float depth = ((f + n) / (f - n) - 2.0 * f * n / ((f - n) * fwd)) * 0.5 + 0.5;
	float vis = 1.0;
	if (info.x >= 0.0)
	{
		float tile = info.x + face, tiles = RT_TILES;
		if (tile >= RT_SMALL_CODE)
		{
			tiles = 2.0 * RT_TILES;
			tile  = tile - RT_SMALL_CODE + 2.0 * RT_SMALL_ROW * tiles;
		}
		vis *= shadow2D(u_RtShadowStatic, vec3((vec2(mod(tile, tiles), floor(tile / tiles)) + faceUV) / tiles, depth));
	}
	if (info.y >= 0.0)
	{
		float mask = floor(fract(info.y) * 64.0 + 0.5);
		if (mod(floor(mask / exp2(face)), 2.0) > 0.5)
		{
			float tile = floor(info.y) + face;
			vis *= shadow2D(u_RtShadowDynamic, vec3((vec2(mod(tile, RT_TILES), floor(tile / RT_TILES)) + faceUV) / RT_TILES, depth));
		}
	}
	return vis;
}

// what light i gives the air at P, as RtLightParticle has it, less the
// distance squared (the steps are spread for it) and before its shadow
float LampShape(int i, vec3 P, out vec3 toLight)
{
	vec4  l0 = u_RtLights[i * 4];
	vec4  l1 = u_RtLights[i * 4 + 1];
	toLight = l0.xyz - P;
	float d2 = dot(toLight, toLight);
	float r2 = l1.w * l1.w;
	if (d2 >= r2) return 0.0;
	float fade = 1.0 - d2 / r2;
	float att  = fade * fade;
	float kind = l0.w > 7.5 ? l0.w - 8.0 : l0.w;
	if (kind > 2.5)
	{
		vec4  l2   = u_RtLights[i * 4 + 2];
		float shut = length(l2.xyz);
		// never nearer than the glow of its shade
		att *= d2 / max(d2, l2.w * l2.w);
		if (l2.w < 0.0) att *= max(0.0, dot(toLight, l2.xyz)) * inversesqrt(max(d2, 1.0)) / shut;
		else if (shut > 1.0) att *= 1.0 - (shut - 1.0) * smoothstep(0.55, 0.2, abs(dot(toLight, l2.xyz)) * inversesqrt(max(d2, 1.0)) / shut);
		else if (shut > 0.0) att *= 1.0 - shut * smoothstep(-0.05, 0.3, dot(toLight, -l2.xyz) * inversesqrt(max(d2, 1.0)) / shut);
	}
	else if (kind > 0.5 && kind < 1.5)
	{
		vec4  l2 = u_RtLights[i * 4 + 2];
		float along = -dot(toLight, l2.xyz);
		if (along <= 0.001) return 0.0;
		float radiusAtDist = l2.w * along;
		float sampleRadius = length(-toLight - l2.xyz * along);
		if (sampleRadius >= radiusAtDist) return 0.0;
		att *= clamp((radiusAtDist - sampleRadius) / 32.0, 0.0, 1.0);
	}
	return att;
}

void main()
{
	float z = texture2D(u_ScreenDepthMap, var_DepthTex).r;
	float depth = 1.0 / mix(u_ViewInfo.x, 1.0, z);
	vec3  ray = var_ViewDir * depth;
	float len = length(ray);
	vec3  D   = ray / len;
	// a start for each pixel of a 4 x 4 block (interleaved gradient noise),
	// which the blur evens out
	float jitter = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(0.06711056, 0.00583715))));
	vec3  sum = vec3(0.0);

	if (u_RtSunDir.w > 0.5 && u_VolParams.z > 0.0)
	{
		float far = min(len, u_VolRange.x);
		float dt  = far / float(SUN_STEPS);
		float lit = 0.0;
		for (int s = 0; s < SUN_STEPS; s++)
		{
			vec3 P = u_ViewOrigin + D * ((float(s) + jitter) * dt);
			lit += u_VolRange.y > 0.5 ? SunShadow(P) : 1.0;
		}
		sum += u_RtSunColor.rgb * (u_VolParams.z * Phase(dot(D, u_RtSunDir.xyz)) * lit * dt);
	}

	if (u_VolParams.w > 0.0)
	{
		int   count = int(u_RtParams.x);
		float phaseMost = Phase(1.0);
		for (int k = 0; k < RT_MAX_LIGHTS; k++)
		{
			if (k >= count) break;
			int  i  = RtListed(k);
			vec4 l0 = u_RtLights[i * 4];
			vec4 l1 = u_RtLights[i * 4 + 1];
			// where the ray is inside its reach
			vec3  oc = l0.xyz - u_ViewOrigin;
			float b  = dot(oc, D);
			float h  = dot(oc, oc) - b * b;
			float r2 = l1.w * l1.w;
			if (h >= r2) continue;
			float w  = sqrt(r2 - h);
			float t0 = max(b - w, 0.0), t1 = min(b + w, len);
			if (t1 <= t0) continue;
			// too little to see at most: what the whole ray takes from a point
			// light it passes h off is pi / h of it
			if (u_VolParams.x * u_VolParams.w * phaseMost * max(l1.r, max(l1.g, l1.b)) * 3.14159 / max(sqrt(h), 16.0) < VOL_CUTOFF) continue;

			// the steps spread evenly by the angle at the lamp, not along the
			// ray (Kulla and Fajardo's equi-angular sampling): as many near
			// it as the light there is, and its 1 / distance squared drops out
			vec4  l3 = u_RtLights[i * 4 + 3];
			bool  shadowed = u_RtParams.z > 0.5 && (l3.x >= 0.0 || l3.y >= 0.0);
			float hh = max(sqrt(h), 16.0);
			float a0 = atan((t0 - b) / hh), a1 = atan((t1 - b) / hh);
			float lamp = 0.0;
			for (int s = 0; s < LAMP_STEPS; s++)
			{
				float t = b + hh * tan(mix(a0, a1, (float(s) + jitter) / float(LAMP_STEPS)));
				vec3  P = u_ViewOrigin + D * t;
				vec3  toLight;
				float f = LampShape(i, P, toLight);
				if (f <= 0.0) continue;
				if (shadowed) f *= LampShadow(l3, -toLight);
				lamp += f * Phase(dot(D, toLight) * inversesqrt(max(dot(toLight, toLight), 1.0)));
			}
			sum += l1.rgb * (lamp * u_VolParams.w * (a1 - a0) / (hh * float(LAMP_STEPS)));
		}
	}

	gl_FragColor = vec4(sum * u_VolParams.x, 1.0);
}
