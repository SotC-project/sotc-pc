#!/bin/bash
# Five minutes of running on from the fourth colossus's start into ground not seen before, with
# the frame times: ./ride.sh <name> [VAR=value ...]   (BUILD=Release for the release build)
# Prints the frames over 25, 33, 40, 50 and 100 ms and the worst, and the long frames' causes.
name=$1; shift
cd /d/sotc-pc || exit 1
cap=${CAP:-E:/sotc-capture}; ft="$cap/ft_$name.txt"; rm -f "$ft"
steps="41:key=a*500,42:key=lshift*300,45:key=a*300,46:key=lshift*300,49:key=a*300,50:key=lshift*300"
for ((s = 54; s < 350; s += 20)); do
    steps="$steps,$s:key=w*19500,$((s + 2)):key=lshift*200,$((s + 5)):key=lshift*200,$((s + 8)):key=lshift*200,$((s + 10)):shot,$((s + 12)):key=lshift*200,$((s + 15)):key=d*500"
done
RUNENV="SOTC_FRAMETIMES=$ft SOTC_HITCH_MS=50 $*" SCRIPT="$steps" ./bossarena.sh "$cap/ride_$name.png" 360 +nointro +skipSplash=true +boss=3 > /dev/null 2>&1
grep -aqE "Unhandled Exception|Assertion Failed|Unreachable" testrun/user/log/shad_log.txt && echo "FAULT in the log"
awk '$1 > 54 && $1 < 350 {n++; if ($2 > 25) a++; if ($2 > 33) b++; if ($2 > 40) c++; if ($2 > 50) d++; if ($2 > 100) e++; if ($2 > w) w = $2; if ($2 > 45) printf "%.0f:%.0f ", $1, $2}
     END {printf "\n%d frames, over 25/33/40/50/100 ms: %d/%d/%d/%d/%d, worst %.0f ms\n", n, a, b, c, d, e, w}' "$ft"
grep -a "hitch " testrun/user/log/shad_log.txt | sed -E 's/^.*hitch /hitch /' | tail -6 | cut -c1-200
echo "pipelines compiled in play: $(grep -ac 'Compiling graphics pipeline' testrun/user/log/shad_log.txt) ($(grep -ac 'in the background' testrun/user/log/shad_log.txt) in the background)"
