"""Per-fight quality summary of a CvC fight (CVC_QUALITY runs): the gate before calling a pair working.

Reads testrun/cvc_quality.bin (each frame, each colossus: root, what our code moved it by, motion,
contact depth, and how its drawn pose differs from its animated one) and the game log's blows, and
flags:
- root jumps: a frame's root move our code does not explain, over 1.2 units (or 1.2 up/down), not
  at a motion change; root turns over 0.25 rad in a frame;
- glides: our code moving a root over 3 units in a second;
- deformation: a joint bent over 20 degrees from its animation, a bone's length to its parent
  changed over 0.3, a bone's axes scaled over 5 % (not rigid);
- interpenetration: hulls over 2 units inside each other, and the longest run over 1;
- trapped: bodies in contact over 4 s without a break;
- foot sliding: the lowest bone, on the ground, moving over 0.6 units in a frame;
- blows: each hit's depth (whether the surfaces met) and impulse;
- no knockout: with the log given, a fight that never reached one ("cvcbrawl: A/B wins") fails (the
  user: a fight must end, its loser's death and the KNOCKOUT banner shown).
Times are seconds from the fight's first record. Exit code 1 when anything is flagged.
Usage: python re/cvc/quality.py [cvc_quality.bin] [shad_log.txt]"""
import os
import re
import struct
import sys

import numpy as np

RECORD = struct.Struct('<dI3f2fQfIffifififii3ffII6fifff3f3f3fI')
FIELDS = ('t k x y z ox oz motion frame touching depth bend bend_bone stretch stretch_bone offset offset_bone '
          'scale scale_bone low_bone lx ly lz yaw frame_no contacts cx cy cz lookx looky lookz act give tilt blend ax ay az rax ray raz rdx rdy rdz view').split()
LIMITS = dict(jump=1.2, turn=0.25, glide=3.0, bend=20.0, stretch=0.3, scale=0.05, depth=2.0, trapped=4.0, slide=0.6)


def load(path):
    raw = open(path, 'rb').read()
    if len(raw) % RECORD.size:
        sys.exit(f'{path}: {len(raw)} bytes, not whole {RECORD.size}-byte records (another build format?)')
    n = len(raw) // RECORD.size
    rows = [RECORD.unpack_from(raw, i * RECORD.size) for i in range(n)]
    q = {f: np.array([r[i] for r in rows]) for i, f in enumerate(FIELDS)}
    # The drawn root (bone 0 as drawn, cvc_bones.bin beside it: the records in the same order):
    # what is seen, after our smoothing; the object's position jumps where a motion starts.
    bones = os.path.join(os.path.dirname(path), 'cvc_bones.bin')
    if os.path.exists(bones):
        raw, at, roots = open(bones, 'rb').read(), 0, []
        while at + 16 <= len(raw) and len(roots) < n:
            count = struct.unpack_from('<I', raw, at + 12)[0]
            roots.append(struct.unpack_from('<3f', raw, at + 16))
            at += 16 + 12 * count
        if len(roots) == n:
            q['rx'], q['ry'], q['rz'] = (np.array([r[i] for r in roots]) for i in range(3))
    return q


def runs(mask, t):
    """(start time, length in seconds, first index, index past the end) of each run where mask holds."""
    out, start = [], None
    for i, m in enumerate(list(mask) + [False]):
        if m and start is None:
            start = i
        if not m and start is not None:
            out.append((t[start], t[i - 1] - t[start], start, i))
            start = None
    return out


