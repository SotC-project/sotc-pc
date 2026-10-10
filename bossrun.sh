#!/bin/bash
# Short boss-jump run; prints the last frame rates. ./bossrun.sh [VAR=value ...]
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; echo "+shadow_jumpToBoss=1" >> "$f"
cd /d/sotc-pc
SAVE=1 ./test.sh 135 SOTC_SCRIPT="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross,130:quit" "$@" > /dev/null
mv "$f.bak" "$f"
grep "Flip: fps" testrun/user/log/shad_log.txt | tail -4 | sed -E 's/^.*fps //' | tr '\n' ' '; echo
