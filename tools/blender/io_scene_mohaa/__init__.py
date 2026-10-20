"""Medal of Honor: Allied Assault models in Blender: import and export .tik, .skd/.skb and .skc."""

bl_info = {
    "name": "MOHAA models (.tik, .skd, .skc)",
    "author": "OpenMoHAA fork contributors",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "File > Import / Export",
    "description": "Import and export Medal of Honor: Allied Assault skeletal models, animations and textures",
    "category": "Import-Export",
}

if "bpy" in locals():
    import importlib
    from . import anim, materials, importer, exporter  # noqa: F401
    for _m in (anim, materials, importer, exporter):
        importlib.reload(_m)

import os

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import exporter, importer

GUESSES = [
    r"C:\Program Files (x86)\EA Games\MOHAA\main",
    r"C:\Program Files\EA Games\MOHAA\main",
    r"C:\GOG Games\Medal of Honor - Allied Assault War Chest\main",
    r"D:\Medal of Honor\main",
    os.path.expanduser("~/.local/share/openmohaa/main"),
]


def _prefs(context):
    addon = context.preferences.addons.get(__package__)
    return addon.preferences if addon else None


def game_folders(context):
    """The game data folders from the add-on preferences (or a guess), main first."""
    p = _prefs(context)
    raw = p.game_folders if p else os.environ.get("MOHAA_FOLDERS", "")
    folders = [f.strip() for f in raw.split(";") if f.strip()]
    if not folders:
        folders = [g for g in GUESSES if os.path.isdir(g)][:1]
    return folders


def extract_dir(context):
    p = _prefs(context)
    if p and p.extract_dir:
        return bpy.path.abspath(p.extract_dir)
    return bpy.utils.user_resource("DATAFILES", path="mohaa_textures", create=True)


class MOHAA_Preferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    game_folders: StringProperty(
        name="Game folders",
        description="Folders holding the game's pk3 files (main, then mainta or maintt), separated by ;",
        default="",
    )
    extract_dir: StringProperty(
        name="Texture folder",
        description="Where textures taken out of pk3 files are written (empty: Blender's user data folder)",
        subtype="DIR_PATH",
        default="",
    )

    def draw(self, context):
        col = self.layout.column()
        col.prop(self, "game_folders")
        col.label(text="For example: C:\\Games\\MOHAA\\main;C:\\Games\\MOHAA\\mainta")
        col.prop(self, "extract_dir")


def _report(op, warnings):
    for w in warnings[:20]:
        op.report({"WARNING"}, w)
    if len(warnings) > 20:
        op.report({"WARNING"}, "... and %d more warnings (see the system console)" % (len(warnings) - 20))


class IMPORT_SCENE_OT_mohaa(bpy.types.Operator, ImportHelper):
    """Import a MOHAA model (.tik, .skd or .skb) with its textures and animations"""
    bl_idname = "import_scene.mohaa"
    bl_label = "Import MOHAA Model"
    bl_options = {"REGISTER", "UNDO", "PRESET"}

    filename_ext = ".tik"
    filter_glob: StringProperty(default="*.tik;*.skd;*.skb", options={"HIDDEN"})

    animations: EnumProperty(
        name="Animations",
        items=(("NONE", "None", "Rest pose only"),
               ("REFERENCE", "Reference", "Only the reference animation"),
               ("FILTER", "Matching", "Animations whose alias matches the filter"),
               ("ALL", "All", "Every animation the TIKI lists (characters list hundreds)")),
        default="REFERENCE",
    )
    anim_filter: StringProperty(name="Filter", description="Aliases to import, as wildcards separated by commas",
                                default="idle*, walk*, run*")
    reference: StringProperty(name="Reference", default="",
                              description="Alias of the animation whose first frame is the rest pose (default: idle)")
    cases: StringProperty(name="Variants", default="",
                          description="TIKI case choices, like: headmodel=head2 headskin=us_z (default: the first)")
    unit: EnumProperty(name="Units",
                       items=(("METERS", "Meters", "Scale the model to real size (16 game units to the foot)"),
                              ("GAME", "Game units", "One Blender unit per game unit")),
                       default="METERS")
    bone_axis: EnumProperty(name="Bone axis",
                            items=(("MOHAA", "Along length", "Turn bones so they point along their length"),
                                   ("KEEP", "As in file", "Keep the engine's bone axes exactly")),
                            default="MOHAA")
    load_textures: BoolProperty(name="Textures", default=True)
    pack_images: BoolProperty(name="Pack images", default=False, description="Pack textures into the .blend")
    custom_normals: BoolProperty(name="File normals", default=True, description="Use the model's own normals")

    def execute(self, context):
        folders = game_folders(context)
        try:
            arm, warnings = importer.import_model(
                context, self.filepath, folders, anim_mode=self.animations, anim_filter=self.anim_filter,
                reference=self.reference, cases=self.cases, unit=self.unit, bone_axis=self.bone_axis,
                load_textures=self.load_textures, pack_images=self.pack_images,
                extract_dir=extract_dir(context), custom_normals=self.custom_normals)
        except Exception as e:  # noqa: BLE001
            self.report({"ERROR"}, "MOHAA import failed: %s" % e)
            raise
        if not folders:
            warnings.insert(0, "no game folder set (Edit > Preferences > Add-ons > MOHAA): "
                               "textures and shared files were looked up next to the file only")
        _report(self, warnings)
        return {"FINISHED"}


