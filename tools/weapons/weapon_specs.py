"""What remodel.py makes of each source: one dict a weapon.

    key          our name (opm_<key>.skd, textures/models/weapons/opm_<key>/)
    folder       under <MOHAA_GAME>/models/
    source       file patterns in the folder; "archive!member" for a file in a .zip or .7z
    piece_re, material_re, pieces
                 the source's meshes to keep: by name (a regex), by material (a regex),
                 or a list of names
    maps         {mesh name, material name or "*": {base|normal|rough|metal|ao: file in the folder}} where
                 the source does not carry its maps
    material_of  {mesh name: material key}: meshes sharing one texture set
    tik          the game's .tik; more_tiks: others on the same model
    place        {mesh name: the game's surface, or a box}: a mesh the source left at its own
                 origin, fitted (at the gun's scale) onto that surface or what is in the box
    fit_scale    "length": the scale from the two models' lengths, the fit then rigid (where
                 a free scale settles too small)
    aim          the arms' aim pose (models/human/animation/viewmodel/): our sights are put on
                 the line from its eye to the game gun's front sight (the edge of the
                 gun's upper outline that ends on its front sight; rear_t: where the rear
                 sight is, cm from the eye, where the outline misses it)
    nudge        a move (cm, the game model's space) after the fit
    carve        {bone: box}: where the source welded a moving part into the rest, its
                 triangles in the box whose nearest of the game's vertices is on the bone
    confine      {bone: box}: that bone's parts are only those centred in the box (the rest go
                 on the main bone), and every part wholly in it goes on it
    overrides    [(box in the game's model space, bone, surface)]: pieces wholly in the box
    keep_box     a box in the source's space: its connected parts centred in it are kept
    hulls        the game's surfaces (a magazine) that take our parts lying mostly inside them
    assign       {mesh name regex: (bone, surface)}: whole meshes, over the nearest-part vote
    ammo         {the hand's ammo .tik: the gun's surface that is the magazine}
    prep, after_fit, tik_edit
                 hooks: f(pieces, stock) -> pieces before and after the fit;
                 f(text, tik path) -> text
"""

WEAPONS = [
    dict(key="colt45", folder="M1911", source=["models/M1911.fbx"],
         piece_re=r".*\.001",
         assign={r"(magazine|follower)_low.*": ("clip", "clip"),
                 r"(slide|sight.*|extractor|pin_firing|firing_pin_stop|plunger_recoil_sprin|barrel|bushing_barrel)_low.*":
                     ("chamber", "colt45")},
         maps=None,
         tik="models/weapons/colt45.tik", ammo={"models/ammo/colt_clip.tik": "clip"}),
    dict(key="p38", folder="Walther P38", source=["source/*.fbx"],
         material_re="MA_1_WaltherP38",
         keep_box=((-9, 9), (-9, 9), (-9, -0.5)),   # of two pistols, the one with its magazine in
         maps={"*": {"base": "textures/MA_1_WaltherP38_Base_Color.jpg",
                     "normal": "textures/MA_1_WaltherP38_Normal_DirectX.jpg",
                     "rough": "textures/MA_1_WaltherP38_Roughness.jpg",
                     "metal": "textures/MA_1_WaltherP38_Metallic.jpg"}},
         hulls=["clip"],
         tik="models/weapons/p38.tik", ammo={"models/ammo/p38_clip.tik": "clip"}),
    dict(key="hspistol", folder="High Standard HDM Silenced", source=["source/textures.zip!HDM.fbx"],
         maps={"*": {"base": "textures/DefaultMaterial_Base_Color.png",
                     "normal": "textures/DefaultMaterial_Normal_DirectX.png",
                     "rough": "textures/DefaultMaterial_Roughness.png",
                     "metal": "textures/DefaultMaterial_Metallic.png",
                     "ao": "textures/DefaultMaterial_Mixed_AO.png"}},
         tik="models/weapons/silencedpistol.tik", ammo={"models/ammo/silencedpistol_clip.tik": "clip"}),
]


def _set(prefix, base="BaseColor", normal="Normal", rough="Roughness", metal="Metallic",
         ao="Ambient_occulsion"):
    """A texture set named <prefix><map>.png."""
    out = {}
    for k, v in (("base", base), ("normal", normal), ("rough", rough), ("metal", metal), ("ao", ao)):
        if v:
            out[k] = prefix + v + ".png" if "." not in v else prefix + v
    return out


# In the game models' space (cm; along the barrel, across, and down: -Z is up)
KAR98_BOLT = ((-2.0, 19.5), (-4.0, 3.0), (-9.5, -1.5))          # the bolt (the chamber bone's)
# The Kar98k source's bolt is in many small pieces, some welded to the receiver: until it is
# cut out by hand, nothing goes on the chamber bone (the bolt does not move)
NOWHERE = ((0.0, 0.0), (0.0, 0.0), (0.0, 0.0))
SPRINGFIELD_SCOPE = ((1.0, 33.0), (-5.0, 1.5), (-14.0, -8.0))   # the scope, above the bolt

