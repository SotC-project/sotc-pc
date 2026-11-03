"""Convex hulls of the colossi's own collision parts, for the rigid-body CvC (any engine).

From the line-test samples (collision.py: rays_*.bin with pose_*.bin, CVC_PROBE_SKEL runs): each
part's hits, in the frame of the bone it rides on, trimmed of stray points (outside 1-99 % per
axis), split into about one cluster per 60 units of volume (at most 10; one hull of a curved shell
fills the hollow under it), each hulled and cut to at most 32 vertices (farthest-point picks over
the hull's corners). Writes re/cvc/hulls.txt: one line per hull: kind part bone volume count
x y z ... (bone-frame units; kind: the boss table's index, Valus 0, Quadratus 1, Gaius 2, Phaedra 3,
Basaran 8, Cenobia 13, Argus 14); then per colossus a line P kind p0 p1 ..., its skeleton's parents
(skeleton.py, -1 the root; as many as it has bones), and M kind n when its matrices list its n
bones once (not twice). Also copied to runtime/src/resources/cvc_hulls.txt.

The recordings: SOURCES below, each a probe folder, the slot (A guest, B host) and the colossus
in it; a colossus is taken from the first folder that has it.
Usage: python re/cvc/hulls.py   (prints a summary)"""
import os, sys
import numpy as np
from scipy.spatial import ConvexHull
from scipy.cluster.vq import kmeans2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from skeleton import load, parents
from collision import rays

SOURCES = [  # (folder, slot, kind)
    ('E:/sotc-capture/probe_2_8', 'A', 2),
    ('E:/sotc-capture/probe_2_8', 'B', 8),
    ('E:/sotc-capture/probe_2_3', 'B', 3),
    ('E:/sotc-capture/probe_2_0', 'B', 0),
    ('E:/sotc-capture/probe_2_1', 'B', 1),
    ('E:/sotc-capture/probe_2_13', 'B', 13),
    ('E:/sotc-capture/probe_2_14', 'B', 14),
]


def bones_of(poses):
    """How many bones a pose list holds: its matrices list them twice (and may carry a marker after);
    the Minotaurs' (Valus, Argus) once, up to the first that is no rotation."""
    det = np.linalg.det(poses[:, :, :3, :3])
    valid = np.all(np.abs(det - 1) < 0.05, axis=0)
    n = int(np.argmin(valid)) if not valid.all() else poses.shape[1]
    for half in range(n // 2, 7, -1):
        if np.abs(poses[0, :half, 3, :3] - poses[0, half:2 * half, 3, :3]).max() < 1e-3:
            return half
    return n


def doubled(poses, n):
    return poses.shape[1] >= 2 * n and np.abs(poses[0, :n, 3, :3] - poses[0, n:2 * n, 3, :3]).max() < 1e-3


def farthest(points, n):
    pick = [int(np.argmax(np.linalg.norm(points - points.mean(0), axis=1)))]
    d = np.linalg.norm(points - points[pick[0]], axis=1)
    while len(pick) < min(n, len(points)):
        pick.append(int(np.argmax(d)))
        d = np.minimum(d, np.linalg.norm(points - points[pick[-1]], axis=1))
    return points[pick]


def hulls(folder, slot):
    poses = load(f'{folder}/pose_{slot}.bin')
    n = bones_of(poses)
    twice = doubled(poses, n)
    poses = poses[:, :n]
    r = rays(f'{folder}/rays_{slot}.bin'); r = r[r['s'] < len(poses)]
    rot, pos = poses[:, :, :3, :3], poses[:, :, 3, :3]; inv = np.linalg.inv(rot)
    part = (r['w'] >> 24) & 0xff
    out = []
    for p in np.unique(part):
        g = r[part == p]
        if len(g) < 30:
            continue
        best = None
        for b in range(n):
            local = np.einsum('nk,nkl->nl', g['p'] - pos[g['s'], b], inv[g['s'], b])
            lo, hi = np.percentile(local, 1, 0), np.percentile(local, 99, 0)
            vol = float(np.prod(hi - lo + 0.1))
            if best is None or vol < best[0]:
                best = (vol, b, local, lo, hi)
        _, b, local, lo, hi = best
        local = local[np.all((local >= lo) & (local <= hi), axis=1)]
        pieces = [local]
        volume = float(ConvexHull(local).volume)
        clusters = int(min(10, max(1, round(volume / 60.0))))
        if clusters > 1 and len(local) > 30 * clusters:
            _, label = kmeans2(local, clusters, seed=1, minit='++')
            pieces = [local[label == i] for i in range(clusters) if (label == i).sum() >= 20]
        for piece in pieces:
            corners = piece[ConvexHull(piece).vertices]
            v = farthest(corners, 32)
            out.append((int(p), b, v, float(ConvexHull(v).volume) if np.ptp(v, 0).min() > 0.2 else 0.0))
    return out, parents(poses), twice


if __name__ == '__main__':
    lines, done = [], set()
    for folder, slot, kind in SOURCES:
        if kind in done or not os.path.exists(f'{folder}/pose_{slot}.bin'):
            continue
        done.add(kind)
        hs, tree, twice = hulls(folder, slot)
        print(f'kind {kind} ({folder} {slot}): {len(hs)} hulls, {len(tree)} bones')
        for p, b, v, vol in hs:
            if vol > 0.0:  # (flat: no body)
                lines.append(f'{kind} {p} {b} {vol:.2f} {len(v)} ' + ' '.join(f'{x:.3f}' for x in v.ravel()))
        lines.append(f'P {kind} ' + ' '.join(str(parent) for _, parent, _, _ in tree))
        if not twice:  # (its matrices list its bones once)
            lines.append(f'M {kind} {len(tree)}')
    here = os.path.dirname(os.path.abspath(__file__))
    for out in (os.path.join(here, 'hulls.txt'), os.path.join(here, '..', '..', 'runtime', 'src', 'resources', 'cvc_hulls.txt')):
        open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
