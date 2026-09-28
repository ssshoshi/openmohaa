#!/bin/bash
# beam drops with a dump each: frames with a limb middle buried, frames with a joint buried
B=${1:-./rdsim}; Z=${2:-200}; cd "$(dirname "$0")/.."
tl=0; tj=0
for w in 6 10 16; do for sc in "draped over a ledge" "onto a ledge sideways" "ledge, pitched back"; do for xy in "0 0" "-10 0" "10 0" "0 -15" "-20 5"; do
 rm -f ragdoll_dump*.txt
 RD_DUMP=1 RD_ONLY="$sc" RD_BOX="-$w -200 70 $w 200 90" RD_STARTZ=$Z RD_STARTXY="$xy" $B >/dev/null 2>&1
 f=$(ls ragdoll_dump*.txt 2>/dev/null | head -1); [ -z "$f" ] && continue
 l=$(awk '/^F /{s=$7} /^L /{if(s>0 && $2!=0) n++} END{print n+0}' $f); j=$(awk '/^F /{s=$7} /^C /{if(s>0 && $4!=0) n++} END{print n+0}' $f)
 tl=$((tl+l)); tj=$((tj+j))
done; done; done
echo "limb-buried frames $tl  joint-buried frames $tj"
