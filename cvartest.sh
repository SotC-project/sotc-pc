#!/bin/bash
# Runs gameplay.sh with extra engine launch options: ./cvartest.sh +name=value ...
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"
for a in "$@"; do echo "$a" >> "$f"; done
out=$(/d/sotc-pc/gameplay.sh)
mv "$f.bak" "$f"
echo "$*"
echo "$out" | sed -n 2p
echo "$out" | grep "stats/3s" | awk '{f=$NF; printf "per frame: drawn %d indirect %d dispatch %d mirrors %d\n", $3/f, $5/f, $7/f, $(NF-2)/f}'
