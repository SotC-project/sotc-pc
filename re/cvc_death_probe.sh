#!/bin/bash
# Each colossus's death motions, as the game plays them in its own arena (re/cvc_killhost.sh):
# the motion records (component +0x260) seen after its health goes to 0, as image offsets and names.
# ./re/cvc_death_probe.sh <boss index>...
cd /d/sotc-pc
for b in "$@"; do
    rm -f testrun/user/dumpat_*.bin
    NOREC=1 KILLAT=${KILLAT:-70} bash re/cvc_killhost.sh "$b" 110 > re/cvc_death_$b.txt 2>&1
    python - "$b" <<'PY'
import glob, re, struct, sys
sys.path.insert(0, 'D:/sotc-pc/re/cvc')
import os; os.chdir('D:/sotc-pc/re/cvc')
from re_ import img, cstr
log = open('D:/sotc-pc/testrun/user/log/shad_log.txt', encoding='utf-8', errors='replace').read()
m = re.search(r'Loading module eboot.bin to (0x[0-9a-f]+)', log)
base = int(m.group(1), 16) if m else 0
moved = 'moved, flag' in log
files = sorted(glob.glob('D:/sotc-pc/testrun/user/dumpat_*.bin'), key=lambda f: int(re.findall(r'(\d+)\.bin', f)[0]))
seen = []
for f in files[0::2]:
    data = open(f, 'rb').read()
    if len(data) < 8: continue
    off = struct.unpack('<Q', data[:8])[0] - base
    if 0 < off < len(img) and off not in seen:
        seen.append(off)
names = []
for off in seen:
    n = '?'
    for k in range(0, 0x40, 8):
        v = struct.unpack_from('<Q', img, off + k)[0]
        if 0xf00000 < v < 0x1600000:
            try:
                c = cstr(v)
                if 3 < len(c) < 60 and all(32 <= x < 127 for x in c): n = c.decode(); break
            except Exception: pass
    names.append(f'{off:#x} {n}')
print(f'boss {sys.argv[1]}: moved {moved}, crash {"Unhandled" in log}, motions: ' + ' | '.join(names))
PY
done
