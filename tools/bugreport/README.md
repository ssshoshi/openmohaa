# In-game reports

Report a bug, an idea or feedback from inside the game, and file it as a GitHub
issue with everything needed to look into it.

## In the game

1. Press **F8** (`bugreport`; bound on first run, rebind freely). What is under
   the crosshair is outlined and described on screen.
   `bugreport <area>` starts with an area chosen: physics, texture, lighting,
   sound, model, script, gameplay, performance, other.
2. Aim at the thing, press **F8** again. The target is locked and the report
   menu opens: pick the type and area, give it a title and details.
   **Re-aim** goes back to aiming, **Cancel** drops it.
3. **Submit**. The game takes a clean and an outlined screenshot, and in single
   player a savegame, and writes the report to
   `<homepath>/main/bugreports/<id>/`.

From the console, without the menu:
`br_submit <bug|idea|feedback> <area> "<title>" ["<details>"]`.

What a report holds: the build and its git hash, renderer and GPU, map,
position and view with a `devmap`/`setviewpos` line to get back there, the
surface under the crosshair (seen through clip brushes) and the light grid
there, the target's model, bounds and surface shaders, the game's view of it
(class, targetname, animation, physics, an actor's AI state), the level's
script threads, every sound playing, the archived cvars, and the console.

## Filing them

```sh
tools/bugreport/labels.sh            # once per repo: the labels used
tools/bugreport/upload.py --dry-run  # what would be filed
tools/bugreport/upload.py            # file every pending report
```

`upload.py` finds the home path under `/mnt/c/Users/*/AppData/Roaming/openmohaa`
(or pass `--home <homepath>/main`). Screenshots, the save and the dumps go to
`reports/<id>/` on the `bug-assets` branch, so the issue can show and link
them; the issue is labelled by type, `area:<area>` and `source:ingame`. A filed
report's `pending` marker becomes `sent`, holding the issue's URL.

Needs `gh` logged in with repo scope, and Pillow to stamp the overlay onto the
outlined screenshot (the game's screenshots are taken before its 2D pass).
