#!/usr/bin/env bash
#
# features.sh - what each fork feature costs: every feature turned off in turn
# (leave one out) against a player's own config, on three still scenes and one
# scripted firefight. One bench.sh process a run; analyse with featan.py.
#
#   OMFEAT_PLAYCFG=/path/to/omconfig.cfg tools/win-bench/features.sh
#   python3 tools/win-bench/featan.py "$OMFEAT_RESULTS"
#
# Env:
#   OMFEAT_PLAYCFG   the player's omconfig.cfg to measure against (required)
#   OMFEAT_RESULTS   where run outputs go (default ~/.cache/omfeat/<date>); a
#                    rerun into the same dir skips runs that already finished
#   OMFEAT_ARMS      comma list of arm names to run (default all); base and
#                    base2 always run, they are what every arm is measured from
#   OMFEAT_SCENES    comma list of scenes (default fight,street,m1l2a,m4l1)
#   OMFEAT_REPS      repetitions of the whole matrix (default 3)
#   OMBENCH_*        passed through to bench.sh (install, out dir, resolution)
#
# Traps this keeps out of the numbers:
# - devmap needs `thereisnomonkey 1; cheats 1` or dog/wuss are refused.
# - more than 32 `+` commands on a command line are silently dropped, so the
#   floor arm (everything off) goes into its seed cfg instead.
# - a run started while another openmohaa.exe is up measures both: stop.
# - some runs land in a slow state (~2x everything); featan.py drops runs over
#   1.5x the base median.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

: "${OMFEAT_PLAYCFG:?set OMFEAT_PLAYCFG to the omconfig.cfg to measure against}"
[ -f "$OMFEAT_PLAYCFG" ] || { echo "features: no $OMFEAT_PLAYCFG"; exit 1; }
OUT="${OMFEAT_RESULTS:-$HOME/.cache/omfeat/$(date +%Y%m%d)}"
REPS="${OMFEAT_REPS:-3}"
mkdir -p "$OUT"

export OMBENCH_INSTALL="${OMBENCH_INSTALL:-/mnt/d/Medal of Honor/orch-test}"
export OMBENCH_OUT="${OMBENCH_OUT:-/mnt/d/Medal of Honor/bench-features}"
export OMBENCH_WIDTH="${OMBENCH_WIDTH:-2560}" OMBENCH_HEIGHT="${OMBENCH_HEIGHT:-1440}"
export OMBENCH_TIMEOUT="${OMBENCH_TIMEOUT:-240}"

COMMON="developer 0; thereisnomonkey 1; cheats 1; com_maxfps 0; com_frameStats 250; com_hitchLog 33"

# name|scope (all = still scenes + fight, dyn = fight only)|cvars turned off
ARMS=(
"base|all|"
"sunShadows|all|r_sunShadows 0"
"sunEntCascades|all|r_sunEntityShadowCascades 0"
"rtl|all|r_realtimeLighting 0"
"rtShadows|all|r_rtShadows 0"
"rtDynamic|all|r_rtDynamicLights 0"
"rtSky|all|r_rtSky 0"
"rtLamps|all|r_rtLamps 0; r_rtLightsAtFixtures 0"
"rtCharShadows|all|r_rtShadowCharacters 0"
"volumetric|all|r_volumetric 0"
"volSmoke|all|r_volumetricSmoke 0"
"gtao|all|r_gtao 0"
"msaa|all|r_ext_framebuffer_multisample 0"
"materials|all|r_normalMapping 0; r_specularMapping 0; r_deluxeMapping 0"
"parallax|all|r_parallaxMapping 0"
"parallaxShadows|all|r_parallaxMapShadows 0"
"cubemaps|all|r_cubeMapping 0"
"water|all|r_water 0"
"groundCover|all|r_groundCover 0"
"post|all|r_bloom 0; r_sharpen 0"
"flares|all|r_flares 0"
"alphaToCov|all|r_alphaToCoverage 0"
"treeSprites|all|r_drawStaticModelSprites 0"
"rain|all|cg_rain 0; r_rainMist 0; r_rainRunoff 0; r_rainBeads 0; r_rainFlash 0; cg_lens_rain 0"
"fpBody|all|cg_firstPersonBody 0; cg_firstPersonShadow 0; cg_firstPersonLegs 0"
"skelGpu|all|r_skelGpu 0"
"ragdoll|dyn|cg_ragdoll 0"
"physics|dyn|cg_physics 0"
"gore|dyn|cg_gore 0"
"aiEnhanced|dyn|ai_enhanced 0"
"floor|all|SEED"
"base2|all|"
)
# Everything the floor arm turns off (written into its seed cfg).
FLOOR="r_sunShadows 0; r_realtimeLighting 0; r_volumetric 0; r_gtao 0; r_ext_framebuffer_multisample 0; r_normalMapping 0; r_specularMapping 0; r_deluxeMapping 0; r_parallaxMapping 0; r_cubeMapping 0; r_water 0; r_groundCover 0; r_bloom 0; r_sharpen 0; r_flares 0; r_alphaToCoverage 0; cg_rain 0; r_rainMist 0; r_rainRunoff 0; r_rainBeads 0; r_rainFlash 0; cg_lens_rain 0; cg_firstPersonBody 0; cg_firstPersonShadow 0; cg_firstPersonLegs 0; cg_ragdoll 0; cg_physics 0; cg_gore 0; ai_enhanced 0"

