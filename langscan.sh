#!/bin/bash
# Sets the game's text language to each one in turn (so many steps right of English on the
# Language page), continues the save, opens the pause menu and keeps a shot of it and the save.
# Usage: ./langscan.sh <first step> <last step>   (1 Finnish ... 14 Dutch)
cd /d/sotc-pc || exit 1
names=(x fi fr de it no pl pt ru es sv tr ar da nl)
for ((k = $1; k <= $2; k++)); do
    code=${names[$k]}
    s="16:key=enter*3500,24:key=enter,30:click=0.15/0.532,32:key=down,32.6:key=down,33.2:key=enter,35:key=down,35.5:key=down,36:key=down,36.5:key=down,37:key=down,37.7:key=enter"
    s=$(python -c "
s='$s'; t=40.0
for i in range($k):
    s+=',%.1f:key=right'%t; t+=0.7
s+=',%.1f:key=backspace,%.1f:key=enter,%.1f:key=backspace,%.1f:key=backspace,%.1f:click=0.15/0.301,%.1f:key=enter*3500,%.1f:key=enter*3500,%.1f:key=escape,%.1f:shotui'%(t+1,t+2.5,t+4.5,t+6,t+8,t+30,t+38,t+47,t+49.5)
print(s)")
    SAVE=userfull RUNENV="SOTC_PRELOAD_ASIDE=1" SCRIPT="$s" ./bossarena.sh testrun/unused_lang.png $((100 + k)) > /dev/null 2>&1
    shot=$(ls -t testrun/user/screenshots/*.png | head -1)
    cp "$shot" "testrun/pause_$code.png"
    d=$(ls -dt testrun/user/home/1000/savedata.bak* | head -1)
    rm -rf "testrun/user/home/1000/savedata.s_lang_$code"; cp -r "$d" "testrun/user/home/1000/savedata.s_lang_$code"
    echo "$code done"
done
