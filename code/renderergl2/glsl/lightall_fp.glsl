// Mirrors lightall_vp.glsl: only normal and parallax mapping read the tangent
// frame, so without a normal map neither the basis nor its varyings exist.
#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT) && defined(USE_NORMALMAP)
  #define USE_TANGENT_FRAME
#endif

uniform sampler2D u_DiffuseMap;

#if defined(USE_LIGHTMAP)
uniform sampler2D u_LightMap;
#endif

#if defined(USE_NORMALMAP)
uniform sampler2D u_NormalMap;
#endif

#if defined(USE_DELUXEMAP)
uniform sampler2D u_DeluxeMap;
#endif

#if defined(USE_SPECULARMAP)
uniform sampler2D u_SpecularMap;
#endif

#if defined(USE_SHADOWMAP)
uniform sampler2D u_ShadowMap;
#endif

#if defined(USE_CUBEMAP)
uniform samplerCube u_CubeMap;
#endif

#if defined(USE_NORMALMAP) || defined(USE_DELUXEMAP) || defined(USE_SPECULARMAP) || defined(USE_CUBEMAP)
// y = deluxe, w = cube
uniform vec4      u_EnableTextures; 
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
uniform vec3  u_PrimaryLightColor;
uniform vec3  u_PrimaryLightAmbient;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
uniform vec4      u_NormalScale;
uniform vec4      u_SpecularScale;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
#if defined(USE_CUBEMAP)
uniform vec4      u_CubeMapInfo;
#endif
#endif

uniform int       u_AlphaTest;

// Added in OPM: wet (tr_water.c). What has been in the sea or under the
// spray, up to the mark the tide reached, is wet, after Lagarde's wet
// surfaces in Remember Me: not a layer over the lit surface but the surface
// changed before it is lit. Water filling a porous surface's pores darkens
// it and deepens its colour (stone, wood, cloth; paint and metal hardly);
// wet, any surface is smoother, and a film of water over it smoother still:
// it flattens the bumps of its normal map, reflects as water does, and gives
// back the sun as a small hard highlight and the sky most at a glancing look.
// The film dries first: under the mark is a band that is dark but no longer
// shines.
uniform vec4      u_Wet;    // how wet, the mark's height, 0, 0
uniform vec4      u_WetSky; // the sky it gives back
uniform vec4      u_WetSun; // towards the sun, and how bright it is
uniform vec3      u_ViewOrigin;
varying vec3      var_WetPos;
varying vec3      var_WetNormal;

float WetNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
	float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
	float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// How wet it is here (x: the water in it; y: the film on it) and which way
// the surface itself faces (zw unused)
vec2 WetAmount(out vec3 surfN)
{
	// faces' winding as the rest of this file has it: the front is the back
	surfN = normalize(!gl_FrontFacing ? var_WetNormal : -var_WetNormal);
	if (u_Wet.x <= 0.0)
		return vec2(0.0);

	// up to the mark, which is not a ruled line; the film a little below it
	float mark = u_Wet.y + (WetNoise(var_WetPos.xy / 70.0 + var_WetPos.z / 50.0) - 0.5) * 16.0;
	float soak = u_Wet.x * (1.0 - smoothstep(mark - 8.0, mark + 2.0, var_WetPos.z));
	float film = u_Wet.x * (1.0 - smoothstep(mark - 22.0, mark - 8.0, var_WetPos.z));

	// the ground is the wet beach's (beach_fp.glsl): only what stands up
	float up = 1.0 - 0.85 * smoothstep(0.75, 0.95, surfN.z);

	// the film runs down what stands up in streaks, here thick, there thin
	float runs = smoothstep(0.25, 0.85, WetNoise(vec2(dot(var_WetPos.xy, vec2(0.11, 0.08)), var_WetPos.z * 0.03)));
	film *= mix(1.0, mix(0.45, 1.0, runs), 1.0 - abs(surfN.z));

	return vec2(soak, film) * up;
}

