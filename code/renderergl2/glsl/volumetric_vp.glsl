// Added in OPM: the light in the air (volumetric_fp.glsl), a quad over the view
attribute vec4 attr_Position;
attribute vec4 attr_TexCoord0;

uniform vec3   u_ViewForward;
uniform vec3   u_ViewLeft;
uniform vec3   u_ViewUp;

varying vec2   var_DepthTex;
varying vec3   var_ViewDir;

void main()
{
	gl_Position = attr_Position;
	var_DepthTex = attr_TexCoord0.xy;
	var_ViewDir = u_ViewForward + u_ViewLeft * -attr_Position.x + u_ViewUp * attr_Position.y;
}
