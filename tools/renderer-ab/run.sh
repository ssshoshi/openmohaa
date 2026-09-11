#!/usr/bin/env bash
#
# Renderer A/B harness.
#
# Loads the same maps under the OpenGL 1 and OpenGL 2 renderers and captures a
# screenshot plus the console log for each, so the two can be compared while the
# GL2 renderer is brought up to parity.
#
# Usage:
#   tools/renderer-ab/run.sh <build-dir> [map ...]
#
# Example:
#   tools/renderer-ab/run.sh build/full/RelWithDebInfo dm/mohdm1 dm/mohdm6
#
# MOHAA_BASEPATH must point at a Medal of Honor installation.

set -u

BUILD_DIR="${1:-}"
if [ -z "$BUILD_DIR" ] || [ ! -x "$BUILD_DIR/openmohaa" ]; then
    echo "usage: $0 <build-dir> [map ...]" >&2
    echo "  <build-dir> must contain the openmohaa binary" >&2
    exit 1
fi
shift

BASEPATH="${MOHAA_BASEPATH:-/mnt/d/Medal of Honor}"
if [ ! -d "$BASEPATH/main" ]; then
    echo "no MOHAA installation at '$BASEPATH' (set MOHAA_BASEPATH)" >&2
    exit 1
fi

# Indoor and terrain-heavy maps both matter: they exercise very different parts
# of the renderer.
MAPS=("$@")
if [ ${#MAPS[@]} -eq 0 ]; then
    MAPS=(dm/mohdm1 dm/mohdm6)
fi

OUT="${OUT_DIR:-$(pwd)/build/renderer-ab}"
HOME_DIR="$OUT/home"
mkdir -p "$OUT" "$HOME_DIR"

# Give the map time to load and settle before the screenshot. Software
# rasterizers need considerably more of it than a real GPU.
SETTLE_FRAMES="${SETTLE_FRAMES:-3000}"
TIMEOUT="${TIMEOUT:-600}"

BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"

# Extra cvars appended to the GL2 runs only, for trying a feature against the
# GL1 reference, e.g. EXTRA_CVARS="+set r_hdr 1 +set r_toneMap 1".
EXTRA_CVARS="${EXTRA_CVARS:-}"

# Logs written by this invocation. The summaries at the end must only consider
# these -- globbing the output directory picks up logs from earlier runs, for
# maps that were not even asked for, and reports their warnings as current.
RUN_LOGS=""

# Which renderers to run. Set to "opengl2" to skip re-capturing the reference.
RENDERERS="${RENDERERS:-opengl1 opengl2}"

for renderer in $RENDERERS; do
    for map in "${MAPS[@]}"; do
        tag="$(echo "$map" | tr '/' '_')-$renderer${TAG_SUFFIX:-}"

        extra=""
        [ "$renderer" = "opengl2" ] && extra="$EXTRA_CVARS"
        echo "=== $tag ==="

        # Start from an empty screenshot directory so the capture below is
        # unambiguous.
        rm -rf "$HOME_DIR/main/screenshots"

        # Archived cvars otherwise leak from one run into the next, so a
        # feature enabled for one comparison silently stays on for later ones,
        # and the renderers overwrite each other's settings. Every run should
        # start from defaults plus whatever is passed explicitly below.
        rm -f "$HOME_DIR/main/configs/omconfig.cfg"

        (
            cd "$BUILD_DIR" || exit 1
            timeout "$TIMEOUT" ./openmohaa \
                +set fs_basepath "$BASEPATH" \
                +set fs_homepath "$HOME_DIR" \
                +set cl_renderer "$renderer" \
                +set developer 1 \
                +set com_viewlog 0 \
                +set r_fullscreen 0 \
                +set r_mode 6 \
                +set com_maxfps 60 \
                +set sv_maxclients 1 \
                $extra \
                +wait 20 \
                +devmap "$map" \
                +wait "$SETTLE_FRAMES" \
                +screenshotJPEG \
                +wait 40 \
                +quit
        ) > "$OUT/$tag.log" 2>&1

        status=$?
        [ $status -ne 0 ] && echo "  exited with status $status (see $OUT/$tag.log)"
        RUN_LOGS="$RUN_LOGS $OUT/$tag.log"

        # The client silently falls back to the other renderer when the
        # requested one cannot be loaded, which makes an A/B look like it
        # passed when both halves were actually the same renderer.
        if grep -q "Loading \"renderer_${renderer}" "$OUT/$tag.log" 2>/dev/null; then
            echo "  ERROR: $renderer failed to load, the client fell back to another renderer"
            grep -m1 -A2 "Loading \"renderer_${renderer}" "$OUT/$tag.log" | sed 's/^/    /'
            FAILED=1
        fi

        # The screenshot lands in the homepath; give it the run's name.
        shot=$(find "$HOME_DIR/main/screenshots" -name 'shot*.jpg' 2>/dev/null | sort | tail -1)
        if [ -n "$shot" ]; then
            mv "$shot" "$OUT/$tag.jpg"
            echo "  screenshot -> $OUT/$tag.jpg"
        else
            echo "  no screenshot produced"
        fi
    done
done

if [ -n "${FAILED:-}" ]; then
    echo
    echo "=== A RENDERER FAILED TO LOAD -- the comparison above is not valid ==="
fi

echo
echo "=== unimplemented paths reported by the GL2 renderer ==="
grep -h "is not implemented in the GL2 renderer" $RUN_LOGS 2>/dev/null | sort -u

echo
echo "=== errors ==="
grep -hiE "^ERROR|Com_Error|RE_.*failed" $RUN_LOGS 2>/dev/null | sort -u
