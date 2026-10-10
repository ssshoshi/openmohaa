// Only normal and parallax mapping consume the tangent frame, and parallax
// height lives in the normal map, so USE_NORMALMAP alone decides whether the
// tangent attribute, the per-vertex basis and its two varyings are needed at
// all. Stock MOH:AA content ships no normal maps, so on nearly every surface
// this drops a vertex attribute, a cross product and eight interpolators.
#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT) && defined(USE_NORMALMAP)
  #define USE_TANGENT_FRAME
#endif

attribute vec4 attr_TexCoord0;
#if defined(USE_LIGHTMAP) || defined(USE_TCGEN) || defined(USE_GROUNDCOVER) || defined(USE_SKEL_GPU)
attribute vec4 attr_TexCoord1;
#endif
attribute vec4 attr_Color;

#if defined(USE_SKEL_GPU)
// Added in OPM: a skeletal model's vertex, posed here (tr_skelgpu.cpp): up
// to eight weights, each the vertex's place by its bone (xyz) and how much of
// it that bone has (w), in attr_Position, attr_Position2, attr_Normal2,
// attr_Tangent2, attr_LightDirection, attr_TexCoord1, attr_PaintColor and
// attr_Color; their bones, four in attr_BoneIndexes and four in
// attr_BoneWeights; and the normal and the tangent (attr_Tangent) as the
// first bone holds them
attribute vec4 attr_Position;
attribute vec4 attr_Position2;
attribute vec4 attr_Normal2;
attribute vec4 attr_Tangent2;
attribute vec4 attr_PaintColor;
attribute vec4 attr_BoneIndexes;
attribute vec4 attr_BoneWeights;
#else
attribute vec3 attr_Position;
#endif
attribute vec3 attr_Normal;
#if defined(USE_TANGENT_FRAME) || defined(USE_GROUNDCOVER) || defined(USE_SKEL_GPU)
attribute vec4 attr_Tangent;
#endif

#if defined(USE_VERTEX_ANIMATION)
attribute vec3 attr_Position2;
attribute vec3 attr_Normal2;
  #if defined(USE_TANGENT_FRAME)
attribute vec4 attr_Tangent2;
  #endif
#elif defined(USE_BONE_ANIMATION)
attribute vec4 attr_BoneIndexes;
attribute vec4 attr_BoneWeights;
#endif

#if defined(USE_SKEL_GPU)
attribute vec4 attr_LightDirection; // a weight
#elif defined(USE_LIGHT) && !defined(USE_LIGHT_VECTOR)
attribute vec3 attr_LightDirection;
#endif

#if defined(USE_DELUXEMAP)
uniform vec4   u_EnableTextures; // x = normal, y = deluxe, z = specular, w = cube
#endif

// Also used unconditionally by the MOH:AA global fog below, so it can no longer
// be guarded by the lighting defines.
uniform vec3   u_ViewOrigin;

#if defined(USE_TCGEN)
uniform int    u_TCGen0;
uniform vec3   u_TCGen0Vector0;
uniform vec3   u_TCGen0Vector1;
#endif

// Needed unconditionally by the MOH:AA global fog below.
uniform vec3   u_LocalViewOrigin;

#if defined(USE_TCMOD)
uniform vec4   u_DiffuseTexMatrix0;
uniform vec4   u_DiffuseTexMatrix1;
uniform vec4   u_DiffuseTexMatrix2;
uniform vec4   u_DiffuseTexMatrix3;
uniform vec4   u_DiffuseTexMatrix4;
uniform vec4   u_DiffuseTexMatrix5;
uniform vec4   u_DiffuseTexMatrix6;
uniform vec4   u_DiffuseTexMatrix7;
#endif

uniform mat4   u_ModelViewProjectionMatrix;
uniform vec4   u_BaseColor;
uniform vec4   u_VertColor;

#if defined(USE_MODELMATRIX)
uniform mat4   u_ModelMatrix;
#endif

