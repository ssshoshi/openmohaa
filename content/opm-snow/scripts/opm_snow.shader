// The snow's own shaders (cg_snow.cpp). Textures drawn by
// tools/content/snow_textures.py.

// A boot's print in the snow: laid on the ground as a mark, lit by the cgame
// through the vertex colour, fading out inside its border.
opm/snow_footprint
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_snow/footprint.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}