# name|map|precmds|measure ms|scope
SCENES=(
"fight|m5l1a|orch 1; dog 1; wuss; orch_runscript orch/scripts/fight.scr; +attackprimary|30000|dyn"
"street|m5l1a|orch 1; orch_campos -1870 -1344 178 0 0; wait 100; orch_campos -1870 -1344 178 0 0|16000|all"
"m1l2a|m1l2a|dog 1; notarget 1|16000|all"
"m4l1|m4l1|dog 1; notarget 1|16000|all"
)

want() { # want <list> <name>: empty list means everything
	[ -z "$1" ] && return 0
	case ",$1," in *",$2,"*) return 0 ;; esac
	return 1
}

# Seeds: the player's config windowed and uncapped, and the same with the
# floor arm's cvars set last (later lines win).
SEED="$OUT/playseed.cfg"
FLOORSEED="$OUT/floorseed.cfg"
sed -e 's/^seta com_maxfps .*/seta com_maxfps "0"/' -e 's/^seta r_fullscreen .*/seta r_fullscreen "0"/' \
	"$OMFEAT_PLAYCFG" | tr -d '\r' > "$SEED"
{ cat "$SEED"; tr ';' '\n' <<< "$FLOOR" | sed -n 's/^ *\([^ ]*\) \(.*\)$/seta \1 "\2"/p'; } > "$FLOORSEED"

# The firefight script goes where orch_runscript looks in bench's homepath.
mkdir -p "$OMBENCH_OUT/main/orch/scripts"
cp tools/win-bench/fight.scr "$OMBENCH_OUT/main/orch/scripts/fight.scr"

for rep in $(seq 1 "$REPS"); do
	for sc in "${SCENES[@]}"; do
		IFS='|' read -r sname map pre meas sscope <<< "$sc"
		want "${OMFEAT_SCENES:-}" "$sname" || continue
		for a in "${ARMS[@]}"; do
			IFS='|' read -r aname ascope acv <<< "$a"
			[ "$sscope" = all ] && [ "$ascope" = dyn ] && continue
			case "$aname" in base|base2) ;; *) want "${OMFEAT_ARMS:-}" "$aname" || continue ;; esac
			tag=${sname}__${aname}__r${rep}
			[ -s "$OUT/$tag.txt" ] && grep -q '^GPU' "$OUT/$tag.txt" && continue
			if powershell.exe -NoProfile -Command "Get-Process openmohaa -ErrorAction SilentlyContinue | Select -Expand Id" 2>/dev/null | tr -d '\r' | grep -q .; then
				echo "$(date +%T) STOPPED: an openmohaa.exe is running before $tag" | tee -a "$OUT/progress.log"
				exit 1
			fi
			seed=$SEED
			[ "$acv" = SEED ] && { seed=$FLOORSEED; acv=; }
			OMBENCH_SEEDCFG=$seed OMBENCH_MAP=$map OMBENCH_PRECMDS="$pre" OMBENCH_MEASURE_MS=$meas \
				OMBENCH_CVARS="$COMMON${acv:+; $acv}" tools/win-bench/bench.sh > "$OUT/$tag.txt" 2>&1
			cp "$OMBENCH_OUT/main/qconsole.log" "$OUT/$tag.log" 2>/dev/null
			echo "$(date +%T) $tag $(grep -a '^rate' "$OUT/$tag.txt" | cut -c1-50)" | tee -a "$OUT/progress.log"
		done
	done
done
echo "ALLDONE $OUT" | tee -a "$OUT/progress.log"
