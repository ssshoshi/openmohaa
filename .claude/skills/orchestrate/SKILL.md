---
name: orchestrate
description: Run a live in-game orchestrator session - the user plays OpenMoHAA, talks to you by voice (always listening), clicks to take screenshots, and you answer by voice and change the game while it runs. Use when the user says "orchestrate", "start the orchestrator", "live session", or wants to narrate changes in game.
---

# Live orchestrator session

The user is in the game, not at the terminal. Everything you say to them goes
through `orch.py say`; they never read your terminal output. Keep spoken
replies to one or two short sentences, and ask at most one question at a time.

Tools live in `tools/orchestrator/` (README.md there has the details):
- `orch.py` (WSL): `setup`, `seed`, `launch`, `cmd`, `say`, `state`, `status`, `stop`
- `sidecar.py` (Windows Python): voice in/out, shots, turn grouping

Paths:
- Live install: `/mnt/d/Medal of Honor/openmohaa-live` (own home path `home/`;
  never touch the user's other installs or `D:\Medal of Honor\main`).
- Sidecar Python: `/mnt/c/Users/<user>/AppData/Local/openmohaa-orch/venv/Scripts/python.exe`

## Start

1. **Worktree.** `git -C ~/projects/openmohaa fetch fork`, then
   `git worktree add -b feat/live-<YYYYMMDD> ~/projects/openmohaa-live-<YYYYMMDD> fork/main`
   (reuse it if it exists). All code changes of the session go there.
2. **Build and install** if the live install is older than the worktree's HEAD
   (check `git log -1 --format=%ct` against the exe's mtime):
   `cmake --build build/win64 -j4` (configure first per the windows-build-deploy skill;
   **never more than -j4**: WSL has 7 GB and a wider build has crashed the VM),
   then `orch.py setup --build build/win64/Release`.
3. **Seed** the user's config and saves: `orch.py seed` (not `--windowed`; that is for tests).
4. **Sidecar** under Monitor (persistent, so each TURN line wakes you):
   ```
   "<venv>/Scripts/python.exe" "$(wslpath -w tools/orchestrator/sidecar.py)" --home "D:\Medal of Honor\openmohaa-live\home"
   ```
   Wait for `READY listening`. `INFO` lines are diagnostics.
5. **Game:** ask what to load if the user didn't say (a save name or a map), then
   `orch.py launch --save <name>` or `--map <map>`. It turns orchestrator mode on.
6. `orch.py say "Ready."`

## Each TURN

A Monitor line `TURN <n> "<speech>" shots=<k> errors=<e> <path>` means the user
finished talking (or clicked a shot). Then:

1. Read the turn JSON. Its `events` are speech, shots and console errors in time
   order. For each shot, read its `json` (aim, target, player position) and look
   at `files.screenshot_marked` (the target outlined) with Read. `server.txt`
   (beside the shot JSON) has the game's view of the target: class, targetname,
   AI think state, script threads.
2. Resolve "this", "that guy", "here" from the shot nearest in time to those
   words; without a shot, run `orch.py state` for where they look now.
3. If the request is unclear, ask one short question with `orch.py say` and wait
   for the next turn. Speech that is just the user thinking aloud needs no
   answer; send nothing.
4. Act, by tier:

   | Change | How | Ask first? |
   |---|---|---|
   | Console, cvars, cheats, debug views (`noclip`, `god`, `timescale`, `ai_shownode 30`, `sv_showbboxes 4`...) | `orch.py cmd "..."` | no |
   | Which objects are physics bodies | edit `home/main/physics.txt` in the live install, then `orch.py cmd phys_reload` | no |
   | Map scripts | a loose `home/main/maps/<map>.scr` override; it takes effect on `restart` (developer 1), which restarts the level, so say so | no, but tell them |
   | Shaders, textures | loose files in `home/main/`, then `vid_restart` | no |
   | C/C++ code | edit in the session worktree, build the one module (`--target cgame`, `game` or `renderer_opengl2`) | **yes**: say a one-line summary, wait for "go" |

   After a code build, swap it in without exiting (verified for cgame):
   rename the loaded DLL in the live install to `<name>.old-<n>.dll`, copy the
   new one in, then `orch.py cmd vid_restart --timeout 60` (cgame, renderer).
   For `game.dll`: `savegame orch_swap`, `killserver`, then `loadgame orch_swap`.
   A loaded DLL can be renamed but not overwritten. Changes to the exe need
   `orch.py stop` and a relaunch (`--save orch_swap`).
5. Tell them what you did in a sentence. Record every change and how to undo it
   in `<session>/actions.jsonl` (the session folder is printed at sidecar start),
   so "undo that" can be done.
6. Commit code changes in the worktree as they land (conventional commit messages).

Shots also save the game (`orch_<id>`) in single player: "load shot 3" is
`orch.py cmd "loadgame <savegame>"` with the savegame from that shot's event.

## Keys the user has (rebindable)

F10 orchestrator mode on/off, MOUSE3 screenshot. Voice-only, handled by the
sidecar: "mute", "unmute", "cancel that".

## End

When the user says they're done: `orch.py say` a one-line summary, stop the
Monitor and `orch.py stop` if asked, then push the worktree branch and open a
PR to ssshoshi/openmohaa main (see the dev workflow: conventional title, three
required checks, never merge without the user). Build the PR body from the
session's turns and actions; screenshots can be posted with
`tools/bugreport/post.py`.
