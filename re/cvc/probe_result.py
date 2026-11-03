"""Reject mistimed CvC tests and report only this run's object snapshots."""
import math
import re
import struct
import sys
from pathlib import Path

root = Path('D:/sotc-pc/testrun/user')
log = (root / 'log/shad_log.txt').read_text(errors='replace')
match = re.search(r'cvctarget: A (0x[0-9a-f]+).*? B (0x[0-9a-f]+)', log)
if not match or not all(int(v, 16) for v in match.groups()):
    sys.exit('INVALID RUN: both colossi were not loaded when targeting was configured')
for marker in ('Unhandled Exception', 'Device lost during submit'):
    if marker in log:
        sys.exit('FAILED RUN: ' + marker)
# The test driver increments pass once per objlist action, including player teleports.
passes = re.findall(r'objlist 238 #0: node .*? object (0x[0-9a-f]+)', log)
if not passes:
    sys.exit('INVALID RUN: no colossus snapshots')
# Read only files newer than the current log; earlier runs reuse the same dump names.
cutoff = (root / 'log/shad_log.txt').stat().st_mtime
files = sorted(root.glob('obj_238_*.bin'), key=lambda p: p.stat().st_mtime)
files = [p for p in files if 0 <= cutoff - p.stat().st_mtime <= 350]
if not files:
    sys.exit('INVALID RUN: no current object dumps')
for path in files:
    data = path.read_bytes()
    if len(data) < 0x14c:
        sys.exit('INVALID RUN: truncated dump ' + path.name)
    position = struct.unpack_from('<3f', data, 0x130)
    local = struct.unpack_from('<4f', data, 0x1f0)
    print(path.name, 'world', tuple(round(v, 2) for v in position),
          'local', tuple(round(v, 2) for v in local))
pairs = {}
for path in files:
    fields = path.stem.split('_')
    pairs.setdefault(fields[2], {})[int(fields[3])] = struct.unpack_from('<3f', path.read_bytes(), 0x130)
for index, pair in pairs.items():
    if 0 in pair and 1 in pair:
        a, b = pair[0], pair[1]
        print('snapshot', index, 'planar separation', round(math.hypot(a[0] - b[0], a[2] - b[2]), 2))
print('Target objects:', *match.groups())
if 'cvcfight hit ' in log:
    sys.exit('INVALID COMBAT PROOF: synthetic proximity damage was active')
