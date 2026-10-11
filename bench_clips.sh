#!/bin/bash
# Benchmark clips, one file per place: 24 s of scripted walking with the runtime's performance
# readout in the picture and the game's sound, at 1280x720 and under 15 MB each.
# ./bench_clips.sh <out folder> <preset> <tag> [tag ...]     (CAP=<folder on a fast disk>)
# FULL=<folder> also keeps each clip at 1920x1080.
# A tag is b<0-15> for a colossus arena, or "world" for Continue from the test save.
# Each clip adds a line to <out folder>/benchmark_clips.txt: place, preset, mean and lowest FPS
# of the 3 s reports that fall inside the recording.
outdir=$1; preset=$2; shift 2
cap=${CAP:-E:/sotc-capture}
cd /d/sotc-pc || exit 1
game="D:\\Games\\CUSA08809T"
cfg=testrun/user/config.json
cp $cfg $cfg.bench_before
cp "/d/Games/CUSA08809T-mods/CommandLineArgs.txt" testrun/CommandLineArgs.bench_before.txt
cp testrun/user/sotc_graphics.json testrun/user/sotc_graphics.bench_before.json
mkdir -p "$cap" && rm -f "$cap"/rec_* "$cap"/aud_*
names=(Valus Quadratus Gaius Phaedra Avion Barba Hydrus Kuromori Basaran Dirge Celosia Pelagia Phalanx Cenobia Argus Malus)
pwsh -NoProfile -File testrun/Settings.ps1 -GameDir "$game" -Preset "$preset" -Fps uncapped -VSync off > /dev/null

for tag in "$@"; do
    for ((w = 0; w < 120; w++)); do
        [ "$(tasklist | grep -ciE 'ShadowOfTheColossus|i3.exe|shadps4')" = "0" ] && break
        sleep 5
    done
    rm -f "$cap"/rec_* "$cap"/aud_*
    if [ "$tag" = world ]; then
        label=${WORLD_LABEL:-Forbidden_Lands}; file=world; args=""; t=${WORLD_T:-175}
        lead="46:key=escape,48:key=down,49:key=down,50:key=down,51:key=enter,64:key=enter,82:key=enter,${WORLD_LEAD:-}"
    else
        n=${tag#b}; label="${names[$n]}_arena"; file=$(printf 'colossus_%02d_%s' $((n + 1)) "${names[$n],,}")
        args="+nointro +skipSplash=true +boss=$n"; t=50; lead=""
    fi
    walk="$t:key=w*6000,$((t + 8)):mouse=30*1500,$((t + 12)):key=w*8000,$((t + 21)):mouse=-30*1500"
    name="${label}_-_${preset^}_preset_-_1080p,_no_frame_limit"
    RUNENV="SOTC_PERFHUD=1 SOTC_PERFHUD_LABEL=$name SOTC_PERFHUD_NOTE=Ryzen_7_5700X3D,_Radeon_RX_9060_XT_-_recording_costs_about_2% SOTC_RECORD=$cap/rec_%p.raw SOTC_RECORD_FULL=1 SOTC_RECORD_FPS=30 SOTC_AUDIODUMP=$cap/aud_%p.f32" \
        BUILD=Release SCRIPT="${lead}$t:rec=ui,${walk},$((t + 24)):rec=stop" \
        ./bossarena.sh "$cap/unused.png" $((t + 34)) $args 2>&1 | grep -E "ran|REFUSED" | sed "s/^/$preset $tag: /"
    fps=$(grep -a "Flip: fps" testrun/user/log/shad_log.txt | sed -E 's/^.*fps ([0-9.]+).*/\1/' | tail -9 | head -7 |
        awk '{s += $1; if (min == "" || $1 < min) min = $1} END {if (NR) printf "average %.0f FPS, lowest %.0f FPS", s / NR, min}')
    v=$(ls -t "$cap"/rec_*.raw 2>/dev/null | head -1)
    [ -z "$v" ] && { echo "$preset $tag: no recording"; continue; }
    pid=${v##*rec_}; pid=${pid%.raw}
    read -r _ skip < "$v.ms"
    part="$outdir/benchmark_${file}_${preset}.mp4"
    ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 30 -i "$v" \
        -f f32le -ar 48000 -ac 2 -i "$cap/aud_$pid.f32" \
        -filter_complex "[0:v]scale=1280:720:flags=lanczos[v];[1:a]atrim=start_sample=${skip:-0},asetpts=PTS-STARTPTS,volume=-5dB,afade=t=in:d=0.4[a]" \
        -map "[v]" -map "[a]" -shortest -c:v libx264 -preset slow -crf 20 -maxrate 4400k -bufsize 8800k \
        -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart "$part" &&
        { [ -z "$FULL" ] || ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 30 -i "$v"             -f f32le -ar 48000 -ac 2 -i "$cap/aud_$pid.f32"             -filter_complex "[1:a]atrim=start_sample=${skip:-0},asetpts=PTS-STARTPTS,volume=-5dB,afade=t=in:d=0.4[a]"             -map 0:v -map "[a]" -shortest -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k             -movflags +faststart "$FULL/benchmark_${file}_${preset}_1080p.mp4"; } &&
        echo "$(basename "$part"): ${label//_/ }, ${preset^} preset, 1080p, no frame limit: $fps" | tee -a "$outdir/benchmark_clips.txt"
    rm -f "$cap"/rec_* "$cap"/aud_*
done

cp $cfg.bench_before $cfg
cp testrun/CommandLineArgs.bench_before.txt "/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp testrun/user/sotc_graphics.bench_before.json testrun/user/sotc_graphics.json
