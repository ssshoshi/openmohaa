#!/bin/bash
B=${1:-./rdsim}; cd "$(dirname "$0")/.."
for c in "onto a ledge sideways|0 -200 14 200 200 40|11" "onto a ledge sideways|-200 0 14 200 200 40|0" "onto a ledge sideways|-200 -200 14 0 200 40|0" "ledge, pitched back|-200 -200 14 200 0 40|0" "ledge, pitched back|-200 -200 14 200 0 40|6" "ledge, pitched back|-200 -200 14 200 0 40|21" "draped over a ledge|-200 -200 14 0 200 40|0" "ledge, pitched back|-200 -200 14 0 200 40|0"; do
 IFS='|' read sc bx j <<< "$c"
 printf "%-22s %-24s j%-2s " "$sc" "$bx" $j; RD_ONLY="$sc" RD_STARTZ=6 RD_BOX="$bx" RD_HANG="3000 100 $j" $B 2>/dev/null | grep -a -E "stretch:|whips" | sed 's/ \+/ /g; s/joint steps over 4 units off the body, //; s/its length, //' | tr '\n' ' '; echo
done
