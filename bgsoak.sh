#!/bin/bash
# Background pipeline builds under stress: each run starts with an empty pipeline cache, so every
# pipeline is new and colour-only draws are built aside. Counts runs that stopped.
# Usage: ./bgsoak.sh <boss> <runs> [VAR=value ...]   (same steps as ab10.sh)
boss=$1; runs=$2; shift 2
cd /d/sotc-pc || exit 1
f="/d/Juegos PS4/CUSA08809T-mods/CommandLineArgs.txt"
c=testrun/user/cache
steps="50:key=w*5000,56:mouse=40*1200,58:key=q*2500,62:key=leftbutton,63:key=leftbutton,65:key=c,67:key=space"
steps="$steps,70:key=w*6000,77:mouse=-40*1500,80:key=r*2500,84:key=a*3000,88:key=d*3000,92:key=2,94:key=q*3000,105:quit"
mv "$c" "$c.keep_bgsoak" || exit 1
for ((n = 1; n <= runs; n++)); do
    rm -rf "$c"; mkdir -p "$c"
    cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n+boss=%d\r\n' "$boss" >> "$f"
    SAVE=1 ./test.sh 108 SOTC_SCRIPT="$steps" SOTC_PIPELINE_BACKGROUND=1 SOTC_SHADER_PREP=skip "$@" > /dev/null 2>&1
    mv "$f.bak" "$f"
    log=testrun/user/log/shad_log.txt
    if grep -aqE "Unhandled Exception|Assertion Failed|Unreachable" "$log"; then r=STOPPED; else r=ok; fi
    echo "run $n: $r, $(grep -ac 'Flip: fps' "$log") reports, $(grep -ac 'in the background' "$log") background builds, $(grep -aoE 'hitch [0-9]+ ms' "$log" | awk '$2>100' | wc -l) hitches over 100 ms"
done
rm -rf "$c"; mv "$c.keep_bgsoak" "$c"
