#!/bin/bash
# Does a colossus killed through its manager's health (as cvcbrawl's knockout does) collapse in its
# own arena? Free Play boot into boss $1's arena (default Gaius, 2), slot A = that colossus with only
# the MGR patch (manager capture), health 0 at KILLAT for 1 s, recorded.
set -e
cd /d/sotc-pc
boss=${1:-2}; secs=${2:-150}
source re/cvc_base.sh 8   # patch sets only
s="12:awaitmoved=238:200:1593ef8"
for t in $(seq 14 2 60); do
    s="$s,$t:ifset=1593ef8:ifzero=1593b08:cvctarget=238#0:238#0:$boss:$boss"
    s="$s,$t:ifset=1593b08:ifzero=1593ef0:${MGR//,/,$t:ifset=1593b08:ifzero=1593ef0:}"
    s="$s,$t:ifset=1593b08:pokeb=1593ef0:1"
done
k=${KILLAT:-70}
for d in 0 1 2 3 4 5 6 7 8 9; do s="$s,$k.$d:ifset=1593ef0:pokef=1593e00>50:0"; done
# The character's current motion (component +0x260 = object +0x380) around the death, and what it points at.
for d in $(seq 0 1 24); do s="$s,$((k - 2 + d)).5:dumpat=1593b08>380:8,$((k - 2 + d)).6:dumpat=1593b08>380>0:180"; done
[ -n "$NOREC" ] || s="$s,30:rec=start"
s="$s,$((secs - 5)):quit"
printf %s "$s" > testrun/cvc_script.txt
SAVE=usercopy ./test.sh $((secs + 10)) SOTC_SCRIPT="@D:/sotc-pc/testrun/cvc_script.txt" \
    SOTC_FREEPLAY="+nointro +skipSplash=true +boss=$boss +shadow_noDamage=true"
grep -E "awaitmoved|cvctarget|Unhandled" testrun/user/log/shad_log.txt | sed 's/.*ScriptTick: //' | uniq | tail
