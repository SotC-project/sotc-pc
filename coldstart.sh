#!/bin/bash
# Cold start timing in the repro profile: ./coldstart.sh <exe name> <max seconds> [VAR=value ...]
# A new exe name makes the graphics driver compile every saved pipeline again.
cd /d/sotc-pc/repro || exit 1
name=$1; max=$2; shift 2
for ((w = 0; w < 60; w++)); do
    [ "$(tasklist | grep -ciE "i3.exe|inFAMOUS")" = "0" ] && break
    sleep 5
done
if [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus|i3.exe|inFAMOUS|Sotc")" != "0" ]; then echo "REFUSED: a game instance is running"; exit 1; fi
cp "${SRC:-../runtime/Build/x64-Clang-RelWithDebInfo/shadps4.exe}" "$name" || exit 1
rm -f user/log/shad_log.txt user/log/hang_stacks.txt user/log/crash_stack.txt
start=$(date +%s)
env SOTC_MUTE=1 "$@" "./$name" > stdout.txt 2>&1 &
pid=$!
for ((i = 0; i < max; i++)); do
    kill -0 $pid 2>/dev/null || break
    grep -q "Preloaded" stdout.txt 2>/dev/null && [ "$(grep -c 'Flip: fps\|stats/3s' stdout.txt)" -gt 3 ] && break
    if [ $((i % 3)) = 0 ] && [ "$(tasklist | grep -ci ShadowOfTheColossus.exe)" != "0" ] && [ "$name" != "ShadowOfTheColossus.exe" ]; then echo "ABORTED: the user's game started"; break; fi
    sleep 1
done
echo "ran $(( $(date +%s) - start ))s, alive $(kill -0 $pid 2>/dev/null && echo yes || echo no)"
kill -9 $pid 2>/dev/null; for ((k = 0; k < 20; k++)); do [ "$(tasklist | grep -ci "$name")" = "0" ] && break; sleep 1; done
grep -h "Preloaded\|stale pipelines\|Critical" stdout.txt | cut -c1-200 | head -5
ls user/log
