#!/usr/bin/env bash
# Builds the ragdoll harness against the solver in this tree.
#
#   ./build.sh            build ./rdsim from code/cgame/cg_ragdoll.cpp
#   ./build.sh some.cpp   build against a variant of the solver instead
#   ./build.sh --jolt     build ./rdsim_jolt, which carries bodies with the Jolt
#                         ragdoll (code/cgame/cg_physics_ragdoll.cpp) once the
#                         blend ends, in a Jolt world built from each scenario
#                         (rdjolt.cpp). RD_SOLVER=0 runs it on the particles.
#                         Links the Jolt library of a CMake build of this tree
#                         (JOLT_LIB, default build/linux/libJolt.a).
#
# Then:
#   ./rdsim                               run all scenarios
#   RD_ONLY="anim death_run03" ./rdsim    run one
#
# rd_anims.h holds 17 real death animations run through forward kinematics. It
# is made from the retail Pak0.pk3 (MOHAA_PAK0, see skcdump.py) the first time
# the harness is built, and is not part of the tree.
#
# Don't run two builds at once in the same directory: they share solver.o.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${REPO:-$(cd "$HERE/../.." && pwd)}"
JOLT=0
if [ "${1:-}" = "--jolt" ]; then JOLT=1; shift; fi
SRC="${1:-$REPO/code/cgame/cg_ragdoll.cpp}"
INC="-I$HERE -I$REPO/code/cgame -I$REPO/code/qcommon -I$REPO/code/renderercommon -I$REPO/code/skeletor -I$REPO/code/tiki"

if [ ! -f "$HERE/rd_anims.h" ]; then
  python3 "$HERE/skcdump.py" --emit "$HERE/rd_anims.h"
fi

for f in q_math q_shared; do
  if [ ! -f "$HERE/$f.o" ] || [ "$REPO/code/qcommon/$f.c" -nt "$HERE/$f.o" ]; then
    gcc -O2 -c -o "$HERE/$f.o" "$REPO/code/qcommon/$f.c" $INC
  fi
done

g++ -O2 -c -o "$HERE/solver.o" "$SRC" $INC
g++ -O2 -c -o "$HERE/props.o" "$REPO/code/cgame/cg_props.cpp" $INC

if [ "$JOLT" = 0 ]; then
  g++ -O2 -o "$HERE/rdsim" "$HERE/rdsim.cpp" "$HERE/solver.o" "$HERE/props.o" "$HERE/q_math.o" "$HERE/q_shared.o" $INC -lm
  echo "built $HERE/rdsim from $SRC"
  exit 0
fi

# Jolt's headers must see the same configuration the library was built with
# (cmake/libraries/jolt.cmake), and the library is link time optimised, so the
# link is too.
JOLT_LIB="${JOLT_LIB:-$REPO/build/linux/libJolt.a}"
[ -f "$JOLT_LIB" ] || { echo "no Jolt library at $JOLT_LIB: build the tree with CMake first, or set JOLT_LIB"; exit 1; }
JFLAGS="-O2 -std=gnu++17 -msse4.2 -mpopcnt -mfpmath=sse -DNDEBUG -DJPH_OBJECT_LAYER_BITS=16 -DJPH_USE_SSE4_1 -DJPH_USE_SSE4_2 -I$REPO/code/thirdparty/JoltPhysics"
g++ $JFLAGS -c -o "$HERE/jolt_ragdoll.o" "$REPO/code/cgame/cg_physics_ragdoll.cpp" $INC
g++ $JFLAGS -c -o "$HERE/jolt_system.o" "$REPO/code/physics/phys_system.cpp" $INC
g++ $JFLAGS -c -o "$HERE/jolt_world.o" "$HERE/rdjolt.cpp" $INC
g++ -O2 -DRD_JOLT -flto=auto -pthread -o "$HERE/rdsim_jolt" "$HERE/rdsim.cpp" "$HERE/solver.o" "$HERE/props.o" \
    "$HERE/jolt_ragdoll.o" "$HERE/jolt_system.o" "$HERE/jolt_world.o" "$HERE/q_math.o" "$HERE/q_shared.o" \
    "$JOLT_LIB" $INC -lm
echo "built $HERE/rdsim_jolt from $SRC with the Jolt ragdoll"