// Added in OPM: the realtime lights light the surface in world space
#if defined(USE_RTLIGHT)
  #if !defined(USE_MODELMATRIX)
uniform mat4   u_ModelMatrix;
  #endif
varying vec3   var_RtPos;
varying vec3   var_RtNormal;
#endif

// Added in OPM: ground cover tufts (tr_groundcover.c). attr_Tangent is where
// the tuft stands, and in w how far this vertex sways (0 on the ground);
// attr_TexCoord1 its phase, and the fraction of the draw distance it reaches
#if defined(USE_GROUNDCOVER)
uniform vec4   u_GroundCover; // time, draw distance, wind
#endif

#if defined(USE_VERTEX_ANIMATION)
uniform float  u_VertexLerp;
#elif defined(USE_BONE_ANIMATION)
uniform mat4 u_BoneMatrix[MAX_GLSL_BONES];
#endif

#if defined(USE_SKEL_GPU)
// the frame's bones, four texels each: where the bone is, then its axes
uniform sampler2D u_SkelBones;
uniform vec4      u_SkelParams;  // the model's first bone, its scale, its lighting (0 none, 1 one colour, 2 lights), lights
uniform vec4      u_SkelAmbient; // 0 to 255
uniform vec4      u_SkelLights[SKEL_MAX_LIGHTS * 3];
// gore dents (R_GoreDentsGpu): the middle and radius, then the way out of the
// hollow; and the joint their bone hangs from (w: how far short of it a dent
// breaks off what it pushes), and up from it
uniform int       u_SkelNumDents;
uniform vec4      u_SkelDents[SKEL_MAX_DENTS * 2];
uniform vec4      u_SkelDentOrigin;
uniform vec4      u_SkelDentUp;

vec4 SkelTexel(int bone, int k)
{
	int t = (int(u_SkelParams.x) + bone) * 4 + k;
	return texelFetch(u_SkelBones, ivec2(t - (t / SKEL_BONE_ROW) * SKEL_BONE_ROW, t / SKEL_BONE_ROW), 0);
}

// a weight's place, by its bone (SkelWeightGetXyz)
vec3 SkelWeight(vec4 w, float bone)
{
	int b = int(bone + 0.5);
	return (w.x * SkelTexel(b, 1).xyz + w.y * SkelTexel(b, 2).xyz + w.z * SkelTexel(b, 3).xyz + SkelTexel(b, 0).xyz) * w.w;
}

// MOH:AA's lighting of a model, at a vertex as the model has it (RB_Light_Real,
// RB_CalcLightGridColor, RB_Light_Fullbright)
vec4 SkelLight(vec3 P, vec3 N)
{
	if (u_SkelParams.z < 0.5)
		return vec4(1.0);
	if (u_SkelParams.z < 1.5)
		return u_SkelAmbient / 255.0;

	vec3 c = vec3(0.0);
	int  n = int(u_SkelParams.w);
	for (int k = 0; k < SKEL_MAX_LIGHTS; k++)
	{
		if (k >= n) break;
		vec4 l0 = u_SkelLights[k * 3];     // colour, type
		vec4 l1 = u_SkelLights[k * 3 + 1]; // origin, spot constant
		vec4 l2 = u_SkelLights[k * 3 + 2]; // direction, spot scale
		int  type = int(l0.w + 0.5);
		if (type == 0) // a point
		{
			vec3  v = l1.xyz - P;
			float d = dot(v, N);
			if (d > 0.0) c += l0.rgb * (d / dot(v, v));
		}
		else if (type == 1) // the sun
		{
			float d = dot(l2.xyz, N);
			if (d > 0.0) c += l0.rgb * d;
		}
		else
		{
			float d = dot(l2.xyz, N);
			if (d > 0.0)
			{
				vec3  v  = l1.xyz - P;
				float d2 = dot(v, v);
				if (type == 3) // a spot, near
				{
					c += l0.rgb * (d / d2);
				}
				else
				{
					float along = dot(v, l2.xyz);
					float most  = (l1.w - d2 / (along * along)) * l2.w;
					if (most > 0.0)
						c += l0.rgb * (d / d2 * min(most, 1.0));
				}
			}
		}
	}
	return vec4(clamp(floor(c) + u_SkelAmbient.rgb, 0.0, 255.0) / 255.0, 1.0);
}
#endif

