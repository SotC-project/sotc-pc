#!/bin/bash
# CvC with damage from real attack contact (no timer damage). Isolated test profile.
# Waits for both colossi (list world+0x238) before patching; 0x1593ef0 marks "set up".
# ./re/cvc_hits_probe.sh [mutual|oneway] [seconds]
set -e
cd /d/sotc-pc
source re/cvc_base.sh 8
source re/cvc_gate.sh 8
mode=${1:-mutual}
secs=${2:-420}
case "$mode" in
    mutual) suffix= ;;
    oneway) suffix=:oneway ;;
    *) printf 'Usage: %s [mutual|oneway] [seconds]\n' "$0" >&2; exit 2 ;;
esac
ATTACK=$(python re/cvc/attack_patch.py)
eval "$(python re/cvc/target_patch.py)"
# Keep TGT2's object-update wrapper (0x746ee0 sets the slot to the object being updated).
TGT4="$(echo "$TGT2" | tr , '
' | grep -E '^patch=(1593a00|1593a40|746ee0):' | paste -sd,),$TGT4"
ready="ifset=1593b08:ifset=1593b10:ifzero=1593ef0"
for t in $(seq 112 6 330); do
    s="$s,$t:ifzero=1593ef0:cvctarget=238#0:238#1:2:8$suffix"
    u=$((t + 1))
    PATCHSETS=${PATCHSETS:-NC TGT2 CUR DUAL MGR}
    for name in $PATCHSETS ATTACK; do
        patches=${!name}
        s="$s,$u:$ready:${patches//,/,$u:$ready:}"
    done
    s="$s,$u:$ready:floor=238#0:27:300000"
    s="$s,$u:$ready:objlist=238#0:@785/30/-1050"
    s="$s,$u:$ready:cvchits=300:20:15:900"
    s="$s,$u:ifset=1593b08:ifset=1593b10:pokeb=1593ef0:1"
done
for t in $(seq 120 15 $((secs - 20))); do
    s="$s,$t:ifset=1593ef0:objlist=238"
done
for t in 150 200 250 300; do
    [ "$t" -lt "$secs" ] && s="$s,$t:shot"
done
s="$s,$((secs - 10)):quit"
printf %s "$s" > testrun/cvc_script.txt
SAVE=usercopy ./test.sh $((secs + 10)) SOTC_SCRIPT="@D:/sotc-pc/testrun/cvc_script.txt" SOTC_FREEPLAY='+shadow_noDamage=true'
grep -E "cvctarget:|cvchits|objlist move|Unhandled Exception" testrun/user/log/shad_log.txt | cut -c60-240 | grep -v "not ready" | tail -40
python re/cvc/probe_result.py || true
