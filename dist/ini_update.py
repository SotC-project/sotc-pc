"""Brings a bindings file up to the 2026-10-10 defaults: python ini_update.py <CUSA08809.ini> [invert]
invert is x, y, xy or none (which mouse directions are turned around). Other lines are kept."""
import sys

path = sys.argv[1]
invert = sys.argv[2] if len(sys.argv) > 2 else None
lines = open(path, encoding='utf-8').read().split('\n')
out = []
for line in lines:
    key = line.split('=')[0].strip()
    if line.replace(' ', '') == 'r1=middlebutton':
        continue
    if line.startswith('# Q or middle mouse'):
        line = '# Q: raise the sword to the light / aim the bow (left mouse draws and fires it).'
    if key == 'mouse_movement_params':
        line = 'mouse_movement_params = 0.25, 1.0, 0.0'
    if key in ('walk_toggle', 'mouse_invert'):
        continue
    if line.startswith('# Left Shift switches between'):
        continue
    out.append(line)
    if key == 'mouse_movement_params' and invert:
        out.append('# Mouse directions turned around: x, y, xy or none.')
        out.append('mouse_invert = ' + invert)
    if key == 'triangle' and 'lshift' in line:
        out.append('# Left Shift switches between running and walking.')
        out.append('walk_toggle = lshift')
out = [l for i, l in enumerate(out) if not (l.startswith('# Mouse directions turned around') and out[i - 1] == l)]
open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
print('updated', path)
