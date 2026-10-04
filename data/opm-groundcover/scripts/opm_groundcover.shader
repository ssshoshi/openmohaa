// The tufts of grass the GL2 renderer stands on grass ground (r_groundCover,
// code/renderergl2/tr_groundcover.c). The texture is drawn by
// tools/groundcover/grass.py: the game's own weeds texture is a solid mass at
// the bottom, which stood up reads as a slab. The colour is the ground's light
// where each tuft stands (rgbGen exactVertex);
// the renderer sways and fades the tufts itself, so no deformVertexes.
groundcover/grass
{
	surfaceparm nonsolid
	surfaceparm nomarks
	surfaceparm alphashadow
	cull none
	nopicmip
	{
		map textures/opm/groundcover_grass.tga
		alphaFunc GE128
		depthWrite
		rgbGen exactVertex
	}
}
