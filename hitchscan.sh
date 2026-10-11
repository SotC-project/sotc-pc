#!/bin/bash
# Long frames in play, by cause: three minutes of the soak's scripted play in each given place.
# ./hitchscan.sh "<b0-15|world> ..." [VAR=value ...]
tags=$1; shift
cd /d/sotc-pc || exit 1
cap=${CAP:-E:/sotc-capture}; ft="$cap/frametimes.txt"
for tag in $tags; do
    rm -f "$ft"
    if [ "$tag" = world ]; then
        args=""; t=175
        lead="46:key=escape,48:key=down,49:key=down,50:key=down,51:key=enter,64:key=enter,82:key=enter,"
    else
        args="+nointro +skipSplash=true +boss=${tag#b}"; t=50; lead=""
    fi
    steps="$lead"
    for ((k = 0; k < 9; k++)); do
        s=$((t + k * 20))
        steps="$steps$s:key=w*6000,$((s + 7)):mouse=40*1500,$((s + 9)):key=q*2000,$((s + 12)):key=w*5000,$((s + 18)):mouse=-30*1200,"
    done
    RUNENV="SOTC_FRAMETIMES=$ft $*" BUILD=Release SCRIPT="${steps%,}" ./bossarena.sh "$cap/unused.png" $((t + 190)) $args > /dev/null 2>&1
    # Play is the 180 s before the last 10.
    n=$(awk '{ts[NR] = $1; gap[NR] = $2} END {for (i = 1; i <= NR; i++) if (ts[i] >= ts[NR] - 190 && gap[i] > 40) n++; print n + 0}' "$ft")
    m=$(awk '{ts[NR] = $1; gap[NR] = $2} END {for (i = 1; i <= NR; i++) if (ts[i] > ts[NR] - 10 && gap[i] > 40) n++; print n + 0}' "$ft")
    grep -a "hitch " testrun/user/log/shad_log.txt | tail -"$n" | head -$((n - m)) | sed -E 's/^.*hitch /hitch /' > "$cap/hitch_$tag.txt"
    awk -v tag="$tag" '{
        ms = $2; pipes = $4; split($0, a, "uploaded [(]"); tex = a[2] + 0; split($0, b, "MB[)], "); buf = b[2] + 0
        if (pipes > 0) { kind = "pipeline" } else if (buf >= 32) { kind = "bigbuffer" } else if (tex >= 20) { kind = "texture" } else { kind = "other" }
        count[kind]++; if (ms > worst[kind]) worst[kind] = ms
    } END {printf "%s: %d frames over 40 ms:", tag, NR; for (k in count) printf " %s %d (worst %d ms)", k, count[k], worst[k]; print ""}' "$cap/hitch_$tag.txt"
done
