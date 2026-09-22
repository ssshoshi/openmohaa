#!/usr/bin/env bash
#
# bench.sh - unattended GL2 GPU benchmark of the *Windows* openmohaa build, driven
# from WSL2. Runs the real .exe on the native NVIDIA driver via Windows interop,
# auto-dismisses the post-load CONTINUE card (Route 1: synthetic keypress), lets
# r_gpuTimers accumulate, and parses the per-pass GPU timings back out.
#
# Nothing here needs the user: launch -> keypress -> measure -> quit -> parse.
#
# Config via env (defaults match this machine's ragdoll-line install):
#   OMBENCH_INSTALL   dir holding openmohaa.exe + renderer_opengl2.dll
#   OMBENCH_BASEPATH  fs_basepath (holds main/Pak*.pk3)
#   OMBENCH_OUT       scratch dir used as fs_homepath (log + screenshots land here)
#   OMBENCH_MAP       map to load (devmap)
#   OMBENCH_WIDTH/HEIGHT   render resolution (r_mode -1 custom)
#   OMBENCH_INTERVAL  r_gpuTimers averaging window, in frames
#   OMBENCH_MEASURE_MS     length of the measurement window
#   OMBENCH_TIMEOUT   hard ceiling (s) before the run is force-killed
set -uo pipefail

INSTALL="${OMBENCH_INSTALL:-/mnt/d/Medal of Honor/openmohaa-ragdoll}"
BASEPATH="${OMBENCH_BASEPATH:-/mnt/d/Medal of Honor}"
OUT="${OMBENCH_OUT:-/mnt/d/Medal of Honor/bench-out}"
MAP="${OMBENCH_MAP:-dm/mohdm1}"
WIDTH="${OMBENCH_WIDTH:-1280}"
HEIGHT="${OMBENCH_HEIGHT:-720}"
INTERVAL="${OMBENCH_INTERVAL:-100}"
MEASURE_MS="${OMBENCH_MEASURE_MS:-16000}"
TIMEOUT="${OMBENCH_TIMEOUT:-140}"
PROC="openmohaa"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$OUT/main/qconsole.log"
SHOTDIR="$OUT/main/screenshots"

command -v wslpath >/dev/null || { echo "bench: wslpath not found (not WSL?)"; exit 1; }
[ -f "$INSTALL/openmohaa.exe" ] || { echo "bench: no openmohaa.exe under '$INSTALL'"; exit 1; }
[ -f "$INSTALL/renderer_opengl2.dll" ] || echo "bench: WARN no renderer_opengl2.dll in install (GL2 may not load)"

win_install="$(wslpath -w "$INSTALL")"
win_base="$(wslpath -w "$BASEPATH")"
win_out="$(wslpath -w "$OUT")"
win_bat="$win_out\\run.bat"

mkdir -p "$OUT/main" "$SHOTDIR"

# --- in-game script. Fully self-driving: `finishloadingscreen` is the console command
#     the CONTINUE button runs (stuffcommand in ui/loadingbar.txt -> UI_ActivateView3D),
#     so we dismiss the post-load card in-engine instead of faking a keypress. `wait` is
#     milliseconds on this engine, so markers fire on a wall-clock schedule.
cat > "$OUT/main/bench.cfg" <<CFG
seta com_maxfps 0
seta r_swapInterval 0
seta r_gpuTimers $INTERVAL
seta logfile 2
echo OMBENCH_START
devmap $MAP
wait 9000
echo OMBENCH_MAPLOADED
finishloadingscreen
wait 3000
echo OMBENCH_MEASURE
wait $MEASURE_MS
echo OMBENCH_DONE
screenshotJPEG
wait 500
quit
CFG

# --- Windows launcher (native paths; r_mode/customres are LATCH so set pre-init).
#     cl_playintro 0 + ui_skip_* keep the EA/title/legal intro videos from playing.
cat > "$OUT/run.bat" <<BAT
@echo off
cd /d "$win_install"
openmohaa.exe +set cl_renderer opengl2 +set fs_basepath "$win_base" +set fs_homepath "$win_out" +set logfile 2 +set cl_playintro 0 +set ui_skip_eamovie 1 +set ui_skip_titlescreen 1 +set ui_skip_legalscreen 1 +set r_fullscreen 0 +set r_mode -1 +set r_customwidth $WIDTH +set r_customheight $HEIGHT +exec bench.cfg
BAT

