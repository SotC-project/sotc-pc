#!/bin/bash
# Records the free play flow with sound, using the runtime's own frame and audio capture (no
# screen recorder): title menu > Free Play > options > Begin Battle > the battle. The game
# restarts itself for the battle, so each of the two processes writes its own picture and sound
# files, which are joined afterwards.
# Usage: ./freeplay_video.sh <out.mp4>     (CAP=<folder on a fast disk>, default E:/sotc-capture)
out=$1
cap=${CAP:-E:/sotc-capture}
cd /d/sotc-pc/testrun || exit 1
for ((w = 0; w < 120; w++)); do [ "$(tasklist | grep -ci "i3.exe")" = "0" ] && break; sleep 5; done
if [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus|i3.exe|SnippingTool")" != "0" ]; then echo "REFUSED: a game instance or the screen recorder is running"; exit 1; fi
mkdir -p "$cap" && rm -f "$cap"/rec_* "$cap"/aud_* "$cap"/part_*
sd=user/home/1000/savedata
marker=user/home/1000/.test_owns_savedata
if [ -e "$marker" ]; then [ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
elif [ -d "$sd" ]; then [ -d "$sd.user" ] && mv "$sd.user" "$sd.user.prev$(date +%s)"; mv "$sd" "$sd.user"; fi
touch "$marker"
cp -r "$sd.fixture" "$sd"
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n' >> "$f"
# First instance: into the field, pause > Quit Game > Yes to reach the title menu, then record:
# down to Free Play, open it, change a few options, Begin Battle.
first="40:key=escape,43:key=down,44:key=down,45:key=down,47:key=enter,49:key=left,50:key=enter"
first="$first,68:rec=ui,71:key=down,72:key=down,73:key=down,75:key=enter"
# Colossus to Gaius, Difficulty to Hard, Time Attack on (the page then offers nothing else), Begin Battle.
first="$first,78:key=right,79:key=right,81:key=down,82:key=right,84:key=down,85:key=right"
first="$first,89:key=down,92:key=enter,96:rec=stop,101:quit"
# Second instance (the battle): record from the start; once Wander can move, a short run toward
# the colossus, sword raised to the light, camera turns, the view of the colossus, swings, a roll
# and a jump. The moves are short so he stays on the platform.
second="4:rec=ui,52:mouse=-40*1200,54:key=r*2500,57:key=w*2000,60:key=leftbutton,61:key=leftbutton,62:key=leftbutton"
second="$second,64:key=a*700,66:mouse=40*900,68:key=space,70:key=q*2500,73:mouse=-40*900,76:key=w*1500,80:key=leftbutton"
second="$second,84:key=q*2000,90:key=s*1500,96:key=leftbutton"
# The run should end by itself when the colossus kills Wander. If he is still alive at the end,
# the test command ends it as a defeat; the log line "Challenge lost" tells which happened.
second="$second,123:chal=dead,129:rec=stop,131:quit"
env SOTC_HEADLESS=1 SOTC_MUTE=1 SOTC_SCRIPT="$first" SOTC_SCRIPT_NEXT="$second" \
    SOTC_RECORD="$cap/rec_%p.raw" SOTC_RECORD_FULL=1 SOTC_RECORD_FPS=30 SOTC_AUDIODUMP="$cap/aud_%p.f32" \
    ../runtime/Build/x64-Clang-RelWithDebInfo/shadps4.exe -f false --game "D:/Games/CUSA08809T/eboot.bin" > /dev/null 2>&1 &
sleep 20
for ((i = 0; i < 300; i++)); do
    [ "$(tasklist | grep -ci "shadps4.exe")" = "0" ] && break
    [ "$(tasklist | grep -ci ShadowOfTheColossus)" != "0" ] && { echo "ABORTED: the user's game started"; break; }
    sleep 1
done
taskkill //IM shadps4.exe //F > /dev/null 2>&1
sleep 2
mv "$f.bak" "$f"
[ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
[ -d "$sd.user" ] && cp -r "$sd.user" "$sd"
rm -f "$marker"
ls -la --time-style=long-iso "$cap"
# One part per process, oldest first: picture plus the sound from the sample the picture began at.
n=0; parts=()
for v in $(ls -tr "$cap"/rec_*.raw); do
    pid=${v##*rec_}; pid=${pid%.raw}
    read -r _ skip < "$v.ms"
    a="$cap/aud_$pid.f32"
    part="$cap/part_$n.mp4"
    ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 30 -i "$v" \
        -f f32le -ar 48000 -ac 2 -i "$a" \
        -filter_complex "[1:a]atrim=start_sample=${skip:-0},asetpts=PTS-STARTPTS[a]" -map 0:v -map "[a]" -shortest \
        -c:v libx264 -preset medium -crf 17 -pix_fmt yuv420p -c:a aac -b:a 192k "$part" && parts+=("$part")
    echo "part $n: pid $pid, sound skipped ${skip:-0} samples"
    n=$((n + 1))
done
if [ "${#parts[@]}" = "2" ]; then
    joined="$cap/joined.mp4"
    ffmpeg -y -loglevel error -i "${parts[0]}" -i "${parts[1]}"         -filter_complex "[0:v][0:a][1:v][1:a]concat=n=2:v=1:a=1[v][a]" -map "[v]" -map "[a]"         -c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p -c:a aac -b:a 192k "$joined"
    # The black wait after Begin Battle (restart and load) is cut to about four seconds: two kept
    # after the picture goes black, two before it returns, with the sound faded in at the join.
    read -r bs be < <(ffmpeg -hide_banner -nostats -i "$joined" -vf "blackdetect=d=5:pix_th=0.06" -an -f null - 2>&1 |
        sed -n 's/.*black_start:\([0-9.]*\) black_end:\([0-9.]*\).*/\1 \2/p' | head -1)
    echo "black wait from ${bs:-?} s to ${be:-?} s"
    if [ -n "$bs" ] && [ -n "$be" ]; then
        a=$(python -c "print($bs + 2)"); b=$(python -c "print($be - 2)")
        ffmpeg -y -loglevel error -i "$joined" -filter_complex             "[0:v]trim=0:$a,setpts=PTS-STARTPTS[v0];[0:a]atrim=0:$a,asetpts=PTS-STARTPTS[a0];[0:v]trim=start=$b,setpts=PTS-STARTPTS[v1];[0:a]atrim=start=$b,asetpts=PTS-STARTPTS,afade=t=in:d=0.6[a1];[v0][a0][v1][a1]concat=n=2:v=1:a=1[v][a]"             -map "[v]" -map "[a]" -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart "$out" && ls -la "$out"
    else
        cp "$joined" "$out" && ls -la "$out"
    fi
else
    echo "expected two parts, got ${#parts[@]}"
fi
