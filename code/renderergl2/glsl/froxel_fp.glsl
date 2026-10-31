// Added in OPM: the light in the air in a grid of cells over the view
// (r_volumetric 2): froxels, each a tile of the view across and a slice of
// its depth. Three passes, one program each (RB_VolumetricFroxels):
//
// FROXEL_DEPTH     each tile of the view: the furthest depth in it
// FROXEL_LIGHT      each cell: how much light its air scatters toward the eye
//                   (rgb, a unit along the ray) and how much it takes out (a)
// FROXEL_INTEGRATE  each cell: what the air from the eye to its far side
//                   adds (rgb) and lets through (a)
// FROXEL_APPLY      each pixel: the view times what the air lets through,
//                   plus what it adds, at the pixel's depth
//
// The grid is a 2D atlas, FROXEL_TILES slices a row, as the renderer has no
// 3D textures. Its slices go from the eye to u_VolRange.x, about even near
// the eye and further apart with distance (SliceDepth).
uniform sampler2D u_ScreenImageMap;  // the grid, lit (integrate) or integrated (apply)
uniform sampler2D u_ScreenDepthMap;  // the view's depth (depth, apply), its tiles' furthest (light)

uniform sampler2DShadow u_ShadowMap;
uniform sampler2DShadow u_ShadowMap2;
uniform sampler2DShadow u_ShadowMap3;
uniform sampler2DShadow u_ShadowMap4;
uniform mat4   u_ShadowMvp;
uniform mat4   u_ShadowMvp2;
uniform mat4   u_ShadowMvp3;
uniform mat4   u_ShadowMvp4;

uniform vec3   u_ViewOrigin;
uniform vec3   u_ViewForward; // a unit forward
uniform vec3   u_ViewLeft;    // left, as far as the view's edge is a unit forward
uniform vec3   u_ViewUp;      // up, likewise
uniform vec4   u_ViewInfo;    // zfar / znear, zfar; depth: a cell's size in the depth image
uniform vec4   u_VolParams;   // the air's scattering per unit, anisotropy, the sun's share, the lamps'
uniform vec4   u_VolRange;    // the grid's depth, the sun's cascades (1: there), the air's extinction per unit, -
uniform vec4   u_Froxel;      // cells across, up, slices, log2 of the depth's curve at the far side

varying vec2   var_DepthTex;
varying vec2   var_CellUV;

#define FROXEL_TILES 8.0
#define FROXEL_PIXELS 16.0
// how far the slices stay about even: one unit of depth in this many
#define FROXEL_CURVE 256.0

// the depth (forward of the eye) of the near side of slice s
float SliceDepth(float s)
{
	return (exp2(s / u_Froxel.z * u_Froxel.w) - 1.0) * FROXEL_CURVE;
}

// the slice (fractional) a depth is at
float DepthSlice(float z)
{
	return log2(z / FROXEL_CURVE + 1.0) / u_Froxel.w * u_Froxel.z;
}

// the atlas texel of a cell (uv over the grid, 0 to 1) in slice s, clamped
// inside its tile so a filtered read never takes the next slice's
vec2 AtlasUV(vec2 uv, float s)
{
	vec2 edge = 0.5 / u_Froxel.xy;
	vec2 tile = vec2(mod(s, FROXEL_TILES), floor(s / FROXEL_TILES));
	return (tile + clamp(uv, edge, 1.0 - edge)) / FROXEL_TILES;
}

// the way through the cell at uv, a unit forward
vec3 CellRay(vec2 uv)
{
	vec2 ndc = uv * 2.0 - 1.0;
	return u_ViewForward - u_ViewLeft * ndc.x + u_ViewUp * ndc.y;
}

#if defined(FROXEL_DEPTH)

// each tile's furthest depth, forward of the eye, from its FROXEL_PIXELS
// square of the view (u_ViewInfo.zw: a cell's size in the depth image)
void main()
{
	vec2  corner = var_DepthTex - 0.5 * u_ViewInfo.zw;
	vec2  step   = u_ViewInfo.zw / FROXEL_PIXELS;
	float far    = 0.0;
	for (int y = 0; y < int(FROXEL_PIXELS); y++)
		for (int x = 0; x < int(FROXEL_PIXELS); x++)
			far = max(far, texture2D(u_ScreenDepthMap, corner + (vec2(float(x), float(y)) + 0.5) * step).r);
	gl_FragColor = vec4(min(u_ViewInfo.y / mix(u_ViewInfo.x, 1.0, far), 60000.0), 0.0, 0.0, 1.0);
}

#elif defined(FROXEL_LIGHT)

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

