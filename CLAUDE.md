# Working on this fork

## Branches and worktrees
- Every change: a `feat/…` or `fix/…` branch cut from `fork/main` in its own worktree, a PR on
  ssshoshi/openmohaa (conventional-commit title: it becomes the squash commit), three required checks,
  squash-merge only when the user says so. No direct pushes to main.
- `~/projects/openmohaa` is the user's own checkout: never switch or reset it.
- Permanent worktrees: `~/projects/openmohaa-main` (clean main: report pipeline and deploy builds),
  `~/projects/openmohaa-agent/main`, `~/projects/openmohaa-orch` (the orchestrator; its skills live there).
- Nothing stays uncommitted at the end of a session: commit work in progress to its branch and push.
  `tools/wt.py status` shows what is where; `tools/wt.py gc` clears what is merged (dry run without `--apply`).

## Building and deploying
- Windows builds: `cmake --build build/win64 -j4`. Never wider: WSL has 7 GB and has crashed.
  A fresh configure leaves GL2 off: pass `-DBUILD_RENDERER_GL2=ON`.
- Deploy only when the user asks: `tools/deploy.py` (`--dry-run` first). Two installs: `openmohaa-play`
  (the user's, the user's own home path) and `openmohaa-live` (the orchestrator's, its own `home/`).
  It refuses while the game runs from a target install.
- Data paks go in the shared `D:\Medal of Honor\main`. A backup pk3 left there is loaded as a pak:
  backups go to `D:\Medal of Honor\pk3-backups`.
- Retail and mod files are never committed (the fork is public): changes to them are diffs under
  `content/<pak>/` (`tools/content/pack.py`).

## Testing in game
- Never test in the user's installs. Unattended tests use `ORCH_LIVE="/mnt/d/Medal of Honor/orch-test"`
  with `tools/orchestrator/orch.py`: `launch --save X`, `wait`, `view x y z --pitch p --yaw y`,
  `frames N --every s --script '…'` (one contact sheet to look at), `script`, `cmd`, `reset` (loose
  overrides left in the install's home shadow the paks), `backlog prune` (shots and saves no open
  item needs).
- Reproducing a backlog item: `orch.py backlog repro <id>`; reading one: `orch.py look <id>` (or a
  turn JSON). Loose files a session leaves in an install: `orch.py promote --pak <pak> --apply`.
- God mode is `dog 1` (not `god`, which does nothing), and `notarget 1`; a bare `dog` toggles, so a second call turns it off.
- `tele x y z` ignores angles (use `orch.py view`). `ai_off` actors never die.
- Script output after a `wait` is not in `orch.py script`'s reply. Loading a save in a running game
  can keep the old compiled map scripts: relaunch after changing a `.scr`.
- TIKI `sfx` / `delayedsfx` run only in effects the client's effect manager plays, not on script models;
  a model's animation `enter` commands run again whenever it comes back into the snapshot.
- Game data questions: `tools/paks.py find|which|cat|shader` (indexed once) instead of scanning paks.
