#!/bin/bash
# Pictures of the benchmark scenes with the performance readout on, one preset.
# ./bench_shots.sh <out dir> [preset]      (default preset: high)
# The readout costs a little, so the tables come from bench.sh, not from these runs.
out=$1; preset=${2:-high}
cd /d/sotc-pc || exit 1
mkdir -p "$out"
game="D:\\Games\\CUSA08809T"
cp testrun/user/config.json testrun/user/config.json.bench_before
cp testrun/user/sotc_graphics.json testrun/user/sotc_graphics.bench_before.json
pwsh -NoProfile -File testrun/Settings.ps1 -GameDir "$game" -Preset $preset -Fps uncapped -VSync off > /dev/null

run_scene() {
    local name=$1 args=$2 script=$3
    local before
    before=$(ls testrun/user/screenshots/*.png | wc -l)
    RUNENV="SOTC_PERFHUD=1 SOTC_PERFHUD_LABEL=${name}_-_${preset}_preset SOTC_PERFHUD_NOTE=Ryzen_7_5700X3D,_Radeon_RX_9060_XT,_1080p" \
        BUILD=Release SCRIPT="$script" ./bossarena.sh "$out/${name}_sheet.png" 100 $args 2>&1 | grep -E "ran|Critical|Segm|ABORT"
    local i=0
    for f in $(ls -tr testrun/user/screenshots/*.png | tail -n +$((before + 1))); do
        [ -s "$f" ] && cp "$f" "$out/${name}_$((i++)).png"
    done
}

walk="50:key=w*6000,58:mouse=30*1500,62:key=w*8000,71:shotui,72:mouse=-30*1500,76:key=w*8000,85:shotui,86:key=s*4000,91:shotui"
run_scene Gaius_arena "+nointro +skipSplash=true +boss=2" "$walk"
run_scene Field_with_Agro "+nointro +skipSplash=true +boss=3" "41:key=a*500,42:key=lshift*300,45:key=a*300,46:key=lshift*300,49:key=a*300,50:key=lshift*300,$walk"
run_scene Valus_arena "+nointro +skipSplash=true +boss=0" "$walk"

cp testrun/user/config.json.bench_before testrun/user/config.json
cp testrun/user/sotc_graphics.bench_before.json testrun/user/sotc_graphics.json
echo "shots done: $out"
