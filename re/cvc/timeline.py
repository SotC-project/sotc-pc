"""One time-aligned timeline of a CvC fight (CVC_QUALITY + SOTC_LOG_TIME runs).

Merges the quality log (testrun/cvc_quality.bin), its flags (quality.py) and the game/emulator log
(each line led by the steady clock) into one text file: the quality summary on top (worst frames
first), then every event in time order: the log's CvC lines, warnings, errors and asserts, each
flagged frame, and once a second a line per colossus (root, motion and frame, director act, contact,
camera). Times are seconds from the fight's start; [clock] is the steady clock shown in the corner of
the video (CVC_QUALITY), so each line can be found in the picture.
Usage: python re/cvc/timeline.py <cvc_quality.bin> <shad_log.txt> <out.txt>
       python re/cvc/timeline.py <cvc_quality.bin> --at <clock> [seconds]   (every frame around a moment)"""
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quality  # noqa: E402

ACTS = ['idle', 'turn', 'walk', 'attack', 'flinch', 'retreat', 'pulse', 'down']
LINE = re.compile(r'^(\d+\.\d+) \[(\w+)\] <(\w+)> \((.*?)\) (\S+\.\w+:\d+) (\S+): (.*)$')


def frame_text(q, i):
    k = int(q['k'][i])
    act = ACTS[int(q['act'][i])] if 0 <= int(q['act'][i]) < len(ACTS) else str(int(q['act'][i]))
    return (f'{"AB"[k]} root {q["x"][i]:.1f} {q["y"][i]:.1f} {q["z"][i]:.1f} yaw {q["yaw"][i]:+.2f} ours '
            f'{np.hypot(q["ox"][i], q["oz"][i]):.2f} motion {int(q["motion"][i]):#x} f{q["frame"][i]:.1f} {act}; '
            f'contacts {int(q["contacts"][i])} depth {q["depth"][i]:.2f}; bend {q["bend"][i]:.1f} (bone '
            f'{int(q["bend_bone"][i])}) stretch {q["stretch"][i]:.2f} offset {q["offset"][i]:.1f} scale '
            f'{q["scale"][i]:.3f}; give {q["give"][i]:.1f} tilt {q["tilt"][i]:.1f}; cam {q["cx"][i]:.0f} '
            f'{q["cy"][i]:.0f} {q["cz"][i]:.0f}')


def main():
    q = quality.load(sys.argv[1])
    t0 = float(q['t'].min())
    if sys.argv[2] == '--at':
        at = float(sys.argv[3])
        span = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5
        for i in np.nonzero(np.abs(q['t'] - at) <= span)[0]:
            print(f'{q["t"][i] - t0:8.3f}s [{q["t"][i]:.3f}] #{int(q["frame_no"][i])} {frame_text(q, i)}')
        return
    log, out = sys.argv[2], sys.argv[3]
    summary_text, _ = quality.report(q, log)
    flags, _ = quality.summary(q, log)
    events = []
    end = float(q['t'].max())
    for raw in open(log, 'rb').read().decode('utf-8', 'replace').splitlines():
        m = LINE.match(raw)
        if not m:
            continue
        t, cls, level, thread, where, fn, msg = m.groups()
        t = float(t)
        if t < t0 - 5 or t > end + 5:
            continue
        if msg.startswith('cvc') or level in ('Warning', 'Error', 'Critical') or 'assert' in msg.lower():
            events.append((t, f'LOG {level[0]} {cls} {where}: {msg[:300]}'))
    for kind, who, t, value, detail in flags:
        events.append((t0 + t, f'FLAG {kind} {who} {value:.2f} {detail}'))
    last = {}
    for i in range(len(q['t'])):
        k = int(q['k'][i])
        if q['t'][i] - last.get(k, -9) >= 1.0:
            last[k] = q['t'][i]
            events.append((q['t'][i], f'#{int(q["frame_no"][i])} {frame_text(q, i)}'))
    events.sort(key=lambda e: e[0])
    gaps = np.diff(q['t'][q['k'] == 0])
    with open(out, 'w', encoding='utf-8') as f:
        f.write(summary_text)
        f.write(f'frame time: median {np.median(gaps) * 1000:.1f} ms, worst {gaps.max() * 1000:.0f} ms, '
                f'{int((gaps > 0.07).sum())} frames over 70 ms\n\n')
        for t, text in events:
            f.write(f'{t - t0:8.3f}s [{t:.3f}] {text}\n')
    print(f'wrote {out} ({len(events)} events)')


if __name__ == '__main__':
    main()
