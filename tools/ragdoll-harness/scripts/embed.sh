#!/bin/bash
# bodies starting across a beam, partly inside it: peak level speed of the centre (a kick out of the beam), whips
B=${1:-./rdsim}; cd "$(dirname "$0")/.."
tp=0; tw=0; n=0; worst=0
for bz in "30 38" "28 36" "33 41"; do for sc in "draped over a ledge" "onto a ledge sideways" "ledge, pitched back"; do for xy in "0 0" "-8 0" "8 0" "0 -10" "-14 6"; do
 read z0 z1 <<< "$bz"
 out=$(RD_ONLY="$sc" RD_BOX="-5 -200 $z0 5 200 $z1" RD_STARTZ=36 RD_STARTXY="$xy" $B 2>/dev/null)
 p=$(echo "$out" | grep -a "spring:" | sed 's/.*peak level speed //'); w2=$(echo "$out" | grep -a "whips:" | awk '{print $2}')
 tp=$((tp+p)); tw=$((tw+w2)); n=$((n+1)); [ $p -gt $worst ] && worst=$p
 [ -n "$VERBOSE" ] && echo "beam $bz $sc $xy peak $p whips $w2"
done; done; done
echo "runs $n  peak level speed sum $tp (worst $worst)  whips $tw"
