// A hot MG42 barrel's glow: the MG42 drawn again over itself in added light
// (cgame, cg_mg42.cpp), the colour of the heat (rgbGen entity), through a mask
// of where the barrel is on its texture (tools/mg42/heatmask.py).
opm_mg42_glow
{
	nopicmip
	cull none
	polygonOffset
	{
		map textures/opm/mg42_heatmask.tga
		blendFunc GL_ONE GL_ONE
		rgbGen entity
	}
	// twice: glowing steel is brighter than the dark metal it is added to
	{
		map textures/opm/mg42_heatmask.tga
		blendFunc GL_ONE GL_ONE
		rgbGen entity
	}
}
