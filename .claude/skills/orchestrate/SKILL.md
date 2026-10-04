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
  `offline`, `backlog`; for checking a change: `wait`, `view x y z --pitch --yaw`,
  `frames N --every s --script '...'` (one contact sheet), `reset` (loose overrides)
- `sidecar.py` (Windows Python): voice in/out, shots, turn grouping

Paths:
- Live install: `/mnt/d/Medal of Honor/openmohaa-live` (own home path `home/`;
  never touch the user's other installs or `D:\Medal of Honor\main`).
- Sidecar Python: `/mnt/c/Users/<user>/AppData/Local/openmohaa-orch/venv/Scripts/python.exe`

## Start

1. **Worktree.** `git -C ~/projects/openmohaa fetch fork`, then
   `git worktree add -b feat/live-<YYYYMMDD> ~/projects/openmohaa-live-<YYYYMMDD> fork/main`
   (reuse it if it exists). All code changes of the session go there.
2. **Everything else in one command**, from the worktree: ask what to load if the user
   didn't say (a save name or a map), then `orch.py start --save <name>` (or `--map <map>`,
   `--queue` if the user only wants to note things down). It rebuilds and reinstalls when the
   live install is older than the worktree (**never more than -j4**: WSL has 7 GB), seeds the
   user's config and saves, starts the sidecar detached, launches the game, waits for it, and
   says "Ready." (with how many backlog items are open: offer to go through them).
3. **Watch the sidecar** with Monitor on the command it prints
   (`tail -F .../orch/sidecar.log | grep --line-buffered -E '^(TURN|QUEUED|MODE|READY|ERROR)'`),
   persistent, re-armed when it expires, so each TURN line wakes you. `QUEUED <id> ...`
   lines (queue mode) need nothing from you; `MODE queue` / `MODE live` say the user switched.
   Any pipe needs line buffering, or the TURN lines wait until exit.

## Each TURN

A Monitor line `TURN <n> "<speech>" shots=<k> errors=<e> context=<c> <path>` means the user
finished talking (or clicked a shot or a mark). `continues=<m>` before the path means they said
"add to that": the turn belongs with turn m, one request built over several turns. Then:

1. `orch.py look <turn json>` gives the turn in brief: what was said, each shot's
   target from the game's side (class, targetname, model, animation, think state),
   the console with the retail spam dropped and repeats counted, and a small copy
   of each marked screenshot (the target outlined) to Read. The turn JSON, each
   shot's `json` and `server.txt` (script threads) have the rest. Its `context` is what the game logged from
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
   | Console, cvars, cheats, debug views (`noclip`, `dog 1` (god mode), `timescale`, `ai_shownode 30`, `sv_showbboxes 4`...) | `orch.py cmd "..."` | no |
   | Which objects are physics bodies | edit `home/main/physics.txt` in the live install, then `orch.py cmd phys_reload` | no |
   | A live script tweak (`$guy3 runto $node_x`, `$tank.health = 2000`, start a thread) | `orch.py script '<code>'`, or `--file x.scr --label main`; runs now, no restart; compile errors come back | no |
   | Map scripts | a loose `home/main/maps/<map>.scr` override; it takes effect on `restart` (developer 1), which restarts the level, so say so | no, but tell them |
   | Shaders, textures | loose files in `home/main/`, then `vid_restart` | no |
   | C/C++ code | edit in the session worktree, build the one module (`--target cgame`, `game` or `renderer_opengl2`) | **yes**: say a one-line summary, wait for "go" |

   After "go", `orch.py swap cgame|game|renderer` from the worktree builds the
   one module and swaps it into the running game (cgame/renderer reload with
   `vid_restart`, about 20 s; game with save, killserver and load, about 6 s).
   Tell them the screen will blink. It refuses when the module's interface with
   the exe changed (then: `orch.py stop`, `orch.py setup --build
   build/win64/Release`, `orch.py launch --save orch_swap`), as do changes to
   the exe itself.
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
are `orch.py cmd orch_freeze`. "Show me the AI / triggers / damage / what
I'm looking at / the frame rate" is `orch.py cmd "orch_debug ai"` (presets:
ai, entinfo, nodes, triggers, cameras, combat, look, perf, scripts, sounds,
tris, normals, bbox, entnums; each toggles, `off` clears, no argument lists). "Why is this guy
standing there": `orch.py cmd scriptinfo`, then `scriptinfo <n>` for a thread.
"Ghost" / "let it run" is `orch.py cmd
orch_ghost`: the same camera with the world running (`"ghost": true`); the body
stays behind and can still be shot, and the camera returns if it dies.

Shots also save the game (`orch_<id>`) in single player: "load shot 3" is
`orch.py cmd "loadgame <savegame>"` with the savegame from that shot's event.

## The backlog

When the user asks you to work it (or a limit has reset and they say "check"):

1. `orch.py backlog` lists the open items; `orch.py look <id>` gives one in brief
   (`backlog show <id> [--full]` the whole JSON); `orch.py backlog repro <id>` loads
   it in the orch-test install where it was filed, the player placed and facing as in
   its shot. An item without a shot has its own savegame and `state` (the map, where
   the player stood and looked).
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
4. Check each change in game when you can: `orch.py backlog repro <id>` puts you
   where the user was, in the orch-test install (`ORCH_LIVE` set to it for the
   commands that follow; `frames` for a contact sheet of what happens). Without the
   game you can still build and reason from the shots; say which items weren't
   checked in game. Close with `orch.py backlog prune --apply`: shots and saves no
   open item needs.
5. Open the PR as at the end of a live session, its body built from the items
   (id, what they asked, what changed). Leave items you didn't finish open.

## Keys the user has (rebindable)

F10 orchestrator mode on/off, F11 freeze/resume the world (they walk around
it; N flies), B ghost (the camera roams the running world, the body stays),
MOUSE3 screenshot, MOUSE5 mark (hold: what is under the crosshair is outlined, the wheel steps
front to back through everything the line passes, "nothing" last; let go to mark it, on "nothing"
to cancel; the wheel's own binding comes back, the ragdoll grabber's or the stock one; recorded as
a shot records it, with no screenshot or save; marks of the last minute are outlined on the next shot), hold MOUSE4 to talk (the
sidecar reads it, only while the game has the focus; `--ptt-key` changes it,
`--always-on` drops it). Pressing it while you speak cuts your reply short.
Voice-only, handled by the sidecar: "mute", "unmute", "cancel that", "queue
mode", "live mode", "add to that" (what follows goes into the last item, or continues the last
turn, until "new item" or two minutes with nothing added).

## End

When the user says they're done: `orch.py say` a one-line summary, stop the
Monitor and `orch.py stop --sidecar` if asked. Loose files the session left in the
install (`orch.py promote` lists them) become repo files with
`orch.py promote --pak <pak> --apply`; commit them. Then push the worktree branch and open a
PR to ssshoshi/openmohaa main (see the dev workflow: conventional title, three
required checks, never merge without the user). Build the PR body from the
session's turns and actions; screenshots can be posted with
`tools/bugreport/post.py`.
