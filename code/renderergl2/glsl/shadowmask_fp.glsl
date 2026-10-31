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

	gl_FragColor = vec4(vec3(result), 1.0);
}
