// Added in OPM: the bloom (tr_postprocess.c, RB_Bloom), over a whole level.
attribute vec4 attr_Position;
attribute vec4 attr_TexCoord0;

varying vec2   var_Tex;

void main()
{
	gl_Position = attr_Position;
	var_Tex     = attr_TexCoord0.xy;
}