class IMPORT_SCENE_OT_mohaa_skc(bpy.types.Operator, ImportHelper):
    """Add MOHAA animations (.skc) to the active armature"""
    bl_idname = "import_scene.mohaa_skc"
    bl_label = "Import MOHAA Animation"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".skc"
    filter_glob: StringProperty(default="*.skc", options={"HIDDEN"})
    files: CollectionProperty(type=bpy.types.OperatorFileListElement, options={"HIDDEN", "SKIP_SAVE"})
    directory: StringProperty(subtype="DIR_PATH", options={"HIDDEN", "SKIP_SAVE"})

    @classmethod
    def poll(cls, context):
        return context.active_object is not None and context.active_object.type == "ARMATURE"

    def execute(self, context):
        paths = [os.path.join(self.directory, f.name) for f in self.files] or [self.filepath]
        actions, warnings = importer.import_animations(context, context.active_object, paths, game_folders(context))
        _report(self, warnings)
        self.report({"INFO"}, "%d animations added" % len(actions))
        return {"FINISHED"}


class EXPORT_SCENE_OT_mohaa(bpy.types.Operator, ExportHelper):
    """Export the selected armature (or meshes) as a MOHAA model: .tik, .skd, .skc and textures"""
    bl_idname = "export_scene.mohaa"
    bl_label = "Export MOHAA Model"
    bl_options = {"REGISTER", "PRESET"}

    filename_ext = ".tik"
    filter_glob: StringProperty(default="*.tik", options={"HIDDEN"})

    animations: EnumProperty(
        name="Animations",
        items=(("ALL", "All", "Every action that moves this armature's bones"),
               ("ACTIVE", "Active", "Only the armature's current action"),
               ("NONE", "None", "Only a rest pose 'idle'")),
        default="ALL",
    )
    skd_version: EnumProperty(name="Format",
                              items=(("5", "Allied Assault (SKD 5)", "Loads in every game"),
                                     ("6", "Spearhead/Breakthrough (SKD 6)", "")),
                              default="5")
    write_textures: BoolProperty(name="Write textures", default=True,
                                 description="Save new material images as .tga next to the model")
    apply_modifiers: BoolProperty(name="Apply modifiers", default=True)
    pk3: BoolProperty(name="Also write a .pk3", default=False,
                      description="Pack everything written into a .pk3 beside the .tik")

    def execute(self, context):
        pk3_path = os.path.splitext(self.filepath)[0] + ".pk3" if self.pk3 else None
        try:
            written, warnings = exporter.export_model(
                context, self.filepath, skd_version=int(self.skd_version), anim_mode=self.animations,
                write_textures=self.write_textures, apply_modifiers=self.apply_modifiers, pk3_path=pk3_path)
        except exporter.ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        _report(self, warnings)
        self.report({"INFO"}, "wrote %d files" % len(written))
        return {"FINISHED"}


class EXPORT_SCENE_OT_mohaa_skc(bpy.types.Operator, ExportHelper):
    """Export the active armature's current action as a MOHAA animation (.skc)"""
    bl_idname = "export_scene.mohaa_skc"
    bl_label = "Export MOHAA Animation"

    filename_ext = ".skc"
    filter_glob: StringProperty(default="*.skc", options={"HIDDEN"})

    @classmethod
    def poll(cls, context):
        o = context.active_object
        return o is not None and o.type == "ARMATURE" and o.animation_data and o.animation_data.action

    def execute(self, context):
        try:
            out, warnings = exporter.export_action(context, self.filepath, context.active_object)
        except exporter.ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        _report(self, warnings)
        self.report({"INFO"}, "wrote %s" % out)
        return {"FINISHED"}


def menu_import(self, context):
    self.layout.operator(IMPORT_SCENE_OT_mohaa.bl_idname, text="MOHAA Model (.tik, .skd)")
    self.layout.operator(IMPORT_SCENE_OT_mohaa_skc.bl_idname, text="MOHAA Animation (.skc)")


def menu_export(self, context):
    self.layout.operator(EXPORT_SCENE_OT_mohaa.bl_idname, text="MOHAA Model (.tik)")
    self.layout.operator(EXPORT_SCENE_OT_mohaa_skc.bl_idname, text="MOHAA Animation (.skc)")


classes = (MOHAA_Preferences, IMPORT_SCENE_OT_mohaa, IMPORT_SCENE_OT_mohaa_skc, EXPORT_SCENE_OT_mohaa,
           EXPORT_SCENE_OT_mohaa_skc)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
