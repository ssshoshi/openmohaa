---
name: orchestrate
description: Run a live in-game orchestrator session - the user plays OpenMoHAA, talks to you by voice (push-to-talk), clicks to take screenshots, and you answer by voice and change the game while it runs. Use when the user says "orchestrate", "start the orchestrator", "live session", or wants to narrate changes in game. Also use it to work the orchestrator backlog (items the user filed in queue mode while no agent could answer, e.g. after a usage limit) when they say "check the backlog", "work the queue", "the list from last night", or the limit has reset.
---

# Live orchestrator session

Two ways to use it, and live fixes are optional:

- **Live**: you answer each turn and change the game while it runs (below).
- **Queue**: nobody answers live. The sidecar files every turn in the backlog
  (`home/main/orch/backlog/<n>.json`: the speech, the shots with their
  savegames, the map and where the player stood and looked) and says "Noted"
  itself. The user starts it without you (`orch.py offline`), or switches a
  live session over by saying "queue mode" ("live mode" switches back), for
  example when your usage limit is reached mid-session. You work the items
  later: see **The backlog**.

The user is in the game, not at the terminal. Everything you say to them goes
through `orch.py say`; they never read your terminal output. Keep spoken
replies to one or two short sentences, and ask at most one question at a time.

Tools live in `tools/orchestrator/` (README.md there has the details):
- `orch.py` (WSL): `setup`, `seed`, `launch`, `cmd`, `say`, `state`, `status`, `stop`,
  `offline`, `backlog`
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
   Add `--queue` if the user only wants to note things down this time.
   Wait for `READY listening`. `INFO` lines are diagnostics; `QUEUED <id> ...`
   lines (queue mode) need nothing from you; `MODE queue` / `MODE live` say the
   user switched. Don't pipe it
   through `tr`, `grep` or the like without line buffering (`stdbuf -oL`,
   `--line-buffered`): a buffered pipe holds the TURN lines back until exit.
5. **Game:** ask what to load if the user didn't say (a save name or a map), then
   `orch.py launch --save <name>` or `--map <map>`. It turns orchestrator mode on.
6. `orch.py say "Ready."`. If `orch.py backlog` lists open items, mention how
   many in that line and offer to go through them.

## Each TURN

A Monitor line `TURN <n> "<speech>" shots=<k> errors=<e> context=<c> <path>` means the user
finished talking (or clicked a shot). Then:

1. Read the turn JSON. Its `events` are speech, shots and console errors in time
   order. For each shot, read its `json` (aim, target, player position) and look
   at `files.screenshot_marked` (the target outlined) with Read. `server.txt`
   (beside the shot JSON) has the game's view of the target: class, targetname,
   AI think state, script threads. Its `context` is what the game logged from
   10 s before until the turn ended (`dt`: seconds from the first words or
   shot): where they stood and looked (`trail`, `gaze`), long frames
   (`hitch`), `damage` and kills, `trigger`s, AI `think` changes and move
   `order`s with the script line that gave them. "That guy just did something
   weird", "it stuttered", "why did he run there" are answered from it, often
   without a shot. `orch.py log --last 60 [--type think,order]` reads further
   back.
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

"Do that later", "put it on the list": `orch.py backlog add "<what, in a sentence>"
--turn <turn json> --state`, then `orch.py say "On the list."`. Do the same for
a code change they don't want to wait for a build on.

While frozen (`"frozen": true` in `orch.py state` and shot JSON) the view and
aim are the free camera's; `player_origin` is still where the body stands.
"Resume here" is `orch.py cmd "orch_freeze here"`; if that put them somewhere
bad ("I'm under the map"), `orch.py cmd orch_return`. "Freeze" / "unfreeze"
are `orch.py cmd orch_freeze`. "Ghost" / "let it run" is `orch.py cmd
orch_ghost`: the same camera with the world running (`"ghost": true`); the body
stays behind and can still be shot, and the camera returns if it dies.

Shots also save the game (`orch_<id>`) in single player: "load shot 3" is
`orch.py cmd "loadgame <savegame>"` with the savegame from that shot's event.

## The backlog

When the user asks you to work it (or a limit has reset and they say "check"):

1. `orch.py backlog` lists the open items; `orch.py backlog show <id>` gives one
   in full. Read each item's shots like a live turn's (shot `json`,
   `files.screenshot_marked`, `server.txt`); `state` has the map, the player's
   position and what they looked at when there was no shot.
2. Group them, and say back in the terminal what you understood and plan for
   each, in one or two lines apiece. Ask about the unclear ones; the user is
   usually at the terminal now, not in game.
3. Work them in the session worktree (Start, step 1), one commit per item,
   `orch.py backlog doing <id>` when you start and
   `orch.py backlog done <id> --note "<what changed, commit>"` when it lands.
   Changes to loose files (`physics.txt`, `.scr` overrides, shaders) go into
   the live install as in a live session; record them in the note.
   `orch.py backlog drop <id> --note "<why>"` for one that turns out moot or
   not wanted.
4. Check each change in game when you can: `orch.py launch --save <savegame>`
   from the item's shot puts you where the user was. Without the game you
   can still build and reason from the shots; say which items weren't
   checked in game.
5. Open the PR as at the end of a live session, its body built from the items
   (id, what they asked, what changed). Leave items you didn't finish open.

## Keys the user has (rebindable)

F10 orchestrator mode on/off, F11 freeze/resume the world (they walk around
it; N flies), B ghost (the camera roams the running world, the body stays),
MOUSE3 screenshot, hold MOUSE4 to talk (the
sidecar reads it, only while the game has the focus; `--ptt-key` changes it,
`--always-on` drops it). Pressing it while you speak cuts your reply short.
Voice-only, handled by the sidecar: "mute", "unmute", "cancel that", "queue
mode", "live mode".

## End

When the user says they're done: `orch.py say` a one-line summary, stop the
Monitor and `orch.py stop` if asked, then push the worktree branch and open a
PR to ssshoshi/openmohaa main (see the dev workflow: conventional title, three
required checks, never merge without the user). Build the PR body from the
session's turns and actions; screenshots can be posted with
`tools/bugreport/post.py`.
