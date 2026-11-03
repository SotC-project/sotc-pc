#!/bin/bash
# One matchup load test: Gaius (guest 2) into host $1's arena, the probe on (its skeleton and
# collision recorded); keeps the log, the probe data (E:/sotc-capture/probe_2_<host>) and a summary
# line in re/cvc_pairs/matrix.txt. ./re/cvc_matrix.sh <host index> [seconds]
h=$1; secs=${2:-200}
cd /d/sotc-pc
bash re/gpu_free.sh 500 || { echo "GPU busy"; exit 1; }
rmdir testrun/.run_lock 2>/dev/null
rm -f testrun/pose_*.bin testrun/rays_*.bin
GUEST=2 HOST=$h CVC_PROBE_SKEL=1 timeout $((secs + 150)) bash re/cvc_arena.sh $secs > re/cvc_pairs/2_$h.txt 2>&1
rm -f GPU_RUNNING
L=testrun/user/log/shad_log.txt
cp $L re/cvc_pairs/2_$h.log
mkdir -p E:/sotc-capture/probe_2_$h && cp testrun/pose_*.bin testrun/rays_*.bin E:/sotc-capture/probe_2_$h/ 2>/dev/null
placed=$(grep -c "fight placed" $L); hits=$(grep -c "cvcbrawl: . hit" $L); crash=$(grep -c "Unhandled Exception\|Device lost" $L)
invalid=$(grep -oE "INVALID RUN[^\r]*|FAILED RUN[^\r]*" re/cvc_pairs/2_$h.txt | head -1)
bones=$(python -c "import struct;r=open('testrun/pose_B.bin','rb').read();print(struct.unpack_from('<I',r,0)[0])" 2>/dev/null)
echo "2 in $h: placed=$placed hits=$hits crash=$crash bones_B=${bones:-none} ${invalid}" | tee -a re/cvc_pairs/matrix.txt
