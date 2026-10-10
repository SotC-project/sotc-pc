#!/bin/bash
# Walks through the hall with extra engine options and counts near-black pixels along the
# bottom edge of six screenshots (the black-tile artefact): ./blacktest.sh +name=value ...
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"
for a in "$@"; do case "$a" in +*) echo "$a" >> "$f";; esac; done
env=(); for a in "$@"; do case "$a" in +*) ;; *) env+=("$a");; esac; done
cd /d/sotc-pc
SAVE=1 ./test.sh 130 "${env[@]}" SOTC_SCRIPT="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross,82:key=s*60000,95:shot,100:shot,105:shot,110:shot,115:shot,120:shot,122:quit" > /dev/null
mv "$f.bak" "$f"
echo "$*: $(python -W ignore "$TEMP/blk.py" 6)"
