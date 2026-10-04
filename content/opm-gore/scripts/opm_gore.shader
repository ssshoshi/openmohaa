// The gore system's own shaders (cg_gore.cpp). Textures drawn by
// tools/content/gore_textures.py. Wounds and drops are coloured and lit by the
// cgame through the vertex colour; every map is clamped and fades out inside
// its border, since a wound is laid over whole triangles reaching past it.

gore/wound_entry
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_entry.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_exit
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_exit.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_frag
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_frag.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_run
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_run.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/burn
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/burn.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/drop
{
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/drop.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/splat
{
	polygonOffset
	nopicmip
	{
		clampmap textures/opm_gore/splat.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/pool
{
	polygonOffset
	nopicmip
	{
		clampmap textures/opm_gore/pool.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}
