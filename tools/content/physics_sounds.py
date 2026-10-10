#!/usr/bin/env python3
"""Writes the opm-physics pak's sound aliases and the list of where to fetch them.

The physics plays phys_<material>_<light|heavy|soft> when a thing strikes
something, phys_flesh_<hard|soft> for a corpse, and phys_<material>_scrape
while one slides (code/physics/phys_impact.cpp). Each alias is a set of
recordings from Half-Life 2 and Day of Defeat: Source, which are Valve's and
not ours to publish. So this writes two files:

    content/opm-physics/ubersound/opm_physics.scr   the aliases (ours)
    content/opm-physics/physics.fetch               where tools/content/pack.py
                                                    downloads each recording

A set is named by a stem under sound/physics/ and takes every numbered take of
it there is ("wood/wood_box_impact_hard" is wood_box_impact_hard1..6), read from
the repositories' file trees at the commits pinned below. The engine picks one
of an alias's takes at random each time it plays; a scrape picks one loop for
each slide.

    physics_sounds.py          rewrite both files (needs the internet)
"""

import json
import os
import re
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAK = os.path.join(ROOT, "content", "opm-physics")

REPOS = {
    "hl2": "84f5de61421d95ce1eb9dcf9625a90782b41b278",
    "dods": "b57142a9825b8aa762b542a5d6c0388e382e7ba9",
}

MAPS = 'maps "m t dm obj lib train"'

# alias: ([stems, "repo:stem" for one not in hl2], volume, volume mod, pitch, pitch mod, min dist, max dist)
SETS = {
    # Props on hard ground, light and heavy; and on soft ground.
    "phys_wood_light": (["wood/wood_box_impact_hard", "wood/wood_plank_impact_hard"], 0.9, 0.1, 0.95, 0.1, 120, 1400),
    "phys_wood_heavy": (["wood/wood_crate_impact_hard", "wood/wood_solid_impact_hard"], 1.0, 0.0, 0.95, 0.1, 200, 2000),
    "phys_wood_soft": (["wood/wood_box_impact_soft", "wood/wood_crate_impact_soft", "wood/wood_solid_impact_soft",
                        "wood/wood_plank_impact_soft", "wood/wood_furniture_impact_soft"], 0.8, 0.1, 0.95, 0.1, 100, 1000),
    "phys_metal_light": (["metal/metal_canister_impact_hard", "metal/paintcan_impact_hard", "metal/soda_can_impact_hard"],
                         0.9, 0.1, 0.95, 0.1, 120, 1400),
    "phys_metal_heavy": (["metal/metal_barrel_impact_hard", "metal/metal_box_impact_hard", "metal/metal_solid_impact_hard"],
                         1.0, 0.0, 0.95, 0.1, 200, 2200),
    "phys_metal_soft": (["metal/metal_canister_impact_soft", "metal/metal_barrel_impact_soft", "metal/metal_box_impact_soft",
                         "metal/metal_solid_impact_soft", "metal/paintcan_impact_soft", "metal/soda_can_impact_soft"],
                        0.8, 0.1, 0.95, 0.1, 100, 1000),
    "phys_weapon_light": (["metal/weapon_impact_hard"], 0.9, 0.1, 0.95, 0.1, 120, 1400),
    "phys_weapon_heavy": (["metal/weapon_impact_hard"], 1.0, 0.0, 0.9, 0.1, 160, 1600),
    "phys_weapon_soft": (["metal/weapon_impact_soft"], 0.8, 0.1, 0.95, 0.1, 100, 1000),
    "phys_helmet_light": (["dods:helmet"], 0.9, 0.1, 0.95, 0.1, 120, 1400),
    "phys_helmet_heavy": (["dods:helmet"], 1.0, 0.0, 0.9, 0.1, 120, 1400),
    "phys_helmet_soft": (["metal/metal_canister_impact_soft"], 0.6, 0.1, 1.0, 0.1, 80, 800),
    "phys_glass_light": (["glass/glass_bottle_impact_hard"], 0.9, 0.1, 0.95, 0.1, 100, 1200),
    "phys_glass_heavy": (["glass/glass_impact_hard", "glass/glass_sheet_impact_hard"], 1.0, 0.0, 0.95, 0.1, 150, 1600),
    "phys_glass_soft": (["glass/glass_impact_soft", "glass/glass_sheet_impact_soft"], 0.7, 0.1, 0.95, 0.1, 80, 800),
    "phys_paper_light": (["cardboard/cardboard_box_impact_soft"], 0.8, 0.1, 0.95, 0.1, 80, 900),
    "phys_paper_heavy": (["cardboard/cardboard_box_impact_hard"], 0.9, 0.1, 0.95, 0.1, 120, 1200),
    "phys_paper_soft": (["cardboard/cardboard_box_impact_soft"], 0.6, 0.1, 0.9, 0.1, 60, 700),
    "phys_stone_light": (["concrete/rock_impact_hard", "concrete/concrete_impact_hard"], 0.9, 0.1, 0.95, 0.1, 120, 1400),
    "phys_stone_heavy": (["concrete/concrete_block_impact_hard", "concrete/boulder_impact_hard"], 1.0, 0.0, 0.95, 0.1, 200, 2200),
    "phys_stone_soft": (["concrete/rock_impact_soft", "concrete/concrete_impact_soft"], 0.8, 0.1, 0.95, 0.1, 100, 1000),
    "phys_default_light": (["wood/wood_box_impact_hard"], 0.9, 0.1, 0.95, 0.1, 120, 1400),
    "phys_default_heavy": (["wood/wood_crate_impact_hard"], 1.0, 0.0, 0.95, 0.1, 200, 2000),
    "phys_default_soft": (["wood/wood_box_impact_soft"], 0.8, 0.1, 0.95, 0.1, 100, 1000),
    # Corpses: a trunk on hard ground; a limb, or soft ground.
    "phys_flesh_hard": (["body/body_medium_impact_hard", "flesh/flesh_impact_hard"], 1.0, 0.0, 0.95, 0.1, 200, 2000),
    "phys_flesh_soft": (["body/body_medium_impact_soft"], 0.9, 0.1, 0.95, 0.1, 120, 1400),
    # Sliding: loops, one picked for each slide. None for glass.
    "phys_wood_scrape": (["wood/wood_box_scrape_rough_loop", "wood/wood_crate_scrape_rough_loop",
                          "wood/wood_solid_scrape_rough_loop", "wood/wood_plank_scrape_rough_loop"], 0.8, 0.0, 1.0, 0.0, 100, 1200),
    "phys_metal_scrape": (["metal/metal_box_scrape_rough_loop", "metal/metal_box_scrape_smooth_loop"], 0.8, 0.0, 1.0, 0.0, 100, 1200),
    "phys_weapon_scrape": (["metal/metal_grenade_scrape_rough_loop", "metal/metal_grenade_scrape_smooth_loop"],
                           0.8, 0.0, 1.0, 0.0, 100, 1200),
    "phys_helmet_scrape": (["metal/canister_scrape_rough_loop", "metal/canister_scrape_smooth_loop",
                            "metal/soda_can_scrape_rough_loop"], 0.8, 0.0, 1.0, 0.0, 100, 1200),
    "phys_paper_scrape": (["cardboard/cardboard_box_scrape_rough_loop", "cardboard/cardboard_box_scrape_smooth_loop"],
                          0.8, 0.0, 1.0, 0.0, 100, 1200),
    "phys_stone_scrape": (["concrete/concrete_block_scrape_rough_loop", "concrete/rock_scrape_rough_loop",
                           "concrete/concrete_scrape_smooth_loop"], 0.8, 0.0, 1.0, 0.0, 100, 1200),
    "phys_default_scrape": (["wood/wood_box_scrape_smooth_loop", "wood/wood_plank_scrape_smooth_loop"], 0.8, 0.0, 1.0, 0.0, 100, 1200),
    "phys_flesh_scrape": (["body/body_medium_scrape_rough_loop", "body/body_medium_scrape_smooth_loop",
                           "flesh/flesh_scrape_rough_loop"], 0.8, 0.0, 1.0, 0.0, 100, 1200),
}

