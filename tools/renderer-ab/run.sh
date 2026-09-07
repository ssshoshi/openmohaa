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

for renderer in opengl1 opengl2; do
    for map in "${MAPS[@]}"; do
        tag="$(echo "$map" | tr '/' '_')-$renderer"
        echo "=== $tag ==="

        # Start from an empty screenshot directory so the capture below is
        # unambiguous.
        rm -rf "$HOME_DIR/main/screenshots"

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
                +wait 20 \
                +devmap "$map" \
                +wait "$SETTLE_FRAMES" \
                +screenshotJPEG \
                +wait 40 \
                +quit
        ) > "$OUT/$tag.log" 2>&1

        status=$?
        [ $status -ne 0 ] && echo "  exited with status $status (see $OUT/$tag.log)"

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

echo
echo "=== unimplemented paths reported by the GL2 renderer ==="
grep -h "is not implemented in the GL2 renderer" "$OUT"/*-opengl2.log 2>/dev/null | sort -u

echo
echo "=== errors ==="
grep -hiE "^ERROR|Com_Error|RE_.*failed" "$OUT"/*.log 2>/dev/null | sort -u
