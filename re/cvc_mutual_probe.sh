#!/bin/bash
# Isolated A/B targeting regression. No synthetic health deductions.
set -e
cd /d/sotc-pc
source re/cvc_base.sh 8
mode=${1:-mutual}
case "$mode" in
    mutual) suffix= ;;
    oneway) suffix=:oneway ;;
    *) printf 'Usage: %s [mutual|oneway]\n' "$0" >&2; exit 2 ;;
esac
s="$s,111:floor=238#0:27:240000,112:cvctarget=238#0:238#1:2:8$suffix"
for patches in "$NC" "$TGT2" "$CUR" "$DUAL" "$MGR"; do
    s="$s,113:${patches//,/,113:}"
done
s="$s,115:objlist=238#0:@785/30/-1050"
for t in 116 120 125 135 145 165 185 205 225 245 265 285 305; do
    s="$s,$t:objlist=238"
done
s="$s,150:shot,250:shot,300:shot,310:quit"
SAVE=usercopy ./test.sh 320 SOTC_SCRIPT="$s" SOTC_FREEPLAY='+shadow_noDamage=true'
python re/cvc/probe_result.py