// light i's shadow at v from it, as RtShadow has it, one tap of its four
// (as volumetric_fp.glsl)
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
// distance squared and before its shadow (as volumetric_fp.glsl)
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
	vec2  fc    = floor(gl_FragCoord.xy);
	vec2  tile  = floor(fc / u_Froxel.xy);
	vec2  cell  = fc - tile * u_Froxel.xy;
	float slice = tile.y * FROXEL_TILES + tile.x;
	float z0    = SliceDepth(slice);
	float z1    = SliceDepth(slice + 1.0);
	vec2  uv    = (cell + 0.5) / u_Froxel.xy;
	vec3  ray   = CellRay(uv);
	vec3  D     = normalize(ray);
	vec3  P     = u_ViewOrigin + ray * (0.5 * (z0 + z1));
	vec3  L     = vec3(0.0);

	// behind everything in its tile and the tiles round it: no pixel sees
	// it, nor filters it in (apply), so its air is left dark
	float seen = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
			seen = max(seen, texture2D(u_ScreenDepthMap, (clamp(cell + vec2(float(x), float(y)), vec2(0.0), u_Froxel.xy - 1.0) + 0.5) / u_Froxel.xy).r);
	if (z0 > seen)
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, u_VolRange.z);
		return;
	}

	// the sun: four taps spread over the cell, across and in depth
	if (u_RtSunDir.w > 0.5 && u_VolParams.z > 0.0)
	{
		float lit = 1.0;
		if (u_VolRange.y > 0.5)
		{
			vec2 du = 0.25 / u_Froxel.xy;
			lit  = SunShadow(u_ViewOrigin + CellRay(uv + vec2(-du.x, -du.y)) * mix(z0, z1, 0.125));
			lit += SunShadow(u_ViewOrigin + CellRay(uv + vec2( du.x, -du.y)) * mix(z0, z1, 0.625));
			lit += SunShadow(u_ViewOrigin + CellRay(uv + vec2(-du.x,  du.y)) * mix(z0, z1, 0.875));
			lit += SunShadow(u_ViewOrigin + CellRay(uv + vec2( du.x,  du.y)) * mix(z0, z1, 0.375));
			lit *= 0.25;
		}
		L += u_RtSunColor.rgb * (u_VolParams.z * Phase(dot(D, u_RtSunDir.xyz)) * lit);
	}

	// the lamps, at the cell's middle: their 1 / distance squared, kept off
	// the nearest 16 units as the ray march keeps it
	if (u_VolParams.w > 0.0)
	{
		int count = int(u_RtParams.x);
		for (int k = 0; k < RT_MAX_LIGHTS; k++)
		{
			if (k >= count) break;
			int  i = RtListed(k);
			vec4 l0 = u_RtLights[i * 4];
			vec4 l1 = u_RtLights[i * 4 + 1];
			vec3 toLight = l0.xyz - P;
			float d2 = dot(toLight, toLight);
			if (d2 >= l1.w * l1.w) continue;
			float f = LampShape(i, P, toLight);
			if (f <= 0.0) continue;
			vec4 l3 = u_RtLights[i * 4 + 3];
			if (u_RtParams.z > 0.5 && (l3.x >= 0.0 || l3.y >= 0.0))
				f *= LampShadow(l3, -toLight);
			L += l1.rgb * (f * Phase(dot(D, toLight) * inversesqrt(max(d2, 1.0))) / max(d2, 256.0));
		}
		L *= u_VolParams.w;
	}

	gl_FragColor = vec4(L * u_VolParams.x, u_VolRange.z);
}

#elif defined(FROXEL_INTEGRATE)

void main()
{
	vec2  fc    = floor(gl_FragCoord.xy);
	vec2  tile  = floor(fc / u_Froxel.xy);
	vec2  cell  = fc - tile * u_Froxel.xy;
	float slice = tile.y * FROXEL_TILES + tile.x;
	vec2  uv    = (cell + 0.5) / u_Froxel.xy;
	float along = length(CellRay(uv));
	vec3  scat  = vec3(0.0);
	float T     = 1.0;
	float zNear = 0.0;

	// front to back, each slice's light as it is let through what is
	// before it (Frostbite's: the light over the slice, as the slice takes
	// some of it out)
	for (int s = 0; s < 128; s++)
	{
		float fs = float(s);
		if (fs > slice) break;
		vec4  v     = texture2D(u_ScreenImageMap, AtlasUV(uv, fs));
		float zFar  = SliceDepth(fs + 1.0);
		float dt    = (zFar - zNear) * along;
		zNear = zFar;
		if (v.a > 1e-6)
		{
			float through = exp(-v.a * dt);
			scat += T * v.rgb * ((1.0 - through) / v.a);
			T    *= through;
		}
		else
		{
			scat += T * v.rgb * dt;
		}
	}

	gl_FragColor = vec4(scat, T);
}

#elif defined(FROXEL_APPLY)

void main()
{
	float z     = texture2D(u_ScreenDepthMap, var_DepthTex).r;
	float depth = u_ViewInfo.y / mix(u_ViewInfo.x, 1.0, z);
	// the cells hold the air to their far side: between the far sides of
	// the slices either side of the pixel's depth, the air from the eye to
	// the near side of slice 0 none
	float f  = min(DepthSlice(depth), u_Froxel.z) - 1.0;
	vec4  a, b;
	if (f < 0.0)
	{
		a = vec4(0.0, 0.0, 0.0, 1.0);
		b = texture2D(u_ScreenImageMap, AtlasUV(var_CellUV, 0.0));
		f += 1.0;
	}
	else
	{
		float s = floor(f);
		a = texture2D(u_ScreenImageMap, AtlasUV(var_CellUV, s));
		b = texture2D(u_ScreenImageMap, AtlasUV(var_CellUV, min(s + 1.0, u_Froxel.z - 1.0)));
		f -= s;
	}

	// added to the view as the view times a, plus rgb (GLS_DSTBLEND_SRC_ALPHA)
	gl_FragColor = mix(a, b, f);
}

#endif
