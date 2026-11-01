// Added in OPM: the water pouring off the eaves of roofs in the rain, and the
// spray where it lands (tr_water.c, R_RainRunoff). The renderer gives each
// polygon its brightness and fade in its vertex colours. All the images are
// our own (tools/content/rain_textures.py).

// the veil off an edge: its streaks and drops twice over, at two sizes and
// speeds, so it never looks one sheet
opm/rain_curtain
{
	nopicmip
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	cull none
	{
		map textures/opm_rain/curtain.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
		tcMod scroll 0 -2.8
	}
	{
		map textures/opm_rain/curtain.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
		tcMod scale 1.6 0.7
		tcMod scroll 0.13 -2.1
	}
}

// a heavier stream in it
opm/rain_stream
{
	nopicmip
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	cull none
	{
		map textures/opm_rain/stream.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
		tcMod scroll 0 -1.6
	}
}

// the splash where it lands (R_RainSplash): the crown of spray, the drops
// thrown off it, the mist rising
opm/rain_crown
{
	nopicmip
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	cull none
	{
		clampmap textures/opm_rain/crown.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

opm/rain_drop
{
	nopicmip
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	cull none
	{
		clampmap textures/opm_rain/drop.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

opm/rain_mist
{
	nopicmip
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	cull none
	{
		clampmap textures/opm_rain/mist.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
	}
}
