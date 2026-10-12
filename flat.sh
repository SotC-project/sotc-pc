#!/bin/bash
# Frame-time steadiness in play: three minutes of scripted play in each given place.
# ./flat.sh "<b0-15|world> ..." [VAR=value ...]     (BUILD=Release by default, OUT=<name> tags files)
# Prints per place: frames, mean FPS, median and 99th percentile frame time, the mean change from
# one frame to the next, the frames over 25, 33 and 40 ms and the worst frame. The frame times stay
# in <capture folder>/flat_<name>_<place>.txt and the renderer's account of the long frames in
# flat_<name>_<place>_hitch.txt.
tags=$1; shift
cd /d/sotc-pc || exit 1
cap=${CAP:-E:/sotc-capture}; out=${OUT:-run}
for tag in $tags; do
    ft="$cap/flat_${out}_$tag.txt"; rm -f "$ft"
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
    RUNENV="SOTC_FRAMETIMES=$ft $*" BUILD=${BUILD:-Release} SCRIPT="${steps%,}" ./bossarena.sh "$cap/unused.png" $((t + 190)) $args > /dev/null 2>&1
    grep -aqE "Unhandled Exception|Assertion Failed|Unreachable" testrun/user/log/shad_log.txt && echo "$tag: FAULT in the log"
    grep -a "hitch " testrun/user/log/shad_log.txt | sed -E 's/^.*hitch /hitch /' > "${ft%.txt}_hitch.txt"
    # Play is the 180 s before the last 10.
    awk -v tag="$tag" '{ts[NR] = $1; gap[NR] = $2} END {
        end = ts[NR]; n = 0
        for (i = 1; i <= NR; i++) {
            if (ts[i] < end - 190 || ts[i] > end - 10) continue
            g[++n] = gap[i]; sum += gap[i]
            if (n > 1) { d = gap[i] - prev; jit += d < 0 ? -d : d }
            prev = gap[i]
            if (gap[i] > 25) o25++; if (gap[i] > 33) o33++; if (gap[i] > 40) o40++
            if (gap[i] > worst) worst = gap[i]
        }
        asort(g)
        printf "%s: %d frames, %.1f FPS, median %.1f ms, 99%% %.1f ms, 99.9%% %.1f ms, frame-to-frame %.2f ms, over 25/33/40 ms: %d/%d/%d, worst %.0f ms\n",
            tag, n, 1000 * n / sum, g[int(n * 0.5)], g[int(n * 0.99)], g[int(n * 0.999)], jit / (n - 1), o25, o33, o40, worst
    }' "$ft"
done
