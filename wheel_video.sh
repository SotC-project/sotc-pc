#!/bin/bash
# Records the weapon wheel, the bow on the left mouse button and the Yes/No pointer in the
# Gaius arena, with the runtime's own frame and sound capture (sound folded to stereo as heard).
# Usage: ./wheel_video.sh <out.mp4>     (CAP=<folder on a fast disk>, default E:/sotc-capture)
out=$1
cap=${CAP:-E:/sotc-capture}
cd /d/sotc-pc || exit 1
mkdir -p "$cap" && rm -f "$cap"/rec_* "$cap"/aud_*
f="/d/Juegos PS4/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n+boss=2\r\n' >> "$f"
s="40:key=escape,43:key=down,44:key=down,45:key=down,47:key=enter,50:rec=ui"
s="$s,52:key=w*1500"
s="$s,55:key=g*2600,56:mouse=30/-60*700,60:key=leftbutton,61:key=leftbutton"   # Sword of the Sun
s="$s,64:key=g*2600,65:mouse=-60/-20*700,68:key=leftbutton*2500"             # Bow
s="$s,72:key=g*2600,73:mouse=-50/50*700,76:key=leftbutton*2000"              # Explosive Arrows
s="$s,80:key=g*2600,81:mouse=-30/70*700,84:key=leftbutton*2000"              # Harpoon of Thunder
s="$s,88:key=g*2600,89:mouse=0/-60*700"                                      # Sword
s="$s,93:key=escape,96:key=down,97:key=down,98:key=down,100:key=enter"       # Main Menu? dialog
s="$s,102:hover=0.46/0.54,104:hover=0.535/0.54,106:key=enter,108:key=escape"
s="$s,111:rec=stop,113:quit"
SAVE=1 ./test.sh 120 SOTC_SCRIPT="$s" SOTC_RECORD="$cap/rec_%p.raw" SOTC_RECORD_FULL=1 \
    SOTC_RECORD_FPS=30 SOTC_AUDIODUMP="$cap/aud_%p.f32" | tail -2
mv "$f.bak" "$f"
v=$(ls -t "$cap"/rec_*.raw | head -1)
pid=${v##*rec_}; pid=${pid%.raw}
read -r _ skip < "$v.ms"
ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 30 -i "$v" \
    -f f32le -ar 48000 -ac 2 -i "$cap/aud_$pid.f32" \
    -filter_complex "[1:a]atrim=start_sample=${skip:-0},asetpts=PTS-STARTPTS[a]" -map 0:v -map "[a]" -shortest \
    -c:v libx264 -preset medium -crf 17 -pix_fmt yuv420p -c:a aac -b:a 192k -movflags +faststart "$out"
ls -la "$out"
