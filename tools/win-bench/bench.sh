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
# The cfg holds at `waitload` until the map has loaded, however long that takes.
# Only binaries older than the waitload command need a fixed wait instead: set
# OMBENCH_LOADWAIT to the ms to wait (9000 suits DM maps, ~45000 single player).
LOADWAIT="${OMBENCH_LOADWAIT:-}"
if [ -n "$LOADWAIT" ]; then LOADCMD="wait $LOADWAIT"; else LOADCMD="waitload"; fi
# Extra cvars, e.g. OMBENCH_CVARS="r_vaoCache 1". Semicolon separated for more
# than one: "r_vaoCache 1; r_finish 1". Applied on the launch line rather than
# in the cfg because several of the interesting ones are CVAR_LATCH -- notably
# r_sunShadows -- and a latched cvar set after GL init does nothing until the
# next vid_restart, which reads as "the setting had no effect".
CVARS="${OMBENCH_CVARS:-}"
# The renderer to load: opengl2 (the default; the only one with r_gpuTimers) or
# opengl1, to reproduce a GL1 problem.
RENDERER="${OMBENCH_RENDERER:-opengl2}"
# Extra console commands run after the measurement, semicolon separated, e.g.
# OMBENCH_CMDS="saveshot save/test.tga 256 256; wait 500" to exercise the saved
# game thumbnail path.
CMDS="${OMBENCH_CMDS:-}"
# Repeated measurement windows inside ONE process, to reproduce effects that
# only appear after the renderer has been up a while. OMBENCH_RESTART picks
# what happens between windows: none, vid_restart (tears down and rebuilds the
# GL context) or map (reloads the map).
# A config to copy into the scratch homepath before the run, so a measurement
# can reproduce what a particular player actually sees rather than the defaults.
# Needed more often than it looks: cg_shadows is registered by the renderer with
# default 1 before cgame registers it with 0, so a default run takes the
# expensive per-entity blob shadow path that a player on cg_shadows 3 does not.
SEEDCFG="${OMBENCH_SEEDCFG:-}"
REPEATS="${OMBENCH_REPEATS:-1}"
RESTART="${OMBENCH_RESTART:-none}"
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
{
  echo "seta com_maxfps 0"
  echo "seta r_swapInterval 0"
  echo "seta r_gpuTimers $INTERVAL"
  echo "seta com_frameTimers $INTERVAL"
  echo "seta cg_frameTimers $INTERVAL"
  echo "seta logfile 2"
  echo "echo OMBENCH_START"
  echo "devmap $MAP"
  echo "$LOADCMD"
  echo "echo OMBENCH_MAPLOADED"
  echo "finishloadingscreen"
  echo "wait 3000"
  i=1
  while [ "$i" -le "$REPEATS" ]; do
    echo "echo OMBENCH_WIN_$i"
    echo "wait $MEASURE_MS"
    echo "echo OMBENCH_ENDWIN_$i"
    if [ "$i" -lt "$REPEATS" ]; then
      case "$RESTART" in
        vid_restart) echo "vid_restart"; echo "wait 8000" ;;
        map)         echo "devmap $MAP"; echo "$LOADCMD"; echo "finishloadingscreen"; echo "wait 3000" ;;
        none)        : ;;
      esac
    fi
    i=$(( i + 1 ))
  done
  if [ -n "$CMDS" ]; then
    echo "$CMDS" | tr ';' '\n' | sed 's/^ *//'
  fi
  echo "echo OMBENCH_ALLDONE"
  echo "screenshotJPEG"
  echo "wait 500"
  echo "quit"
} > "$OUT/main/bench.cfg"

# --- Windows launcher (native paths; r_mode/customres are LATCH so set pre-init).
#     cl_playintro 0 + ui_skip_* keep the EA/title/legal intro videos from playing.
cvar_args=""
if [ -n "$CVARS" ]; then
  while IFS= read -r kv; do
    kv="$(echo "$kv" | sed 's/^ *//;s/ *$//')"
    [ -n "$kv" ] && cvar_args="$cvar_args +set $kv"
  done <<< "$(echo "$CVARS" | tr ';' '\n')"
fi

cat > "$OUT/run.bat" <<BAT
@echo off
cd /d "$win_install"
openmohaa.exe$cvar_args +set cl_renderer $RENDERER +set fs_basepath "$win_base" +set fs_homepath "$win_out" +set logfile 2 +set cl_playintro 0 +set ui_autoContinue 1 +set ui_skip_eamovie 1 +set ui_skip_titlescreen 1 +set ui_skip_legalscreen 1 +set r_fullscreen 0 +set r_mode -1 +set r_customwidth $WIDTH +set r_customheight $HEIGHT +exec bench.cfg
BAT

