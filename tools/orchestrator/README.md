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
| `sessions/<stamp>/turns/<n>.json`, `timeline.jsonl` | sidecar | the agent |

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
- **MOUSE3** (wheel click): screenshot. It records what is under the crosshair
  and the player's position, takes a clean and an outlined screenshot, and in
  single player saves the game as `orch_<id>`.
- Say **"mute"** / **"unmute"** to stop and start listening, **"cancel that"**
  to drop what you said since the last turn.

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

## Swapping code in while the game runs

Windows won't overwrite a loaded DLL, but it will rename one. So: rename
`cgame.dll` to `cgame.old-1.dll`, copy the new `cgame.dll` in, `vid_restart`.
The same works for the renderers. `game.dll` reloads on `killserver` followed by
`loadgame`. `orch.py setup` deletes the `*.old-*.dll` files.

## Testing without a microphone

```bash
python.exe sidecar.py --home "D:\...\home" --wav speech16k.wav --no-tts
```
