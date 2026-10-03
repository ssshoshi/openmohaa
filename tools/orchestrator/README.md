# Live orchestrator

Play the game and talk to an agent (Claude Code) while it runs. The agent hears
you, sees your screenshots, answers by voice and changes the game live. The
agent's side of a session is the `/orchestrate` skill
(`.claude/skills/orchestrate/SKILL.md`).

```
 game (openmohaa-live)  --orch/events--->  sidecar.py (Windows)  --stdout TURN-->  Claude Code (WSL)
                        <--orch/cmd------  mic, Whisper, Kokoro  <--orch/say-----  orch.py say / cmd
```

All the exchange is files under the live install's home path,
`D:\Medal of Honor\openmohaa-live\home\main\orch\`:

| Folder | Written by | Read by |
|---|---|---|
| `cmd/<n>.txt` | orch.py, sidecar | the game (`com_cmddir`): runs each command at once, writes the output to `cmd/<n>.out` |
| `events/<id>.json`, `<id>.server.txt` | the game, per shot | sidecar |
| `say/<n>.txt` | orch.py say | sidecar: spoken with Kokoro |
| `log/client.<n>.jsonl` (a ring of 64 batches), `log/game.jsonl` | the game, while orchestrator mode is on | sidecar, `orch.py log` |
| `sessions/<stamp>/turns/<n>.json`, `timeline.jsonl` | sidecar | the agent |
| `backlog/<n>.json` | sidecar (queue mode), `orch.py backlog add` | the agent, later |

## In game

- **F10**: orchestrator mode on/off (the panel top right shows `ORCH` and the
  voice state: `[hold MOUSE4 to talk]`, `[REC]`, `[THINKING]`, `[SPEAKING]`, `[MUTED]`).
- **Hold MOUSE4** (thumb button) to talk. The sidecar reads the key itself and
  only while the game window has the focus; it keeps the 0.4 s before the
  press, so a word started early isn't lost. Pressing it while a reply is
  being spoken cuts the reply short. `--ptt-key v` (or `mouse5`, `f11`, ...)
  picks another key; `--always-on` listens all the time instead.
- **F11**: freeze the world. AI, scripts, physics and ragdolls stop where
  they are (the game is paused) while you keep walking around, with
  collisions. **N** switches to flying through everything and back
  (`orch_flyspeed`, 2 by default). F11 again resumes where you were;
  `orch_freeze here` (say "resume here") resumes at the camera instead, unless
  it's inside something or over nothing. `orch_return` goes back to where you
  froze, for a "here" that went wrong (under the terrain isn't caught). When
  not frozen, N is the ordinary `noclip`.
- **B**: ghost. The same camera, with the world still running: your body
  stays where it was and stays in the fight (enemies still see and shoot it;
  the camera itself is nothing they can see). The panel says when the body is
  hit, and the camera comes back if it dies. B again returns to the body,
  `orch_ghost here` brings the body to the camera, F11 freezes the world
  without leaving the camera, and B from frozen lets it run again.
- **MOUSE3** (wheel click): screenshot. It records what is under the crosshair
  and the player's position, takes a clean and an outlined screenshot, and in
  single player saves the game as `orch_<id>`.
- Say **"mute"** / **"unmute"** to stop and start listening, **"cancel that"**
  to drop what you said since the last turn, **"queue mode"** / **"live mode"**
  to switch between noting things for later and live answers (below).

## Without an agent: queue mode

Live fixes are optional. When the agent can't answer (a daily or weekly usage
limit reached) or you'd rather just play and note things, run the orchestrator
yourself in queue mode:

```bash
tools/orchestrator/orch.py offline --save <name>     # or --map m1l1; Ctrl+C stops the sidecar
tools/orchestrator/orch.py offline --save <name> -- --ptt-key mouse5   # sidecar options after --
```

It starts the game if it isn't running and the sidecar with `--queue`. Talk and
take shots as usual; the panel shows `[QUEUE]`, and each turn is filed in
`orch/backlog/<n>.json` with your words, the shots (with their savegames), the
map and where you stood and looked; the sidecar answers "Noted, 4". In a live
session, saying "queue mode" does the same from then on.

Later (the limit reset), ask the agent to work the backlog: it reads the items,
plans them with you, fixes them in a worktree and marks them off.

```bash
tools/orchestrator/orch.py backlog            # what is open
tools/orchestrator/orch.py backlog show 4
tools/orchestrator/orch.py backlog add "the MG42 nest on m2l1 should face the road"
tools/orchestrator/orch.py backlog done 4 --note "fixed in 1a2b3c"   # also doing, drop, reopen
```

With `--always-on`, use headphones: the sidecar mutes the microphone while it
speaks, but game sound from speakers would be heard.

## Setup (once)

```bash
# Windows Python packages for the sidecar (into %LOCALAPPDATA%\openmohaa-orch\venv)
powershell.exe -ExecutionPolicy Bypass -File "$(wslpath -w tools/orchestrator/setup_windows.ps1)"

