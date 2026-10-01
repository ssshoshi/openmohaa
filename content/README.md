# Content paks

Game data changes that go with the engine, one folder per pak. Each folder
builds into `zzzzzzzzz-<folder>.pk3`, which sorts after the retail and mod paks
and so overrides them.

Most of these change a retail or mod file that is not ours to publish, so the
folder holds only a unified diff against it (`<path>.patch`). The build takes
the original from your own paks, the copy the game would load (the last pak in
load order that has it), and applies the diff. Files of our own go in as they
are, at their path under the folder.

    tools/content/pack.py                    # every pak, into build/content/
    tools/content/pack.py opm-hrrtm-blood    # one of them
    tools/content/pack.py --install "/path/to/main"

`MOHAA_GAME` (or `--game`) is the folder holding `main/*.pk3`, by default
`/mnt/d/Medal of Honor`. To make a diff from an edited copy of a game file:

    tools/content/pack.py --diff <folder> <path in the pak> <edited file>

| Pak | Needs | What it does |
|---|---|---|
| `opm-cabinet-ragdoll` | retail | The man who falls out of a shot hidden cabinet (`global/cabinet.scr`, m3l2, m5l1a, m5l1b) dies once he is clear of it, so the ragdoll takes over his fall. |
| `opm-hrrtm-blood` | HRRTM Blood Effects Addon | Bullet hits on people splatter the wall behind at once and at full strength, light hits as well as hard ones. Built from the addon's own effect, so without the addon it is not built. |