#if defined(USE_LIGHT_VECTOR)
uniform vec4   u_LightOrigin;
uniform float  u_LightRadius;
uniform vec3   u_DirectedLight;
uniform vec3   u_AmbientLight;
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
uniform vec4  u_PrimaryLightOrigin;
uniform float u_PrimaryLightRadius;
#endif

varying vec4   var_TexCoords;
varying float  var_FogDist;

varying vec4   var_Color;
#if defined(USE_LIGHT_VECTOR) && !defined(USE_FAST_LIGHT)
varying vec4   var_ColorAmbient;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
varying vec4   var_Normal;
  #if defined(USE_TANGENT_FRAME)
varying vec4   var_Tangent;
varying vec4   var_Bitangent;
  #else
varying vec3   var_ViewDir;
  #endif
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
varying vec4   var_LightDir;
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
varying vec4   var_PrimaryLightDir;
#endif

#if defined(USE_TCGEN)
vec2 GenTexCoords(int TCGen, vec3 position, vec3 normal, vec3 TCGenVector0, vec3 TCGenVector1)
{
	vec2 tex = attr_TexCoord0.st;

	if (TCGen == TCGEN_LIGHTMAP)
	{
		tex = attr_TexCoord1.st;
	}
	else if (TCGen == TCGEN_ENVIRONMENT_MAPPED)
	{
		vec3 viewer = normalize(u_LocalViewOrigin - position);
		vec2 ref = reflect(viewer, normal).yz;
		tex.s = ref.x * -0.5 + 0.5;
		tex.t = ref.y *  0.5 + 0.5;
	}
	else if (TCGen == TCGEN_ENVIRONMENT_MAPPED2)
	{
		// MOH:AA's environmentmodel, as GL1 has it: where the surface faces
		// the viewer the view direction itself is used, not its reflection
		vec3 viewer = normalize(u_LocalViewOrigin - position);
		float d = dot(normal, viewer);
		vec3 ref = (d > 0.0) ? viewer : viewer - 2.0 * d * normal;
		tex.s = 0.5 + ref.y * 0.5;
		tex.t = 0.5 - ref.z * 0.5;
	}
	else if (TCGen == TCGEN_VECTOR)
	{
		tex = vec2(dot(position, TCGenVector0), dot(position, TCGenVector1));
	}

	return tex;
}
#endif

#if defined(USE_TCMOD)
vec2 ModTexCoords(vec2 st, vec3 position, vec4 texMatrix[8])
{
	vec2 st2 = st;
	vec2 offsetPos = vec2(position.x + position.z, position.y);

	st2 = vec2(st2.x * texMatrix[0].x + st2.y * texMatrix[0].y + texMatrix[0].z,
	           st2.x * texMatrix[1].x + st2.y * texMatrix[1].y + texMatrix[1].z);
	st2 += texMatrix[0].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[1].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[2].x + st2.y * texMatrix[2].y + texMatrix[2].z,
	           st2.x * texMatrix[3].x + st2.y * texMatrix[3].y + texMatrix[3].z);
	st2 += texMatrix[2].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[3].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[4].x + st2.y * texMatrix[4].y + texMatrix[4].z,
	           st2.x * texMatrix[5].x + st2.y * texMatrix[5].y + texMatrix[5].z);
	st2 += texMatrix[4].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[5].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[6].x + st2.y * texMatrix[6].y + texMatrix[6].z,
	           st2.x * texMatrix[7].x + st2.y * texMatrix[7].y + texMatrix[7].z);
	st2 += texMatrix[6].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[7].w * 2.0 * M_PI));

	return st2;
}
#endif


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


