#!/bin/bash
# Continues the save with extra launch arguments, takes shots, writes a sheet:
# ./argshot.sh <sheet.png> <seconds> "+arg=value" ...
out=$1; secs=$2; shift 2
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; for a in "$@"; do echo "$a" >> "$f"; done
cd /d/sotc-pc
before=$(ls testrun/user/screenshots/*.png | wc -l)
script="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross"
for ((t = 80; t < secs - 8; t += 20)); do script="$script,$t:shot"; done
SAVE=1 ./test.sh "$secs" SOTC_SCRIPT="$script,$((secs - 5)):quit" $RUNENV | tail -3
mv "$f.bak" "$f"
grep "Flip: fps" testrun/user/log/shad_log.txt | sed -E 's/^.*fps //' | tr '\n' ' '; echo
python - "$out" "$before" <<'PY'
import glob, os, sys
from PIL import Image
fs = sorted(glob.glob('D:/sotc-pc/testrun/user/screenshots/*.png'), key=os.path.getmtime)[int(sys.argv[2]):]
print(len(fs), 'shots')
if fs:
    o = Image.new('RGB', (640 * 3, 360 * ((len(fs[:9]) + 2) // 3)))
    for i, f in enumerate(fs[:9]):
        o.paste(Image.open(f).convert('RGB').resize((640, 360)), (640 * (i % 3), 360 * (i // 3)))
    o.save(sys.argv[1])
PY
