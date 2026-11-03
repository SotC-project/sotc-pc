#!/bin/bash
# One CvC quality gate run: waits for the GPU to be quiet (no JC4) for 2 minutes, borrows the PC's RAM
# from the BT3 league, runs Gaius in host $1's arena for $2 seconds with the quality log, and prints
# the quality summary and the attacks' outcomes. Copies the run to E:/sotc-capture/gate_<host>/.
# ./re/cvc_gaterun.sh <host> [seconds]   (GUEST=<boss index>: another colossus brought in; default Gaius, 2)
h=$1; secs=${2:-150}
cd /d/sotc-pc
busy() { [ -e /d/jc4-tf2/GPU_RUNNING ] || tasklist | grep -qi "JustCause4.exe"; }
quiet=0
while [ $quiet -lt 120 ]; do if busy; then quiet=0; else quiet=$((quiet + 15)); fi; sleep 15; done
bash re/borrow_ram.sh 180
export CVC_QUALITY=1 SOTC_LOG_TIME=1
g=${GUEST:-2}
if [ "$g" = 2 ]; then
    timeout $((secs + 300)) bash re/cvc_matrix.sh "$h" "$secs" 2>&1 | tail -1
else  # another guest: the arena run itself (re/cvc_matrix.sh is Gaius's)
    rm -f /d/sotc-pc/testrun/cvc_quality.bin
    bash re/gpu_free.sh 500 && GUEST=$g HOST=$h timeout $((secs + 300)) bash re/cvc_arena.sh "$secs" > re/cvc_pairs/${g}_$h.txt 2>&1
    tail -2 re/cvc_pairs/${g}_$h.txt
    cp testrun/user/log/shad_log.txt re/cvc_pairs/2_$h.log
fi
rm -f GPU_RUNNING; rmdir testrun/.run_lock 2>/dev/null
mkdir -p E:/sotc-capture/gate_$h
cp testrun/cvc_quality.bin testrun/cvc_bones.bin re/cvc_pairs/2_$h.log E:/sotc-capture/gate_$h/ 2>/dev/null
python re/cvc/quality.py testrun/cvc_quality.bin re/cvc_pairs/2_$h.log
python re/cvc/attack_stats.py re/cvc_pairs/2_$h.log
grep -aoE "struck on its [a-z]+" re/cvc_pairs/2_$h.log | sort | uniq -c
