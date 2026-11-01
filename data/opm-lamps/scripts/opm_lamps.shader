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

// The bulb in a remodelled lamp (tools/lamps/remodel.py): it is the light,
// lit by nothing
opm_bulb
{
	surfaceparm nolightmap
	{
		map $whiteimage
		rgbGen const 1 0.9 0.72
	}
}

// A candle's or a lantern's flame: it flickers with its light (tr_rtlight.c)
opm_flame
{
	cull none
	surfaceparm nolightmap
	{
		map $whiteimage
		blendFunc GL_ONE GL_ONE
		rgbGen wave noise 0.8 0.2 0 7
	}
}