HEADER = """\
// Added in OPM
// Written by tools/content/physics_sounds.py; change the sets there.
//
// What physics bodies sound like striking things (code/physics/phys_impact.cpp):
// phys_<material>_<light|heavy|soft> for props, light and heavy on hard ground by
// the thing's weight, soft on earth, grass, sand, snow or carpet; phys_flesh_<hard|soft>
// for corpses (a trunk on hard ground; a limb, or soft ground); phys_<material>_scrape,
// loops, while a thing slides (none for glass). The code scales volume by how hard
// the knock was or how fast the slide, and pitch by the weight.
//
// The recordings are Half-Life 2's and Day of Defeat: Source's physics sounds, which
// are not ours to publish: physics.fetch says where the build fetches them from,
// and they go into the pak built on the player's machine, never into the repository.
"""


def tree(repo, sha):
    url = f"https://api.github.com/repos/sourcesounds/{repo}/git/trees/{sha}?recursive=1"
    with urllib.request.urlopen(url, timeout=60) as r:
        return [e["path"] for e in json.load(r)["tree"] if e["type"] == "blob" and e["path"].startswith("sound/physics/")]


def takes(files, stem):
    rx = re.compile(re.escape("sound/physics/" + stem) + r"(\d*)\.wav$", re.I)
    found = sorted((int(m.group(1) or 0), p) for p in files for m in [rx.match(p)] if m)
    if not found:
        raise SystemExit(f"no recordings for {stem}")
    return [p for _, p in found]


def main():
    files = {repo: tree(repo, sha) for repo, sha in REPOS.items()}
    scr, fetch, seen = [HEADER], [], set()

    fetch.append("# The sounds ubersound/opm_physics.scr plays: <path in the pak> <url>. Fetched")
    fetch.append("# by tools/content/pack.py when the pak is built, never kept in the repository.")
    fetch.append("# Half-Life 2 and Day of Defeat: Source (Valve), from github.com/sourcesounds.")
    fetch.append("# Written by tools/content/physics_sounds.py.")

    for alias, (stems, vol, volmod, pitch, pitchmod, mindist, maxdist) in SETS.items():
        n = 0
        for stem in stems:
            repo, stem = stem.split(":", 1) if ":" in stem else ("hl2", stem)
            for path in takes(files[repo], stem):
                n += 1
                scr.append(f"aliascache {alias}{n} {path} soundparms {vol} {volmod} {pitch} {pitchmod} "
                           f"{mindist} {maxdist} auto loaded {MAPS}")
                if path not in seen:
                    seen.add(path)
                    fetch.append(f"{path} https://raw.githubusercontent.com/sourcesounds/{repo}/{REPOS[repo]}/{path}")
        scr.append("")
    scr.append("end")

    with open(os.path.join(PAK, "ubersound", "opm_physics.scr"), "w", newline="\n") as f:
        f.write("\n".join(scr) + "\n")
    with open(os.path.join(PAK, "physics.fetch"), "w", newline="\n") as f:
        f.write("\n".join(fetch) + "\n")
    print(f"{len(SETS)} aliases, {len(seen)} recordings")


if __name__ == "__main__":
    main()
