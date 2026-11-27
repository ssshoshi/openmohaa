// Added in OPM: simulated explosions (tr_pyro.c), a quad over the view
// (scissored to the volume's box)
attribute vec4 attr_Position;
attribute vec4 attr_TexCoord0;

void main()
{
	gl_Position = attr_Position;
}
