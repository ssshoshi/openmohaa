#!/usr/bin/env bash
# Builds the ragdoll harness against the solver in this tree.
#
#   ./build.sh            build ./rdsim from code/cgame/cg_ragdoll.cpp
#   ./build.sh some.cpp   build against a variant of the solver instead
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
g++ -O2 -o "$HERE/rdsim" "$HERE/rdsim.cpp" "$HERE/solver.o" "$HERE/props.o" "$HERE/q_math.o" "$HERE/q_shared.o" $INC -lm
echo "built $HERE/rdsim from $SRC"
