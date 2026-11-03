"""Every colossus motion record (static MotionKind records, 0x1c00000-0x1e00000) with its name.

A record's name pointer sits 8 bytes in; rela.npy maps relocated pointers. Writes motions_all.txt:
offset name, sorted by offset. Usage: python re/cvc/motions.py [prefix ...]  (prints those only)"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.chdir(os.path.dirname(os.path.abspath(__file__)))
from re_ import cstr

r = np.load('rela.npy')
recs = []
for a, t in zip(r[0].tolist(), r[1].tolist()):
    if (a - 8) % 8 == 0 and 0x1c00000 < a < 0x1e00000 and 0xf00000 < t < 0x1604000:
        try:
            s = cstr(t).decode('latin1')
        except Exception:
            continue
        if '_' in s and s[0].isupper():
            recs.append((a - 8, s))
recs.sort()
open('motions_all.txt', 'w').write(''.join(f'{a:#x} {s}\n' for a, s in recs))
want = tuple(sys.argv[1:])
for a, s in recs:
    if not want or s.startswith(want):
        print(hex(a), s)
