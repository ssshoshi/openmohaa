// Added in OPM: camera motion blur (tr_motionblur.c, r_motionBlur).
//
// Where each pixel was in the last frame is worked out from its depth and
// the two cameras (as taa_fp.glsl does), and the frame is read at even
// steps along the way it moved, as a camera's shutter, open a set time,
// would have smeared it. Only the camera's movement is followed: what moves
// by itself is as sharp as it was drawn. The view weapon goes with the
// camera and is left sharp, and nothing of it is smeared into the world
// round it. Runs on the frame as lit, before its glow and tone map, so a
// bright lamp leaves a bright streak.

uniform sampler2D u_ScreenImageMap; // the frame
uniform sampler2D u_ScreenDepthMap; // its depth, from the prepass

uniform mat4 u_TaaReproject; // from this view's clip space to the last frame's
uniform vec4 u_TaaParams;    // how much of the frame's movement the shutter takes in, the longest smear (pixels), the depth under which it is the view weapon, -
uniform vec4 u_TaaBox;       // where the view is in the images, x, y, width, height (0 to 1)
uniform vec4 u_BloomTexel;   // one over the images' width and height, their width and height

varying vec2 var_Tex;
varying vec2 var_View;

#define MB_SAMPLES 10

void main()
{
	vec3  here  = texture2D(u_ScreenImageMap, var_Tex).rgb;
	float depth = texture2D(u_ScreenDepthMap, var_Tex).r;

	// the view weapon stays sharp
	if (depth < u_TaaParams.z) {
		gl_FragColor = vec4(here, 1.0);
		return;
	}

	vec4 p   = u_TaaReproject * vec4(var_View * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec2 was = p.w > 0.0 ? p.xy / p.w * 0.5 + 0.5 : var_View;

	// how far it moved in the frame, in pixels, as much of it as the shutter
	// takes in, and no further than the longest smear
	vec2  pixels = u_TaaBox.zw * u_BloomTexel.zw;
	vec2  moved  = (var_View - was) * pixels * u_TaaParams.x;
	float len    = length(moved);

	if (len < 0.5) {
		gl_FragColor = vec4(here, 1.0);
		return;
	}
	if (len > u_TaaParams.y) {
		moved *= u_TaaParams.y / len;
	}

	// in the images, centred on the pixel
	vec2 step  = moved / pixels * u_TaaBox.zw / float(MB_SAMPLES - 1);
	vec2 start = var_Tex - step * float(MB_SAMPLES - 1) * 0.5;
	vec2 lo    = u_TaaBox.xy + 0.5 * u_BloomTexel.xy;
	vec2 hi    = u_TaaBox.xy + u_TaaBox.zw - 0.5 * u_BloomTexel.xy;

	vec3  sum    = vec3(0.0);
	float weight = 0.0;
	for (int i = 0; i < MB_SAMPLES; i++) {
		vec2 uv = clamp(start + step * float(i), lo, hi);

		// what the view weapon covers is not smeared into the world
		if (texture2D(u_ScreenDepthMap, uv).r < u_TaaParams.z) {
			continue;
		}
		sum    += texture2D(u_ScreenImageMap, uv).rgb;
		weight += 1.0;
	}

	gl_FragColor = vec4(weight > 0.0 ? sum / weight : here, 1.0);
}
