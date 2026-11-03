"""Validate cave patch layout without launching the game."""
import re
from pathlib import Path
import capstone

source = Path(__file__).parents[1] / 'cvc_base.sh'
text = source.read_text()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
sets = {}
for name, value in re.findall(r'^(\w+)="([^"]*)"', text, re.M):
    if name not in ('NC', 'TGT2', 'CUR', 'DUAL', 'MGR', 'DMG'):
        continue
    patches = []
    for address, payload in re.findall(r'patch=([0-9a-f]+):([0-9a-f]+)', value):
        code = bytes.fromhex(payload)
        start = int(address, 16)
        decoded = list(md.disasm(code, start))
        assert sum(i.size for i in decoded) == len(code), (name, hex(start))
        patches.append((start, start + len(code)))
    sets[name] = patches
ranges = [(start, end, name) for name in ('NC', 'TGT2', 'CUR', 'DUAL', 'MGR', 'DMG')
          for start, end in sets[name]]
ranges.sort()
for previous, current in zip(ranges, ranges[1:]):
    assert previous[1] <= current[0], (previous, current)
for start, end, name in ranges:
    if 0x1593000 <= start < 0x1594000:
        for low, high in ((0x1593b00, 0x1593b38), (0x1593e00, 0x1593e20),
                          (0x1593f00, 0x1593f80)):
            assert end <= low or start >= high, (name, hex(start), hex(low))
print(f'{len(ranges)} patches decode fully; no code/data overlap')
