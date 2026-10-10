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
    if grep -aq "Unhandled Exception" testrun/user/log/shad_log.txt; then
        crashed=$((crashed + 1))
        echo "run $n: CRASH $(grep -a -m1 'Unhandled Exception' testrun/user/log/shad_log.txt | grep -o '(.*) signals' | cut -c1-60)"
    else
        echo "run $n: ok, flips $(grep -ac 'Flip: fps' testrun/user/log/shad_log.txt)"
    fi
done
echo "crashed $crashed of $runs ($*)"
