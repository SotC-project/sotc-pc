#!/bin/bash
# Benchmark video: the three benchmark scenes under the given presets, about 20 s each, with the
# runtime's own performance readout in the picture and the game's sound.
# ./bench_video.sh <out.mp4> [preset ...]     (default: medium high; CAP=<folder on a fast disk>)
# Frames come from the runtime's own capture at 1080p30 (measured cost: about 2% of frame rate).
out=$1; shift
presets=${*:-medium high}
cap=${CAP:-E:/sotc-capture}
cd /d/sotc-pc || exit 1
game="D:\\Games\\CUSA08809T"
cfg=testrun/user/config.json
cp $cfg $cfg.bench_before
cp "/d/Games/CUSA08809T-mods/CommandLineArgs.txt" testrun/CommandLineArgs.bench_before.txt
cp testrun/user/sotc_graphics.json testrun/user/sotc_graphics.bench_before.json
mkdir -p "$cap" && rm -f "$cap"/rec_* "$cap"/aud_* "$cap"/clip_*

clips=()
# scene label, file tag, preset, launch args, script lead-in
clip() {
    local label=$1 tag=$2 preset=$3 args=$4 lead=$5
    for ((w = 0; w < 120; w++)); do
        [ "$(tasklist | grep -ciE 'ShadowOfTheColossus|i3.exe|shadps4')" = "0" ] && break
        sleep 5
    done
    rm -f "$cap"/rec_* "$cap"/aud_*
    local walk="50:key=w*6000,58:mouse=30*1500,62:key=w*8000,71:mouse=-30*1500"
    local name="${label}_-_${preset^}_preset_-_1080p,_no_frame_limit"
    RUNENV="SOTC_PERFHUD=1 SOTC_PERFHUD_LABEL=$name SOTC_PERFHUD_NOTE=Ryzen_7_5700X3D,_Radeon_RX_9060_XT_-_recording_costs_about_2% SOTC_RECORD=$cap/rec_%p.raw SOTC_RECORD_FULL=1 SOTC_RECORD_FPS=30 SOTC_AUDIODUMP=$cap/aud_%p.f32" \
        BUILD=Release SCRIPT="${lead}50:rec=ui,${walk},74:rec=stop" \
        ./bossarena.sh "$cap/unused.png" 84 $args 2>&1 | grep -E "ran|REFUSED" | sed "s/^/$preset $tag: /"
    grep -a "Flip: fps" testrun/user/log/shad_log.txt | sed -E 's/^.*fps ([0-9.]+).*/\1/' | tail -9 | head -7 | tr '\n' ' '; echo
    local v; v=$(ls -t "$cap"/rec_*.raw 2>/dev/null | head -1)
    [ -z "$v" ] && { echo "$preset $tag: no recording"; return; }
    local pid=${v##*rec_}; pid=${pid%.raw}
    local skip; read -r _ skip < "$v.ms"
    local part="$cap/clip_${preset}_${tag}.mp4"
    ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 30 -i "$v" \
        -f f32le -ar 48000 -ac 2 -i "$cap/aud_$pid.f32" \
        -filter_complex "[1:a]atrim=start_sample=${skip:-0},asetpts=PTS-STARTPTS,volume=-5dB,afade=t=in:d=0.4[a]" \
        -map 0:v -map "[a]" -shortest -c:v libx264 -preset medium -crf 17 -pix_fmt yuv420p -c:a aac -b:a 192k "$part" &&
        clips+=("$part")
    rm -f "$cap"/rec_* "$cap"/aud_*
}

for preset in $presets; do
    pwsh -NoProfile -File testrun/Settings.ps1 -GameDir "$game" -Preset $preset -Fps uncapped -VSync off > /dev/null
    clip "Gaius_arena" gaius $preset "+nointro +skipSplash=true +boss=2" ""
    clip "Field_with_Agro" field $preset "+nointro +skipSplash=true +boss=3" "41:key=a*500,42:key=lshift*300,45:key=a*300,46:key=lshift*300,49:key=a*300,"
    clip "Valus_arena" valus $preset "+nointro +skipSplash=true +boss=0" ""
done

cp $cfg.bench_before $cfg
cp testrun/CommandLineArgs.bench_before.txt "/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp testrun/user/sotc_graphics.bench_before.json testrun/user/sotc_graphics.json

if [ "${#clips[@]}" -gt 0 ]; then
    inputs=(); filter=""
    for i in "${!clips[@]}"; do inputs+=(-i "${clips[$i]}"); filter="$filter[$i:v][$i:a]"; done
    ffmpeg -y -loglevel error "${inputs[@]}" -filter_complex "${filter}concat=n=${#clips[@]}:v=1:a=1[v][a]" \
        -map "[v]" -map "[a]" -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k \
        -movflags +faststart "$out" && ls -la "$out"
fi
echo "clips: ${#clips[@]}"