// How porous a surface is, by how rough and how metallic it looks: rough
// and dull drinks the water, smooth or metallic sheds it
float WetPorosity(float roughness, vec3 specular)
{
	float metal = smoothstep(0.08, 0.4, max(max(specular.r, specular.g), specular.b));
	return smoothstep(0.25, 0.75, roughness) * (1.0 - metal);
}

// The water in it: darker, deeper in colour; smoother. Lagarde's factors.
void WetMaterial(vec2 wet, float porosity, inout vec3 diffuse, inout float roughness)
{
	float factor = mix(1.0, 0.45, porosity);
	float lum    = dot(diffuse, vec3(0.2125, 0.7154, 0.0721));

	diffuse   = mix(vec3(lum), diffuse, 1.0 + 0.25 * wet.x * porosity);
	diffuse   = max(diffuse, vec3(0.0)) * mix(1.0, factor, wet.x);
	roughness = roughness * mix(1.0, factor, 0.5 * wet.x);
	// the film: water's own smoothness
	roughness = mix(roughness, 0.12, wet.y);
}

// What the film gives back with no cubemap: the sky above the horizon, the
// dark beach and sea below it, with the fresnel of a surface this rough and
// as much of the sky as the surface's light says it sees
vec3 WetSky(vec3 N, vec3 E, float roughness, vec3 specular, vec3 light)
{
	float NE  = clamp(dot(N, E), 0.0, 1.0);
	vec3  R   = reflect(-E, N);
	vec3  sky = u_WetSky.rgb * mix(0.12, 1.0, smoothstep(-0.15, 0.35, R.z));
	vec3  F   = specular + (max(vec3(1.0 - roughness), specular) - specular) * pow(1.0 - NE, 5.0);
	float sees = clamp(dot(light, vec3(0.2125, 0.7154, 0.0721)) * 1.6, 0.25, 1.0);
	// seen edge on, the film's own ripples hide what it gives back: at a
	// model's outline, where its normal turns from the eye, the most of it
	// was a bright line a pixel wide (a Higgins boat's side)
	float edge = smoothstep(0.0, 0.25, dot(N, E));

	return sky * F * sees * (1.0 - roughness) * edge;
}

#if defined(USE_RTLIGHT)
varying vec3      var_RtPos;
varying vec3      var_RtNormal;
#endif

uniform vec4      u_GlobalFogColor;
uniform vec2      u_GlobalFogParams;

// Added in OPM: r_linearLighting. The colours read as linear light (gamma
// 2.2, as the display showed them), added up as light; the tone map shows the
// frame (GLSL_BindProgram sets it, for the world's view alone)
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

varying vec4      var_TexCoords;
varying float     var_FogDist;

varying vec4      var_Color;
#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4      var_ColorAmbient;
#endif

#if (defined(USE_LIGHT) && !defined(USE_FAST_LIGHT))
varying vec4   var_Normal;
  #if defined(USE_TANGENT_FRAME)
varying vec4   var_Tangent;
varying vec4   var_Bitangent;
  #else
varying vec3   var_ViewDir;
  #endif
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
varying vec4      var_LightDir;
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
varying vec4      var_PrimaryLightDir;
#endif


#define EPSILON 0.00000001

#if defined(USE_PARALLAXMAP)
float SampleDepth(sampler2D normalMap, vec2 t)
{
  #if defined(SWIZZLE_NORMALMAP)
	return 1.0 - texture2D(normalMap, t).r;
  #else
	return 1.0 - texture2D(normalMap, t).a;
  #endif
}

// Added in OPM: the depth read with the pixel's own gradients, so that a
// search that stops at its hit (non-uniform flow, where implicit derivatives
// are undefined) still reads the mip the pixel would
#if __VERSION__ >= 130
#define PARALLAX_GRAD
float SampleDepthGrad(sampler2D normalMap, vec2 t, vec2 gx, vec2 gy)
{
  #if defined(SWIZZLE_NORMALMAP)
	return 1.0 - textureGrad(normalMap, t, gx, gy).r;
  #else
	return 1.0 - textureGrad(normalMap, t, gx, gy).a;
  #endif
}
#else
#define SampleDepthGrad(m, t, gx, gy) SampleDepth(m, t)
#endif

