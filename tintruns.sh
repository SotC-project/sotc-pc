#!/bin/bash
# N gameplay runs with extra engine options, then an N-up contact sheet to judge colour tint:
# ./tintruns.sh <n> <sheet.png> [+name=value ...]
n=$1; out=$2; shift 2
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"
for a in "$@"; do echo "$a" >> "$f"; done
for ((i = 0; i < n; i++)); do /d/sotc-pc/gameplay.sh $RUNENV | sed -n 2p | cut -c1-10 | tr '\n' ' '; done
mv "$f.bak" "$f"
python - "$n" "$out" <<'PY'
import sys, glob, os
from PIL import Image
n = int(sys.argv[1])
fs = sorted(glob.glob('D:/sotc-pc/testrun/user/screenshots/*.png'), key=os.path.getmtime)[-n:]
cols = 4; rows = (n + cols - 1) // cols
out = Image.new('RGB', (480 * cols, 270 * rows))
for i, f in enumerate(fs):
    out.paste(Image.open(f).convert('RGB').resize((480, 270)), (480 * (i % cols), 270 * (i // cols)))
out.save(sys.argv[2])
PY
