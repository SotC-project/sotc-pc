#!/bin/bash
# Colossus vs colossus as a player sees it, with the menus: title menu > Free Play > the
# "Colossus vs Colossus" row > Begin Battle > the battle (the script built into the program) >
# knockout > back to a plain start. Three game instances (menu, battle, plain start), each
# recording its picture and sound with the runtime's own capture; joined afterwards.
# Usage: ./re/cvc_ui_video.sh <out.mp4>     (CAP=<folder on a fast disk>, default E:/sotc-capture/ui)
out=$1
cap=${CAP:-E:/sotc-capture/ui}
cd /d/sotc-pc/testrun || exit 1
# Same rules as test.sh: never with a game running, one game on the GPU at a time.
if [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus")" != "0" ]; then echo "REFUSED: a game instance is running"; exit 1; fi
jc4_busy() { [ -e /d/jc4-tf2/GPU_RUNNING ] || [ "$(tasklist | grep -ci "JustCause4.exe")" != "0" ]; }
for ((w = 0; w < 360 && $(jc4_busy && echo 1 || echo 0); w++)); do sleep 5; done
if jc4_busy; then echo "REFUSED: JC4 is using the GPU"; exit 1; fi
lock=/d/sotc-pc/testrun/.run_lock
for ((w = 0; w < 360; w++)); do mkdir "$lock" 2> /dev/null && break; sleep 5; done
[ -d "$lock" ] && [ "$w" -lt 360 ] || { echo "REFUSED: another test run holds the lock"; exit 1; }
touch /d/sotc-pc/GPU_RUNNING
trap 'rm -f /d/sotc-pc/GPU_RUNNING; rmdir "$lock"' EXIT
sleep 5
if jc4_busy; then echo "REFUSED: JC4 started at the same time"; exit 1; fi
mkdir -p "$cap" && rm -f "$cap"/ui_* "$cap"/aud_* "$cap"/part_* "$cap"/battle_log.txt
sd=user/home/1000/savedata
marker=user/home/1000/.test_owns_savedata
if [ -e "$marker" ]; then [ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
elif [ -d "$sd" ]; then [ -d "$sd.user" ] && mv "$sd.user" "$sd.user.prev$(date +%s)"; mv "$sd" "$sd.user"; fi
touch "$marker"
cp -r "$sd.fixture" "$sd"
# First instance (+nointro: straight into the field): pause > Quit Game > Yes to the title menu,
# then recorded: down to Free Play, open it, the Colossus vs Colossus row on, Begin Battle.
# (DELAY seconds later when the start is slow; the field took over a minute to come up once.)
d=${DELAY:-30}
at() { python -c "print($1 + $d)"; }
first="$(at 40):key=escape,$(at 43):key=down,$(at 44):key=down,$(at 45):key=down,$(at 47):key=enter,$(at 49):key=left,$(at 50):key=enter"
# (Below the row: First / Second Colossus, Arena, then Begin Battle; OPPONENT_STEPS times right on
# Second Colossus picks another opponent for Gaius (1 Valus, 2 Quadratus, 3 Phaedra, 4 Argus; 0
# Basaran), ARENA_STEPS times right on Arena the other's arena.)
first="$first,$(at 62):rec=ui,$(at 65):key=down,$(at 66.5):key=down,$(at 68):key=down,$(at 70):key=enter,$(at 73):key=right"
first="$first,$(at 75.5):key=down,$(at 76.5):key=down"
for ((i = 0; i < ${OPPONENT_STEPS:-0}; i++)); do first="$first,$(at "77 + $i * 0.5"):key=right"; done
first="$first,$(at 79.5):key=down"
for ((i = 0; i < ${ARENA_STEPS:-0}; i++)); do first="$first,$(at "80 + $i * 0.3"):key=right"; done
first="$first,$(at 80.5):key=down,$(at 83):key=enter"
# Second instance: the battle, recorded from its start. Third: the plain start it goes back to.
battle="$cap/battle.txt"
# (Ended 2 s after the knockout's banner sends it back to the title, 0x1593b68 marking that; the third
# instance shows the title. QUIT_AT: at the latest.)
{ cat "${BATTLE:-../runtime/src/resources/cvc_gaius_vs_basaran.txt}"; printf ",2:rec=ui,${QUIT_AT:-600}:quit"
  for ((t = 30; t < ${QUIT_AT:-600}; t += 2)); do printf ",$t:ifset=1593b68:quit"; done; } > "$battle"   # BATTLE=<file>: another pair (cvc_arena.sh PRODUCT=)
third="2:rec=ui,${TAIL:-45}:rec=stop,$((${TAIL:-45} + 2)):quit"
log=user/log/shad_log.txt
env SOTC_HEADLESS=1 SOTC_MUTE=1 SOTC_LAUNCH_EXTRA="+nointro" SOTC_SCRIPT="$first" \
    SOTC_SCRIPT_NEXT="@$battle" SOTC_SCRIPT_NEXT2="$third" \
    SOTC_RECORD="$cap/ui_%p.raw" SOTC_RECORD_FPS=30 SOTC_AUDIODUMP="$cap/aud_%p.f32" \
    ../runtime/Build/x64-Clang-RelWithDebInfo/shadps4.exe -f false --game "D:/Games/CUSA08809T/eboot.bin" > /dev/null 2>&1 &
sleep 20
# Until no instance is left (each hands over to the next); 15 minutes at most.
for ((i = 0; i < 900; i++)); do
    [ "$(tasklist | grep -ci "shadps4.exe")" = "0" ] && break
    [ "$(tasklist | grep -ci ShadowOfTheColossus)" != "0" ] && { echo "ABORTED: the user's game started"; break; }
    grep -aqE "Unhandled Exception" "$log" 2>/dev/null && { echo "CRASH"; grep -aE "Unhandled Exception|guest stack" "$log" | head -3; }
    # The battle's log (each instance starts its own): kept while it holds the fight.
    # (Only ever by a longer one: the next instance's new log, or one read mid-write, is shorter.)
    if grep -aq "cvcbrawl" "$log" 2>/dev/null &&
        [ "$(stat -c %s "$log")" -ge "$(stat -c %s "$cap/battle_log.txt" 2>/dev/null || echo 0)" ]; then
        cp "$log" "$cap/battle_log.txt"
    fi
    sleep 1
done
taskkill //IM shadps4.exe //F > /dev/null 2>&1
sleep 2
[ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
[ -d "$sd.user" ] && cp -r "$sd.user" "$sd"
rm -f "$marker"
ls -la --time-style=long-iso "$cap"
# One part per instance, oldest first: picture plus the sound from the sample the picture began at.
n=0; parts=()
for v in $(ls -tr "$cap"/ui_*.raw); do
    pid=${v##*ui_}; pid=${pid%.raw}
    read -r _ skip < "$v.ms"
    a="$cap/aud_$pid.f32"
    part="$cap/part_$n.mp4"
    ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 960x540 -r 30 -i "$v" \
        -f f32le -ar 48000 -ac 2 -i "$a" \
        -filter_complex "[1:a]atrim=start_sample=${skip:-0},asetpts=PTS-STARTPTS[a]" -map 0:v -map "[a]" -shortest \
        -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k "$part" && parts+=("$part")
    echo "part $n: pid $pid, sound skipped ${skip:-0} samples"
    n=$((n + 1))
done
inputs=(); chain=""
for i in "${!parts[@]}"; do inputs+=(-i "${parts[$i]}"); chain="$chain[$i:v][$i:a]"; done
ffmpeg -y -loglevel error "${inputs[@]}" -filter_complex "${chain}concat=n=${#parts[@]}:v=1:a=1[v][a]" \
    -map "[v]" -map "[a]" -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k \
    -movflags +faststart "$out" && ls -la "$out"