// (OPM: steps, as many as the relief spans pixels, 16 at most; the search
// stops at the first point inside, which was all it kept)
float RayIntersectDisplaceMap(vec2 dp, vec2 ds, sampler2D normalMap, vec2 gx, vec2 gy, float steps)
{
	const int linearSearchSteps = 16;
	const int binarySearchSteps = 6;

	// current size of search window
	float size = 1.0 / steps;

	// adjust position if offset above surface
	dp -= ds * r_parallaxMapOffset;

	// current depth position
	float depth = 0.0;

	// best match found (starts with last position 1.0)
	float bestDepth = 1.0;

	// texture depth at best depth
	float texDepth = 0.0;

	float prevT = SampleDepthGrad(normalMap, dp, gx, gy);
	float prevTexDepth = prevT;

	// search front to back for first point inside object
	for(int i = 0; i < linearSearchSteps - 1; ++i)
	{
		if (float(i) >= steps - 1.0)
			break;

		depth += size;
		
		float t = SampleDepthGrad(normalMap, dp + ds * depth, gx, gy);
		
		if(depth >= t)
		{
			bestDepth = depth;	// store best depth
			texDepth = t;
			prevTexDepth = prevT;
#if defined(PARALLAX_GRAD)
			break;
#endif
		}
		prevT = t;
	}

	depth = bestDepth;

#if !defined (USE_RELIEFMAP)
	float div = 1.0 / (1.0 + (prevTexDepth - texDepth) * steps);
	bestDepth -= (depth - size - prevTexDepth) * div;
#else
	// recurse around first point (depth) for closest match
	for(int i = 0; i < binarySearchSteps; ++i)
	{
		size *= 0.5;

		float t = SampleDepthGrad(normalMap, dp + ds * depth, gx, gy);
		
		if(depth >= t)
		{
			bestDepth = depth;
			depth -= 2.0 * size;
		}

		depth += size;
	}
#endif

	return bestDepth - r_parallaxMapOffset;
}

// (OPM: gx, gy, the pixel's own texture gradients, as for the search: the ray
// stops at its first hit and is only cast where parallax is on, both per
// pixel, so the plain reads it made had undefined derivatives and could fetch
// any mip, the noise the search had before it read with gradients)
float LightRay(vec2 dp, vec2 ds, sampler2D normalMap, vec2 gx, vec2 gy)
{
	const int linearSearchSteps = 16;

	// current size of search window
	float size = 1.0 / float(linearSearchSteps);

	// current height from initial texel depth
	float height = 0.0;

	float startDepth = SampleDepthGrad(normalMap, dp, gx, gy);

	// find a collision or escape
	for(int i = 0; i < linearSearchSteps - 1; ++i)
	{
		height += size;

		if (startDepth < height)
			return 1.0;
		
		float t = SampleDepthGrad(normalMap, dp + ds * height, gx, gy);

		if (startDepth > t + height)
			return 0.0;
	}

	return 1.0;
}

// Added in OPM: LightRay for a light in tangent space. No direction (a model
// lit per vertex) or a light at or below the surface's plane casts no relief
// shadow: dividing by z there made the ray NaN, and whatever that fetch read
// put black blotches in every crevice. The shadow fades in over the first few
// degrees above the plane, where the ray would stretch across the texture.
float ParallaxShadow(vec3 lightDir, float scale, vec2 dp, sampler2D normalMap, vec2 gx, vec2 gy)
{
	float fade = smoothstep(0.0, 0.1, lightDir.z);
	if (fade <= 0.0)
		return 1.0;

	return mix(1.0, LightRay(dp, lightDir.xy * (scale / lightDir.z), normalMap, gx, gy), fade);
}
#endif

vec3 CalcDiffuse(vec3 diffuseAlbedo, float NH, float EH, float roughness)
{
#if defined(USE_BURLEY)
	// modified from https://disney-animation.s3.amazonaws.com/library/s2012_pbs_disney_brdf_notes_v2.pdf
	float fd90 = -0.5 + EH * EH * roughness;
	float burley = 1.0 + fd90 * 0.04 / NH;
	burley *= burley;
	return diffuseAlbedo * burley;
#else
	return diffuseAlbedo;
#endif
}

