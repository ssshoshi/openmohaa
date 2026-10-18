// The lamps' glass, lit from inside: made by tools/lamps/remodel.py shaders
// from lights/lamps.txt. The remodelled lamps (remodel.py) name a surface
// opm_<lamp>_glass, which takes the shader of its name.

opm_ceilinglamp1_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/ceilinglamp1.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/ceilinglamp1.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_chandalier_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/chandalier.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/chandalier.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_fattablelamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/fattablelamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/fattablelamp.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_glasslamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/glasslamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/glasslamp.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_hang3lamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/hang3lamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/hang3lamp.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_hang5lamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/hang5lamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/hang5lamp.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_hanglamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/hanglamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/hanglamp.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_tablelamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/tablelamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/tablelamp.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_tiffanylamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/tiffanyshade.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/tiffanyshade.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.34 0.15
	}
}

opm_wallsconce_cutglass_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/wallsconce-cutglass.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/wallsconce-cutglass.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_wallsconce_frosted_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/wallsconce-frosted.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/wallsconce-frosted.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_wallsconce_single_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/wallsconce-single.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/wallsconce-single.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightbulb_covered_nowire_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/bulbcover_one.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/bulbcover_one.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightbulb_covered_on_wire_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/bulbcover.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/bulbcover.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightbulb_covered_on_wire_short_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/bulbcover.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/bulbcover.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightpost_globe_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/globe.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/globe.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightpost_globe_winter_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/globe_winter.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/globe_winter.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightpost_sidemounted_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/hanger.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/hanger.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightpost_sidemounted_winter_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/hanger_snow.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/hanger_snow.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightpost_triple_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/support.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/support.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightpost_tripler_winter_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/support_snow.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/support_snow.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_static_domelight_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/bunker/lightbulb.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/bunker/lightbulb.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightbulb_desertcage_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/desertlamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/desertlamp.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.55 0.44 0.30
	}
}

opm_lightbulb_caged_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/cagelight.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_lightbulb_caged_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/cagelight.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_static_cagelight_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/bunker/fixture2.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_lightbulb_caged_red_lit_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/cagelight-red-lit.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.12 0.06
	}
}

opm_lightbulb_nowire_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/bulbcover_two.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_lightbulb_on_wire_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/bulb.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_lightbulb_on_wire_s_ns_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/bulb.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_lightbulb_on_wire_short_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/bulb.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_worklamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/items/worklamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_static_6lamp_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/submodels/6lamp.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_static_e_light_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/submodels/e-light.tga
		alphaFunc GE128
		depthWrite
		rgbGen const 1.00 0.92 0.75
	}
}

opm_wallsconce_candle_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/wallsconce-candle.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/wallsconce-candle.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.60 0.42 0.22
	}
}

opm_swinging_lantern_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/glantern.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/glantern.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.60 0.42 0.22
	}
}

opm_german_lantern_glass
{
	cull none
	surfaceparm nolightmap
	{
		map textures/models/lights/glantern.tga
		alphaFunc GE128
		depthWrite
		rgbGen static
	}
	{
		map textures/models/lights/glantern.tga
		blendFunc GL_ONE GL_ONE
		alphaFunc GE128
		rgbGen const 0.60 0.42 0.22
	}
}