echo "bench: install   $INSTALL"
echo "bench: map       $MAP @ ${WIDTH}x${HEIGHT}, gpuTimers interval ${INTERVAL} frames"
[ "$REPEATS" -gt 1 ] && echo "bench: repeats   $REPEATS windows, '$RESTART' between"
[ -n "$CVARS" ] && echo "bench: cvars     $CVARS"
[ -n "$SEEDCFG" ] && echo "bench: seedcfg   $SEEDCFG"
echo "bench: clearing previous run"
taskkill.exe /IM "$PROC.exe" /F >/dev/null 2>&1
rm -f "$LOG" "$SHOTDIR"/*.jpg "$SHOTDIR"/*.tga 2>/dev/null
# cl_renderer is CVAR_ARCHIVE|CVAR_LATCH and defaults to opengl1; a stale value in this
# homepath's omconfig.cfg silently loads GL1 (no r_gpuTimers) and outranks the launch +set.
# Force GL2 in the persisted config so the GL2 renderer is what actually loads.
# Every cvar this run sets is CVAR_ARCHIVE and gets written back here on exit,
# so leaving the file in place makes the next run inherit it. A sweep that does
# not wipe it measures the union of everything tried so far, not one variable.
# Start each run from a known file holding nothing but the renderer choice.
CFGFILE="$OUT/main/configs/omconfig.cfg"
mkdir -p "$(dirname "$CFGFILE")"
if [ -n "$SEEDCFG" ]; then
  [ -f "$SEEDCFG" ] || { echo "bench: FAIL - OMBENCH_SEEDCFG '$SEEDCFG' not found"; exit 1; }
  cp "$SEEDCFG" "$CFGFILE"
  # cl_renderer is CVAR_ARCHIVE|CVAR_LATCH and outranks the launch line, so a
  # seeded config carrying opengl1 would silently load GL1 and report no timers.
  if grep -q 'cl_renderer' "$CFGFILE"; then
    sed -i "s/^seta cl_renderer .*/seta cl_renderer \"$RENDERER\"/" "$CFGFILE"
  else
    echo "seta cl_renderer \"$RENDERER\"" >> "$CFGFILE"
  fi
else
  echo "seta cl_renderer \"$RENDERER\"" > "$CFGFILE"
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

focus_loop_pid=""
if [ "$REPEATS" -gt 1 ] && [ "$RESTART" = "vid_restart" ]; then
  # vid_restart tears down and recreates the window, which drops foreground.
  # Re-assert it for the whole run rather than once at the start.
  ( while :; do
      powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$(wslpath -w "$SCRIPT_DIR/focus.ps1")" "$PROC" >/dev/null 2>&1
      sleep 4
    done ) &
  focus_loop_pid=$!
fi

if ! wait_for OMBENCH_ALLDONE "$hard_deadline"; then
  [ -n "$focus_loop_pid" ] && kill "$focus_loop_pid" 2>/dev/null
  echo "bench: FAIL - measurement window did not complete"; taskkill.exe /IM "$PROC.exe" /F >/dev/null 2>&1; exit 1
fi

[ -n "$focus_loop_pid" ] && kill "$focus_loop_pid" 2>/dev/null

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
cpu_wall="$(grep -a '^cpu .*ms wall =' "$clean" | tail -1)"
eng_line="$(grep -a '^frame .*ms = sleep' "$clean" | tail -1)"
cg_line="$(grep -a '^cgame .*ms =' "$clean" | tail -1)"
cge_line="$(grep -a '^cgame ents ' "$clean" | tail -1)"
cpu_back="$(grep -a '^cpu backend =' "$clean" | tail -1)"
cpu_pass="$(grep -a '^cpu   in drawsurfs:' "$clean" | tail -1)"

