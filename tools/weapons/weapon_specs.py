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
    aim, sight_at
                 the arms' aim pose (models/human/animation/viewmodel/) and our rear and
                 front sights as (depth, right, up) cm in the camera's frame (measured with
                 REMODEL_NO_SIGHTS=1): both are put on the line the camera looks down
    slide        {bone, travel (cm), anims {animation: {frame: share of travel}}}: a part the
                 game's animations leave still, moved back along the barrel in copies of them
    new_bones    [(bone, parent)]: bones the game's model lacks (a slide of its own)
    shift        [{box (the fit's space, parts centred in it), forward (cm along the barrel),
                 assign (bone, surface)}]: parts the source has out of place (a slide locked
                 back), moved and put on a bone
    sight_drop   cm the rear sight goes under the aim line (the butt dropped, about the front
                 sight): the eye looking over the receiver at both sights
    aim_pose_fix the sights lined up by making over the arms' aim pose (turned about the eye,
                 the eye moved) instead of moving the weapon, which stays where the game's
                 idle and reload hold it
    level        the gun turned upright in the aim view (about the aim line)
    ammo_fit     "hand": the hand's magazine fitted onto the game's hand-held one, not moved
                 as the game's gun magazine is (ours sits elsewhere in the gun)
    two_sided    its shaders cull nothing (thin parts the animations show from behind)
    nudge        a move (cm, the game model's space) after the fit
    carve        {bone: box}: where the source welded a moving part into the rest, its
                 triangles in the box whose nearest of the game's vertices is on the bone
    confine      {bone: box}: that bone's parts are only those centred in the box (the rest go
                 on the main bone), and every part wholly in it goes on it
    overrides    [(box in the game's model space, bone, surface)]: pieces wholly in the box
    drop         boxes in the source's space: its connected parts centred in one go
    keep_box     a box in the source's space: its connected parts centred in it are kept
    hulls        the game's surfaces (a magazine) that take our parts lying mostly inside them
    assign       {mesh name regex: (bone, surface)}: whole meshes, over the nearest-part vote
    picks        [{name, mesh, box (source space), material?, scale (to the gun's units),
                 target (a surface, "bone:<name>" or a box), assign (bone, surface),
                 offset (cm, after it is placed)}]: parts
                 of a mesh (a display's loose bolt or magazine) placed onto the game's part
    ammo_picks   {the hand's ammo .tik: {mesh, box, scale}}: what it holds, from the source
    index_parts  parts are what the mesh joins, not what merely touches
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
         # the source: two pistols on a cloth, two loose magazines, rounds. The loaded one (its
         # slide forward; it has no magazine in) and one of the loose magazines, put in its grip
         material_re="MA_1_WaltherP38",
         keep_box=((-9, 9), (-9, 9), (1.0, 9)),
         drop=[((3.0, 4.0), (-1, 1), (1.0, 1.7))],   # a lever below its grip
         picks=[dict(name="magazine", mesh="Bullet_Shell_low", material="MA_1_WaltherP38",
                     box=((1.45, 1.645), (-0.05, 0.1), (0.1, 0.55)), target="clip", assign=("p38clip", "clip"),
                     offset=(0.45, 0.0, -2.0))],   # seated in our grip, shorter than the game's
         material_of={"Bullet_Shell_low": "MA_1_WaltherP38"},
         maps={"*": {"base": "textures/MA_1_WaltherP38_Base_Color.jpg",
                     "normal": "textures/MA_1_WaltherP38_Normal_DirectX.jpg",
                     "rough": "textures/MA_1_WaltherP38_Roughness.jpg",
                     "metal": "textures/MA_1_WaltherP38_Metallic.jpg"}},
         fit_without=["clip"], fit_scale="length",   # the game's grip is too tall for a free scale
         # both of the source's pistols have their slides locked back: the slide (and the
         # safety and the small parts on its back) forward into battery, on a bone
         # of its own, back and home with each shot
         new_bones=[("opm_slide", "Box01")],
         shift=[dict(box=((-5.0, 3.0), (-4.0, 1.0), (-7.0, -4.4)), forward=4.0, assign=("opm_slide", "P381"))],
         slide=dict(bone="opm_slide", travel=4.0, anims={"fire": {0: 1.0, 1: 1.0, 3: 0.0}}),
         tik="models/weapons/p38.tik", ammo={"models/ammo/p38_clip.tik": "clip"}),
    dict(key="hspistol", folder="High Standard HDM Silenced", source=["source/textures.zip!HDM.fbx"],
         maps={"*": {"base": "textures/DefaultMaterial_Base_Color.png",
                     "normal": "textures/DefaultMaterial_Normal_DirectX.png",
                     "rough": "textures/DefaultMaterial_Roughness.png",
                     "metal": "textures/DefaultMaterial_Metallic.png",
                     "ao": "textures/DefaultMaterial_Mixed_AO.png"}},
         # one mesh, its slide's pieces only touching the frame: parts by the mesh's own joins,
         # the slide (the top behind the barrel) on the chamber bone. Its magazine is only a
         # floor plate, so the hand's stays the game's
         index_parts=True, confine={"chamber": ((-3.5, 10.3), (-2.6, 0.7), (-5.8, -2.4))},
         # the game's Hi-Standard never moves it: blowback, back with the shot and home
         slide=dict(bone="chamber", travel=2.5, anims={"fire": {0: 1.0, 1: 1.0, 3: 0.0}}),
         two_sided=True,   # its magazine is a thin plate: seen from behind in the reload
         tik="models/weapons/silencedpistol.tik"),
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
# the glTF download (metres, the display's layout): its meshes by their old OBJ names
KAR98_SOURCE = dict(source=["*.glb"], rename={"rears.*ght-button": "model_0", "rifle_": "model_1",
                                              "scope-body": "model_5"})
KAR98_MATS = {"model_0": "rifle", "model_1": "rifle"}
# its renders' metal is near black (a PBR metal takes no diffuse light): ours keeps a part
KAR98_MAPS = {"*": {"metal_diffuse": 0.4}}
# in model_0 (the display's copy): the bolt, its firing pin, shroud and cocking piece (not
# the extractor lying beside it); the stripper clip with its rounds
KAR98_BOLT_PICK = dict(name="bolt", mesh="model_0", box=((-0.06, 0.025), (0.085, 0.16), (-0.34, -0.10)),
                       target="bone:chamber", assign=("chamber", "KAR981"))
KAR98_CLIP_PICK = dict(mesh="model_0", box=((0.08, 0.18), (-0.01, 0.03), (-0.28, -0.15)))
SPRINGFIELD_SCOPE = ((1.0, 33.0), (-5.0, 1.5), (-14.0, -8.0))   # the scope, above the bolt

WEAPONS += [
    # Notes.txt: no bipod, no carrying handle, sights and shoulder plate down. The source's
    # body (model_0) has none of the loose parts on it (bipod, handle, plate, leaf: the rest
    # are at their own origins, unplaced), its rear sight folded; its magazine (model_17) is
    # one of the loose parts, put where the game's is
    # The rear sight (model_1, the leaf with its pin and windage knob, model_3 its aperture
    # slide) is loose too, folded down at its own origin: put on the receiver's sight base and
    # raised, the aperture on the aim line
    dict(key="bar", folder="BAR", source=["model_*.obj"], pieces=["model_0", "model_17", "model_1", "model_3"],
         maps={"*": _set("M1918_", "albedo", "Normal", "roughness", "metallic", "ao")},
         material_of={"model_0": "bar", "model_17": "bar", "model_1": "bar", "model_3": "bar"},
         prep=lambda pieces, stock: _bar_rear_sight(pieces),
         aim="bar_stand_aim.skc", sight_at=((8.0, -0.4, -0.7), (85.0, 0.0, -0.4)),
         place={"model_17": "bar4"}, fit_without=["bar4"], assign={"model_17": ("Box01", "bar4"),
                                                   "model_1": ("Box01", "bar3"), "model_3": ("Box01", "bar3")},
         tik="models/weapons/bar.tik", ammo={"models/ammo/bar_clip.tik": "bar4"}),
    # Notes.txt: the Kar98k without its scope, the sniper's with it (the ZF scope)
    # The assembled rifle (model_1, in metres) has no bolt; the field-stripped one beside it
    # (model_0, in cm) has, and a stripper clip of five rounds: the bolt goes on the chamber
    # bone where the game's is, the clip is the reload's
    dict(key="kar98", folder="Kar98k", **KAR98_SOURCE, pieces=["model_1"], material_of=KAR98_MATS, maps=KAR98_MAPS,
         picks=[KAR98_BOLT_PICK], confine={"chamber": NOWHERE},
         aim="kar98_stand_aim.skc", sight_at=((28.0, 0.0, -0.15), (90.0, -0.3, 0.05)),
         sight_drop=1.2,    # the eye over the rear sight and the receiver, the front post in view
         ammo_picks={"models/ammo/kar98_clip_reload.tik": KAR98_CLIP_PICK},
         tik="models/weapons/kar98.tik"),
    dict(key="kar98sniper", folder="Kar98k", **KAR98_SOURCE, pieces=["model_1", "model_5"],
         material_of=KAR98_MATS, maps=KAR98_MAPS,
         place={"model_5": "scope"}, fit_without=["scope", "lens"],
         assign={"model_5": ("sniper", "scope")},
         picks=[KAR98_BOLT_PICK], confine={"chamber": NOWHERE},
         tik="models/weapons/KAR98sniper.tik"),
    # Notes.txt: the one with the shorter scope (model_4; model_3 is the long one, beside the
    # display's loose parts)
    # The rifle (model_1, metres) has no bolt either: the display's (model_0, cm, two of
    # them) goes on the bolt bone, and one of its cartridges is the round the hand loads
    dict(key="springfield", folder="Springfield", source=["model_*.obj"], pieces=["model_1", "model_4"],
         maps={"model_1": _set("springfield-1903_"), "model_0": _set("springfield-1903_"),
               "model_4": _set("springfield-scope_")},
         material_of={"model_0": "rifle", "model_1": "rifle"},
         place={"model_4": SPRINGFIELD_SCOPE}, fit_without=[SPRINGFIELD_SCOPE, "lens"],
         assign={"model_4": ("springfield01", "springfield1")},
         picks=[dict(name="bolt", mesh="model_0", box=((-23.5, -14.0), (4.5, 15.0), (-22.5, -2.5)),
                     scale=0.01, target="bone:Object02", assign=("Object02", "springfield1"))],
         confine={"Object02": NOWHERE},
         ammo_picks={"models/ammo/springfield_clip_reload.tik":
                     dict(mesh="model_0", box=((-16.0, -7.5), (-1.0, 1.0), (-22.5, -17.5)), scale=0.01)},
         tik="models/weapons/springfield.tik"),
    dict(key="mp40", folder="Mp40", source=["source/model.zip!model/model.dae"],
         maps={"*": {"base": "textures/GunMetal_albedo.jpeg", "normal": "textures/GunMetal_normal.jpeg",
                     "rough": "textures/GunMetal_roughness.jpeg", "metal": "textures/GunMetal_metallic.jpeg",
                     "ao": "textures/GunMetal_AO.jpeg"}},
         aim="mp40_stand_aim.skc", sight_at=((26.0, 0.05, -0.6), (65.0, -0.3, -0.35)),
         tik="models/weapons/mp40.tik"),
    dict(key="mp44", folder="STG44", source=["*.glb"],
         # the game's StG has its grip and magazine further forward than the real one: ours
         # by its grip, where the right hand holds it
         fit_without=["mp44clip"], fit_scale="length", nudge=(6.0, 0.0, 0.0), hulls=["mp44clip"],
         level=True, ammo_fit="hand",
         # its charging handle (on the left) on the game's cock bone, nothing else
         confine={"Box02": ((6.9, 11.8), (-4.9, -1.2), (-9.0, -6.8))},
         aim="mp44_idle_stand_aim.skc", sight_at=((26.0, -0.2, -2.3), (67.0, -0.3, -2.25)),
         tik="models/weapons/mp44.tik", ammo={"models/ammo/mp44_clip.tik": "mp44clip"}),
    dict(key="shotgun", folder="Winchester M12 Trench Gun", source=["*.glb"],
         # the glTF download: its two materials (wood set, metal set) with every map, the
         # images in textures/ beside it
         confine={"Object01": ((26.0, 47.0), (-4.5, 0.5), (-7.0, -1.0))},
         tik="models/weapons/shotgun.tik"),
    dict(key="bazooka", folder="M1 Bazooka", source=["model_*.obj"], pieces=["model_0", "model_2"],
         maps={"*": _set("M1A1_Paint_", "Albedo", "Normal", "Roughness", "Metallic", None)},
         place={"model_2": "bazookashell"}, fit_without=["bazookashell"],
         assign={"model_2": ("bazooka_bazooka", "bazookashell")},
         # the eyehole, and the middle of the front sight frame
         aim="pose_bazooka_aim.skc", aim_pose_fix=True, sight_at=((4.5, 4.7, 0.9), (54.0, 0.5, -3.55)),
         tik="models/weapons/bazooka.tik", ammo={"models/ammo/bazooka_shell.tik": "bazookashell"}),
    dict(key="panzerschreck", folder="Panzershreck", source=["model_*.obj"],
         pieces=["model_2"],     # model_0 and model_1 are the sling and its fittings
         maps={"*": {"base": "DEFAULT_0_0.png"}},
         keep_stock=["shell"], fit_without=["shell"],
         # the rear sight's notch, the front sight's post (the shield's window between)
         aim="pose_panzershrek_aim.skc", aim_pose_fix=True, sight_at=((50.0, 2.65, -0.7), (88.0, -1.45, 1.2)),
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


# The BAR's sight base, in the source's space (cm, y up, the muzzle toward -z): the block on
# the receiver's top from z 22 to 30, its top at y 8.49. The leaf's pin is the rear sight
# model's origin; folded, the leaf lies forward of it, 1.32 thick
BAR_SIGHT_PIN = (0.0, 8.49 + 0.66, 29.5)
BAR_APERTURE_UP = 1.2     # the slide raised this far up the leaf, clear of the base
BAR_LEAF_ACROSS = 2.7     # the leaf's frame onto the gun's middle, round the slide


def _bar_rear_sight(pieces):
    import numpy as np
    up = np.eye(4)
    up[:3, :3] = [[1, 0, 0], [0, 0, -1], [0, 1, 0]]     # the leaf raised: its forward (-z) to up
    pin = np.eye(4)
    pin[:3, 3] = BAR_SIGHT_PIN
    out = []
    for p in pieces:
        if p.name == "model_1":      # its frame is 2.7 cm to the side of its pin's origin
            across = np.eye(4)
            across[0, 3] = BAR_LEAF_ACROSS
            p = p.moved(across @ pin @ up)
        elif p.name == "model_3":
            lift = np.eye(4)
            lift[2, 3] = -BAR_APERTURE_UP          # along the folded leaf, before it is raised
            p = p.moved(pin @ up @ lift)
        out.append(p)
    return out


def _colt_maps():
    m = {}
    for s in ("set1", "set2"):
        m[s] = {"base": "textures/m1911_%s_albedo.png" % s, "normal": "textures/m1911_%s_normal.png" % s,
                "metal": "textures/m1911_%s_metallic.png" % s, "ao": "textures/m1911_%s_AO.png" % s,
                "rough": "textures/m1911-a1_low_SP_%s_Roughness.1001.png" % s}
    return m


WEAPONS[0]["maps"] = _colt_maps()
