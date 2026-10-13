#!/bin/bash
# Short repeated runs of one arena on two builds in turn, counting the runs that stopped.
# Usage: ./ab10.sh <boss> <runs each> <bin dir A> <bin dir B> [VAR=value ...]
boss=$1; runs=$2; a=$3; b=$4; shift 4
cd /d/sotc-pc || exit 1
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
steps="50:key=w*5000,56:mouse=40*1200,58:key=q*2500,62:key=leftbutton,63:key=leftbutton,65:key=c,67:key=space"
steps="$steps,70:key=w*6000,77:mouse=-40*1500,80:key=r*2500,84:key=a*3000,88:key=d*3000,92:key=2,94:key=q*3000,105:quit"
for ((n = 1; n <= runs; n++)); do
    for bin in "$a" "$b"; do
        cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n+boss=%d\r\n' "$boss" >> "$f"
        # A bin dir given as <dir>@capped runs without the unlocked frame mode line.
        case "$bin" in *@capped) sed -i -b '/TargetFramesPerSeconds/d' "$f"; bin=${bin%@capped}; tag=capped ;; *) tag=$(basename "$bin") ;; esac
        BIN="$bin" SAVE=1 ./test.sh 108 SOTC_SCRIPT="$steps" "$@" > /dev/null 2>&1
        mv "$f.bak" "$f"
        log=testrun/user/log/shad_log.txt
        if grep -aqE "Unhandled Exception|Assertion Failed|Unreachable" "$log"; then r=STOPPED; else r=ok; fi
        echo "$tag run $n: $r, $(grep -ac 'Flip: fps' "$log") reports"
    done
done
