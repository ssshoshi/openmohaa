// The addon's two wall splats (blood_splat, blood_splat2) on the brighter
// copies of their textures that blood.derive makes. This pak loads after the
// addon's, so these win.

blood_splat
{
	polygonOffset
	{
		map textures/opm_hrrtm/blood_splat.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

blood_splat2
{
	polygonOffset
	{
		map textures/opm_hrrtm/blood_splat2.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}
