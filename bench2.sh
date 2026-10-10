#!/bin/bash
# Benchmark: three fixed scenes under each graphics preset, with frame times and resource use.
# ./bench.sh <out dir> [preset ...]      (default presets: low medium high ultra)
# Each run: Release build, test profile, scripted movement, 60 s measured after loading.
# Extra Settings.ps1 switches for every run: EXTRA="-Window 4k -Upscaling on" ./bench.sh ...
out=$1; shift
presets=${*:-low medium high ultra}
cd /d/sotc-pc || exit 1
mkdir -p "$out"
game="D:\\Games\\CUSA08809T"
cfg=testrun/user/config.json
cp $cfg $cfg.bench_before
cp "/d/Games/CUSA08809T-mods/CommandLineArgs.txt" testrun/CommandLineArgs.bench_before.txt
cp testrun/user/sotc_graphics.json testrun/user/sotc_graphics.bench_before.json

wait_free() {
    for ((w = 0; w < 120; w++)); do
        [ "$(tasklist | grep -ciE 'ShadowOfTheColossus|i3.exe|inFAMOUS|shadps4')" = "0" ] && return
        sleep 5
    done
}

# Samples memory, CPU time and GPU memory of the game process every 5 s into $1.
sample() {
    powershell -NoProfile -Command "
        while (\$true) {
            \$p = Get-Process shadps4 -ErrorAction SilentlyContinue | Select-Object -First 1
            if (\$p) {
                \$g = (Get-Counter \"\\GPU Process Memory(pid_\$(\$p.Id)_*)\\Dedicated Usage\" -ErrorAction SilentlyContinue).CounterSamples | Measure-Object CookedValue -Sum
                '{0} {1} {2} {3}' -f [int](Get-Date -UFormat %s), [int](\$p.WorkingSet64 / 1MB), [int]\$p.TotalProcessorTime.TotalSeconds, [int](\$g.Sum / 1MB)
            }
            Start-Sleep 5
        }" > "$1" 2>/dev/null &
    sampler=$!
}

# scene name, seconds, launch args, script (measure window: script seconds 50-110)
run_scene() {
    local name=$1 preset=$2 args=$3 script=$4
    # SCENES="arena_gaius field_agro" limits the run to those scenes.
    if [ -n "$SCENES" ] && ! echo " $SCENES " | grep -q " $name "; then return; fi
    wait_free
    local tag="$out/${preset}_${name}"
    sample "$tag.res"
    RUNENV="SOTC_FRAMETIMES=D:/sotc-pc/$tag.ft" BUILD=Release SCRIPT="$script" \
        ./bossarena.sh "$tag.png" 122 $args 2>&1 | grep -E "ran|Critical|Segm|ABORT" | sed "s/^/$preset $name: /"
    kill $sampler 2>/dev/null
}

for preset in $presets; do
    pwsh -NoProfile -File testrun/Settings.ps1 -GameDir "$game" -Preset $preset -Fps uncapped -VSync off $EXTRA > /dev/null
    grep -o '"[A-Za-z]*": "[a-z0-9]*"' testrun/user/sotc_graphics.json | tr '\n' ' ' > "$out/${preset}.settings"
    walk="50:key=w*6000,58:mouse=30*1500,62:key=w*8000,72:mouse=-30*1500,76:key=w*8000,86:key=s*4000,92:mouse=40*1500,96:key=w*8000,106:shot"
    run_scene arena_gaius $preset "+nointro +skipSplash=true +boss=2" "$walk"
    run_scene field_agro $preset "+nointro +skipSplash=true +boss=3" "41:key=a*500,42:key=lshift*300,45:key=a*300,46:key=lshift*300,49:key=a*300,50:key=lshift*300,$walk"
    run_scene arena_valus $preset "+nointro +skipSplash=true +boss=0" "$walk"
done

cp $cfg.bench_before $cfg
cp testrun/CommandLineArgs.bench_before.txt "/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp testrun/user/sotc_graphics.bench_before.json testrun/user/sotc_graphics.json
echo "bench done: $out"
