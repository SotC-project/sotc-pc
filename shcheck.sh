#!/bin/bash
# Runs gameplay with the probe shader's buffers dumped and counts corrupt light-probe records
# (80-byte half-float records written on the GPU): ./shcheck.sh [VAR=value ...]
cd /d/sotc-pc && rm -f dist/buflog_*.raw dist/bufgpu_*.raw
./gameplay.sh SOTC_BUFLOG=79704cba "$@" | sed -n 1p
cd dist && python - <<'PY'
import numpy as np, glob, os
best = None
for f in glob.glob('bufgpu_*.raw'):
    if os.path.getsize(f) == 81920:
        best = f
if not best:
    raise SystemExit('no 80-byte record buffer dumped')
r = np.fromfile(best, dtype=np.uint8).reshape(1024, 80)
h = r.copy().view(np.float16).reshape(1024, 40).astype(np.float64)
used = np.nonzero(r.any(axis=1))[0]
fin = np.isfinite(h[used]); a = np.where(fin, np.abs(h[used]), 0)
bad = (a > 100) | ~fin
print(f'{best}: {len(used)} records, {int(bad.any(axis=1).sum())} corrupt, per column {np.nonzero(bad.sum(axis=0) > 20)[0].tolist()}')
PY
