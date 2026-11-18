// Added in OPM: temporal anti-aliasing (tr_taa.c), over the whole view.
attribute vec4 attr_Position;
attribute vec4 attr_TexCoord0;

varying vec2   var_Tex;  // in the frame sized images
varying vec2   var_View; // across and up the view, 0 to 1

void main()
{
	gl_Position = attr_Position;
	var_Tex     = attr_TexCoord0.xy;
	var_View    = attr_Position.xy * 0.5 + 0.5;
}
