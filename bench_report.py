# Turns the files written by bench.sh into a table.
# Usage: python bench_report.py <bench dir>
# Measured window: the last 60 s of gameplay before the run's final 8 s (shots and quit).
import glob
import os
import sys

d = sys.argv[1]
rows = []
for ft in sorted(glob.glob(os.path.join(d, "*.ft"))):
    name = os.path.basename(ft)[:-3]
    preset, scene = name.split("_", 1)
    frames = []
    for line in open(ft):
        parts = line.split()
        if len(parts) == 2:
            frames.append((float(parts[0]), float(parts[1])))
    if len(frames) < 600:
        rows.append((preset, scene, None))
        continue
    end = frames[-1][0] - 8.0
    window = sorted(g for t, g in frames if end - 60.0 <= t <= end)
    if not window:
        rows.append((preset, scene, None))
        continue
    total = sum(window)
    avg = 1000.0 * len(window) / total
    # 1% low: average frame rate over the slowest 1% of frames.
    worst = window[-max(1, len(window) // 100):]
    low1 = 1000.0 * len(worst) / sum(worst)
    p99 = window[int(len(window) * 0.99)]
    hitches = sum(1 for g in window if g > 40.0)
    ram = vram = cpu = None
    res = os.path.join(d, name + ".res")
    if os.path.exists(res):
        samples = [list(map(int, l.split())) for l in open(res) if len(l.split()) == 4]
        if len(samples) >= 4:
            ram = max(s[1] for s in samples)
            vram = max(s[3] for s in samples)
            a, b = samples[len(samples) // 2], samples[-1]
            if b[0] > a[0]:
                cpu = (b[2] - a[2]) / (b[0] - a[0])  # cores busy, second half of the run
    rows.append((preset, scene, (avg, low1, p99, max(window), hitches, ram, vram, cpu)))

order = {"low": 0, "medium": 1, "high": 2, "ultra": 3}
rows.sort(key=lambda r: (r[1], order.get(r[0], 9)))
print("| Scene | Preset | Avg FPS | 1% low FPS | 99th pct frame ms | Worst frame ms | Frames >40 ms | RAM MB | VRAM MB | CPU cores busy |")
print("|---|---|---|---|---|---|---|---|---|---|")
for preset, scene, m in rows:
    if m is None:
        print(f"| {scene} | {preset} | run failed | | | | | | | |")
        continue
    avg, low1, p99, worst, hitches, ram, vram, cpu = m
    print(f"| {scene} | {preset} | {avg:.0f} | {low1:.0f} | {p99:.1f} | {worst:.0f} | {hitches} | "
          f"{ram if ram is not None else '?'} | {vram if vram else '?'} | "
          f"{f'{cpu:.1f}' if cpu is not None else '?'} |")
