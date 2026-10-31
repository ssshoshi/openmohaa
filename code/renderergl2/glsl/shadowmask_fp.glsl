uniform sampler2D u_ScreenDepthMap;

uniform sampler2DShadow u_ShadowMap;
#if defined(USE_SHADOW_CASCADE)
uniform sampler2DShadow u_ShadowMap2;
uniform sampler2DShadow u_ShadowMap3;
uniform sampler2DShadow u_ShadowMap4;
#endif

uniform mat4      u_ShadowMvp;
#if defined(USE_SHADOW_CASCADE)
uniform mat4      u_ShadowMvp2;
uniform mat4      u_ShadowMvp3;
uniform mat4      u_ShadowMvp4;
#endif

uniform vec3   u_ViewOrigin;
uniform vec4   u_ViewInfo; // zfar / znear, zfar

// Added in OPM: contact shadows (r_contactShadows). The sun's way (xyz) and how
// far towards it the depth is searched (w, 0 none); how thick what is found in
// front is taken to be (u_PrimaryLightRadius); the view's axes, as long as its
// far plane and the far plane's half width and height
uniform vec4   u_PrimaryLightOrigin;
uniform float  u_PrimaryLightRadius;
uniform vec3   u_ViewForward;
uniform vec3   u_ViewLeft;
uniform vec3   u_ViewUp;
varying vec2   var_Ndc;

varying vec2   var_DepthTex;
varying vec3   var_ViewDir;

// depth is GL_DEPTH_COMPONENT24
// so the maximum error is 1.0 / 2^24
#define DEPTH_MAX_ERROR 0.000000059604644775390625

// OPM: interleaved gradient noise (Jimenez, "Next Generation Post Processing
// in Call of Duty: Advanced Warfare"), as an angle. The old seed,
// mod(123456789., 1e-7 + 256. * dot(p, r)), was white noise at best (a float
// mod of a number past float precision), with small, banded angles where the
// screen's corner makes the divisor small. This one turns neighbouring pixels'
// taps as far apart as it can, so the few taps read as fine, even grain.
float random( const vec2 p )
{
	return 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

// (OPM: size, the map's texels across: the whole level's cascade has its own,
// r_sunShadowFarSize)
float PCF(const sampler2DShadow shadowmap, const vec2 st, const float dist, const float size)
{
	float mult;
	float scale = 2.0 / size;

#if 0
	// from http://http.developer.nvidia.com/GPUGems/gpugems_ch11.html
	vec2 offset = vec2(greaterThan(fract(var_DepthTex.xy * r_FBufScale * 0.5), vec2(0.25)));
	offset.y += offset.x;
	if (offset.y > 1.1) offset.y = 0.0;
	
	mult = shadow2D(shadowmap, vec3(st + (offset + vec2(-1.5,  0.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2( 0.5,  0.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2(-1.5, -1.5)) * scale, dist))
	     + shadow2D(shadowmap, vec3(st + (offset + vec2( 0.5, -1.5)) * scale, dist));
	 
	mult *= 0.25;
#endif

#if defined(USE_SHADOW_FILTER)
	float r = random(var_DepthTex.xy);
	float sinr = sin(r) * scale;
	float cosr = cos(r) * scale;
	mat2 rmat = mat2(cosr, sinr, -sinr, cosr);

	mult =  shadow2D(shadowmap, vec3(st + rmat * vec2(-0.7055767, 0.196515), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.3524343, -0.7791386), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.2391056, 0.9189604), dist));
  #if defined(USE_SHADOW_FILTER2)
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.07580382, -0.09224417), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.5784913, -0.002528916), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.192888, 0.4064181), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.6335801, -0.5247476), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(-0.5579782, 0.7491854), dist));
	mult += shadow2D(shadowmap, vec3(st + rmat * vec2(0.7320465, 0.6317794), dist));

	mult *= 0.11111;
  #else
    mult *= 0.33333;
  #endif
#else
	mult = shadow2D(shadowmap, vec3(st, dist));
#endif

	return mult;
}

float getLinearDepth(sampler2D depthMap, vec2 tex, float zFarDivZNear)
{
	float sampleZDivW = texture2D(depthMap, tex).r - DEPTH_MAX_ERROR;
	return 1.0 / mix(zFarDivZNear, 1.0, sampleZDivW);
}

#if defined(USE_SHADOW_CASCADE)
// OPM: over the last tenth of a cascade's square the next one is blended in,
// so the step from its sharper texels to the next one's coarser ones is a
// ramp, not a line that crosses the ground as the view moves.
#define CASCADE_BLEND 0.1

bool Inside(const vec4 sp)
{
	return all(lessThan(abs(sp.xyz), vec3(abs(sp.w))));
}

float Edge(const vec4 sp)
{
	return smoothstep(1.0 - CASCADE_BLEND, 1.0, max(abs(sp.x), abs(sp.y)) / abs(sp.w));
}

