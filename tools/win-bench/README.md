# win-bench — unattended GL2 GPU benchmark of the Windows build, from WSL2

Runs the **real Windows `openmohaa.exe` on the native NVIDIA driver** via WSL2 Windows
interop, with no user interaction, and reports the per-pass GPU frame timings from
`r_gpuTimers`. Use this instead of the Linux/WSLg run when you want numbers that match the
user's actual machine — the Linux path renders on Mesa (llvmpipe, or D3D12-translated GL
with `GALLIUM_DRIVER=d3d12`), neither of which is comparable to the native driver.

## Usage

```bash
tools/win-bench/bench.sh
```

Override anything via env:

```bash
OMBENCH_MAP=dm/mohdm2 OMBENCH_WIDTH=1920 OMBENCH_HEIGHT=1080 \
OMBENCH_INTERVAL=600 tools/win-bench/bench.sh
```

| var | default | meaning |
|-----|---------|---------|
| `OMBENCH_INSTALL`  | `/mnt/d/Medal of Honor/openmohaa-play` | dir with `openmohaa.exe` + `renderer_opengl2.dll` |
| `OMBENCH_BASEPATH` | `/mnt/d/Medal of Honor` | `fs_basepath` (holds `main/Pak*.pk3`) |
| `OMBENCH_OUT`      | `/mnt/d/Medal of Honor/bench-out` | scratch `fs_homepath`; log + screenshots land here |
| `OMBENCH_MAP`      | `dm/mohdm1` | map loaded via `devmap` |
| `OMBENCH_WIDTH/HEIGHT` | `1280`/`720` | render resolution (`r_mode -1` custom) |
| `OMBENCH_INTERVAL` | `300` | `r_gpuTimers` averaging window, in frames |
| `OMBENCH_MEASURE_MS` | `16000` | length of the measurement window |
| `OMBENCH_LOADWAIT` | -- | only for binaries without the `waitload` command: a fixed ms wait for the map to load (9000 for DM maps, ~45000 for single player) |
| `OMBENCH_TIMEOUT`  | `140` | hard ceiling (s) before the run is force-killed |
| `OMBENCH_CVARS`    | -- | extra cvars, applied on the launch line: `"r_vaoCache 1; r_finish 1"` |
| `OMBENCH_REPEATS`  | `1` | measurement windows inside one process |
| `OMBENCH_RESTART`  | `none` | between windows: `none`, `vid_restart` or `map` |
| `OMBENCH_SEEDCFG`  | -- | an `omconfig.cfg` to copy in first, to reproduce a specific player's settings instead of defaults |

## How it works

1. Generates `run.bat` + `bench.cfg` into `OMBENCH_OUT`, pointing `fs_homepath` there so the
   console log (`qconsole.log`) and screenshots are readable from WSL.
2. Launches `openmohaa.exe` in the background through `cmd.exe /c`.
3. Waits (by polling `qconsole.log`) for the `OMBENCH_MAPLOADED` marker.
4. The cfg holds at **`waitload`** until the map has loaded, and the launch line sets
   `ui_autoContinue 1` so the load goes straight into the game. The cfg still issues
   **`finishloadingscreen`** — the console command MOHAA's CONTINUE
   button runs (`stuffcommand` in `ui/loadingbar.txt`; it calls `UI_ActivateView3D`). That
   clears the post-load **CONTINUE** card in-engine, so no synthetic input or window focus is
   needed. This is the "no user intervention" part. (`dismiss.ps1` is a keypress fallback,
   unused by default.)
5. Lets the world render for `OMBENCH_MEASURE_MS`; `r_gpuTimers` prints an averaged per-pass
   breakdown every `OMBENCH_INTERVAL` frames.
6. Quits, force-kills any straggler, and parses the **last** GPU report block (steady state —
   the first world frame's timings are stale by design).

The boot intro videos (EA / title / legal) are suppressed with `+set cl_playintro 0` and the
`ui_skip_*` cvars on the launch line, so nothing sits in front of the map load.

Exit status is 0 only if a `gpu … ms =` line was captured.

## The report

`r_gpuTimers` passes (from `tr_gputimer.c`): `frame` (total) = `sunshadow` (`sun0..sun3`
nested) + `prepass` + `shadowmask` + `main3d` + `post` + `present` + `other`. The `other`
column is unattributed frame time; a breakdown that is mostly `other` means passes are
mis-attributed. `gpu submission: N draws N uploads` are counts, not times — immune to GPU
clock drift between runs, so they're the most stable thing to diff across builds.

## Gotchas

- **`cl_renderer` must be `opengl2`.** It defaults to `opengl1` and is `CVAR_ARCHIVE|CVAR_LATCH`,
  so a stale `seta cl_renderer "opengl1"` in the homepath's `omconfig.cfg` silently loads the GL1
  renderer — which renders fine and prints `r_speeds`, but has **no `r_gpuTimers`** at all. bench.sh
  forces GL2 both on the launch line and by normalizing that cfg line. If a run shows no gpu report,
  first confirm the log says `Trying to load "renderer_opengl2.dll"` and `using GLSL version ...`.

- **CONTINUE card.** `devmap` loads the map but MOHAA holds on a "CONTINUE" card until a key
  is pressed; until then no world frames render and `r_gpuTimers` prints nothing. `dismiss.ps1`
  handles it. If a run reports "world likely never rendered", check the screenshot — if it
  shows the framed level photo, the keypress didn't land (window focus stolen, or a second
  menu gate); re-run, or extend the wait before the keypress in `bench.cfg`.
- **Renderer/exe API version.** The install must be the ragdoll-line build (REF_API v15). The
  freshly cross-built `feat/opengl2` renderer DLL is v14 and won't load against that exe — see
  the `windows-build-deploy` skill and the REF_API memory note.
- **Not comparable to the Linux run.** Numbers here are the native NVIDIA driver; the WSL/GL
  runs are Mesa. Only compare win-bench to win-bench.
- **Resolution matters.** GPU pass times scale with resolution — hold `WIDTH`/`HEIGHT` fixed
  when diffing builds.

- **Runs are at DEFAULTS, not at your config.** The scratch `omconfig.cfg` is rewritten
  every run, which is what keeps a sweep honest — but it also means the run does not
  reproduce what the player actually sees unless you pass their settings via
  `OMBENCH_CVARS`. This is not hypothetical: `cg_shadows` is registered by the *renderer*
  with default `1` (`renderergl2/tr_init.c:1595`) before cgame registers it with default
  `0` (`cg_main.c:160`), so the effective default is `1` — the expensive per-entity blob
  shadow path, worth ~5.3ms/frame on a populated `m1l1`. A user running `cg_shadows 3`
  takes an early return and pays ~0.05ms. Benching at the default and reporting it as the
  user's experience is a whole wasted investigation.

- **The scene gets heavier the longer a map has been up**, so *when* a window is measured
  matters as much as what it is measuring. On `m1l1` with a static camera, four consecutive
  15s windows went 111 → 76 → 59 → 60 fps with no cvar changed and no restart, while draws
  went 513 → 1012 and sun cascade surfaces 203 → 460: AI actors populating the level, each
  submitted to the main view, the depth prepass and two shadow cascades. This is why the
  default is one window per process. Never A/B two settings by running them back to back in
  one process — that was how a previous cascade sweep concluded "no difference" when the
  runs simply were not comparable. Use `OMBENCH_REPEATS` to *study* the effect, not to
  collect a sweep.