# The live install, from a Windows build of this tree
cmake --build build/win64 -j4
tools/orchestrator/orch.py setup --build build/win64/Release
tools/orchestrator/orch.py seed          # your config, physics.txt and saves (copied, one way)
```

The Whisper model (`small.en`) downloads on the sidecar's first start; Kokoro's
model files (about 340 MB) download to `%LOCALAPPDATA%\openmohaa-orch\kokoro`.

## Engine pieces

- `com_cmddir <dir>` (qcommon/common.c): every 100 ms, runs `<dir>/*.txt` from
  the home path with `Cmd_ExecuteString`, so a cfg `wait` doesn't hold it back
  (`wait` lines themselves are ignored). Output goes to `<dir>/<name>.out`.
- `cgame/cg_orch.cpp`: `orch`, `orch_shot`, `orch_msg [-heard] <text>`,
  `orch_status <state>`, `orch_state`; the panel.
- `cgame/cg_pick.cpp`: what is under the crosshair, shared with the F8 reports.
- `bugreport_server <id> <entnum> orch` (fgame) writes the shot's `server.txt`.
- `orch_debug <preset...>` (cgame) toggles debug views by name; `orch_debug off`
  clears them and `orch_debug` lists them. The panel shows which are on.
  | Preset | Shows |
  |---|---|
  | `ai` | each actor's path (coloured by think: blue idle/patrol, cyan running, yellow curious, red attack), script goal, patrol chain (dashed), leash, cover/aim/look targets and a label; its line to its enemy (green seen, red not), the enemy's last known position, hearing and field of view (`ai_showpaths`, `ai_showsenses`) |
  | `entinfo` | number, name and health over every entity, think and enemy over actors (`g_entinfo 4`) |
  | `nodes` | path nodes and routes |
  | `triggers` | trigger boxes (dim when off), arrows to their targets, labels (`g_showtriggers`) |
  | `cameras` | script cameras |
  | `combat` | a rising number where each hit lands, and its direction (`g_showdamage`); grenade decisions |
  | `look` | a card in the panel on what the crosshair is on, and the game's label over it |
  | `perf` | fps, frame time, worst frame and entity count in the panel; `r_gpuTimers` |
  | `scripts` | the script threads near you (their `self` within 1500 units) and the map script's, in the panel: file:line, waiting on what, whose; asked of the game every second (`orch_scripts`) |
  | `sounds` | a box and the name, entity and distance on each 3D and looping sound playing; one-shots yellow, loops cyan (`s_showsounds`, drawn by the exe from the sound channels and the last world view) |
  | `tris`, `normals`, `bbox`, `entnums` | the renderer's and server's own debug views |

  The game-drawn ones (paths, senses, triggers, damage, entinfo) are single
  player only, and reach `ai_showpaths_dist` / `g_showtriggers_dist` (3000)
  from the player. `scriptinfo [threadnum]` (fgame) prints the script threads,
  or one in detail, through `orch.py cmd`.
- The log, while orchestrator mode is on (single player for the game's side).
  Every line has `t`, Unix seconds, so the sidecar lines it up with speech.
  - cgame (`cg_orch.cpp`): `trail` (position, view, health, weapon, camera;
    every 250 ms when it changes, or every 5 s), `gaze` (what is under the
    crosshair, when it changes), `hitch` (a frame over 50 ms), `shot`, `save`,
    `freeze`, `ghost`, `resume`, `return`, `level`, `cgame_start`. cgame can't
    append to a file, so it writes a batch every 500 ms to the next of 64 files.
  - fgame (`g_orch.cpp`): `damage` (victim, attacker, mod, hit location,
    health before and after, `killed`), `trigger` (once per 2 s per trigger and
    activator), `think` (an actor's think state changing, with its enemy),
    `order` (runto/walkto/crouchto/crawlto/moveto/patrolpath with the script
    and line that gave it; a repeated order within 5 s is skipped), `level`.
  - Each turn's `context` is the log from 10 s before what was said or shot
    until the turn ends, `dt` seconds from its start; trail points a second
    apart, at most 40 of a type and 200 in all, nearest the moment kept.

## Swapping code in while the game runs

`orch.py swap cgame|game|renderer` (run from the worktree; `--build` is the
cmake folder, `build/win64` by default):

1. Refuses if the module's interface with the exe changed since `orch.py setup`
   (setup writes `orch-build.json` with hashes of `cg_public.h`, `g_public.h`,
   `tr_public.h`/`tr_types.h`, `bg_public.h` and `q_shared.h`): then the exe
   must be rebuilt too, so stop, setup and relaunch. `--force` skips this.
2. Builds the one target (`-j4`; `--no-build` swaps what is built).
3. Windows won't overwrite a loaded DLL but will rename one: the loaded one
   becomes `<name>.old-<n>.dll` and the new one is copied in.
4. Reloads: `vid_restart` for cgame and the renderer (about 20 s), and for
   the game `savegame orch_swap`, `killserver`, `loadgame orch_swap`, since a
   same-map load keeps the DLL (about 6 s). If the save fails (dead, a
   cinematic) the old DLL is put back and nothing is reloaded.
5. Checks the game is back in the level (closing the main menu if it came up)
   and that nothing failed to load; otherwise says to relaunch.

`orch.py setup` deletes the `*.old-*.dll` files.

`orch.py script '$guy3 runto $node_x'` writes the code to
`orch/scripts/<stamp>.scr` and runs it now with `orch_runscript` (fgame);
compile errors come back. `--file x.scr --label main` runs a file from a label.
`orch_runscript maps/m3l2.scr <label>` reruns a thread of the map's own script
without recompiling it (recompiling ends a script's running threads, so only
files under `orch/` are recompiled).

## Testing without a microphone

```bash
python.exe sidecar.py --home "D:\...\home" --wav speech16k.wav --no-tts
```