float Sample(const sampler2DShadow shadowmap, vec4 sp, const float size)
{
	sp.xyz = sp.xyz * (0.5 / sp.w) + vec3(0.5);
	return PCF(shadowmap, sp.xy, sp.z, size);
}
#endif

// Added in OPM: contact shadows. The cascades' texels are an inch or more
// across, and a shadow a foot or a crate's corner casts under that is lost.
// A few steps from the point towards the sun through the view's own depth:
// something in front of a step, though not by more than it can be thick, is
// what stands between the point and the sun. (UE4's and Days Gone's.)
#define CONTACT_STEPS 8

float ContactShadow(vec3 P, float depth)
{
	vec3  sunDir = u_PrimaryLightOrigin.xyz;
	float len    = u_PrimaryLightOrigin.w;
	float ff     = dot(u_ViewForward, u_ViewForward);
	vec2  texPerNdc = vec2(dFdx(var_DepthTex.x) / dFdx(var_Ndc.x), dFdy(var_DepthTex.y) / dFdy(var_Ndc.y));
	// (not dithered: with nothing to average it over frames, a dither of the
	// start showed as speckle on the shadowed side)
	float shadow = 0.0;

	// longer further off, where a texel of depth covers more
	len *= 1.0 + depth * u_ViewInfo.y / 512.0;

	for (int i = 0; i < CONTACT_STEPS; i++)
	{
		float t  = (float(i) + 0.5) / float(CONTACT_STEPS);
		vec3  v  = P + sunDir * (len * t) - u_ViewOrigin;
		float vf = dot(v, u_ViewForward);
		if (vf <= 0.0)
			break;
		vec2  ndc = vec2(-dot(v, u_ViewLeft) / dot(u_ViewLeft, u_ViewLeft), dot(v, u_ViewUp) / dot(u_ViewUp, u_ViewUp)) * ff / vf;
		if (any(greaterThan(abs(ndc), vec2(1.0))))
			break;
		vec2  tex   = var_DepthTex + (ndc - var_Ndc) * texPerNdc;
		float scene = getLinearDepth(u_ScreenDepthMap, tex, u_ViewInfo.x) * u_ViewInfo.y;
		float here  = vf / ff * u_ViewInfo.y;
		float front = here - scene;
		// in front of it, not by much (the step's own surface), and thin
		if (front > 0.5 + here * 0.002 && front < u_PrimaryLightRadius)
			shadow = max(shadow, 1.0 - t * t);
	}
	return 1.0 - shadow;
}

void main()
{
	float result;

	float depth = getLinearDepth(u_ScreenDepthMap, var_DepthTex, u_ViewInfo.x);
	vec4 biasPos = vec4(u_ViewOrigin + var_ViewDir * (depth - 0.5 / u_ViewInfo.x), 1.0);

	if ( depth >= 0.999 )
	{
		result = 1.0;
	}
	else
	{
#if defined(USE_SHADOW_CASCADE)
		vec4 s0 = u_ShadowMvp  * biasPos;
		vec4 s1 = u_ShadowMvp2 * biasPos;
		vec4 s2 = u_ShadowMvp3 * biasPos;
		float t;

		if (Inside(s0))
		{
			result = Sample(u_ShadowMap, s0, r_shadowMapSize);
			t = Edge(s0);
			if (t > 0.0 && Inside(s1))
				result = mix(result, Sample(u_ShadowMap2, s1, r_shadowMapSize), t);
		}
		else if (Inside(s1))
		{
			result = Sample(u_ShadowMap2, s1, r_shadowMapSize);
			t = Edge(s1);
			if (t > 0.0 && Inside(s2))
				result = mix(result, Sample(u_ShadowMap3, s2, r_shadowMapSize), t);
		}
		else if (Inside(s2))
		{
			result = Sample(u_ShadowMap3, s2, r_shadowMapSize);
			t = Edge(s2);
			if (t > 0.0)
				result = mix(result, Sample(u_ShadowMap4, u_ShadowMvp4 * biasPos, r_sunShadowFarSize), t);
		}
		else
		{
			result = Sample(u_ShadowMap4, u_ShadowMvp4 * biasPos, r_sunShadowFarSize);
		}
#else
		vec4 shadowpos = u_ShadowMvp * biasPos;
		shadowpos.xyz = shadowpos.xyz * (0.5 / shadowpos.w) + vec3(0.5);
		result = PCF(u_ShadowMap, shadowpos.xy, shadowpos.z, r_sunShadowFarSize);
#endif
	}

	// Added in OPM: what the cascades let through, what stands close enough
	// to the point to be under their texels may still stop
	if (u_PrimaryLightOrigin.w > 0.0 && result > 0.0 && depth < 0.999)
		result *= ContactShadow(u_ViewOrigin + var_ViewDir * depth, depth);

	gl_FragColor = vec4(vec3(result), 1.0);
}
