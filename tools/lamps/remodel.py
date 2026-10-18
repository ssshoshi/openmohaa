#!/usr/bin/env python3
"""The game's lamps made again in Blender: glass that glows, bulbs in it.

    tools/lamps/remodel.py models/lights/hang5lamp.tik --work ~/lamps
    tools/lamps/remodel.py --all --work ~/lamps --pk3 ~/lamps/zzzzzzzzz-opm-lampmodels.pk3
    tools/lamps/remodel.py shaders          # data/opm-lamps/scripts/opm_lampglass.shader from lamps.txt

Each lamp in data/opm-lamps/lights/lamps.txt is taken from the player's
paks, made again by remodel_blender.py (its glass a surface of its own,
rounded; a bulb or a flame at each of its bulbs) and written as its .skd at
the path the game loads it from. The game's .tik is not touched: the new
surfaces are named after their shaders, which the TIKI loader gives a
surface it names no shader for. Those shaders are in the opm-lamps pak.

The pk3 holds models made from the game's own and is not committed (the fork
is public): it is built from the player's paks, like the remastered props.
Review WORK/<lamp>/preview.png before putting it in main/.

Blender: $BLENDER, else blender on PATH, else (WSL) the newest in
C:\\Program Files\\Blender Foundation (tools/blender/mohaa_blender.py).
"""

import argparse
import json
import re
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "blender"))
sys.path.insert(0, HERE)

import lamps  # noqa: E402
import mohaa_blender as mb  # noqa: E402

SHADERS = os.path.join(HERE, "..", "..", "data", "opm-lamps", "scripts", "opm_lampglass.shader")

# how each kind's glass glows, lit from inside: added over the lit glass
GLOW = {
    "shaded": (0.95, 0.78, 0.55),
    "candle": (0.95, 0.68, 0.36),
    "lantern": (0.95, 0.68, 0.36),
}


def lamp_name(model):
    return os.path.splitext(os.path.basename(model))[0].lower().replace("-", "_")


def skd_path(model):
    t = lamps.parse_tiki(model)
    return t["skel"][0] if t["skel"] else None


def glass_shader(model, entry):
    """The shader of a lamp's glass: its texture lit as the game lights it, and
    its own light over it; a bare bulb's, its light alone."""
    t = lamps.parse_tiki(model)
    shader = next(iter(t["surfaces"].values()), None)
    image = lamps.shader_image(shader) if shader else None
    if not image:
        return None
    kind = (entry.get("kind") or ["shaded"])[0]
    name = "opm_%s_glass" % lamp_name(model)
    color = entry.get("color")
    if kind == "bulb":
        c = [float(x) for x in color] if color else [1.0, 0.92, 0.75]
        body = ["\t{", "\t\tmap %s" % image, "\t\talphaFunc GE128", "\t\tdepthWrite",
                "\t\trgbGen const %.2f %.2f %.2f" % tuple(c), "\t}"]
    else:
        glow = GLOW.get(kind, GLOW["shaded"])
        if color:
            c = [float(x) for x in color]
            glow = tuple(g * x / max(c) for g, x in zip(glow, c))
        body = ["\t{", "\t\tmap %s" % image, "\t\talphaFunc GE128", "\t\tdepthWrite", "\t\trgbGen static", "\t}",
                "\t{", "\t\tmap %s" % image, "\t\tblendFunc GL_ONE GL_ONE", "\t\talphaFunc GE128",
                "\t\trgbGen const %.2f %.2f %.2f" % glow, "\t}"]
    return "\n".join([name, "{", "\tcull none", "\tsurfaceparm nolightmap"] + body + ["}", ""])


