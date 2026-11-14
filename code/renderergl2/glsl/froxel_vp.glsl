// Added in OPM: the light in the air in a grid of cells (r_volumetric 2,
// froxel_fp.glsl), a quad over the grid's atlas or over the view
attribute vec4 attr_Position;
attribute vec4 attr_TexCoord0;

varying vec2   var_DepthTex;
varying vec2   var_CellUV;

void main()
{
	gl_Position  = attr_Position;
	var_DepthTex = attr_TexCoord0.xy;
	var_CellUV   = attr_Position.xy * 0.5 + 0.5;
}
