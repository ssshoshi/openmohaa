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

// With the HRRTM Blood Effects addon installed, its two blood splats are used
// as wounds too (variants 5 and 6). They are the addon's textures, not ours:
// the cgame uses these only when textures/effects/blood_splat.tga exists.

gore/wound_entry5
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/effects/blood_splat.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_entry6
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/effects/blood_splat2.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_exit5
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/effects/blood_splat.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_exit6
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/effects/blood_splat2.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_frag5
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/effects/blood_splat.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/wound_frag6
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/effects/blood_splat2.tga
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

gore/stump
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/stump.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/brain
{
	polygonOffset
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/brain.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/chunk
{
	cull none
	nopicmip
	{
		clampmap textures/opm_gore/chunk.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

// The bits a round breaks out of a head (CG_GoreSpawnBits): drawn over the
// game's small debris models in place of their own shader
gore/bit_bone
{
	{
		map textures/opm_gore/bit_bone.tga
		rgbGen lightingSpherical
	}
}

gore/bit_flesh
{
	{
		map textures/opm_gore/bit_flesh.tga
		rgbGen lightingSpherical
	}
}

gore/bit_brain
{
	{
		map textures/opm_gore/bit_brain.tga
		rgbGen lightingSpherical
	}
}

// The stain under a bit where it lies (CG_GoreUpdateBit): the HRRTM blood
// effects' splats, so the cgame uses these only when they are there, and
// gore/splat otherwise
gore/stain1
{
	polygonOffset
	nopicmip
	{
		clampmap textures/effects/blood_splat.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

gore/stain2
{
	polygonOffset
	nopicmip
	{
		clampmap textures/effects/blood_splat2.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}