vec3 EnvironmentBRDF(float roughness, float NE, vec3 specular)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float v = 1.0 - max(roughness, NE);
	v *= v * v;
	return vec3(v) + specular;
}

vec3 CalcSpecular(vec3 specular, float NH, float EH, float roughness)
{
	// from http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	float rr = roughness*roughness;
	float rrrr = rr*rr;
	float d = (NH * NH) * (rrrr - 1.0) + 1.0;
	float v = (EH * EH) * (roughness + 0.5) + EPSILON;
	return specular * (rrrr / (4.0 * d * d * v));
}


float CalcLightAttenuation(float point, float normDist)
{
	// zero light at 1.0, approximating q3 style
	// also don't attenuate directional light
	float attenuation = (0.5 * normDist - 1.5) * point + 1.0;

	// clamp attenuation
	#if defined(NO_LIGHT_CLAMP)
	attenuation = max(attenuation, 0.0);
	#else
	attenuation = clamp(attenuation, 0.0, 1.0);
	#endif

	return attenuation;
}

#if defined(USE_BOX_CUBEMAP_PARALLAX)
vec4 hitCube(vec3 ray, vec3 pos, vec3 invSize, float lod, samplerCube tex)
{
	// find any hits on cubemap faces facing the camera
	vec3 scale = (sign(ray) - pos) / ray;

	// find the nearest hit
	float minScale = min(min(scale.x, scale.y), scale.z);

	// if the nearest hit is behind the camera, ignore
	// should not be necessary as long as pos is inside the cube
	//if (minScale < 0.0)
		//return vec4(0.0);

	// calculate the hit position, that's our texture coordinates
	vec3 tc = pos + ray * minScale;

	// if the texture coordinates are outside the cube, ignore
	// necessary since we're not fading out outside the cube
	if (any(greaterThan(abs(tc), vec3(1.00001))))
		return vec4(0.0);

	// fade out when approaching the cubemap edges
	//vec3 fade3 = abs(pos);
	//float fade = max(max(fade3.x, fade3.y), fade3.z);
	//fade = clamp(1.0 - fade, 0.0, 1.0);
			
	//return vec4(textureCubeLod(tex, tc, lod).rgb * fade, fade);
	return vec4(textureCubeLod(tex, tc, lod).rgb, 1.0);
}
#endif

