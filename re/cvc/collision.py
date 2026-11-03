"""The colossi's own collision, from the game's line test (testrun/rays_A.bin, rays_B.bin with
pose_*.bin; CVC_PROBE_SKEL runs). Each ray record: u32 sample, 3 floats hit point, u64 word.
Hits are grouped by word; each group is put in the frame of every bone and given to the bone
it stays most compact on (the part that bone carries); a capsule (principal axis, length,
radius) is fitted there. Usage: python re/cvc/collision.py [A|B]"""
import sys, struct
import numpy as np
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from skeleton import load

def rays(path):
    raw = open(path, 'rb').read()
    rec = np.frombuffer(raw[:len(raw) // 24 * 24], dtype=[('s', '<u4'), ('p', '<f4', 3), ('w', '<u8')])
    return rec

def parts(k):
    poses = load(f'D:/sotc-pc/testrun/pose_{k}.bin')[:, :35]
    r = rays(f'D:/sotc-pc/testrun/rays_{k}.bin')
    r = r[r['s'] < len(poses)]
    rot, pos = poses[:, :, :3, :3], poses[:, :, 3, :3]
    inv = np.linalg.inv(rot)
    out = []
    for w in np.unique(r['w']):
        g = r[r['w'] == w]
        if len(g) < 8:
            out.append((w, len(g), -1, None)); continue
        best = None
        for b in range(35):
            local = np.einsum('nk,nkl->nl', g['p'] - pos[g['s'], b], inv[g['s'], b])
            c = local - local.mean(0)
            size = float(np.sqrt((c ** 2).sum(1).mean()))
            if best is None or size < best[0]:
                best = (size, b, local)
        size, b, local = best
        c = local - local.mean(0)
        _, sv, vt = np.linalg.svd(c, full_matrices=False)
        axis = vt[0]; along = c @ axis
        radial = np.linalg.norm(c - np.outer(along, axis), axis=1)
        out.append((w, len(g), b, dict(center=local.mean(0), axis=axis, length=float(np.ptp(along)),
                                         radius=float(np.median(radial)), rms=size)))
    return out

if __name__ == '__main__':
    for k in sys.argv[1:] or ['A', 'B']:
        res = parts(k)
        print(f'{k}: {len(res)} words, {sum(n for _, n, _, _ in res)} hits')
        for w, n, b, cap in sorted(res, key=lambda x: -x[1]):
            if cap is None:
                print(f'  word {w:#x}: {n} hits'); continue
            print(f'  word {w:#x}: {n:5d} hits, bone {b:2d}, center {np.round(cap["center"], 1)}, '
                  f'length {cap["length"]:.1f}, radius {cap["radius"]:.1f}, rms {cap["rms"]:.1f}')