def summary(q, log=None):
    t0 = q['t'].min()
    flags = []

    # After a knockout the loser plays the game's own death (its root and feet go as it collapses):
    # its frames from then on are not ours to judge; the winner's still are.
    ko_t, loser = None, None
    if log:
        m = re.search(r'^(\d+\.\d+) .*cvcbrawl: (A|B) wins', open(log, 'rb').read().decode('utf-8', 'replace'), re.M)
        if m:
            ko_t, loser = float(m.group(1)), 1 if m.group(2) == 'A' else 0

    # Nothing before the loading cover lifts is seen (the battle's setup behind it).
    shown_t = None
    if log:
        m = re.search(r'^(\d+\.\d+) .*Loading cover released', open(log, 'rb').read().decode('utf-8', 'replace'), re.M)
        if m:
            shown_t = float(m.group(1))

    def flag(kind, k, t, value, detail=''):
        if ko_t is not None and int(k) == loser and t >= ko_t:
            return
        if shown_t is not None and t < shown_t:
            return
        flags.append((kind, 'AB'[int(k)], float(t - t0), float(value), detail))

    if log and ko_t is None:
        flags.append(('no knockout', 'A', float(q['t'].max() - t0), 0.0, 'the fight never ended'))

    for k in (0, 1):
        s = q['k'] == k
        c = {f: v[s] for f, v in q.items()}
        if len(c['t']) < 2:
            continue
        t = c['t']
        dt = np.diff(t)
        # Pops, as seen: the drawn root (after our smoothing) moving over 1.2 in a frame and 3 times what
        # it moves in the frames around it (an attack's lunge moves it fast but smoothly). Whose: ours
        # when its animation's root did not jump with it.
        drawn = np.linalg.norm(np.diff(np.stack([c['rdx'], c['rdy'], c['rdz']], 1), axis=0), axis=1)
        anim = np.linalg.norm(np.diff(np.stack([c['rax'], c['ray'], c['raz']], 1), axis=0), axis=1)
        changed = c['motion'][1:] != c['motion'][:-1]
        around = np.array([np.median(np.r_[drawn[max(i - 5, 0):i], drawn[i + 1:i + 6]]) if len(drawn) > 1 else 0.0
                           for i in range(len(drawn))])
        settled = t[1:] - t[0] > 1.0  # (the first second: placed under the loading cover)
        for i in np.nonzero((drawn > LIMITS['jump']) & (drawn > 3.0 * around) & (dt < 0.1) & settled)[0]:
            # (Its animation's own uneven root motion, a quickened charge's gait, up to 2 a frame.)
            whose = "the game's" if anim[i] > 0.5 * drawn[i] else 'ours'
            if whose == 'ours' or drawn[i] > 2.0:
                flag('jump', k, t[i + 1], drawn[i], f'{whose}, motion {int(c["motion"][i + 1]):#x}')
        turn = np.abs((np.diff(c['yaw']) + np.pi) % (2 * np.pi) - np.pi)
        for i in np.nonzero((turn > LIMITS['turn']) & ~changed & (dt < 0.1))[0]:
            flag('turn', k, t[i + 1], turn[i], f'motion {int(c["motion"][i + 1]):#x}')
        # Glides: our moves summed over each second (not while it flinches or falls: a blow's shove
        # goes with its own animation).
        ours = np.where(np.isin(c['act'], (4, 5, 7)), 0.0, np.hypot(c['ox'], c['oz']))  # (and stepping back)
        j = 0
        for i in range(len(t)):
            while t[i] - t[j] > 1.0:
                j += 1
            if ours[j:i + 1].sum() > LIMITS['glide'] and (not flags or flags[-1][0] != 'glide' or
                                                           t[i] - t0 - flags[-1][2] > 1.0):
                flag('glide', k, t[i], ours[j:i + 1].sum())
        # Within 0.8 s of a motion change the drawn pose is still carrying on from the last motion's
        # (inertia: each bone turned along its shortest way to the new one): far from the new
        # animation's bends by design; its bone lengths still count.
        change_t = t[1:][changed]
        since_change = np.full(len(t), 1e9)
        if len(change_t):
            idx = np.searchsorted(change_t, t, side='right') - 1
            since_change = np.where(idx >= 0, t - change_t[np.clip(idx, 0, None)], 1e9)
        for name, bone in (('bend', 'bend_bone'), ('stretch', 'stretch_bone'), ('scale', 'scale_bone')):
            over = c[name] > LIMITS[name]
            if name == 'bend':
                over &= (since_change > 0.8) & (c['blend'] < 0.05)
            for start, length, i, e in runs(over, t):
                w = i + int(np.argmax(c[name][i:e]))
                flag(name, k, t[w], c[name][w], f'bone {int(c[bone][w])}, {length:.2f} s')
        # Foot sliding that is ours: the lowest bone, on the ground and planted in its animation
        # (moving under 0.1 a frame there), moved where it is drawn.
        # (Not a sword lying on the ground: Gaius, always A, its bones 28, 31 and 33.)
        same = (c['low_bone'][1:] == c['low_bone'][:-1]) & ~np.isin(c['low_bone'][1:], (28, 31, 33) if k == 0 else ())
        ground = np.minimum.accumulate(c['ly'])  # (a running floor; flat arenas)
        near = c['ly'][1:] < ground[1:] + 1.0
        planted = np.hypot(np.diff(c['ax']), np.diff(c['az'])) < 0.1
        slide = np.where(planted, np.hypot(np.diff(c['lx']), np.diff(c['lz'])), 0.0)
        for start, length, i, e in runs(same & near & (slide > LIMITS['slide']) & (dt < 0.1), t[1:]):
            # (Not one frame's slip under 1.0: the game's own foot pops, 0.61-0.65, on Quadratus and Phaedra.)
            if length > 0.0 or slide[i:e].max() > 1.0:
                flag('slide', k, start, slide[i:e].max(), f'{length:.2f} s, bone {int(c["low_bone"][i + 1])}')
    a = q['k'] == 0
    t, depth, touching = q['t'][a], q['depth'][a], q['touching'][a] > 0
    blow_t = []
    if log:
        # (Blows, and attacks stopped by the other's body: impacts both.)
        for m in re.finditer(r'^(\d+\.\d+) .*cvcbrawl: . (hit |stopped by the other)', open(log, 'rb').read().decode('utf-8', 'replace'), re.M):
            blow_t.append(float(m.group(1)))
    blow_t = np.array(blow_t)
    for start, length, i, e in runs(depth > LIMITS['depth'], t):
        # (Over 2 deep for longer than 0.3 s, or over 4 at all; a blow's own impact, the striking part
        # in as it lands and recovers, under 4 deep and 0.4 s.)
        if depth[i:e].max() < 4.0 and (length <= 0.3 or (len(blow_t) and np.min(np.abs(blow_t - start)) < 0.5
                                                         and length < 0.4)):
            continue
        flag('inside', 0, t[i + int(np.argmax(depth[i:e]))], depth[i:e].max(), f'{length:.2f} s')
    # Trapped: in contact over 4 s while their roots came no further apart than 2.
    b = q['k'] == 1
    n = min(a.sum(), b.sum())
    apart = np.hypot(q['x'][a][:n] - q['x'][b][:n], q['z'][a][:n] - q['z'][b][:n])
    for start, length, i, e in runs(touching[:n], t[:n]):
        # (Pressed together over a unit deep, not just touching: standing in light contact is no trap;
        # pushing shallow contact apart, the game's own collision popped the bodies 3 units.)
        if length > LIMITS['trapped'] and apart[e - 1] - apart[i] < 2.0 and depth[i:e].mean() > 1.0:
            flag('trapped', 0, start, length, f'roots {apart[i]:.0f} to {apart[e - 1]:.0f} apart, '
                                              f'{depth[i:e].mean():.2f} deep on average')
    # The view: both colossi out of sight from the camera, or it over a rim, cliff or chasm, over 1 s
    # (after the first second; the loser's death, the camera is on it).
    view = q['view'][a].astype(int)
    for name, bit in (('hidden', 1), ('rim', 2)):
        for start, length, i, e in runs((view & bit) == 0, t):
            if length > 1.0 and start - t[0] > 1.0:
                flag(name, 0, start, length, f'{length:.1f} s')
    deep_runs = runs(depth > 1.0, t)
    hits = []
    if log:
        text = open(log, 'rb').read().decode('utf-8', 'replace')
        hits = [(float(d), float(i)) for i, d in re.findall(r'cvcphys: . bone \d+ met . at [-\d ]+, closing [-\d]+, '
                                                             r'impulse ([-\d.]+), depth ([-\d.]+)', text)]
        blows = len(re.findall(r'cvcbrawl: . hit ', text))
    else:
        blows = 0
    return flags, dict(seconds=float(q['t'].max() - t0), frames=int(len(t)), longest_inside_1=max(
        (r[1] for r in deep_runs), default=0.0), touches=len(hits), blows=blows,
        touch_depth=(min((d for d, _ in hits), default=0.0), max((d for d, _ in hits), default=0.0)))


