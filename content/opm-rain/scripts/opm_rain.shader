// Added in OPM: the water pouring off the eaves of roofs in the rain and the
// splash where it lands (tr_water.c, R_RainRunoff). The renderer gives each
// polygon its brightness and fade in its vertex colours; the stream is our own
// (tools/content/rain_textures.py), the splash the game's.

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
