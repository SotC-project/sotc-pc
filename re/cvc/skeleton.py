"""Skeleton parents from recorded poses (testrun/pose_A.bin, pose_B.bin; CVC_PROBE_SKEL runs).

Each sample: u32 count, then count 4x4 float matrices (rows: axes, translation in row 3). A bone
rides rigidly on its parent: its position in the parent's frame stays the same in every sample,
so its parent is the earlier-or-any bone with the steadiest such offset (ties: the nearest).
Usage: python re/cvc/skeleton.py [A|B]   prints index, parent, spread, offset length."""
import sys, struct
import numpy as np

def load(path):
    raw = open(path, 'rb').read()
    poses, at = [], 0
    while at + 4 <= len(raw):
        n = struct.unpack_from('<I', raw, at)[0]; at += 4
        poses.append(np.frombuffer(raw, '<f4', n * 16, at).reshape(n, 4, 4)); at += n * 64
    n = min(len(p) for p in poses)
    return np.stack([p[:n] for p in poses])            # samples x bones x 4 x 4

def parents(m):
    """Rigid riding goes both ways, so: the spread of every pair, a minimum spanning tree over it
    (ties to the nearer bone), hung from bone 0."""
    s, n = m.shape[:2]
    rot, pos = m[:, :, :3, :3], m[:, :, 3, :3]
    inv = np.linalg.inv(rot)
    spread = np.zeros((n, n)); length = np.zeros((n, n))
    for i in range(n):
        local = np.einsum('sjk,skl->sjl', pos - pos[:, i:i + 1], inv[:, i])   # every bone in i's frame
        mean = local.mean(0)
        spread[i] = np.percentile(np.linalg.norm(local - mean, axis=2), 95, axis=0)
        length[i] = np.linalg.norm(mean, axis=1)
    weight = np.maximum(spread, spread.T) + 0.001 * np.maximum(length, length.T)
    parent = [-1] * n; done = {0}
    best = {j: (weight[0, j], 0) for j in range(1, n)}
    while best:
        j = min(best, key=lambda x: best[x][0]); w, i = best.pop(j)
        parent[j] = i; done.add(j)
        for x in best:
            if weight[j, x] < best[x][0]:
                best[x] = (weight[j, x], j)
    return [(j, parent[j], float(spread[parent[j], j]) if parent[j] >= 0 else 0.0,
             float(length[parent[j], j]) if parent[j] >= 0 else 0.0) for j in range(n)]

if __name__ == '__main__':
    for k in sys.argv[1:] or ['A', 'B']:
        m = load(f'D:/sotc-pc/testrun/pose_{k}.bin')[:, :35]  # (35-69 repeat 0-34)
        print(f'{k}: {m.shape[0]} samples, {m.shape[1]} bones')
        for j, i, spread, length in parents(m):
            print(f'  {j:3d} <- {i:3d}   spread {spread:6.3f}  offset {length:7.2f}')