void main()
{
#if defined(USE_VERTEX_ANIMATION)
	vec3 position  = mix(attr_Position,    attr_Position2,    u_VertexLerp);
	vec3 normal    = mix(attr_Normal,      attr_Normal2,      u_VertexLerp);
  #if defined(USE_TANGENT_FRAME)
	vec3 tangent   = mix(attr_Tangent.xyz, attr_Tangent2.xyz, u_VertexLerp);
  #endif
#elif defined(USE_BONE_ANIMATION)
	mat4 vtxMat  = u_BoneMatrix[int(attr_BoneIndexes.x)] * attr_BoneWeights.x;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.y)] * attr_BoneWeights.y;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.z)] * attr_BoneWeights.z;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.w)] * attr_BoneWeights.w;
	mat3 nrmMat = mat3(cross(vtxMat[1].xyz, vtxMat[2].xyz), cross(vtxMat[2].xyz, vtxMat[0].xyz), cross(vtxMat[0].xyz, vtxMat[1].xyz));

	vec3 position  = vec3(vtxMat * vec4(attr_Position, 1.0));
	vec3 normal    = normalize(nrmMat * attr_Normal);
  #if defined(USE_TANGENT_FRAME)
	vec3 tangent   = normalize(nrmMat * attr_Tangent.xyz);
  #endif
#elif defined(USE_SKEL_GPU)
	vec3 position = SkelWeight(attr_Position, attr_BoneIndexes.x);
	if (attr_Position2.w != 0.0) position += SkelWeight(attr_Position2, attr_BoneIndexes.y);
	if (attr_Normal2.w != 0.0)   position += SkelWeight(attr_Normal2, attr_BoneIndexes.z);
	if (attr_Tangent2.w != 0.0)  position += SkelWeight(attr_Tangent2, attr_BoneIndexes.w);
	if (attr_LightDirection.w != 0.0) position += SkelWeight(attr_LightDirection, attr_BoneWeights.x);
	if (attr_TexCoord1.w != 0.0) position += SkelWeight(attr_TexCoord1, attr_BoneWeights.y);
	if (attr_PaintColor.w != 0.0) position += SkelWeight(attr_PaintColor, attr_BoneWeights.z);
	if (attr_Color.w != 0.0)     position += SkelWeight(attr_Color, attr_BoneWeights.w);
	position *= u_SkelParams.y;
	int  nb = int(attr_BoneIndexes.x + 0.5);
	vec3 normal = attr_Normal.x * SkelTexel(nb, 1).xyz + attr_Normal.y * SkelTexel(nb, 2).xyz + attr_Normal.z * SkelTexel(nb, 3).xyz;
  #if defined(USE_TANGENT_FRAME)
	vec3 tangent = attr_Tangent.x * SkelTexel(nb, 1).xyz + attr_Tangent.y * SkelTexel(nb, 2).xyz + attr_Tangent.z * SkelTexel(nb, 3).xyz;
  #endif
	// pushed into the dents, as R_GoreDentVertex does; what is broken off
	// clips away every triangle it is part of (R_GoreDropFolded)
	float goreClip = 1.0;
	for (int k = 0; k < SKEL_MAX_DENTS; k++)
	{
		if (k >= u_SkelNumDents) break;
		vec3  dc = u_SkelDents[k * 2].xyz;
		float dr = u_SkelDents[k * 2].w;
		vec3  dd = u_SkelDents[k * 2 + 1].xyz;
		vec3  jo = u_SkelDentOrigin.xyz;
		vec3  ju = u_SkelDentUp.xyz;
		if (dot(position - jo, ju) <= 0.0) continue; // below the joint: the neck stays
		if (dot(position - jo, dd) <= 0.0) continue; // the other side of the middle: a hollow only goes in
		vec3  d = position - dc;
		float along   = dot(d, dd);
		float across2 = dot(d, d) - along * along;
		if (across2 >= dr * dr) continue;
		float fl = -sqrt(dr * dr - across2);
		if (along <= fl) continue;
		vec3 moved = position + (fl - along) * dd;
		if (dot(moved - jo, dd) < u_SkelDentOrigin.w || dot(moved - jo, ju) <= 0.0)
		{
			// past the middle, or below the joint: broken off and gone
			position = jo;
			normal   = ju;
			goreClip = -1000.0;
		}
		else
		{
			position = moved;
			normal   = normalize(dc - moved);
		}
	}
	// what has ended up inside another hollow is gone as well
	if (goreClip > 0.0 && u_SkelNumDents > 0 && dot(position - u_SkelDentOrigin.xyz, u_SkelDentUp.xyz) > 0.0)
	{
		for (int k = 0; k < SKEL_MAX_DENTS; k++)
		{
			if (k >= u_SkelNumDents) break;
			vec4 ds = u_SkelDents[k * 2];
			vec3 d  = position - ds.xyz;
			if (dot(d, d) < ds.w * ds.w * 0.95)
			{
				position = u_SkelDentOrigin.xyz;
				goreClip = -1000.0;
				break;
			}
		}
	}
  #if !defined(GL_ES)
	gl_ClipDistance[0] = goreClip; // only while GL_CLIP_DISTANCE0 is on (RB_SkelGpuBind)
  #endif
	vec4 skelColor = SkelLight(position, normal);
