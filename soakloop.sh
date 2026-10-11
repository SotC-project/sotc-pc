#!/bin/bash
# Three minutes in each colossus arena with scripted movement, camera turns, sword and bow, and
# a count of the runs in which the game or the runtime stopped.
# Usage: ./soakloop.sh <first boss 0-15> <last boss> [VAR=value ...]   (runs a copy of the build)
first=$1; last=$2; shift 2
cd /d/sotc-pc || exit 1
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
# Steps in time order: walk, turn, raise the sword, swing, roll, jump, aim the bow and shoot,
# then the same again further on; a look at the colossus now and then.
steps="50:key=w*5000,56:mouse=40*1200,58:key=q*2500,62:key=leftbutton,63:key=leftbutton,65:key=c,67:key=space"
steps="$steps,70:key=w*6000,77:mouse=-40*1500,80:key=r*2500,84:key=a*3000,88:key=d*3000,92:key=2,94:key=q*3000"
steps="$steps,95:key=leftbutton*1200,99:key=1,102:key=w*8000,111:mouse=40*2000,114:key=s*4000,119:key=space"
steps="$steps,122:key=w*8000,131:mousey=-20*800,133:key=leftbutton,135:key=leftbutton,138:key=e*3000"
steps="$steps,143:key=w*8000,152:mouse=-40*2000,156:key=r*2500,160:key=w*8000,170:key=c,172:key=w*8000"
steps="$steps,182:mouse=40*1500,186:key=q*2500,190:key=w*8000,200:key=space,204:key=w*6000,214:quit"
stopped=0; runs=0
for ((boss = first; boss <= last; boss++)); do
    cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n+boss=%d\r\n' "$boss" >> "$f"
    EXE=sotc_ab.exe SAVE=1 ./test.sh 218 SOTC_SCRIPT="$steps" "$@" > /dev/null 2>&1
    mv "$f.bak" "$f"
    runs=$((runs + 1))
    log=testrun/user/log/shad_log.txt
    flips=$(grep -ac 'Flip: fps' "$log")
    worst=$(grep -a 'Flip: fps' "$log" | sed -E 's/^.*worst ([0-9]+) ms.*/\1/' | sort -n | tail -1)
    if grep -aqE "Unhandled Exception|Assertion Failed|Unreachable" "$log"; then
        stopped=$((stopped + 1))
        echo "boss $boss: STOPPED after $flips reports: $(grep -aE -m1 'Unhandled Exception|Assertion Failed|Unreachable' "$log" | cut -c1-150)"
    else
        echo "boss $boss: ok, $flips reports, worst frame $worst ms, $(grep -a 'Flip: fps' "$log" | sed -E 's/^.*fps ([0-9.]+).*/\1/' | sort -n | sed -n '1p;$p' | tr '\n' ' ')FPS low/high over 3 s"
    fi
done
echo "stopped $stopped of $runs ($*)"
