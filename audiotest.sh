#!/bin/bash
# Sound of a colossus intro, dumped for measurement: ./audiotest.sh <name> <boss 0-15> [secs] [VAR=value ...]
# Writes E:/sotc-capture/at_<name>.f32 (stereo 48 kHz), .params (SOTC_PARAMLOG) and keeps the log.
name=$1; boss=$2; secs=${3:-90}; shift 3
cap=${CAP:-E:/sotc-capture}
cd /d/sotc-pc || exit 1
mkdir -p "$cap" && rm -f "$cap"/aud_*
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n+boss=%s\r\n' "$boss" >> "$f"
SAVE=1 ./test.sh "$secs" SOTC_SCRIPT="40:shot,55:shot,70:shot,$((secs - 5)):quit" SOTC_AUDIODUMP="$cap/aud_%p.f32" \
    SOTC_AUDIO_DIAG=1 SOTC_PARAMLOG="$cap/at_$name.params" "$@" | tail -2
mv "$f.bak" "$f"
mv "$(ls -t "$cap"/aud_*.f32 | head -1)" "$cap/at_$name.f32"
cp testrun/user/log/shad_log.txt "$cap/at_$name.log"