void main()
{
	vec3 viewDir, lightColor, ambientColor, reflectance;
	vec3 L, N, E, H;
	float NL, NH, NE, EH, attenuation;

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	vec3 surfNormal = (!gl_FrontFacing ? var_Normal : -var_Normal).xyz;
  #if defined(USE_TANGENT_FRAME)
	mat3 tangentToWorld = mat3(var_Tangent.xyz, var_Bitangent.xyz, surfNormal);
	viewDir = vec3(var_Normal.w, var_Tangent.w, var_Bitangent.w);
  #else
	viewDir = var_ViewDir;
  #endif
	E = normalize(viewDir);
#endif

	lightColor = Lin(var_Color.rgb);

#if defined(USE_RTLIGHT) && defined(USE_LIGHT_VERTEX)
	// all of it live: the baked vertex lighting gives way to the ambient
	if (u_RtParams.y > 1.5)
		lightColor = u_RtAmbient.rgb * RtAmbientOcclusion();
#endif

#if defined(USE_LIGHTMAP)
	vec4 lightmapColor = texture2D(u_LightMap, var_TexCoords.zw);
  #if defined(RGBM_LIGHTMAP)
	lightmapColor.rgb *= lightmapColor.a;
  #endif
  #if defined(USE_PBR) && !defined(USE_FAST_LIGHT)
	lightmapColor.rgb *= lightmapColor.rgb;
  #endif
	lightColor *= Lin(lightmapColor.rgb);

  #if defined(USE_RTLIGHT)
	// all of it live: the lightmap gives way to the ambient
	if (u_RtParams.y > 1.5)
		lightColor = Lin(var_Color.rgb) * u_RtAmbient.rgb * RtAmbientOcclusion();
  #endif
#endif

	vec2 texCoords = var_TexCoords.xy;

#if defined(USE_PARALLAXMAP)
	vec3 offsetDir = E * tangentToWorld;

	// Added in OPM: the offset grows as 1/z toward grazing views, without
	// bound, and slid the texture across the surface as the view moved. Cap
	// it at 4x the depth, and fade it out where the surface is seen edge on.
	offsetDir.xy *= -u_NormalScale.a * smoothstep(0.0, 0.25, offsetDir.z) / max(offsetDir.z, 0.25);

	// Added in OPM: how far the deepest of it moves on the screen. Under half
	// a pixel (a wall across the room) it cannot be seen, and is not searched
	// for, nor its shadows; above, a step a pixel, 4 to 16.
	vec2  parallaxGx = dFdx(texCoords), parallaxGy = dFdy(texCoords);
	float reliefPixels = length(offsetDir.xy) / max(max(length(parallaxGx), length(parallaxGy)), 1e-7);
	bool  parallaxOn = reliefPixels >= 0.5;

	if (parallaxOn)
		texCoords += offsetDir.xy * RayIntersectDisplaceMap(texCoords, offsetDir.xy, u_NormalMap, parallaxGx, parallaxGy, clamp(ceil(reliefPixels), 4.0, 16.0));
#endif

	vec4 diffuse = texture2D(u_DiffuseMap, texCoords);
	
	float alpha = diffuse.a * var_Color.a;
	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}
	// Added in OPM: alpha to coverage (RB_AlphaToCoverage): the same cut, as a
	// ramp a pixel wide that the samples it covers smooth
	else if (u_AlphaTest == 4)
	{
		alpha = clamp((alpha - 0.5) / max(fwidth(alpha), 0.0001) + 0.5, 0.0, 1.0);
		if (alpha <= 0.0)
			discard;
	}

	// (r_linearLighting: the texture's colour as light; its alpha as it is)
	diffuse.rgb = Lin(diffuse.rgb);

	// Added in OPM: how wet (see WetAmount), and the light it had baked, for
	// how much sky it sees
	vec3 wetSurfN;
	vec2 wet       = WetAmount(wetSurfN);
	vec3 wetLight  = lightColor;

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	L = var_LightDir.xyz;
  #if defined(USE_DELUXEMAP)
	L += (texture2D(u_DeluxeMap, var_TexCoords.zw).xyz - vec3(0.5)) * u_EnableTextures.y;
  #endif
	float sqrLightDist = dot(L, L);
	// Added in OPM: no direction at all (a model lit per vertex) is no light
	// from any way: all of it ambient, as it was where the division made it
	// NaN and the driver's clamps turned that to nothing
	L = sqrLightDist > 0.0 ? L * inversesqrt(sqrLightDist) : vec3(0.0);

  #if defined(USE_LIGHT_VECTOR)
	attenuation  = CalcLightAttenuation(float(var_LightDir.w > 0.0), var_LightDir.w / sqrLightDist);
  #else
	attenuation  = 1.0;
  #endif

  #if defined(USE_NORMALMAP)
    #if defined(SWIZZLE_NORMALMAP)
	N.xy = texture2D(u_NormalMap, texCoords).ag - vec2(0.5);
    #else
	N.xy = texture2D(u_NormalMap, texCoords).rg - vec2(0.5);
    #endif
	N.xy *= u_NormalScale.xy;
	N.z = sqrt(clamp((0.25 - N.x * N.x) - N.y * N.y, 0.0, 1.0));
	N = tangentToWorld * N;
  #else
	N = surfNormal;
  #endif

	N = normalize(N);

  #if defined(USE_SHADOWMAP) 
	vec2 shadowTex = gl_FragCoord.xy * r_FBufScale;
	float shadowValue = texture2D(u_ShadowMap, shadowTex).r;

	// surfaces not facing the light are always shadowed
	shadowValue *= clamp(dot(N, var_PrimaryLightDir.xyz), 0.0, 1.0);

    #if defined(SHADOWMAP_MODULATE)
	lightColor *= shadowValue * (1.0 - u_PrimaryLightAmbient.r) + u_PrimaryLightAmbient.r;
    #endif
  #endif

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	float parallaxShadow = parallaxOn ? ParallaxShadow(L * tangentToWorld, u_NormalScale.a, texCoords, u_NormalMap, parallaxGx, parallaxGy) : 1.0;
  #endif


  #if !defined(USE_LIGHT_VECTOR)
	ambientColor = lightColor;
	float surfNL = clamp(dot(surfNormal, L), 0.0, 1.0);

	// reserve 25% ambient to avoid black areas on normalmaps
	lightColor *= 0.75;

	// Scale the incoming light to compensate for the baked-in light angle
	// attenuation.
	lightColor /= max(surfNL, 0.25);

	// Recover any unused light as ambient, in case attenuation is over 4x or
	// light is below the surface
	ambientColor = max(ambientColor - lightColor * surfNL, vec3(0.0));
  #else
	ambientColor = Lin(var_ColorAmbient.rgb);
  #endif

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	// Added in OPM: the relief shadows only the direct light, after the
	// ambient is split off; shadowing it all left lightmapped crevices black
	lightColor *= parallaxShadow;
  #endif

	NL = clamp(dot(N, L), 0.0, 1.0);
	NE = clamp(dot(N, E), 0.0, 1.0);
	H = normalize(L + E);
	EH = clamp(dot(E, H), 0.0, 1.0);
	NH = clamp(dot(N, H), 0.0, 1.0);

  #if defined(USE_SPECULARMAP)
	vec4 specular = texture2D(u_SpecularMap, texCoords);
  #else
	vec4 specular = vec4(1.0);
  #endif
	specular *= u_SpecularScale;

  #if defined(USE_PBR)
	diffuse.rgb *= diffuse.rgb;
  #endif

  #if defined(USE_PBR)
	// diffuse rgb is base color
	// specular red is gloss
	// specular green is metallicness
	float gloss = specular.r;
	float metal = specular.g;
	specular.rgb = metal * diffuse.rgb + vec3(0.04 - 0.04 * metal);
	diffuse.rgb *= 1.0 - metal;
  #else
	// diffuse rgb is diffuse
	// specular rgb is specular reflectance at normal incidence
	// specular alpha is gloss
	float gloss = specular.a;

	// adjust diffuse by specular reflectance, to maintain energy conservation
	diffuse.rgb *= vec3(1.0) - specular.rgb;
  #endif

  #if defined(GLOSS_IS_GLOSS)
	float roughness = exp2(-3.0 * gloss);
  #elif defined(GLOSS_IS_SMOOTHNESS)
	float roughness = 1.0 - gloss;
  #elif defined(GLOSS_IS_ROUGHNESS)
	float roughness = gloss;
  #elif defined(GLOSS_IS_SHININESS)
	float roughness = pow(2.0 / (8190.0 * gloss + 2.0), 0.25);
  #endif

	// Added in OPM: wet, the material before it is lit; the film flattens
	// the normal map's bumps
	if (wet.x > 0.0)
	{
		// what drinks the water holds less of it on top
		float porosity = WetPorosity(roughness, specular.rgb);
		wet.y *= mix(1.0, 0.5, porosity);
		WetMaterial(wet, porosity, diffuse.rgb, roughness);
		specular.rgb = mix(specular.rgb, max(specular.rgb, vec3(0.02)), wet.y);
		N  = normalize(mix(N, surfNormal, 0.8 * wet.y));
		NL = clamp(dot(N, L), 0.0, 1.0);
		NE = clamp(dot(N, E), 0.0, 1.0);
		NH = clamp(dot(N, H), 0.0, 1.0);
	}

	reflectance  = CalcDiffuse(diffuse.rgb, NH, EH, roughness);

  #if defined(r_deluxeSpecular)
    #if defined(USE_LIGHT_VECTOR)
	reflectance += CalcSpecular(specular.rgb, NH, EH, roughness) * r_deluxeSpecular;
    #else
	reflectance += CalcSpecular(specular.rgb, NH, EH, pow(roughness, r_deluxeSpecular));
    #endif
  #endif

	gl_FragColor.rgb  = lightColor   * reflectance * (attenuation * NL);
	gl_FragColor.rgb += ambientColor * diffuse.rgb;

  #if defined(USE_CUBEMAP)
	reflectance = EnvironmentBRDF(roughness, NE, specular.rgb);

	vec3 R = reflect(E, N);

	// parallax corrected cubemap (cheaper trick)
	// from http://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/
	vec3 parallax = u_CubeMapInfo.xyz + u_CubeMapInfo.w * viewDir;

  #if defined(USE_BOX_CUBEMAP_PARALLAX)
	vec3 cubeLightColor = hitCube(R * u_CubeMapInfo.w, parallax, u_CubeMapInfo.www, ROUGHNESS_MIPS * roughness, u_CubeMap).rgb * u_EnableTextures.w;
  #else
	vec3 cubeLightColor = textureCubeLod(u_CubeMap, R + parallax, ROUGHNESS_MIPS * roughness).rgb * u_EnableTextures.w;
  #endif

	// normalize cubemap based on last roughness mip (~diffuse)
	// multiplying cubemap values by lighting below depends on either this or the cubemap being normalized at generation
	//vec3 cubeLightDiffuse = max(textureCubeLod(u_CubeMap, N, ROUGHNESS_MIPS).rgb, 0.5 / 255.0);
	//cubeLightColor /= dot(cubeLightDiffuse, vec3(0.2125, 0.7154, 0.0721));

    #if defined(USE_PBR)
	cubeLightColor *= cubeLightColor;
    #else
	// Added in OPM: a cubemap holds what was shown, not light (r_linearLighting)
	cubeLightColor = Lin(cubeLightColor);
    #endif

	// multiply cubemap values by lighting
	// not technically correct, but helps make reflections look less unnatural
	//cubeLightColor *= lightColor * (attenuation * NL) + ambientColor;

	gl_FragColor.rgb += cubeLightColor * reflectance;
  #endif

  #if defined(USE_PRIMARY_LIGHT) || defined(SHADOWMAP_MODULATE)
	vec3 L2, H2;
	float NL2, EH2, NH2;

	L2 = var_PrimaryLightDir.xyz;

	// enable when point lights are supported as primary lights
	//sqrLightDist = dot(L2, L2);
	//L2 /= sqrt(sqrLightDist);

	NL2 = clamp(dot(N, L2), 0.0, 1.0);
	H2 = normalize(L2 + E);
	EH2 = clamp(dot(E, H2), 0.0, 1.0);
	NH2 = clamp(dot(N, H2), 0.0, 1.0);

	reflectance  = CalcSpecular(specular.rgb, NH2, EH2, roughness);

	// bit of a hack, with modulated shadowmaps, ignore diffuse
    #if !defined(SHADOWMAP_MODULATE)
	reflectance += CalcDiffuse(diffuse.rgb, NH2, EH2, roughness);
    #endif

	lightColor = Lin(u_PrimaryLightColor);

    #if defined(USE_SHADOWMAP)
	lightColor *= shadowValue;
    #endif

	// enable when point lights are supported as primary lights
	//lightColor *= CalcLightAttenuation(float(u_PrimaryLightDir.w > 0.0), u_PrimaryLightDir.w / sqrLightDist);

  #if defined(USE_PARALLAXMAP) && defined(USE_PARALLAXMAP_SHADOWS)
	if (parallaxOn)
		lightColor *= ParallaxShadow(L2 * tangentToWorld, u_NormalScale.a, texCoords, u_NormalMap, parallaxGx, parallaxGy);
  #endif

	gl_FragColor.rgb += lightColor * reflectance * NL2;
  #endif

  #if defined(USE_PBR)
	gl_FragColor.rgb = sqrt(gl_FragColor.rgb);
  #endif

	// Added in OPM: what the wet film gives back (a cubemap's reflection is
	// the surface's own), and the sun where no primary light pass draws it
	if (wet.y > 0.0)
	{
    #if !defined(USE_CUBEMAP)
		gl_FragColor.rgb += wet.y * WetSky(N, E, roughness, specular.rgb, wetLight);
    #endif
    #if !defined(USE_PRIMARY_LIGHT) && !defined(SHADOWMAP_MODULATE)
		{
			vec3 Hs = normalize(u_WetSun.xyz + E);
			gl_FragColor.rgb += wet.y * u_WetSun.w * clamp(dot(N, u_WetSun.xyz), 0.0, 1.0)
				* CalcSpecular(specular.rgb, clamp(dot(N, Hs), 0.0, 1.0), clamp(dot(E, Hs), 0.0, 1.0), roughness);
		}
    #endif
	}

