#!/bin/bash
# Repeats the pause > Quit Game > Yes path to the title and counts the runs that crashed.
# Usage: ./quitloop.sh <runs> [VAR=value ...]   (runs a copy of the build, so building may go on)
runs=$1; shift
cd /d/sotc-pc || exit 1
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
crashed=0
for ((n = 1; n <= runs; n++)); do
    cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n' >> "$f"
    EXE=sotc_ab.exe SAVE=1 ./test.sh 70 SOTC_SCRIPT="40:key=escape,43:key=down,44:key=down,45:key=down,47:key=enter,49:key=left,50:key=enter,66:quit" "$@" > /dev/null 2>&1
    mv "$f.bak" "$f"
    if grep -aqE "Unhandled Exception|Assertion Failed|Unreachable" testrun/user/log/shad_log.txt; then
        crashed=$((crashed + 1))
        echo "run $n: CRASH $(grep -aE -m1 "Unhandled Exception|Assertion Failed|Unreachable" testrun/user/log/shad_log.txt | cut -c1-150)"
    else
        echo "run $n: ok, flips $(grep -ac 'Flip: fps' testrun/user/log/shad_log.txt), late results dropped $(grep -a 'late compute results dropped' testrun/user/log/shad_log.txt | sed -E 's/^.*: ([0-9]+) late.*/\1/' | awk '{s+=$1} END{print s+0}')"
    fi
done
echo "crashed $crashed of $runs ($*)"