#else
	vec3 position  = attr_Position;
	vec3 normal    = attr_Normal;
  #if defined(USE_TANGENT_FRAME)
	vec3 tangent   = attr_Tangent.xyz;
  #endif
#endif

#if defined(USE_GROUNDCOVER)
	{
		vec3  root  = attr_Tangent.xyz;
		float t     = u_GroundCover.x;
		float phase = attr_TexCoord1.x;
		float end   = attr_TexCoord1.y * u_GroundCover.y;

		// sinks into the ground over the last fifth of its distance
		float sink = clamp((end - distance(root, u_ViewOrigin)) / (end * 0.2), 0.0, 1.0);
		float gust = 0.6 + 0.4 * sin(t * 0.31 + root.x * 0.0021 + root.y * 0.0013);
		float sway = sin(t * 1.9 + phase + (root.x + root.y) * 0.011) * 0.65
		           + sin(t * 3.7 + phase * 1.7) * 0.35;

		position = root + (position - root) * sink;
		position += vec3(0.8, 0.6, 0.0) * ((sway * 2.2 + 1.2) * gust * attr_Tangent.w * u_GroundCover.z * sink);
	}
#endif

#if defined(USE_TCGEN)
	vec2 texCoords = GenTexCoords(u_TCGen0, position, normal, u_TCGen0Vector0, u_TCGen0Vector1);
#else
	vec2 texCoords = attr_TexCoord0.st;
#endif

#if defined(USE_TCMOD)
	vec4 diffuseTexMatrix[8];
	diffuseTexMatrix[0] = u_DiffuseTexMatrix0;
	diffuseTexMatrix[1] = u_DiffuseTexMatrix1;
	diffuseTexMatrix[2] = u_DiffuseTexMatrix2;
	diffuseTexMatrix[3] = u_DiffuseTexMatrix3;
	diffuseTexMatrix[4] = u_DiffuseTexMatrix4;
	diffuseTexMatrix[5] = u_DiffuseTexMatrix5;
	diffuseTexMatrix[6] = u_DiffuseTexMatrix6;
	diffuseTexMatrix[7] = u_DiffuseTexMatrix7;
	var_TexCoords.xy = ModTexCoords(texCoords, position, diffuseTexMatrix);
#else
	var_TexCoords.xy = texCoords;
#endif

	gl_Position = u_ModelViewProjectionMatrix * vec4(position, 1.0);

#if defined(USE_RTLIGHT)
	var_RtPos    = (u_ModelMatrix * vec4(position, 1.0)).xyz;
	var_RtNormal = (u_ModelMatrix * vec4(normal,   0.0)).xyz;
#endif

