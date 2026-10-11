#!/bin/bash
# Starts a colossus battle (a different one each run) and counts the runs that crashed while loading.
# Usage: ./quitloop.sh <runs> [VAR=value ...]   (runs a copy of the build, so building may go on)
runs=$1; shift
cd /d/sotc-pc || exit 1
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
crashed=0
for ((n = 1; n <= runs; n++)); do
    cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n' >> "$f"
    printf '+boss=%d\r\n' $((n % 16)) >> "$f"
    EXE=sotc_ab.exe SAVE=1 ./test.sh 44 SOTC_SCRIPT="40:quit" "$@" > /dev/null 2>&1
    mv "$f.bak" "$f"
    if grep -aqE "Unhandled Exception|Assertion Failed|Unreachable" testrun/user/log/shad_log.txt; then
        crashed=$((crashed + 1))
        echo "run $n: CRASH $(grep -aE -m1 "Unhandled Exception|Assertion Failed|Unreachable" testrun/user/log/shad_log.txt | cut -c1-150)"
    else
        echo "run $n: ok, flips $(grep -ac 'Flip: fps' testrun/user/log/shad_log.txt)"
    fi
done
echo "crashed $crashed of $runs ($*)"
