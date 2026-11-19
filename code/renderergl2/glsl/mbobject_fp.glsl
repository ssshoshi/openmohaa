// Added in OPM: r_motionBlur's moving models (tr_motionblur.c, mbobject_vp.glsl):
// how far each pixel of a model moved on the screen in the frame, in pixels,
// where the model is what the frame shows there (its depth is the frame's).

uniform sampler2D u_ScreenDepthMap; // the frame's depth
uniform vec4      u_MbParams;       // -, -, the view's width and height
uniform vec4      u_MbDepth;        // the projection's [10] and [14], to tell distances by

varying vec4 var_Now;
varying vec4 var_Was;

float Distance(float depth)
{
	return u_MbDepth.y / (depth * 2.0 - 1.0 + u_MbDepth.x);
}

void main()
{
	// not what the frame shows here: behind it, or a cut-out's hole in front
	float frame = texelFetch(u_ScreenDepthMap, ivec2(gl_FragCoord.xy), 0).r;
	float here  = Distance(gl_FragCoord.z);
	if (abs(here - Distance(frame)) > max(1.0, here * 0.01))
		discard;

	vec2 now = var_Now.xy / var_Now.w;
	vec2 was = var_Was.w > 0.0 ? var_Was.xy / var_Was.w : now;

	gl_FragColor = vec4((now - was) * 0.5 * u_MbParams.zw, 0.0, 1.0);
}
