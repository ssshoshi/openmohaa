// The vehicles' glass, a surface a pane (tools/vehicles/vehicle_glass.py cuts
// each model's glass up and gives each pane three skins in its .tik files,
// content/opm-lamp-states: whole, opm_glass_cracked, opm_glass_heavy), which
// bullets crack, craze and bring down, and blasts bring down (fgame g_glass.cpp).
// Each darkens what is seen through it by blending over it (not a multiply:
// the light in the air before it, a headlight's beam in the haze, was darkened
// with it, and the pane showed as a dark slab in the beam).
// The Opel's three panes are drawn whole with these: The game's glass was its smudge
// texture lit grey and half opaque: a pale sheet, brightest at night.

// Whole: tinted, darkening what is seen through it, the smudges and the wipers'
// arcs on it (the game's opelwindows) catching a little light. A model that
// names no shader for a pane draws it with the shader of the pane's name.
opm_opelglass_front
{
	cull none
	surfaceparm nolightmap
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const 0.050 0.055 0.060
		alphaGen const 0.54
	}
	{
		map textures/models/vehicles/OpelTruck/opelwindows.tga
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen const 0.1 0.11 0.12
	}
}

opm_opelglass_left
{
	cull none
	surfaceparm nolightmap
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const 0.050 0.055 0.060
		alphaGen const 0.54
	}
	{
		map textures/models/vehicles/OpelTruck/opelwindows.tga
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen const 0.1 0.11 0.12
	}
}

opm_opelglass_right
{
	cull none
	surfaceparm nolightmap
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const 0.050 0.055 0.060
		alphaGen const 0.54
	}
	{
		map textures/models/vehicles/OpelTruck/opelwindows.tga
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen const 0.1 0.11 0.12
	}
}

// A static model's own glass (models/static/, placed in a map and drawn by the
// renderer alone, which nothing can change): not drawn. The copy of it the game
// puts there draws its glass (its skin1 and on)
opm_glass_none
{
	cull none
	surfaceparm nolightmap
	surfaceparm nomarks
	{
		map $whiteimage
		blendFunc GL_ZERO GL_ONE
	}
}
// Any vehicle's pane hit twice: cracked across, the cracks (the game's
// opelwindows_d2) white in the light
opm_glass_cracked
{
	cull none
	surfaceparm nolightmap
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const 0.050 0.055 0.060
		alphaGen const 0.54
	}
	{
		map textures/models/vehicles/OpelTruck/opelwindows_d2.tga
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen const 0.14 0.145 0.15
	}
}

// Hit three times and more: crazed through, the cracks bright and the glass
// gone milky and dark, more and more of it not seen through
opm_glass_heavy
{
	cull none
	surfaceparm nolightmap
	{
		map $whiteimage
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen const 0.026 0.029 0.031
		alphaGen const 0.76
	}
	{
		map textures/models/vehicles/OpelTruck/opelwindows_d2.tga
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen const 0.24 0.25 0.26
	}
	{
		map $whiteimage
		blendFunc GL_ONE GL_ONE
		rgbGen const 0.035 0.037 0.04
	}
}

// Where a round went through (models/fx/opm_glasscrack0-2.tik, g_glass.cpp):
// a clean hole and a star of cracks with a spiderweb round it
// (tools/vehicles/glass_crack.py), white where the cracks catch the light
opm_glasscrack0
{
	cull none
	nopicmip
	surfaceparm nolightmap
	surfaceparm nomarks
	{
		clampmap textures/opm/vehicles/glasscrack0.tga
		blendFunc blend
		rgbGen const 0.26 0.27 0.28
	}
}

opm_glasscrack1
{
	cull none
	nopicmip
	surfaceparm nolightmap
	surfaceparm nomarks
	{
		clampmap textures/opm/vehicles/glasscrack1.tga
		blendFunc blend
		rgbGen const 0.26 0.27 0.28
	}
}

opm_glasscrack2
{
	cull none
	nopicmip
	surfaceparm nolightmap
	surfaceparm nomarks
	{
		clampmap textures/opm/vehicles/glasscrack2.tga
		blendFunc blend
		rgbGen const 0.26 0.27 0.28
	}
}

// The shards of a pane brought down (models/fx/opm_glass_collapse.tik's
// sprite, tools/vehicles/glass_crack.py): slivers of clear glass. They take
// the light where they are (a colliding effect's colour is the light there),
// and a faint cool glint, so they never go quite black at night
opm_glassshard
{
	cull none
	nopicmip
	spritegen oriented
	{
		map textures/opm/vehicles/glassshard.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
	{
		map textures/opm/vehicles/glassshard.tga
		blendFunc GL_SRC_ALPHA GL_ONE
		rgbGen const 0.05 0.06 0.065
		alphaGen vertex
	}
}
