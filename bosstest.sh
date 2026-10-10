#!/bin/bash
# Continues the save with +shadow_jumpToBoss=<n> and takes shots: ./bosstest.sh <n> <sheet.png> [VAR=value ...]
n=$1; out=$2; shift 2
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; echo "+shadow_jumpToBoss=$n" >> "$f"
cd /d/sotc-pc
before=$(ls testrun/user/screenshots/*.png | wc -l)
SAVE=1 ./test.sh 250 SOTC_SCRIPT="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross,100:shot,130:shot,160:shot,190:shot,215:key=w*5000,222:mouse=30*800,235:shot,240:quit" "$@" | tail -3
mv "$f.bak" "$f"
grep "Flip: fps" testrun/user/log/shad_log.txt | sed -E 's/^.*fps //' | tr '\n' ' '; echo
python - "$out" "$before" <<'PY'
import glob, os, sys
from PIL import Image
fs = sorted(glob.glob('D:/sotc-pc/testrun/user/screenshots/*.png'), key=os.path.getmtime)[int(sys.argv[2]):]
print(len(fs), 'shots')
if fs:
    o = Image.new('RGB', (640 * 3, 360 * 2))
    for i, f in enumerate(fs[:6]):
        o.paste(Image.open(f).convert('RGB').resize((640, 360)), (640 * (i % 3), 360 * (i // 3)))
    o.save(sys.argv[1])
PY
