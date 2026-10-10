#!/bin/bash
# Continue the stored save with extra engine options, take shots: ./spawn.sh +name=value ...
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"
for a in "$@"; do echo "$a" >> "$f"; done
cd /d/sotc-pc
SAVE=1 ./test.sh 125 SOTC_SCRIPT="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross,90:shot,100:shot,112:shot,115:quit" | tail -2
mv "$f.bak" "$f"
grep "Flip: fps" testrun/user/log/shad_log.txt | tail -8 | sed -E 's/^.*fps //' | tr '\n' ' '; echo
