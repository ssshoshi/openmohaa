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
// light_off, content/opm-lamp-states): its clear glass, no longer lit from
// inside: a faint grey filter, and a faint sheen of the night on it
opm_swinging_lantern_glass_off
{
	surfaceparm nolightmap
	sort 12
	{
		map textures/opm/lamps/swinging_lantern_glass.tga
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen const 0.85 0.85 0.83
	}
	{
		map textures/opm/lamps/swinging_lantern_glass.tga
		blendFunc GL_ONE GL_ONE
		rgbGen const 0.05 0.05 0.06
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

// The swinging lantern's cut-out card through its middle (its wire handle;
// models/lights/swinging_lantern.tik's glanterncull, content/opm-lamp-states):
// glanterncull with the glass cut away cleanly (tools/lamps/lantern_cull.py)
opm_glanterncull
{
	cull none
	{
		map textures/opm/lamps/glantern_cull.tga
		rgbGen lightingspherical
		alphafunc ge128
		depthwrite
	}
}

// The swinging lantern's flame (models/fx/opm_lanternflame.tik): a square
// turned to the viewer, playing the loop of a candle flame swaying
// (tools/lamps/flame.py --square), added, brighter than white (opm_flame: it
// blooms)
opm_flame_lantern
{
	cull none
	nopicmip
	surfaceparm nolightmap
	surfaceparm nomarks
	deformVertexes autosprite
	{
		animMap 15 textures/opm/lamps/lanternflame/flame00.tga textures/opm/lamps/lanternflame/flame01.tga textures/opm/lamps/lanternflame/flame02.tga textures/opm/lamps/lanternflame/flame03.tga textures/opm/lamps/lanternflame/flame04.tga textures/opm/lamps/lanternflame/flame05.tga textures/opm/lamps/lanternflame/flame06.tga textures/opm/lamps/lanternflame/flame07.tga textures/opm/lamps/lanternflame/flame08.tga textures/opm/lamps/lanternflame/flame09.tga textures/opm/lamps/lanternflame/flame10.tga textures/opm/lamps/lanternflame/flame11.tga textures/opm/lamps/lanternflame/flame12.tga textures/opm/lamps/lanternflame/flame13.tga textures/opm/lamps/lanternflame/flame14.tga textures/opm/lamps/lanternflame/flame15.tga textures/opm/lamps/lanternflame/flame16.tga textures/opm/lamps/lanternflame/flame17.tga textures/opm/lamps/lanternflame/flame18.tga textures/opm/lamps/lanternflame/flame19.tga textures/opm/lamps/lanternflame/flame20.tga textures/opm/lamps/lanternflame/flame21.tga textures/opm/lamps/lanternflame/flame22.tga textures/opm/lamps/lanternflame/flame23.tga textures/opm/lamps/lanternflame/flame24.tga textures/opm/lamps/lanternflame/flame25.tga textures/opm/lamps/lanternflame/flame26.tga textures/opm/lamps/lanternflame/flame27.tga textures/opm/lamps/lanternflame/flame28.tga textures/opm/lamps/lanternflame/flame29.tga textures/opm/lamps/lanternflame/flame30.tga textures/opm/lamps/lanternflame/flame31.tga textures/opm/lamps/lanternflame/flame32.tga textures/opm/lamps/lanternflame/flame33.tga textures/opm/lamps/lanternflame/flame34.tga textures/opm/lamps/lanternflame/flame35.tga textures/opm/lamps/lanternflame/flame36.tga textures/opm/lamps/lanternflame/flame37.tga textures/opm/lamps/lanternflame/flame38.tga textures/opm/lamps/lanternflame/flame39.tga
		blendFunc GL_ONE GL_ONE
	}
}
