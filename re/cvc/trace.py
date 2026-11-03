"""Snaps in a CvC fight from the root trace (testrun/root_trace.bin, CVC_TRACE runs).

Each frame and colossus: time, root, what our code moved it by since the frame before, its motion
and frame, whether the bodies touch. A root's move that our code does not explain is the game's:
its motion's root motion (walks: under ~0.5 units a frame) or a correction (collision, ground,
navigation). Lists the frames where the unexplained move is a jump (default over 1.2 units).
Usage: python re/cvc/trace.py [threshold]"""
import struct
import sys

import numpy as np

RECORD = struct.Struct('<fI3f2ffQII')


def load(path='D:/sotc-pc/testrun/root_trace.bin'):
    raw = open(path, 'rb').read()
    rows = [RECORD.unpack_from(raw, i) for i in range(0, len(raw) - RECORD.size + 1, RECORD.size)]
    return rows


def snaps(rows, threshold):
    last = {}
    found = []
    for t, k, x, y, z, ox, oz, frame, motion, touching, _ in rows:
        if k in last:
            lt, lx, ly, lz, lmotion = last[k]
            moved = np.array([x - lx, z - lz])
            ours = np.array([ox, oz])
            game = moved - ours
            if np.linalg.norm(game) > threshold or abs(y - ly) > threshold:
                found.append((t, k, float(np.linalg.norm(game)), float(np.linalg.norm(ours)), y - ly, motion,
                              lmotion, frame, touching))
        last[k] = (t, x, y, z, motion)
    return found


if __name__ == '__main__':
    threshold = float(sys.argv[1]) if len(sys.argv) > 1 else 1.2
    rows = load()
    print(f'{len(rows)} records')
    found = snaps(rows, threshold)
    start = rows[0][0] if rows else 0.0
    for t, k, game, ours, dy, motion, before, frame, touching in found:
        print(f'{t - start:8.2f}s {"AB"[k]} game moved {game:5.1f} (ours {ours:4.1f}, up {dy:+5.1f}) '
              f'motion {motion:#x}{" (was " + hex(before) + ")" if motion != before else ""} frame {frame:5.1f}'
              f'{" touching" if touching else ""}')
    print(f'{len(found)} snaps over {threshold}')
