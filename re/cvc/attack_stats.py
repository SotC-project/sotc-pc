"""How each attack motion did in a CvC fight log: per colossus and motion, how often it landed a
blow, missed, or was stopped by the other's body (the director's lines in shad_log.txt).
Usage: python re/cvc/attack_stats.py [shad_log.txt]"""
import collections
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'D:/sotc-pc/testrun/user/log/shad_log.txt'
text = open(path, 'rb').read().decode('utf-8', 'replace')
names = {}
for line in open('D:/sotc-pc/re/cvc/motions_all.txt', encoding='utf-8', errors='replace'):
    parts = line.split()
    if len(parts) >= 2:
        names[int(parts[0], 16)] = parts[1]
current = {}
tally = collections.defaultdict(collections.Counter)
for m in re.finditer(r'cvcbrawl: (A|B) (attacks \((0x[0-9a-f]+)|hit |missed|stopped by)', text):
    who, what = m.group(1), m.group(2)
    if what.startswith('attacks'):
        current[who] = int(m.group(3), 16)
        tally[(who, current[who])]['tries'] += 1
    elif who in current:
        tally[(who, current[who])][what.split()[0]] += 1
        if what.startswith('hit'):
            current.pop(who)  # (one blow per attack counted)
for (who, motion), c in sorted(tally.items()):
    print(f'{who} {motion:#x} {names.get(motion, "?"):45s} tries {c["tries"]:2d}  hit {c["hit"]:2d}  missed {c["missed"]:2d}  '
          f'stopped {c["stopped"]:2d}')
