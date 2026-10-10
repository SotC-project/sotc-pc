#!/bin/bash
# Boots straight into a colossus arena and takes shots: ./bossarena.sh <sheet.png> <seconds> "+arg" ...
# Typical: ./bossarena.sh out.png 130 +nointro +skipSplash=true +boss=0
out=$1; secs=$2; shift 2
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; for a in "$@"; do echo "$a" >> "$f"; done
cd /d/sotc-pc
before=$(ls testrun/user/screenshots/*.png | wc -l)
script="${SCRIPT:-45:shot,60:key=w*4000,66:shot,70:mouse=40*800,74:shot,80:key=s*4000,86:shot}"
for ((t = 95; t < secs - 8; t += 15)); do script="$script,$t:shot"; done
SAVE=${SAVE:-1} ./test.sh "$secs" SOTC_SCRIPT="$script,$((secs - 5)):quit" $RUNENV | tail -3
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
