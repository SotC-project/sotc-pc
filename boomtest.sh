#!/bin/bash
# Explosive arrows in the Gaius arena with the sound dumped, for A/B runs of the sound chain;
# the wheel is held open 4 s to see its slow motion. Usage: ./boomtest.sh <name> [VAR=value ...]
# Writes E:/sotc-capture/boom_<name>.f32 (stereo, 48 kHz) and, with REC=1, boom_<name>.mp4.
name=$1; shift
cap=${CAP:-E:/sotc-capture}
cd /d/sotc-pc || exit 1
mkdir -p "$cap" && rm -f "$cap"/rec_* "$cap"/aud_*
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n+boss=2\r\n' >> "$f"
s="40:key=escape,43:key=down,44:key=down,45:key=down,47:key=enter,50:rec=ui"
s="$s,52:key=g*2600,53:mouse=-50/50*700"                                      # Explosive Arrows
s="$s,56:key=leftbutton*1800,59:key=leftbutton*1800,62:key=leftbutton*1800,65:key=leftbutton*1800"
s="$s,69:key=g*4000,70:mouse=0/-60*700,74:key=w*3000"                         # slow motion, Sword
s="$s,79:rec=stop,80:quit"
rec=()
[ -n "$REC" ] && rec=(SOTC_RECORD="$cap/rec_%p.raw" SOTC_RECORD_FULL=1 SOTC_RECORD_FPS=30)
SAVE=1 ./test.sh 85 SOTC_SCRIPT="$s" SOTC_AUDIODUMP="$cap/aud_%p.f32" "${rec[@]}" "$@" | tail -2
mv "$f.bak" "$f"
a=$(ls -t "$cap"/aud_*.f32 | head -1)
mv "$a" "$cap/boom_$name.f32"
if [ -n "$REC" ]; then
    v=$(ls -t "$cap"/rec_*.raw | head -1)
    read -r _ skip < "$v.ms"
    ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 30 -i "$v" \
        -f f32le -ar 48000 -ac 2 -i "$cap/boom_$name.f32" \
        -filter_complex "[1:a]atrim=start_sample=${skip:-0},asetpts=PTS-STARTPTS[a]" -map 0:v -map "[a]" -shortest \
        -c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p -c:a aac -b:a 192k "$cap/boom_$name.mp4"
    rm -f "$v"
fi
