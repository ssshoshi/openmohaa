"""Writing and reading keyframes on Blender 4.4+ slotted actions and older ones alike."""

import bpy


def _slot_for(action, id_data):
    """The action's slot for id_data (made when missing)."""
    slots = getattr(action, "slots", None)
    if slots is None:
        return None
    id_type = id_data.id_type if hasattr(id_data, "id_type") else "OBJECT"
    for s in slots:
        if s.target_id_type == id_type and s.name_display == id_data.name:
            return s
    return slots.new(id_type=id_type, name=id_data.name)


def assign(id_data, action):
    """Make action the one id_data plays, on its own slot."""
    ad = id_data.animation_data or id_data.animation_data_create()
    ad.action = action
    if hasattr(ad, "action_slot") and action is not None:
        slot = _slot_for(action, id_data)
        ad.action_slot = slot
    return ad


def fcurves(action, id_data):
    """The F-curve collection of action for id_data."""
    if hasattr(action, "fcurves"):  # before 4.4 (gone in 5.0)
        return action.fcurves
    from bpy_extras import anim_utils
    slot = _slot_for(action, id_data)
    return anim_utils.action_ensure_channelbag_for_slot(action, slot).fcurves


def new_fcurve(action, id_data, data_path, index=0, group=""):
    curves = fcurves(action, id_data)
    fc = curves.find(data_path, index=index)
    if fc is not None:
        return fc
    try:
        return curves.new(data_path, index=index, group_name=group)
    except TypeError:
        return curves.new(data_path, index=index, action_group=group)


def set_keys(fc, frames, values, interpolation="LINEAR"):
    """Fill an F-curve with (frame, value) keys in one go."""
    n = len(frames)
    fc.keyframe_points.add(n)
    co = [0.0] * (2 * n)
    co[0::2] = frames
    co[1::2] = values
    fc.keyframe_points.foreach_set("co", co)
    interp = bpy.types.Keyframe.bl_rna.properties["interpolation"].enum_items[interpolation].value
    fc.keyframe_points.foreach_set("interpolation", [interp] * n)
    fc.update()


def all_fcurves(action):
    """Every F-curve of an action, whatever slot it is on (reads only)."""
    if hasattr(action, "fcurves"):
        return list(action.fcurves)
    out = []
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                out.extend(bag.fcurves)
    return out


def animates_bones(action):
    return any(fc.data_path.startswith("pose.bones[") for fc in all_fcurves(action))


def frame_range(action):
    if getattr(action, "use_frame_range", False):
        return action.frame_start, action.frame_end
    return tuple(action.frame_range)
