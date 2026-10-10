#!/bin/bash
# Live steering: ./live.sh "<action>" ... sends actions to the running game, waits, takes a
# screenshot and writes a 960x540 copy to $OUT (default scratch live.png).
# Example: ./live.sh "key=w*3000" wait=4
F=/d/sotc-pc/dist/live_cmds.txt
OUT=${OUT:-$TEMP/claude/D--python/d4a3733b-ad5d-473c-870f-bbc3424f5fa8/scratchpad/live.png}
w=1
for a in "$@"; do
    case "$a" in wait=*) w=${a#wait=};; *) echo "$a" >> "$F";; esac
done
sleep "$w"
echo ${SHOT:-shot} >> "$F"
sleep 1.5
python - "$OUT" <<'PY'
import sys, glob, os
from PIL import Image
f = max(glob.glob('D:/sotc-pc/testrun/user/screenshots/*.png'), key=os.path.getmtime)
Image.open(f).convert('RGB').resize((960, 540)).save(sys.argv[1])
PY
grep "Flip: fps" /d/sotc-pc/testrun/user/log/shad_log.txt | tail -1 | sed -E 's/^.*fps /fps /'
