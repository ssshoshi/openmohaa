// Added in OPM: r_motionBlur's moving models (tr_motionblur.c). A skeletal
// model drawn again over the frame, where it is now and where it was a frame
// ago, for how far each pixel of it moved on the screen: posed twice, from
// this frame's bones and the last frame's (USE_SKEL_GPU, as lightall_vp.glsl
// poses it), or as the CPU posed it, moved only as the whole model moved.

#if defined(USE_SKEL_GPU)
// a vertex as tr_skelgpu.cpp keeps it: up to eight weights, each its place by
// its bone (xyz) and how much (w), their bones four and four
attribute vec4 attr_Position;
attribute vec4 attr_Position2;
attribute vec4 attr_Normal2;
attribute vec4 attr_Tangent2;
attribute vec4 attr_LightDirection;
attribute vec4 attr_TexCoord1;
attribute vec4 attr_PaintColor;
attribute vec4 attr_Color;
attribute vec4 attr_BoneIndexes;
attribute vec4 attr_BoneWeights;
attribute vec4 attr_TexCoord0; // zw: its morphs, the first and how many

uniform sampler2D u_SkelBones;   // this frame's bones
uniform sampler2D u_MbPrevBones; // and the last frame's, at the same places
uniform vec4      u_SkelParams;  // the model's first bone, its scale
uniform int       u_SkelNumDents;
uniform vec4      u_SkelDents[SKEL_MAX_DENTS * 2];
uniform sampler2D u_SkelMorphMap;
uniform int       u_SkelNumMorphs;
uniform vec4      u_SkelMorphs[SKEL_MAX_MORPHS / 4];
#else
attribute vec3 attr_Position;
#endif

uniform mat4 u_ModelViewProjectionMatrix; // as the frame's depth was drawn (with r_taa's nudge)
uniform mat4 u_MbCurrent;                 // the model to this frame's clip space, without the nudge
uniform mat4 u_MbPrevious;                // the model, where it was, to the last frame's
uniform vec4 u_MbParams;                  // the scale it was at, -, the view's width and height

varying vec4 var_Now;
varying vec4 var_Was;

#if defined(USE_SKEL_GPU)
vec3 SkelMorph(vec2 list)
{
	vec3 m = vec3(0.0);
	int  first = int(list.x + 0.5);
	int  n     = int(list.y + 0.5);

	for (int k = 0; k < 64; k++)
	{
		if (k >= n) break;
		int  t = first + k;
		vec4 e = texelFetch(u_SkelMorphMap, ivec2(t - (t / SKEL_BONE_ROW) * SKEL_BONE_ROW, t / SKEL_BONE_ROW), 0);
		int  i = int(e.w + 0.5);
		m += u_SkelMorphs[i / 4][i - (i / 4) * 4] * e.xyz;
	}
	return m;
}

vec4 SkelTexel(sampler2D bones, int bone, int k)
{
	int t = (int(u_SkelParams.x) + bone) * 4 + k;
	return texelFetch(bones, ivec2(t - (t / SKEL_BONE_ROW) * SKEL_BONE_ROW, t / SKEL_BONE_ROW), 0);
}

vec3 SkelWeight(sampler2D bones, vec4 w, float bone)
{
	int b = int(bone + 0.5);
	return (w.x * SkelTexel(bones, b, 1).xyz + w.y * SkelTexel(bones, b, 2).xyz + w.z * SkelTexel(bones, b, 3).xyz
		+ SkelTexel(bones, b, 0).xyz) * w.w;
}

// as lightall_vp.glsl poses it, by the bones given
vec3 SkelPose(sampler2D bones, vec4 firstWeight)
{
	vec3 p = SkelWeight(bones, firstWeight, attr_BoneIndexes.x);
	if (attr_Position2.w != 0.0)      p += SkelWeight(bones, attr_Position2, attr_BoneIndexes.y);
	if (attr_Normal2.w != 0.0)        p += SkelWeight(bones, attr_Normal2, attr_BoneIndexes.z);
	if (attr_Tangent2.w != 0.0)       p += SkelWeight(bones, attr_Tangent2, attr_BoneIndexes.w);
	if (attr_LightDirection.w != 0.0) p += SkelWeight(bones, attr_LightDirection, attr_BoneWeights.x);
	if (attr_TexCoord1.w != 0.0)      p += SkelWeight(bones, attr_TexCoord1, attr_BoneWeights.y);
	if (attr_PaintColor.w != 0.0)     p += SkelWeight(bones, attr_PaintColor, attr_BoneWeights.z);
	if (attr_Color.w != 0.0)          p += SkelWeight(bones, attr_Color, attr_BoneWeights.w);
	return p;
}
#endif

void main()
{
#if defined(USE_SKEL_GPU)
	vec4 firstWeight = attr_Position;
	if (u_SkelNumMorphs > 0 && attr_TexCoord0.w > 0.5)
		firstWeight.xyz += SkelMorph(attr_TexCoord0.zw);

	vec3 position = SkelPose(u_SkelBones, firstWeight) * u_SkelParams.y;
	vec3 was      = SkelPose(u_MbPrevBones, firstWeight) * u_MbParams.x;

	// pushed into the gore's dents as lightall_vp.glsl does, and where it was
	// by as much: a dent is not a movement
	vec3 dented = position;
	for (int k = 0; k < SKEL_MAX_DENTS; k++)
	{
		if (k >= u_SkelNumDents) break;
		vec4  ds = u_SkelDents[k * 2];
		vec3  dd = u_SkelDents[k * 2 + 1].xyz;
		vec3  d  = dented - ds.xyz;
		float along   = dot(d, dd);
		float across2 = dot(d, d) - along * along;
		if (across2 >= ds.w * ds.w) continue;
		float fl = -sqrt(ds.w * ds.w - across2);
		if (along <= fl) continue;
		dented += (fl - along) * dd;
	}
	was     += dented - position;
	position = dented;
#else
	vec3 position = attr_Position;
	vec3 was      = attr_Position;
#endif

	gl_Position = u_ModelViewProjectionMatrix * vec4(position, 1.0);
	var_Now     = u_MbCurrent * vec4(position, 1.0);
	var_Was     = u_MbPrevious * vec4(was, 1.0);
}
