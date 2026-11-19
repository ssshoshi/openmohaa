// Added in OPM: r_motionBlur (tr_motionblur.c), each pixel's smear: how far
// it moved on the screen in the frame, a moving model's as mbobject_fp.glsl
// found it, anything else's from its depth and the two cameras (as
// taa_fp.glsl does), as much of it as the shutter takes in and no longer than
// the longest smear. Kept as half of it (the smear is centred on the pixel),
// with the pixel's distance; the view weapon's is none, its distance -1.

uniform sampler2D u_ScreenDepthMap;  // the frame's depth
uniform sampler2D u_MbVelocityMap;   // the moving models', a set where there are some

uniform mat4 u_TaaReproject; // from this view's clip space to the last frame's
uniform vec4 u_TaaParams;    // how much of the frame's movement the shutter takes in, the longest smear (pixels), the depth under which it is the view weapon, whether there are models' movements
uniform vec4 u_TaaBox;       // where the view is in the images, x, y, width, height (0 to 1)
uniform vec4 u_BloomTexel;   // one over the images' width and height, their width and height
uniform vec4 u_MbDepth;      // the projection's [10] and [14], to tell distances by

varying vec2 var_Tex;
varying vec2 var_View;

void main()
{
	ivec2 px    = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(u_ScreenDepthMap, px, 0).r;

	if (depth < u_TaaParams.z)
	{
		gl_FragColor = vec4(0.0, 0.0, -1.0, 0.0);
		return;
	}

	vec2 pixels = u_TaaBox.zw * u_BloomTexel.zw;
	vec2 moved;
	vec4 model = vec4(0.0);

	// a moving model's movement where it is the nearest thing about the
	// pixel: its edge's pixels, which its colour is blended into (the
	// anti-aliasing, r_taa) but the depth there is what is behind, are
	// smeared with it, not left as a sharp outline
	if (u_TaaParams.w > 0.5)
	{
		ivec2 lo = ivec2(u_TaaBox.xy * u_BloomTexel.zw);
		ivec2 hi = lo + ivec2(pixels) - 1;
		ivec2 nearest = px;
		float nearDepth = depth;
		for (int y = -1; y <= 1; y++)
		{
			for (int x = -1; x <= 1; x++)
			{
				ivec2 at = clamp(px + ivec2(x, y), lo, hi);
				float d  = texelFetch(u_ScreenDepthMap, at, 0).r;
				if (d < nearDepth && d >= u_TaaParams.z)
				{
					nearDepth = d;
					nearest   = at;
				}
			}
		}
		model = texelFetch(u_MbVelocityMap, nearest, 0);
	}

	if (model.a > 0.5)
	{
		moved = model.xy;
	}
	else
	{
		vec4 p   = u_TaaReproject * vec4(var_View * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
		vec2 was = p.w > 0.0 ? p.xy / p.w * 0.5 + 0.5 : var_View;
		moved    = (var_View - was) * pixels;
	}

	moved *= u_TaaParams.x;
	float len = length(moved);
	if (len > u_TaaParams.y)
		moved *= u_TaaParams.y / len;

	float dist = u_MbDepth.y / (depth * 2.0 - 1.0 + u_MbDepth.x);
	gl_FragColor = vec4(moved * 0.5, clamp(dist, 0.0, 60000.0), 0.0);
}
