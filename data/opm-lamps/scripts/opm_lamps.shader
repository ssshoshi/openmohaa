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

// A candle's or a lantern's flame, as remodelled before opm_candleflame: a
// teardrop, steady (a flicker on it read as a bulb blinking)
opm_flame
{
	cull none
	surfaceparm nolightmap
	{
		map $whiteimage
		blendFunc GL_ONE GL_ONE
		rgbGen const 0.95 0.72 0.4
	}
}

// A candle's or a lantern's flame (tools/lamps/remodel_blender.py): a quad
// standing up that turns round its upright to the viewer, playing the loop of
// a flame swaying, drawn by tools/lamps/flame.py. Its light flickers with it
// (tr_rtlight.c, R_RtFlicker).
opm_candleflame
{
	cull none
	surfaceparm nolightmap
	surfaceparm nomarks
	deformVertexes autosprite2
	{
		animMap 15 textures/opm/lamps/flame/flame00.tga textures/opm/lamps/flame/flame01.tga textures/opm/lamps/flame/flame02.tga textures/opm/lamps/flame/flame03.tga textures/opm/lamps/flame/flame04.tga textures/opm/lamps/flame/flame05.tga textures/opm/lamps/flame/flame06.tga textures/opm/lamps/flame/flame07.tga textures/opm/lamps/flame/flame08.tga textures/opm/lamps/flame/flame09.tga textures/opm/lamps/flame/flame10.tga textures/opm/lamps/flame/flame11.tga textures/opm/lamps/flame/flame12.tga textures/opm/lamps/flame/flame13.tga textures/opm/lamps/flame/flame14.tga textures/opm/lamps/flame/flame15.tga textures/opm/lamps/flame/flame16.tga textures/opm/lamps/flame/flame17.tga textures/opm/lamps/flame/flame18.tga textures/opm/lamps/flame/flame19.tga textures/opm/lamps/flame/flame20.tga textures/opm/lamps/flame/flame21.tga textures/opm/lamps/flame/flame22.tga textures/opm/lamps/flame/flame23.tga textures/opm/lamps/flame/flame24.tga textures/opm/lamps/flame/flame25.tga textures/opm/lamps/flame/flame26.tga textures/opm/lamps/flame/flame27.tga textures/opm/lamps/flame/flame28.tga textures/opm/lamps/flame/flame29.tga textures/opm/lamps/flame/flame30.tga textures/opm/lamps/flame/flame31.tga textures/opm/lamps/flame/flame32.tga textures/opm/lamps/flame/flame33.tga textures/opm/lamps/flame/flame34.tga textures/opm/lamps/flame/flame35.tga textures/opm/lamps/flame/flame36.tga textures/opm/lamps/flame/flame37.tga textures/opm/lamps/flame/flame38.tga textures/opm/lamps/flame/flame39.tga
		blendFunc GL_ONE GL_ONE
		rgbGen identity
	}
}

// The desert lantern's frame (lightbulb_desertcage): the game's shader drew it
// a second time whole, unlit (rgbGen identity), so that it glowed from outside
// as much as in; lit only as the world lights it now, its candle's light in it
static_desertlamp
{
	qer_editorimage textures/models/lights/desertlamp.tga
	cull none
	{
		map textures/models/lights/desertlamp.tga
		rgbGen vertex
		alphafunc ge128
		depthwrite
	}
}
