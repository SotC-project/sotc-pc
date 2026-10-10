"""Closed-loop steering toward the brightest part of the view (daylight): seek.py <steps>.
Needs a game started with SOTC_SCRIPT_FILE=dist/live_cmds.txt (see live.sh)."""
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
            for _ in range(20):  # the file is still being written at first
                time.sleep(0.3)
                try:
                    return Image.open(path).convert('L').resize((96, 54))
                except OSError:
                    pass
    raise SystemExit('no screenshot')


previous = None
stuck = 0
field_steps = 0
for step in range(int(sys.argv[1])):
    image = shot()
    # Same view as before the last move: walking into something. Back off and turn away.
    if previous is not None:
        diff = sum(abs(a - b) for a, b in zip(image.getdata(), previous.getdata())) / (96 * 54)
        if diff < 2.0:
            stuck += 1
            print(f'step {step}: stuck ({diff:.1f}), turning', flush=True)
            send('key=s*900')
            time.sleep(1.2)
            send(f'mouse={60 if stuck % 2 else -60}*{500 + 250 * (stuck % 3)}')
            time.sleep(1.6)
            send('key=w*1500')
            time.sleep(1.8)
            previous = None
            continue
    previous = image
    # "field" mode: stop once the lower part of the view is sunlit ground with no dark
    # masonry in it for a few steps in a row, i.e. open terrain.
    if len(sys.argv) > 2 and sys.argv[2] == 'field':
        ground = [image.getpixel((x, y)) for y in range(36, 54) for x in range(96)]
        dark = sum(1 for v in image.getdata() if v < 40)
        open_view = sum(ground) / len(ground) > 95 and dark < 260
        field_steps = field_steps + 1 if open_view else 0
        if field_steps >= 4:
            print(f'step {step}: open terrain', flush=True)
            break
    # Brightness per column over the upper two thirds; daylight openings dominate.
    cols = [sum(image.getpixel((x, y)) for y in range(4, 36)) for x in range(96)]
    window = [sum(cols[x:x + 12]) for x in range(84)]
    best = max(range(84), key=window.__getitem__) + 6
    offset = (best - 48) / 48  # -1 (left edge) .. 1 (right edge)
    bright = max(window) / (12 * 32 * 255)
    print(f'step {step}: target {offset:+.2f} brightness {bright:.2f}', flush=True)
    if bright < 0.12:  # nothing bright in view: look around
        send('mouse=60*700')
        time.sleep(1.2)
        continue
    if abs(offset) > 0.12:
        send(f'mouse={60 if offset > 0 else -60}*{int(abs(offset) * 520)}')
        time.sleep(abs(offset) * 0.52 + 0.6)
    send('key=w*2500')
    time.sleep(2.8)