#else

	// Added in OPM: wet, with no material maps: a porous dull surface
	if (wet.x > 0.0)
	{
		float wetRough = 0.6;
		vec3  Ew       = normalize(u_ViewOrigin - var_WetPos);
		vec3  Hs       = normalize(u_WetSun.xyz + Ew);
		vec3  F0       = vec3(0.02);

		wet.y *= 0.65;
		WetMaterial(wet, 0.7, diffuse.rgb, wetRough);
		gl_FragColor.rgb  = diffuse.rgb * lightColor;
		gl_FragColor.rgb += wet.y * WetSky(wetSurfN, Ew, wetRough, F0, wetLight);
		gl_FragColor.rgb += wet.y * u_WetSun.w * clamp(dot(wetSurfN, u_WetSun.xyz), 0.0, 1.0)
			* CalcSpecular(F0, clamp(dot(wetSurfN, Hs), 0.0, 1.0), clamp(dot(Ew, Hs), 0.0, 1.0), wetRough);
	}
	else
		gl_FragColor.rgb = diffuse.rgb * lightColor;

#endif

	gl_FragColor.a = alpha;


#if defined(USE_RTLIGHT)
	// Added in OPM: the realtime lights, on the surface's own colour; not on
	// a glow added on top, nor on dust and smoke without a light of their own
	// (R_RtStageKind), every pixel of a muzzle flash through every light
	if (u_RtStage != RT_STAGE_NONE)
	{
  #if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
		vec3 rtNormal = N;
		// the surface's own, which the baked lamps were worked out for, on
		// the side the normal map's is
		rt_geomN = normalize(var_RtNormal);
		if (dot(rt_geomN, N) < 0.0) rt_geomN = -rt_geomN;

		// the lights' and the sun's shine, with all of the light live
		rt_specOn    = u_RtParams.y > 1.5 && u_RtSurface.z > 0.0;
		rt_specView  = E;
		rt_specRough = roughness;
  #else
		vec3 rtNormal = normalize(!gl_FrontFacing ? var_RtNormal : -var_RtNormal);
  #endif
  #if defined(USE_LIGHTMAP)
		rt_skyLM = var_TexCoords.zw;
  #endif
  #if defined(USE_LIGHTMAP) || defined(USE_LIGHT_VERTEX)
		// the map's own lighting: with the direct light live alone
		// (r_realtimeLighting 1), what the lights change of it
		gl_FragColor.rgb = RtDelta(gl_FragColor.rgb, diffuse.rgb * RtLight(var_RtPos, rtNormal, u_RtParams.y < 1.5));
  #else
		// lit at run time: the lights, when all of it is live
		if (u_RtParams.y > 1.5)
			gl_FragColor.rgb += diffuse.rgb * RtLight(var_RtPos, rtNormal, false);
  #endif
  #if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
		gl_FragColor.rgb += rt_spec * specular.rgb;
  #endif
	}
#endif

	// MOH:AA global distance fog, matching the linear falloff the GL1 renderer
	// gets from fixed function GL_FOG.
	float fogFactor = clamp((var_FogDist - u_GlobalFogParams.x) * u_GlobalFogParams.y, 0.0, 1.0);
	gl_FragColor.rgb = Fog(gl_FragColor.rgb, u_GlobalFogColor.rgb, fogFactor * u_GlobalFogColor.a);
}