#if defined(USE_MODELMATRIX)
	position  = (u_ModelMatrix * vec4(position, 1.0)).xyz;
	normal    = (u_ModelMatrix * vec4(normal,   0.0)).xyz;
  #if defined(USE_TANGENT_FRAME)
	tangent   = (u_ModelMatrix * vec4(tangent,  0.0)).xyz;
  #endif
#endif

	// MOH:AA global fog. USE_MODELMATRIX is only defined for animated entities,
	// so everywhere else position is still in model space and has to be
	// measured against the model space view origin instead. Getting this wrong
	// puts entities at a huge apparent distance and fogs them out completely.
#if defined(USE_MODELMATRIX)
	var_FogDist = distance(position, u_ViewOrigin);
#else
	var_FogDist = distance(position, u_LocalViewOrigin);
#endif

#if defined(USE_TANGENT_FRAME)
	vec3 bitangent = cross(normal, tangent) * attr_Tangent.w;
#endif

#if defined(USE_LIGHT_VECTOR)
	vec3 L = u_LightOrigin.xyz - (position * u_LightOrigin.w);
#elif defined(USE_LIGHT) && !defined(USE_FAST_LIGHT) && defined(USE_SKEL_GPU)
	// a model lit per vertex keeps no direction its light came from, as the
	// CPU's skinning leaves it none: its light is the vertex colour as it is,
	// and the normal map shades the realtime lights alone (lightall_fp.glsl)
	vec3 L = vec3(0.0);
#elif defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	vec3 L = attr_LightDirection;
  #if defined(USE_MODELMATRIX)
	L = (u_ModelMatrix * vec4(L, 0.0)).xyz;
  #endif
#endif

#if defined(USE_LIGHTMAP)
	var_TexCoords.zw = attr_TexCoord1.st;
#endif

#if defined(USE_SKEL_GPU)
	var_Color = u_VertColor * skelColor + u_BaseColor;
#else
	var_Color = u_VertColor * attr_Color + u_BaseColor;
#endif

#if defined(USE_LIGHT_VECTOR)
  #if defined(USE_FAST_LIGHT)
	float sqrLightDist = dot(L, L);
	float NL = clamp(dot(normalize(normal), L) / sqrt(sqrLightDist), 0.0, 1.0);
	float attenuation = CalcLightAttenuation(u_LightOrigin.w, u_LightRadius * u_LightRadius / sqrLightDist);

	var_Color.rgb *= u_DirectedLight * (attenuation * NL) + u_AmbientLight;
  #else
	var_ColorAmbient.rgb = u_AmbientLight * var_Color.rgb;
	var_Color.rgb *= u_DirectedLight;
    #if defined(USE_PBR)
	var_ColorAmbient.rgb *= var_ColorAmbient.rgb;
    #endif
  #endif
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT) && defined(USE_PBR)
	var_Color.rgb *= var_Color.rgb;
#endif

#if defined(USE_PRIMARY_LIGHT) || defined(USE_SHADOWMAP)
	var_PrimaryLightDir.xyz = u_PrimaryLightOrigin.xyz - (position * u_PrimaryLightOrigin.w);
	var_PrimaryLightDir.w = u_PrimaryLightRadius * u_PrimaryLightRadius;
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
  #if defined(USE_LIGHT_VECTOR)
	var_LightDir = vec4(L, u_LightRadius * u_LightRadius);
  #else
	var_LightDir = vec4(L, 0.0);
  #endif
  #if defined(USE_DELUXEMAP)
	var_LightDir -= u_EnableTextures.y * var_LightDir;
  #endif
#endif

#if defined(USE_LIGHT) && !defined(USE_FAST_LIGHT)
	vec3 viewDir = u_ViewOrigin - position;
  #if defined(USE_TANGENT_FRAME)
	// store view direction in the spare tangent frame components to save on varyings
	var_Normal    = vec4(normal,    viewDir.x);
	var_Tangent   = vec4(tangent,   viewDir.y);
	var_Bitangent = vec4(bitangent, viewDir.z);
  #else
	var_Normal    = vec4(normal, 0.0);
	var_ViewDir   = viewDir;
  #endif
#endif
}
