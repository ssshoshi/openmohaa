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
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen const 1 0.78 0.45
		alphaGen wave noise 0.8 0.2 0 7
	}
}

// A remodelled swinging lantern put out (models/lights/swinging_lantern.tik's
// light_off, content/opm-lantern): its glass as lit by what is round it,
// no longer from inside
opm_swinging_lantern_glass_off
{
	cull none
	surfaceparm nolightmap
	{
		map textures/opm/lamps/swinging_lantern_glass.tga
		rgbGen lightingSpherical
	}
}

// The Opel truck with its blackout headlights on (models/vehicles/opeltruck.tik's
// opel1 second skin, chosen by the renderer while its live headlights are on,
// lamps.txt glowsurface): the paint as ever, and the slit in each lens cover
// glowing, brighter than white so that it blooms (opm_glow: only the unlit stage,
// three times a lamp's bulb: a slit is small)
opm_glow_opel
{
	{
		map textures/models/vehicles/OpelTruck/opel.tga
		rgbGen lightingSpherical
	}
	{
		map textures/opm/vehicles/opel_slit.png
		blendFunc GL_ONE GL_ONE
		rgbGen const 1 0.86 0.62
	}
}