echo
echo "======================= openmohaa GL2 win-bench ======================="
echo "renderer : ${renderer:-<not found>}"
echo "gl       : ${glver:-<not found>}"
echo "map      : $MAP @ ${WIDTH}x${HEIGHT}"
[ -n "$CVARS" ] && echo "cvars    : $CVARS"
[ -n "$SEEDCFG" ] && echo "seedcfg  : $SEEDCFG"
if [ -n "$gpu_line" ]; then
  frame_ms="$(sed -E 's/^gpu +([0-9.]+)ms.*/\1/' <<<"$gpu_line")"
  wall_ms="$(sed -E 's/^cpu +([0-9.]+)ms wall.*/\1/' <<<"${cpu_wall:-}")"
  echo "GPU      : $gpu_line"
  [ -n "$cpu_wall" ] && echo "CPU      : $cpu_wall"
  [ -n "$cpu_back" ] && echo "           $cpu_back"
  [ -n "$cpu_pass" ] && echo "           $cpu_pass"
  # The engine ledger: client contains the renderer, so client minus the
  # renderer's backend is what the renderer cannot see. Needs an exe with
  # com_frameTimers; absent on older builds.
  [ -n "$eng_line" ] && echo "engine   : $eng_line"
  [ -n "$cg_line" ] && echo "cgame    : $cg_line"
  [ -n "$cge_line" ] && echo "           $cge_line"

  # The headline. GPU frame time alone cannot say whether the card was the
  # limit -- a frame that spends 9ms on the GPU and 20ms on the main thread
  # has a 9ms GPU frame. Only wall clock, measured swap to swap, settles it,
  # so report the measured rate and name the bound rather than assuming one.
  if [ -n "$wall_ms" ]; then
    awk -v w="$wall_ms" -v g="$frame_ms" 'BEGIN{
      if (w <= 0) exit
      printf "rate     : %.0f fps measured (%.2fms wall)", 1000.0/w, w
      printf " vs %.0f fps if GPU bound (%.2fms)\n", (g>0 ? 1000.0/g : 0), g
      if (g > 0 && w > g * 1.10)
        printf "           CPU bound: %.2fms/frame the card is not waited on for\n", w - g
      else
        printf "           GPU bound: the main thread keeps up with the card\n"
    }'
  else
    fps="$(awk -v m="$frame_ms" 'BEGIN{ if(m>0) printf "%.0f", 1000.0/m; else print "n/a" }')"
    echo "rate     : ~${fps} fps if GPU bound (no cpu report - old renderer DLL?)"
  fi

  [ -n "$sub_line" ] && echo "submit   : $sub_line"
  [ -n "$fe_pf" ]    && echo "frontend : $fe_pf"
  [ -n "$fe_ms" ]    && echo "           $fe_ms"
else
  echo "GPU      : <no gpuTimers report captured>"
  echo "           world likely never rendered - check the screenshot below."
fi
if [ "$REPEATS" -gt 1 ]; then
  echo "-----------------------------------------------------------------------"
  echo "per window ('$RESTART' between):"
  w=1
  while [ "$w" -le "$REPEATS" ]; do
    seg="$(awk -v s="OMBENCH_WIN_$w\$" -v e="OMBENCH_ENDWIN_$w\$" \
             '$0 ~ s {f=1; next} $0 ~ e {f=0} f' "$clean")"
    g="$(grep -a '^gpu .*ms =' <<<"$seg" | tail -1)"
    c="$(grep -a '^cpu .*ms wall =' <<<"$seg" | tail -1)"
    if [ -n "$c" ] || [ -n "$g" ]; then
      gm="$(sed -E 's/^gpu +([0-9.]+)ms.*/\1/' <<<"${g:-}")"
      cm="$(sed -E 's/^cpu +([0-9.]+)ms wall.*/\1/' <<<"${c:-}")"
      awk -v w="$w" -v cm="$cm" -v gm="$gm" 'BEGIN{
        printf "  window %d: ", w
        if (cm+0 > 0) printf "%.2fms wall (%.0f fps)", cm, 1000.0/cm; else printf "wall n/a"
        if (gm+0 > 0) printf "  |  %.2fms gpu", gm
        printf "\n" }'
      # Draw and upload counts, not times: these are what distinguish the
      # renderer getting slower from the scene getting bigger, and unlike the
      # ms figures they cannot drift with the card's clocks.
      sm="$(grep -a '^gpu submission:' <<<"$seg" | tail -1)"
      cs="$(grep -a '^gpu cascade surfs:' <<<"$seg" | tail -1)"
      [ -n "$sm" ] && echo "            ${sm}${cs:+  |  $cs}"
      p="$(grep -a '^cpu frames:' <<<"$seg" | tail -1)"
      [ -n "$p" ] && echo "            $p"
      b="$(grep -a '^cpu backend =' <<<"$seg" | tail -1)"
      [ -n "$b" ] && echo "            $b"
      e="$(grep -a '^frame .*ms = sleep' <<<"$seg" | tail -1)"
      [ -n "$e" ] && echo "            $e"
      cg="$(grep -a '^cgame .*ms =' <<<"$seg" | tail -1)"
      [ -n "$cg" ] && echo "            $cg"
      cge="$(grep -a '^cgame ents ' <<<"$seg" | tail -1)"
      [ -n "$cge" ] && echo "            $cge"
      cgt="$(grep -a '^cgame etype =' <<<"$seg" | tail -1)"
      [ -n "$cgt" ] && echo "            $cgt"
      cgm="$(grep -a '^cgame modelanim =' <<<"$seg" | tail -1)"
      [ -n "$cgm" ] && echo "            $cgm"
      cgw="$(grep -a '^cgame worst:' <<<"$seg" | tail -1)"
      [ -n "$cgw" ] && echo "            $cgw"
    else
      echo "  window $w: <no report captured>"
    fi
    w=$(( w + 1 ))
  done
  echo "-----------------------------------------------------------------------"
fi

shot="$(ls -t "$SHOTDIR"/*.jpg 2>/dev/null | head -1)"
echo "screenshot: ${shot:-<none>}"
echo "log       : $LOG"
echo "======================================================================="
rm -f "$clean"
[ -n "$gpu_line" ]