WEAPONS += [
    # Notes.txt: no bipod, no carrying handle, sights and shoulder plate down. The source's
    # body (model_0) has none of the loose parts on it (bipod, handle, plate, leaf: the rest
    # are at their own origins, unplaced), its rear sight folded; it has no magazine, so the
    # game's stays (bar4, and the hand's models/ammo/bar_clip.tik)
    dict(key="bar", folder="BAR", source=["model_*.obj"], pieces=["model_0"],
         maps={"*": _set("M1918_", "albedo", "Normal", "roughness", "metallic", "ao")},
         keep_stock=["bar4"], fit_without=["bar4"],
         tik="models/weapons/bar.tik"),
    # Notes.txt: the Kar98k without its scope, the sniper's with it (the ZF scope)
    dict(key="kar98", folder="Kar98k", source=["model_*.obj"], pieces=["model_1"],
         maps={"model_1": _set("Kar98k-rifle_"), "model_5": _set("Kar98k-scope_")},
         confine={"chamber": NOWHERE}, aim="kar98_stand_aim.skc", rear_t=(24.0, 34.0),
         tik="models/weapons/kar98.tik"),
    dict(key="kar98sniper", folder="Kar98k", source=["model_*.obj"], pieces=["model_1", "model_5"],
         maps={"model_1": _set("Kar98k-rifle_"), "model_5": _set("Kar98k-scope_")},
         place={"model_5": "scope"}, fit_without=["scope", "lens"], keep_stock=["lens"],
         assign={"model_5": ("sniper", "scope")},
         confine={"chamber": NOWHERE},
         tik="models/weapons/KAR98sniper.tik"),
    # Notes.txt: the one with the shorter scope (model_4; model_3 is the long one, beside the
    # display's loose parts)
    dict(key="springfield", folder="Springfield", source=["model_*.obj"], pieces=["model_1", "model_4"],
         maps={"model_1": _set("springfield-1903_"), "model_4": _set("springfield-scope_")},
         place={"model_4": SPRINGFIELD_SCOPE}, fit_without=[SPRINGFIELD_SCOPE, "lens"], keep_stock=["lens"],
         assign={"model_4": ("springfield01", "springfield1")},
         confine={"Object02": NOWHERE},   # its bolt, too, is in welded pieces
         tik="models/weapons/springfield.tik"),
    dict(key="mp40", folder="Mp40", source=["source/model.zip!model/model.dae"],
         maps={"*": {"base": "textures/GunMetal_albedo.jpeg", "normal": "textures/GunMetal_normal.jpeg",
                     "rough": "textures/GunMetal_roughness.jpeg", "metal": "textures/GunMetal_metallic.jpeg",
                     "ao": "textures/GunMetal_AO.jpeg"}},
         aim="mp40_stand_aim.skc",
         tik="models/weapons/mp40.tik"),
    dict(key="mp44", folder="STG44", source=["*.glb"],
         # the game's StG has its grip and magazine further forward than the real one: ours
         # by its grip, where the right hand holds it
         fit_without=["mp44clip"], fit_scale="length", nudge=(6.0, 0.0, 0.0), hulls=["mp44clip"],
         tik="models/weapons/mp44.tik", ammo={"models/ammo/mp44_clip.tik": "mp44clip"}),
    dict(key="shotgun", folder="Winchester M12 Trench Gun", source=["model_*.obj"],
         maps={"*": _set("winchester_m12_trench_gun_assembled_wood_", "B", "N", "R", "M",
                         None)},
         confine={"Object01": ((26.0, 47.0), (-4.5, 0.5), (-7.0, -1.0))},
         tik="models/weapons/shotgun.tik"),
    dict(key="bazooka", folder="M1 Bazooka", source=["model_*.obj"], pieces=["model_0", "model_2"],
         maps={"*": _set("M1A1_Paint_", "Albedo", "Normal", "Roughness", "Metallic", None)},
         place={"model_2": "bazookashell"}, fit_without=["bazookashell"],
         assign={"model_2": ("bazooka_bazooka", "bazookashell")},
         tik="models/weapons/bazooka.tik", ammo={"models/ammo/bazooka_shell.tik": "bazookashell"}),
    dict(key="panzerschreck", folder="Panzershreck", source=["model_*.obj"],
         pieces=["model_2"],     # model_0 and model_1 are the sling and its fittings
         maps={"*": {"base": "DEFAULT_0_0.png"}},
         keep_stock=["shell"], fit_without=["shell"],
         tik="models/weapons/panzerschreck.tik"),
    dict(key="m2frag", folder="Frag Grenade", source=["source/grenade.7z!grenade_low.fbx"],
         pieces=["grenade_low_1"],
         maps={"*": {"base": "textures/grenade_albedo.jpeg", "normal": "textures/grenade_normal.png",
                     "rough": "textures/grenade_roughness.jpeg", "metal": "textures/grenade_metallic.jpeg",
                     "ao": "textures/grenade_AO.jpeg"}},
         tex_size=1024,
         tik="models/weapons/m2frag_grenade_base.txt",
         also=[dict(key="m2frag_air", tik="models/projectiles/m2fgrenade_base.txt")]),
    dict(key="stielhandgranate", folder="Stielhandgranate", source=["source/*.fbx"],
         maps={"*": {"base": "textures/Stielhandgranate_BaseColor.png", "normal": "textures/Stielhandgranate_normal.png",
                     "rough": "textures/Stielhandgranate_Roughness.png", "metal": "textures/Stielhandgranate_Metallic.png",
                     "ao": "textures/Stielhandgranate_ao.png"}},
         tex_size=1024,
         tik="models/weapons/steilhandgranate_base.txt",
         also=[dict(key="stielhandgranate_air", tik="models/projectiles/steilhandgranate_base.txt")]),
]


def _colt_maps():
    m = {}
    for s in ("set1", "set2"):
        m[s] = {"base": "textures/m1911_%s_albedo.png" % s, "normal": "textures/m1911_%s_normal.png" % s,
                "metal": "textures/m1911_%s_metallic.png" % s, "ao": "textures/m1911_%s_AO.png" % s,
                "rough": "textures/m1911-a1_low_SP_%s_Roughness.1001.png" % s}
    return m


WEAPONS[0]["maps"] = _colt_maps()