echo "bench: install   $INSTALL"
echo "bench: map       $MAP @ ${WIDTH}x${HEIGHT}, gpuTimers interval ${INTERVAL} frames"
echo "bench: clearing previous run"
taskkill.exe /IM "$PROC.exe" /F >/dev/null 2>&1
rm -f "$LOG" "$SHOTDIR"/*.jpg "$SHOTDIR"/*.tga 2>/dev/null
# cl_renderer is CVAR_ARCHIVE|CVAR_LATCH and defaults to opengl1; a stale value in this
# homepath's omconfig.cfg silently loads GL1 (no r_gpuTimers) and outranks the launch +set.
# Force GL2 in the persisted config so the GL2 renderer is what actually loads.
CFGFILE="$OUT/main/configs/omconfig.cfg"
if [ -f "$CFGFILE" ]; then
  if grep -q 'cl_renderer' "$CFGFILE"; then
    sed -i 's/^seta cl_renderer .*/seta cl_renderer "opengl2"/' "$CFGFILE"
  else
    echo 'seta cl_renderer "opengl2"' >> "$CFGFILE"
  fi
fi

wait_for() {   # marker deadline_epoch
  local marker="$1" deadline="$2"
  while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -qa "$marker" "$LOG" 2>/dev/null && return 0
    sleep 0.5
  done
  return 1
}

start_epoch=$(date +%s)
hard_deadline=$(( start_epoch + TIMEOUT ))

echo "bench: launching Windows build (native NVIDIA driver)..."
cmd.exe /c "$win_bat" >/dev/null 2>&1 &
launch_pid=$!

if ! wait_for OMBENCH_MAPLOADED "$hard_deadline"; then
  echo "bench: FAIL - map never loaded (see $LOG)"; taskkill.exe /IM "$PROC.exe" /F >/dev/null 2>&1; exit 1
fi
echo "bench: map loaded (cfg runs finishloadingscreen); foregrounding window so world renders at rate"
# r_gpuTimers only counts world frames, and MOHAA throttles rendering when unfocused,
# so the window must be foreground during the measurement window.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$SCRIPT_DIR/focus.ps1")" "$PROC" 2>&1 | tr -d '\r' | sed 's/^/bench:   /'

if ! wait_for OMBENCH_DONE "$hard_deadline"; then
  echo "bench: FAIL - measurement window did not complete"; taskkill.exe /IM "$PROC.exe" /F >/dev/null 2>&1; exit 1
fi

# let it quit cleanly, then make sure it's gone
for _ in $(seq 1 20); do
  grep -qa "RE_Shutdown" "$LOG" 2>/dev/null && break
  sleep 0.5
done
taskkill.exe /IM "$PROC.exe" /F >/dev/null 2>&1
wait "$launch_pid" 2>/dev/null

# --- parse -------------------------------------------------------------------
clean="$(mktemp)"; tr -d '\r' < "$LOG" | sed -E 's/^\[[^]]*\] //' > "$clean"

renderer="$(grep -a 'GL_RENDERER:' "$clean" | head -1 | sed 's/.*GL_RENDERER: //')"
glver="$(grep -a 'GL_VERSION:' "$clean" | head -1 | sed 's/.*GL_VERSION: //')"
# take the LAST full gpu report block (steady state; first world frame is stale by design)
gpu_line="$(grep -a '^gpu .*ms =' "$clean" | tail -1)"
sub_line="$(grep -a '^gpu submission:' "$clean" | tail -1)"
fe_pf="$(grep -a '^frontend per frame:' "$clean" | tail -1)"
fe_ms="$(grep -a '^frontend ms/frame:' "$clean" | tail -1)"

echo
echo "======================= openmohaa GL2 win-bench ======================="
echo "renderer : ${renderer:-<not found>}"
echo "gl       : ${glver:-<not found>}"
echo "map      : $MAP @ ${WIDTH}x${HEIGHT}"
if [ -n "$gpu_line" ]; then
  frame_ms="$(sed -E 's/^gpu +([0-9.]+)ms.*/\1/' <<<"$gpu_line")"
  fps="$(awk -v m="$frame_ms" 'BEGIN{ if(m>0) printf "%.0f", 1000.0/m; else print "n/a" }')"
  echo "GPU      : $gpu_line"
  echo "           ~${fps} fps GPU-bound (1000 / ${frame_ms}ms)"
  [ -n "$sub_line" ] && echo "submit   : $sub_line"
  [ -n "$fe_pf" ]    && echo "frontend : $fe_pf"
  [ -n "$fe_ms" ]    && echo "           $fe_ms"
else
  echo "GPU      : <no gpuTimers report captured>"
  echo "           world likely never rendered - check the screenshot below."
fi
shot="$(ls -t "$SHOTDIR"/*.jpg 2>/dev/null | head -1)"
echo "screenshot: ${shot:-<none>}"
echo "log       : $LOG"
echo "======================================================================="
rm -f "$clean"
[ -n "$gpu_line" ]
