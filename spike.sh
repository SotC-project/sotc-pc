#!/bin/bash
# Long frames inside the 24 s benchmark walk, with or without the recording running.
# ./spike.sh <b0-15|world> <rec 0|1> [VAR=value ...]
# Prints the frames over 30 ms inside the walk and the renderer's account of those over 40 ms.
tag=$1; rec=$2; shift 2
cd /d/sotc-pc || exit 1
cap=${CAP:-E:/sotc-capture}
ft="$cap/frametimes.txt"; rm -f "$ft" "$cap"/rec_* "$cap"/aud_*
if [ "$tag" = world ]; then
    args=""; t=175
    lead="46:key=escape,48:key=down,49:key=down,50:key=down,51:key=enter,64:key=enter,82:key=enter,"
else
    args="+nointro +skipSplash=true +boss=${tag#b}"; t=50; lead=""
fi
walk="$t:key=w*6000,$((t + 8)):mouse=30*1500,$((t + 12)):key=w*8000,$((t + 21)):mouse=-30*1500"
env="SOTC_PERFHUD=1 SOTC_FRAMETIMES=$ft $*"
steps="${lead}${walk}"
if [ "$rec" = 1 ]; then
    env="$env SOTC_RECORD=$cap/rec_%p.raw SOTC_RECORD_FULL=1 SOTC_RECORD_FPS=30 SOTC_AUDIODUMP=$cap/aud_%p.f32"
    steps="${lead}$t:rec=ui,${walk},$((t + 24)):rec=stop"
fi
RUNENV="$env" BUILD=Release SCRIPT="$steps" ./bossarena.sh "$cap/unused.png" $((t + 34)) $args > /dev/null 2>&1
rm -f "$cap"/rec_* "$cap"/aud_*
# The quit step falls 29 s after the walk begins; the walk is the 24 s before the last 5.
awk -v tag="$tag" -v rec="$rec" '{ts[NR] = $1; gap[NR] = $2} END {
    end = ts[NR]; a = end - 29; b = end - 5; n = 0; over30 = 0; over40 = 0; after40 = 0; worst = 0; sum = 0
    for (i = 1; i <= NR; i++) {
        if (ts[i] >= a && gap[i] > 40) after40++
        if (ts[i] < a || ts[i] > b) continue
        n++; sum += gap[i]; if (gap[i] > 30) { over30++; list = list sprintf(" %.0f", gap[i]) }
        if (gap[i] > 40) over40++; if (gap[i] > worst) worst = gap[i]
    }
    printf "%s rec=%s: %d frames, mean %.1f FPS, worst %.0f ms, over 30 ms: %d (%s ), over 40 ms: %d\n", tag, rec, n, 1000 * n / sum, worst, over30, list, over40
    print after40 > "/tmp/spike_after"; print over40 > "/tmp/spike_in"
}' "$ft"
after=$(cat /tmp/spike_after); in=$(cat /tmp/spike_in)
[ "$in" -gt 0 ] && grep -a "hitch " testrun/user/log/shad_log.txt | tail -"$after" | head -"$in" | sed -E 's/^.*hitch /  hitch /'
exit 0
