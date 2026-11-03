#!/bin/bash
# One colossus-vs-colossus matchup test: ./re/cvc_pair.sh <guest index> <host index> [seconds]
# The guest is brought into the host's arena (re/cvc_arena.sh GUEST/HOST), recorded at 10 fps from
# when they close in until 30 s after the knockout; prints one result line and keeps the log and
# a contact sheet (re/cvc_pairs/<guest>_<host>.*).
g=$1; h=$2; secs=${3:-420}
cd /d/sotc-pc
mkdir -p re/cvc_pairs
out=re/cvc_pairs/${g}_${h}
for i in $(seq 1 120); do tasklist | grep -qi JustCause4 || [ -e /d/jc4-tf2/GPU_RUNNING ] || break; sleep 5; done
rm -f E:/sotc-capture/rec_cvc.raw
GUEST=$g HOST=$h FILM=1 REACH=${REACH:-18} DMG_A=${DMG_A:-0.3} DMG_B=${DMG_B:-0.3} SOTC_RECORD=E:/sotc-capture/rec_cvc.raw \
    SOTC_RECORD_FPS=10 SHOTSTEP=600 STEER=2 CAMDIST=55 CAMHEIGHT=60 bash re/cvc_arena.sh $secs > $out.txt 2>&1
cp testrun/user/log/shad_log.txt $out.log 2>/dev/null
L=$out.log
r=$(head -1 $out.txt | grep -o "REFUSED.*")
hits=$(grep -c "cvcbrawl: . hit" $L)
win=$(grep -oE "cvcbrawl: . wins" $L | head -1 | cut -c11)
shown=$(grep -c "knockout shown" $L)
death=$(grep -oE "death motion 0x[0-9a-f]+ on" $L | head -1)
place=$(grep -oE "cvcplace: A to [^,]*" $L | head -1)
ready=$(grep -c "cvctarget: A" $L)
crash=$(grep -c "Unhandled Exception" $L)
echo "pair $g in $h: ${r:-ok} ready=$ready $place hits=$hits winner=${win:--} shown=$shown ${death:-} crash=$crash $(grep -E 'ran ' $out.txt)"
[ -f E:/sotc-capture/rec_cvc.raw ] && ffmpeg -v error -y -f rawvideo -pix_fmt rgb24 -s 960x540 -r 10 -i E:/sotc-capture/rec_cvc.raw \
    -vf "fps=1/3,scale=320:180,tile=6x8" -frames:v 1 $out.png
