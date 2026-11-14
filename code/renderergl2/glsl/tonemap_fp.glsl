uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;

uniform vec4      u_Color;


uniform vec2      u_AutoExposureMinMax;
uniform vec3      u_ToneMinAvgMaxLinear;

// Added in OPM: r_linearLighting, the frame is linear light (GLSL_BindProgram)
uniform float     u_Linear;

// Added in OPM: the picture's grade (r_grade, tr_atmos.c)
uniform vec4      u_GradeLift;  // lift, saturation
uniform vec4      u_GradeGamma; // one over the gamma, vibrance
uniform vec4      u_GradeGain;  // gain, contrast
uniform vec4      u_GradeMisc;  // vignette, grain, a number a frame, whether there is a grade

varying vec2      var_TexCoords;
varying float     var_InvWhite;

const vec3  LUMINANCE_VECTOR =   vec3(0.2125, 0.7154, 0.0721); //vec3(0.299, 0.587, 0.114);

float FilmicTonemap(float x)
{
	const float SS  = 0.22; // Shoulder Strength
	const float LS  = 0.30; // Linear Strength
	const float LA  = 0.10; // Linear Angle
	const float TS  = 0.20; // Toe Strength
	const float TAN = 0.01; // Toe Angle Numerator
	const float TAD = 0.30; // Toe Angle Denominator

	return ((x*(SS*x+LA*LS)+TS*TAN)/(x*(SS*x+LS)+TS*TAD)) - TAN/TAD;
}

// Added in OPM: the finished picture graded as a film is: lift tints the darks
// and leaves white, gain scales it all, gamma bends the middle; then the
// contrast about middle grey, the colour (vibrance: more to what has little),
// darker corners and grain.
vec3 Grade(vec3 c)
{
	c = c * u_GradeGain.rgb + u_GradeLift.rgb * (vec3(1.0) - c);
	c = pow(max(c, vec3(0.0)), u_GradeGamma.rgb);
	c = max((c - vec3(0.5)) * u_GradeGain.a + vec3(0.5), vec3(0.0));

	float luma = dot(c, LUMINANCE_VECTOR);
	float spread = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
	c = mix(vec3(luma), c, u_GradeLift.a * (1.0 + u_GradeGamma.a * (1.0 - spread)));

	vec2 d = var_TexCoords - vec2(0.5);
	c *= 1.0 - u_GradeMisc.x * smoothstep(0.2, 1.0, dot(d, d) * 2.0);

	vec2 at = gl_FragCoord.xy + vec2(mod(u_GradeMisc.z * 17.0, 289.0), mod(u_GradeMisc.z * 31.0, 311.0));
	float noise = fract(sin(dot(at, vec2(12.9898, 78.233))) * 43758.5453);
	c += vec3((noise - 0.5) * u_GradeMisc.y * 0.12 * (1.0 - 0.6 * clamp(luma, 0.0, 1.0)));

	return clamp(c, 0.0, 1.0);
}

void main()
{
	vec4 color = texture2D(u_TextureMap, var_TexCoords) * u_Color;

#if defined(USE_PBR)
	color.rgb *= color.rgb;
#endif

	vec3 minAvgMax = texture2D(u_LevelsMap, var_TexCoords).rgb;
	vec3 logMinAvgMaxLum = clamp(minAvgMax * 20.0 - 10.0, -u_AutoExposureMinMax.y, -u_AutoExposureMinMax.x);

	float invAvgLum = u_ToneMinAvgMaxLinear.y * exp2(-logMinAvgMaxLum.y);

	// Added in OPM: r_linearLighting. The frame is light: its black level as
	// light, white where the exposure without adapting (the fixed level, whose
	// log is -1: tr_image.c *fixedLevels) puts it, so adapting still brightens
	// and darkens; then shown, gamma 2.2. u_Color carries r_linearExposure.
	if (u_Linear > 0.5)
	{
		float black = pow(u_ToneMinAvgMaxLinear.x, 2.2);
		float white = u_Color.r * u_ToneMinAvgMaxLinear.y * 2.0 * u_ToneMinAvgMaxLinear.z;

		color.rgb = max(vec3(0.0), color.rgb * invAvgLum - vec3(black));
		color.r = FilmicTonemap(color.r);
		color.g = FilmicTonemap(color.g);
		color.b = FilmicTonemap(color.b);
		color.rgb = clamp(color.rgb / FilmicTonemap(max(white - black, 1e-4)), 0.0, 1.0);
		color.rgb = pow(color.rgb, vec3(1.0 / 2.2));
	}
	else
	{
	color.rgb = color.rgb * invAvgLum - u_ToneMinAvgMaxLinear.xxx;
	color.rgb = max(vec3(0.0), color.rgb);

	color.r = FilmicTonemap(color.r);
	color.g = FilmicTonemap(color.g);
	color.b = FilmicTonemap(color.b);

	color.rgb = clamp(color.rgb * var_InvWhite, 0.0, 1.0);
	}

#if defined(USE_PBR)
	color.rgb = sqrt(color.rgb);
#endif

	if (u_GradeMisc.w > 0.5)
	{
		color.rgb = Grade(color.rgb);
	}

	// add a bit of dither to reduce banding
	color.rgb += vec3(1.0/510.0 * mod(gl_FragCoord.x + gl_FragCoord.y, 2.0) - 1.0/1020.0);

	gl_FragColor = color;
}
