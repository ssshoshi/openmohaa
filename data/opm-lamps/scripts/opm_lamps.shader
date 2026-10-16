// The cords lamps hang from (CG_PhysicsDrawCords, cg_physics_props.cpp): a
// strip the client builds along each cord every frame, near black. Drawn as a
// filter (what is behind it times the texture) rather than lit: the lamp at
// its end, and the dynamic lights passing, lit an opaque one into a white
// line, where a filter of near black stays near black whatever lights it.
opm/lampcord
{
	surfaceparm nonsolid
	surfaceparm nomarks
	surfaceparm nodlight
	cull none
	nopicmip
	nomipmaps
	{
		map textures/opm/lampcord.tga
		blendFunc filter
		rgbGen identity
	}
}