def report(q, log=None):
    """The summary as text, and whether it is clean."""
    flags, info = summary(q, log)
    out = []
    print = out.append  # noqa: A001 (the lines below, collected)
    print(f'{info["seconds"]:.0f} s, {info["frames"]} frames; blows {info["blows"]}, first touches '
          f'{info["touches"]} (depth {info["touch_depth"][0]:.2f}..{info["touch_depth"][1]:.2f}); longest run '
          f'over 1 unit inside each other {info["longest_inside_1"]:.2f} s')
    kinds = sorted({f[0] for f in flags})
    for kind in kinds:
        these = [f for f in flags if f[0] == kind]
        print(f'{kind}: {len(these)} (limit {LIMITS.get(kind, "")})')
        for _, who, t, value, detail in sorted(these, key=lambda f: -f[3])[:5]:
            print(f'    {t:7.2f}s {who} {value:7.2f} {detail}')
    print('CLEAN' if not flags else f'FLAGGED: {len(flags)}')
    return '\n'.join(out) + '\n', not flags


if __name__ == '__main__':
    text, clean = report(load(sys.argv[1] if len(sys.argv) > 1 else 'D:/sotc-pc/testrun/cvc_quality.bin'),
                         sys.argv[2] if len(sys.argv) > 2 else None)
    sys.stdout.write(text)
    sys.exit(0 if clean else 1)
