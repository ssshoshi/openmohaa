// OPM (content/opm-water): the water the splashes and blasts throw up
// (models/fx/opm_splash_*.tik, bh_water_*.tik, grenexp_water.tik), as the
// retail sprites of the same textures but keeping their own colour. Spray is
// white with the daylight in it: with all of the light live
// (r_realtimeLighting 2) a sprite's colour gives way to the ambient and the
// live light at it, and a sprite over water took the shadow of the bottom
// behind it (water is not in the depth prepass), so a plume came up black.
// rgbGen identity is a stage the realtime lights leave alone (R_RtStageKind).

opm_waterplume
{
	cull none
	spriteGen parallel_oriented
	{
		clampmap textures/effects/h2o_hit.tga
		blendfunc blend
		alphaGen vertex
		rgbGen identity
	}
}

opm_water_g
{
	cull none
	spriteGen parallel_oriented
	{
		clampmap textures/sprites/water_g.tga
		blendfunc blend
		alphaGen vertex
		rgbGen identity
	}
}

opm_oceanspray
{
	cull none
	spriteGen parallel_oriented
	{
		clampmap textures/effects/oceanspray.tga
		blendfunc blend
		alphaGen vertex
		rgbGen identity
	}
}

opm_ocean_plume
{
	cull none
	spriteGen parallel_upright
	{
		animmap 10 textures/sprites/waterplume1.tga textures/sprites/waterplume2.tga textures/sprites/waterplume3.tga textures/sprites/waterplume4.tga textures/sprites/waterplume5.tga textures/sprites/waterplume6.tga
		blendfunc blend
		alphaGen vertex
		rgbGen identity
	}
}
