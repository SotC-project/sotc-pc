#!/bin/bash
# Frame rate standing still in a far view: the Gaius arena as it starts, looking across the lake.
# ./farbench.sh [runs] [VAR=value ...]   (BUILD=Release for the release build; BOSS=<n> other arena)
# Prints per run the mean of the 3 s frame-rate reports over 45 s of standing, and the time the
# command thread spent per frame in draws, dispatches, copy-back and flushes.
runs=${1:-1}; shift
cd /d/sotc-pc || exit 1
for ((r = 0; r < runs; r++)); do
    RUNENV="$*" SCRIPT="50:shot" ./bossarena.sh E:/sotc-capture/unused.png 112 +nointro +skipSplash=true +boss=${BOSS:-2} > /dev/null 2>&1
    python - <<'PY'
import re
fps=[];st=[];tm=[]
for l in open('D:/sotc-pc/testrun/user/log/shad_log.txt',errors='ignore'):
    m=re.search(r'Flip: fps ([\d.]+)',l)
    if m: fps.append(float(m.group(1)))
    m=re.search(r'stats/3s: drawn (\d+) indirect (\d+) dispatch (\d+)',l)
    if m: st.append(list(map(int,m.groups())))
    m=re.search(r'time/3s ms: pipeline (\d+) bind (\d+) draws (\d+) dispatches (\d+) mirror (\d+) flushes (\d+)',l)
    if m: tm.append(list(map(int,m.groups())))
f=fps[-17:-2]; s=st[-17:-2]; t=tm[-17:-2]
if len(f) < 15: print('short run:', len(fps), 'reports')
else:
    frames=sum(f)*3
    print('%.1f FPS (low %.1f high %.1f) | per frame: indirect %d dispatch %d | ms: draws %.2f dispatch %.2f mirror %.2f flush %.2f pipeline %.2f bind %.2f' % (
        sum(f)/len(f), min(f), max(f), sum(x[1] for x in s)/frames, sum(x[2] for x in s)/frames,
        sum(x[2] for x in t)/frames, sum(x[3] for x in t)/frames, sum(x[4] for x in t)/frames, sum(x[5] for x in t)/frames, sum(x[0] for x in t)/frames, sum(x[1] for x in t)/frames))
PY
done
