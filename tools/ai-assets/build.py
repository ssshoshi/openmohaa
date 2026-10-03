#!/usr/bin/env python3
"""Pack the enemy AI's smoke grenade into a local pk3.

With ai_smoke, German squads throw smoke (code/fgame/actor.cpp,
Actor::DecideToThrowSmoke). Allied Assault has no smoke grenade, so this takes
Spearhead's German one (the Nebelhandgranate) from your own Spearhead install,
as tools/m3l2-assault/build.py does for that map, but with its sounds for
every map. The game files are copyrighted, so they are never committed: this
builds a pk3 on your machine. Without it the AI simply never throws smoke.

The pk3 holds:
  models/projectiles/nebelhandgranate*, models/fx/NebelhandgranateExplosion.tik
                                 the thrown grenade and its smoke, as in Spearhead
  textures, sounds               what those use
  scripts/opm_ai.shader          the two Spearhead shaders they use (the smoke in grey)
  ubersound/opm_ai.scr           Spearhead's sound aliases for them, on every map

  tools/ai-assets/build.py                         # into D:\\Medal of Honor\\main
  tools/ai-assets/build.py --out some.pk3
  tools/ai-assets/build.py --spearhead /path/to/mainta
"""

import argparse
import importlib.util
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PK3_NAME = "zzzzzzzzz-opm-ai.pk3"

# The smoke grenade's files, shaders and sounds are the m3l2 add-on's.
_spec = importlib.util.spec_from_file_location("m3l2_build", os.path.join(HERE, "..", "m3l2-assault", "build.py"))
m3l2 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(m3l2)

# Sound aliases load on maps whose names contain one of these: every
# single-player and multiplayer map.
ALL_MAPS = 'maps "m t dm obj lib"'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spearhead", default=os.path.join(m3l2.GAME_ROOT, "mainta"), help="Spearhead's mainta folder")
    ap.add_argument("--out", default=os.path.join(m3l2.GAME_ROOT, "main", PK3_NAME))
    args = ap.parse_args()

    paks = m3l2.Paks(args.spearhead)
    shaders = "// Spearhead's German smoke grenade, for the enemy AI (ai_smoke)\n\n"
    for path, names in m3l2.SHADERS.items():
        text = paks.read(path).decode("latin-1")
        shaders += "".join(m3l2.shader_block(text, n) + "\n" for n in names)
    shaders = shaders.replace(*m3l2.SMOKE_TEXTURE)
    lines = [re.sub(r'maps\s+"[^"]*"', ALL_MAPS, l) for l in m3l2.aliases(paks.read("ubersound/ubersound.scr").decode("latin-1"))]
    if not lines:
        sys.exit("no smoke grenade sound aliases in Spearhead's ubersound.scr")
    uber = "// Spearhead's smoke grenade sounds, for the enemy AI (ai_smoke)\n\n" + "\n".join(lines) + "\n\nend\n"

    tmp = args.out + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for name in m3l2.FILES:
            z.writestr(name, paks.read(name))
        z.writestr("scripts/opm_ai.shader", shaders)
        z.writestr("ubersound/opm_ai.scr", uber)
    os.replace(tmp, args.out)
    print(f"wrote {args.out}: {len(m3l2.FILES) + 2} files, {len(lines)} sound aliases")


if __name__ == "__main__":
    main()
