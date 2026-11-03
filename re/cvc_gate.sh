# Source after re/cvc_base.sh <boss>: replaces the timed live jump (106/107 s) with one that
# waits until colossus A (first of list world+0x238) has started moving (cave byte 0x1593ef8,
# set by awaitmoved=), so the jump never leaves a dormant, frozen A behind. 0x1593ef9 = jumped.
boss=$1
s="${s//,106:poke=325e8b4:$boss/}"
s="${s//,106:poke=325e8b8:0/}"
s="${s//,107:patch=15939f0:01/}"
s="$s,90:awaitmoved=238:200:1593ef8"
# A wakes only after its intro cinematic, which Wander's arrival triggers: warp him there again
# if it has not woken.
# Single retries only: warping again while the area streams restarts the load for good.
for t in 140 185; do
    s="$s,$t:ifzero=1593ef8:objlist=228:@310/90/-1440"
done
for t in $(seq 106 3 236); do
    g="ifset=1593ef8:ifzero=1593ef9"
    s="$s,$t:$g:poke=325e8b4:$boss,$t:$g:poke=325e8b8:0,$t:$g:patch=15939f0:01,$t:$g:pokeb=1593ef9:1"
done
