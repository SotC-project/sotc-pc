#!/bin/bash
# Continues the stored save and runs through the Shrine hall for a minute; reports late frame rates.
cd /d/sotc-pc || exit 1
SAVE=1 ./test.sh 160 SOTC_SCRIPT="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross,82:key=s*60000,125:shot,148:shot,150:quit" "$@" | tail -2
L=testrun/user/log/shad_log.txt
grep "Flip: fps" $L | tail -18 | sed -E 's/^.*fps //' | tr '\n' ' '; echo
grep "stats/3s" $L | tail -1 | sed -E 's/^.*stats\/3s: //' | awk '{f=$NF; printf "per frame: drawn %d indirect %d dispatch %d mirrors %d\n", $2/f, $4/f, $6/f, $(NF-2)/f}'
