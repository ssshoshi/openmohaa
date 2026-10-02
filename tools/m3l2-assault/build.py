#!/usr/bin/env python3
"""Pack the m3l2 assault add-on into a local pk3.

The add-on (m3l2_opm.scr, next to this file) reworks m3l2's attack on the
house front: waves, each led by German smoke grenades and a volley of
grenades. Allied Assault has no smoke grenade, so this takes Spearhead's
German one (the Nebelhandgranate) from your own Spearhead install. The game
files are copyrighted, so they are never committed: this builds a pk3 on your
machine.

The pk3 holds:
  maps/m3l2_opm.scr                  the add-on (run by Level::StartMapAddon)
  models/projectiles/nebelhandgranate*, models/fx/NebelhandgranateExplosion.tik
                                     the thrown grenade and its smoke, as in Spearhead
  textures, sounds                   what those use
  scripts/opm_m3l2.shader            the two Spearhead shaders they use (the smoke in grey)
  ubersound/opm_m3l2.scr             Spearhead's sound aliases for them, for m3l2

  tools/m3l2-assault/build.py                      # into D:\\Medal of Honor\\main
  tools/m3l2-assault/build.py --out some.pk3
  tools/m3l2-assault/build.py --spearhead /path/to/mainta
"""

import argparse
import glob
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
GAME_ROOT = "/mnt/d/Medal of Honor"
PK3_NAME = "zzzzzzzzz-opm-m3l2-assault.pk3"

FILES = [
    "models/projectiles/nebelhandgranate.tik",
    "models/projectiles/nebelhandgranate_base.txt",
    "models/projectiles/nebelhandgranate/nebelhandgranate.skd",
    "models/projectiles/nebelhandgranate/nebelhandgranate.skc",
    "models/fx/NebelhandgranateExplosion.tik",
    "models/fx/SmokeGrenadeExplosionGerman_base.txt",
    "textures/models/weapons/german_smoke_grenade.jpg",
    "textures/models/weapons/german_smoke_grenade.dds",
    "textures/sprites/hrpuffnstuff.tga",
    "textures/sprites/hrpuffnstuff.dds",
    "sound/weapons/fire/smoke_exp_start1.wav",
    "sound/weapons/fire/smoke_exp_start2.wav",
    "sound/weapons/fire/smoke_exp_loop2.wav",
    "sound/weapons/fire/smoke_exp_end2.wav",
    "sound/weapons/fire/smokegrenade_air_loop.wav",
    "sound/weapons/foley/smokegrenade_land1.wav",
    "sound/weapons/foley/smokegrenade_land2.wav",
]
SHADERS = {
    "scripts/tasprites.shader": ["smokegrenade_german"],
    "scripts/weapons_germans.shader": ["germ_smoke_gren"],
}
# Spearhead tints German smoke green; plain grey smoke looks right on m3l2.
SMOKE_TEXTURE = ("textures/sprites/hrpuffnstuff_german.tga", "textures/sprites/hrpuffnstuff.tga")
ALIASES = re.compile(r"^(nebelsmokegrenade_exp_(start|loop|end)\d+|smokegrenade_bounce_\w+|smokegrenade_air_loop)$")


class Paks:
    """Spearhead's files by name, later paks overriding earlier ones."""

    def __init__(self, folder):
        self.index = {}
        paks = sorted(glob.glob(os.path.join(folder, "*.pk3")), key=lambda p: os.path.basename(p).lower())
        if not paks:
            sys.exit(f"no pk3 files in {folder}: pass --spearhead <your Spearhead mainta folder>")
        for pak in paks:
            with zipfile.ZipFile(pak) as z:
                for name in z.namelist():
                    if not name.endswith("/"):
                        self.index[name.lower()] = (pak, name)

    def read(self, name):
        found = self.index.get(name.lower())
        if not found:
            sys.exit(f"{name} is not in your Spearhead paks")
        with zipfile.ZipFile(found[0]) as z:
            return z.read(found[1])


def shader_block(text, name):
    m = re.search(r"^\s*" + re.escape(name) + r"\s*\n\s*\{", text, re.M | re.I)
    if not m:
        sys.exit(f"shader {name} not found")
    depth, i = 0, text.index("{", m.start())
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return name + "\n" + text[i:j + 1] + "\n"
    sys.exit(f"shader {name} is unterminated")


def aliases(text):
    out = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) > 2 and parts[0] == "aliascache" and ALIASES.match(parts[1]):
            # Spearhead limits them to its own maps; these are for m3l2.
            out.append(re.sub(r'maps\s+"[^"]*"', 'maps "m3l2"', line.strip()))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spearhead", default=os.path.join(GAME_ROOT, "mainta"), help="Spearhead's mainta folder")
    ap.add_argument("--out", default=os.path.join(GAME_ROOT, "main", PK3_NAME))
    args = ap.parse_args()

    paks = Paks(args.spearhead)
    shaders = "// Spearhead's German smoke grenade, for maps/m3l2_opm.scr\n\n"
    for path, names in SHADERS.items():
        text = paks.read(path).decode("latin-1")
        shaders += "".join(shader_block(text, n) + "\n" for n in names)
    shaders = shaders.replace(*SMOKE_TEXTURE)
    lines = aliases(paks.read("ubersound/ubersound.scr").decode("latin-1"))
    if not lines:
        sys.exit("no smoke grenade sound aliases in Spearhead's ubersound.scr")
    uber = "// Spearhead's smoke grenade sounds, for maps/m3l2_opm.scr\n\n" + "\n".join(lines) + "\n\nend\n"

    tmp = args.out + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(HERE, "m3l2_opm.scr"), "maps/m3l2_opm.scr")
        for name in FILES:
            z.writestr(name, paks.read(name))
        z.writestr("scripts/opm_m3l2.shader", shaders)
        z.writestr("ubersound/opm_m3l2.scr", uber)
    os.replace(tmp, args.out)
    print(f"wrote {args.out}: {len(FILES) + 3} files, {len(lines)} sound aliases")


if __name__ == "__main__":
    main()
