// Added in OPM: r_motionBlur (tr_motionblur.c), the smear gathered.
//
// Each pixel reads the frame at steps along the longest smear about its tile
// (mbtile_fp.glsl) and along its own, both ways from it (the smear is
// centred), and takes what it reads by depth and how far each smear reaches
// (as Jimenez's, after McGuire's reconstruction filter): what is behind it
// where its own smear reaches, what is in front of it where that one's smear
// reaches back to the pixel. So a soldier running across a still view smears
// over the wall behind, and the wall shows through his smear's ends; with the
// camera turning everything smears as the camera moved. The view weapon is
// left sharp and nothing of it is read into the world round it. Runs on the
// frame as lit, before its glow and tone map, so a bright lamp leaves a bright
// streak.

uniform sampler2D u_ScreenImageMap; // the frame
uniform sampler2D u_MbVelocityMap;  // half of each pixel's smear, its distance (-1: the view weapon)
uniform sampler2D u_MbTileMap;      // the longest half smear about each tile
uniform sampler2D u_ScreenDepthMap; // r_motionBlurDebug: the moving models' own movements

uniform vec4 u_MbParams;   // where the view starts in the images, in pixels; where it ends (the last pixel in it)
uniform vec4 u_TaaParams;  // a number for the frame, for the steps' noise; r_motionBlurDebug
uniform vec4 u_BloomTexel; // one over the images' width and height, their width and height

varying vec2 var_Tex;
varying vec2 var_View;

#define MB_SAMPLES 12

void main()
{
	ivec2 px     = ivec2(gl_FragCoord.xy);
	ivec2 lo     = ivec2(u_MbParams.xy);
	ivec2 hi     = ivec2(u_MbParams.zw);
	vec3  here   = texelFetch(u_ScreenImageMap, px, 0).rgb;
	vec4  centre = texelFetch(u_MbVelocityMap, px, 0);

	if (u_TaaParams.y > 0.5)
	{
		gl_FragColor = vec4(0.5 + centre.xy / 4.0, texelFetch(u_ScreenDepthMap, px, 0).a, 1.0);
		return;
	}

	// the view weapon stays sharp
	if (centre.z < 0.0)
	{
		gl_FragColor = vec4(here, 1.0);
		return;
	}

	vec4  tile    = texelFetch(u_MbTileMap, (px - lo) / MB_TILE, 0);
	vec2  most    = tile.xy;
	float mostLen = length(most);
	if (mostLen < 0.5)
	{
		gl_FragColor = vec4(here, 1.0);
		return;
	}

	float ownLen = length(centre.xy);
	vec2  own    = ownLen > 0.5 ? centre.xy : most;
	float soft   = max(2.0, centre.z * 0.02);

	// the steps nudged by up to half a step, differently pixel to pixel
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + u_TaaParams.x * 5.588238, vec2(0.06711056, 0.00583715)))) - 0.5;

	vec2  lov    = (vec2(lo) + 0.5) * u_BloomTexel.xy;
	vec2  hiv    = (vec2(hi) + 0.5) * u_BloomTexel.xy;
	vec3  sum    = vec3(0.0);
	float weight = 0.0;

	// everything about smeared near alike and no weapon to keep out (the
	// camera turning): the frame read along it, nothing more to tell
	if (tile.w < 0.5 && mostLen - tile.z < 1.0)
	{
		for (int i = 0; i < MB_SAMPLES; i++)
		{
			float t = ((float(i) + 0.5 + noise) / float(MB_SAMPLES)) * 2.0 - 1.0;
			sum += texture2D(u_ScreenImageMap, clamp((vec2(px) + 0.5 + most * t) * u_BloomTexel.xy, lov, hiv)).rgb;
		}
		gl_FragColor = vec4(sum / float(MB_SAMPLES), 1.0);
		return;
	}

	for (int i = 0; i < MB_SAMPLES; i++)
	{
		// even steps along the longest smear about, odd along its own
		vec2  along = (i - (i / 2) * 2) == 0 ? most : own;
		float t     = ((float(i / 2) + 0.5 + noise) / float(MB_SAMPLES / 2)) * 2.0 - 1.0;
		vec2  off   = along * t;
		float dist  = length(off);

		ivec2 sp = clamp(ivec2(floor(vec2(px) + 0.5 + off)), lo, hi);
		vec4  sv = texelFetch(u_MbVelocityMap, sp, 0);

		// what the view weapon covers is not read into the world
		if (sv.z < 0.0)
			continue;

		// behind the pixel: taken where the pixel's own smear reaches it; in
		// front: where its smear reaches back to the pixel
		float behind  = clamp(0.5 + (sv.z - centre.z) / soft, 0.0, 1.0);
		float inFront = 1.0 - behind;
		float w = behind * clamp(ownLen - dist + 1.0, 0.0, 1.0) + inFront * clamp(length(sv.xy) - dist + 1.0, 0.0, 1.0);

		vec2 uv = clamp((vec2(px) + 0.5 + off) * u_BloomTexel.xy, lov, hiv);
		sum    += w * texture2D(u_ScreenImageMap, uv).rgb;
		weight += w;
	}

	// what the steps did not take is the pixel's own colour
	gl_FragColor = vec4((sum + (float(MB_SAMPLES) - weight) * here) / float(MB_SAMPLES), 1.0);
}
