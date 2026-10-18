// Added in OPM: water (tr_water.c). The surface as the map has it: the waves
// are in the fragment program, in its normal.
attribute vec3 attr_Position;
attribute vec3 attr_Normal;

uniform mat4   u_ModelViewProjectionMatrix;
uniform mat4   u_ModelMatrix;

varying vec3   var_Position; // in the world
varying vec3   var_Normal;   // the surface's own, in the world

void main()
{
	var_Position = (u_ModelMatrix * vec4(attr_Position, 1.0)).xyz;
	var_Normal   = normalize((u_ModelMatrix * vec4(attr_Normal, 0.0)).xyz);
	gl_Position  = u_ModelViewProjectionMatrix * vec4(attr_Position, 1.0);
}
