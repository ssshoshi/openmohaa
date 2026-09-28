#!/bin/bash
# drops from height onto beams of several widths; sums spring, whips; counts hung
B=${1:-./rdsim}; Z=${2:-200}; cd "$(dirname "$0")/.."
tg=0; tu=0; tw=0; hung=0; n=0; worst=0
for w in 6 10 16; do for sc in "draped over a ledge" "onto a ledge sideways" "ledge, pitched back"; do for xy in "0 0" "-10 0" "10 0" "0 -15" "-20 5"; do
 out=$(RD_ONLY="$sc" RD_BOX="-$w -200 70 $w 200 90" RD_STARTZ=$Z RD_STARTXY="$xy" $B 2>/dev/null)
 g=$(echo "$out" | grep -a "spring:" | sed 's/.*gained \([0-9]*\) u.*/\1/'); u=$(echo "$out" | grep -a "spring:" | sed 's/.*upward \([0-9]*\) u.*/\1/')
 w2=$(echo "$out" | grep -a "whips:" | awk '{print $2}'); h=$(echo "$out" | grep -a "body centre at height" | sed 's/.*height //')
 tg=$((tg+g)); tu=$((tu+u)); tw=$((tw+w2)); n=$((n+1)); [ $g -gt $worst ] && worst=$g
 if (( $(echo "$h > 20 && $h < 82" | bc) )); then hung=$((hung+1)); fi
 [ -n "$VERBOSE" ] && echo "w$w $sc $xy gain $g up $u whips $w2 h $h"
done; done; done
echo "runs $n  spring gain sum $tg (worst $worst)  up sum $tu  whips $tw  hung $hung"