def cmd_shaders(args):
    table = lamps.read_table()
    out = ["// The lamps' glass, lit from inside: made by tools/lamps/remodel.py shaders",
           "// from lights/lamps.txt. The remodelled lamps (remodel.py) name a surface",
           "// opm_<lamp>_glass, which takes the shader of its name.", ""]
    for model, entry in table.items():
        if (entry.get("kind") or [""])[0] == "off":
            continue
        s = glass_shader(model, entry)
        if s:
            out.append(s)
    with open(SHADERS, "w", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote", os.path.relpath(SHADERS))


def remodel(model, entry, work, args):
    exe = mb.find_blender()
    win = mb.is_windows_exe(exe)
    name = lamp_name(model)
    out = os.path.join(work, name)
    os.makedirs(out, exist_ok=True)
    spec = {"model": model, "kind": (entry.get("kind") or ["shaded"])[0], "bulb": entry["bulb"]}
    if "bulbSize" in entry:
        spec["bulbSize"] = float(entry["bulbSize"][0])
    if "glassThreshold" in entry:
        spec["glassThreshold"] = float(entry["glassThreshold"][0])
    if "round" in entry:
        spec["round"] = entry["round"][0] != "0"
    spec_path = os.path.join(out, "spec.json")
    json.dump(spec, open(spec_path, "w"))

    margs = argparse.Namespace(game=args.game, ta=False, tt=False)
    tmp = os.path.join(out, "game")
    tik = mb.materialize(model, margs, tmp)
    folders = ";".join(mb.to_host(f, win) for f in mb.game_folders(margs))
    cmd = [exe, "-b", "--factory-startup", "--python", mb.to_host(os.path.join(HERE, "remodel_blender.py"), win), "--",
           mb.to_host(tik, win), mb.to_host(os.path.join(out, "export"), win), mb.to_host(spec_path, win),
           "--folders", folders]
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    result = None
    for line in proc.stdout.splitlines():
        if line.startswith("MOHAA_RESULT "):
            result = json.loads(line[len("MOHAA_RESULT "):])
    if proc.returncode != 0 or result is None:
        sys.stdout.write(proc.stdout[-4000:])
        sys.stderr.write(proc.stderr[-4000:])
        raise SystemExit("%s: Blender failed (exit %d)" % (model, proc.returncode))
    json.dump(result["report"], open(os.path.join(out, "report.json"), "w"), indent=1)
    for w in result["warnings"]:
        print("  warning:", w)

    # the .skd, at the path the game loads it from
    skd = skd_path(model)
    made = os.path.join(out, "export", name, os.path.basename(skd))
    if not os.path.exists(made):
        found = [os.path.join(dp, f) for dp, _d, fs in os.walk(os.path.join(out, "export")) for f in fs if f.endswith(".skd")]
        if len(found) != 1:
            raise SystemExit("%s: no .skd written (%s)" % (model, found))
        made = found[0]
    dest = os.path.join(work, "out", *re.sub("/+", "/", skd).split("/"))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(made, dest)
    print("%s: %s" % (model, json.dumps(result["report"].get("counts", {}))))
    return dest


def cmd_build(args):
    table = lamps.read_table()
    models = list(table) if args.all else [m.lower() for m in args.models]
    for model in models:
        if model not in table:
            raise SystemExit("%s is not in lamps.txt" % model)
        if (table[model].get("kind") or [""])[0] == "off":
            continue
        remodel(model, table[model], args.work, args)
    if args.pk3:
        root = os.path.join(args.work, "out")
        with zipfile.ZipFile(args.pk3, "w", zipfile.ZIP_DEFLATED) as z:
            for dp, _d, fs in os.walk(root):
                for f in fs:
                    p = os.path.join(dp, f)
                    z.write(p, os.path.relpath(p, root).replace(os.sep, "/"))
        print("wrote", args.pk3)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "shaders":
        return cmd_shaders(None)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--work", required=True)
    ap.add_argument("--pk3")
    ap.add_argument("--game", default=os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor"))
    args = ap.parse_args()
    if not args.models and not args.all:
        ap.error("models, or --all")
    cmd_build(args)


if __name__ == "__main__":
    main()
