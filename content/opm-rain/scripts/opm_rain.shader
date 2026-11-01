// Added in OPM: the water pouring off the eaves of roofs in the rain, and the
// spray where it lands (tr_water.c, R_RainRunoff). The renderer gives each
// polygon its brightness and fade in its vertex colours. The veil and the
// streams are our own (tools/content/rain_textures.py); the splash and the
// mist are the game's water splashes.

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

opm/rain_splash
{
	nopicmip
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	cull none
	{
		clampmap textures/effects/water_splash2.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

// the mist thrown up where it lands
opm/rain_mist
{
	nopicmip
	surfaceparm nolightmap
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm nomarks
	cull none
	{
		clampmap textures/effects/water_splash.tga
		blendfunc blend
		rgbGen vertex
		alphaGen vertex
	}
}
