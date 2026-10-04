// The gore system's own shaders (cg_gore.cpp). Textures drawn by
// tools/content/gore_textures.py. Wounds and drops are coloured and lit by the
// cgame through the vertex colour; every map is clamped and fades out inside
// its border, since a wound is laid over whole triangles reaching past it.

gore/wound_entry1
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_entry1.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_entry2
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_entry2.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_entry3
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_entry3.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_entry4
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_entry4.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_exit1
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_exit1.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_exit2
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_exit2.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_exit3
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_exit3.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_exit4
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_exit4.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_frag1
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_frag1.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_frag2
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_frag2.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_frag3
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_frag3.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_frag4
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/wound_frag4.tga
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
