#!/bin/bash
# Headless test run: ./test.sh <seconds> [VAR=value ...]. Parks any save so every run starts clean.
# EXE=<name> runs a copy of the build under another file name.
cd /d/sotc-pc/testrun || exit 1
secs=$1; shift
# Never start while a game instance is already running (the user may be playing): this script
# swaps the save folder under it.
if [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus")" != "0" ]; then echo "REFUSED: a game instance is running"; exit 1; fi
# Another port (i3.exe) is tested on this PC too; two instances do not fit in memory. Wait for
# its short runs to end rather than starving both.
for ((w = 0; w < 60; w++)); do
    [ "$(tasklist | grep -ci "i3.exe")" = "0" ] && break
    sleep 5
done
if [ "$(tasklist | grep -ci "i3.exe")" != "0" ]; then echo "REFUSED: i3.exe is running"; exit 1; fi
# The script clock starts at the first frame, which the pipeline cache preload delays.
secs=$((secs + 30))
sd=user/home/1000/savedata
# The live save belongs to the user. It is set aside as savedata.user for the run and put back
# afterwards; the marker tells a save left behind by an interrupted test run from theirs.
marker=user/home/1000/.test_owns_savedata
if [ -e "$marker" ]; then
    [ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
elif [ -d "$sd" ]; then
    [ -d "$sd.user" ] && mv "$sd.user" "$sd.user.prev$(date +%s)"
    mv "$sd" "$sd.user"
fi
touch "$marker"
# SAVE=1 starts from the stored save (new game already begun) instead of a clean profile.
# SAVE=field starts from the save made in the field outside the Shrine.
[ -n "$SAVE" ] && cp -r "$sd.${SAVE/1/fixture}" "$sd"
bin=${BIN:-../runtime/Build/x64-Clang-${BUILD:-RelWithDebInfo}}
exe=$bin/shadps4.exe
if [ -n "$EXE" ]; then
    cp "$exe" "$bin/$EXE" && exe=$bin/$EXE
fi
env SOTC_HEADLESS=1 ${UNMUTE:-SOTC_MUTE=1} "$@" "$exe" -f false --game "D:/Games/CUSA08809T/eboot.bin" > /dev/null 2>&1 &
pid=$!
# Poll instead of `timeout`: a process stuck inside the GPU driver never exits, and waiting on
# it would hang this script as well.
for ((i = 0; i < secs; i++)); do
    kill -0 $pid 2>/dev/null || break
    # Give way at once when the user starts their own game.
    if [ $((i % 3)) = 0 ] && [ "$(tasklist | grep -ci ShadowOfTheColossus)" != "0" ]; then echo "ABORTED: the user's game started"; break; fi
    sleep 1
done
if kill -0 $pid 2>/dev/null; then
    kill -9 $pid 2>/dev/null
    sleep 2
    kill -0 $pid 2>/dev/null && echo "STUCK: process $pid did not exit"
fi
# Put the user's save back.
[ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
[ -d "$sd.user" ] && cp -r "$sd.user" "$sd"
rm -f "$marker"
echo "ran ${i}s, stats $(grep -c 'stats/3s' user/log/shad_log.txt), flips $(grep -c 'Flip: fps' user/log/shad_log.txt)"
if grep -q "Critical" user/log/shad_log.txt; then
    # Keep the log of every crashed run for later study.
    mkdir -p user/log/crashes && cp user/log/shad_log.txt "user/log/crashes/$(date +%m%d_%H%M%S).txt"
fi
grep -n "Critical" -A1 user/log/shad_log.txt | head -4 | cut -c1-300
