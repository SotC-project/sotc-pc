"""Closed-loop approach to Agro in open, bright terrain: agro.py <steps>.
Finds the horse as the large near-black blob in the view, turns to face it, walks up and
presses the mount button. Needs a live session (see live.sh)."""
import glob, os, sys, time
from PIL import Image

CMDS = 'D:/sotc-pc/dist/live_cmds.txt'
SHOTS = 'D:/sotc-pc/testrun/user/screenshots/*.png'


def send(action):
    with open(CMDS, 'a', newline='\n') as f:
        f.write(action + '\n')


def shot():
    before = set(glob.glob(SHOTS))
    send('shot')
    for _ in range(40):
        time.sleep(0.25)
        new = set(glob.glob(SHOTS)) - before
        if new:
            path = max(new, key=os.path.getmtime)
            for _ in range(20):
                time.sleep(0.3)
                try:
                    return Image.open(path).convert('L').resize((96, 54))
                except OSError:
                    pass
    raise SystemExit('no screenshot')


def horse(image):
    # Near-black pixels outside the area Wander himself occupies.
    xs = [x for y in range(8, 54) for x in range(96)
          if image.getpixel((x, y)) < 28 and not (38 <= x <= 58 and y >= 20)]
    return len(xs), (sum(xs) / len(xs) - 48) / 48 if xs else 0.0


missing = 0
for step in range(int(sys.argv[1])):
    count, offset = horse(shot())
    print(f'step {step}: horse pixels {count} offset {offset:+.2f}', flush=True)
    if count < 12:
        missing += 1
        send('key=space*500' if missing % 2 else 'mouse=60*900')
        time.sleep(4 if missing % 2 else 1.5)
        continue
    if abs(offset) > 0.15:
        send(f'mouse={60 if offset > 0 else -60}*{int(abs(offset) * 520)}')
        time.sleep(abs(offset) * 0.52 + 0.6)
        continue
    if count < 230:
        send('key=w*900')
        time.sleep(1.3)
    else:
        send('key=w*900')
        time.sleep(0.25)
        send('key=e*250')
        time.sleep(2.5)
