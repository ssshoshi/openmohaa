#!/bin/bash
# usage: drag.sh binary  -> per-run jumps/worst/jerk and totals
B=${1:-./rdsim}; cd "$(dirname "$0")/.."
tj=0; tw=0; tk=0
for sc in "draped over a ledge" "onto a ledge sideways" "ledge, pitched back"; do for d in "-80 0 -30" "-120 0 -40" "-150 0 -80" "-100 0 0" "-150 20 -60" "-60 60 -40"; do
 l=$(RD_ONLY="$sc" RD_DRAG="2500 4 $d 4000" $B 2>/dev/null | grep "drag:")
 j=$(echo "$l" | sed 's/.*units \([0-9]*\),.*/\1/'); w=$(echo "$l" | sed 's/.*worst \([0-9.]*\),.*/\1/'); k=$(echo "$l" | sed 's/.*jerk \([0-9.]*\)/\1/')
 printf "%3s %5s %s | " $j $w $k
 tj=$((tj+j)); tw=$(echo "$tw+$w"|bc); tk=$(echo "$tk+$k"|bc)
done; echo; done
echo "TOTAL jumps $tj worst-sum $tw jerk-sum $tk"
