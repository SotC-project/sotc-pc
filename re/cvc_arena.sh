#!/bin/bash
# Colossus vs colossus from a direct arena boot (no story save walk): Free Play boot into Gaius's
# arena (+boss=2), wait until he is active and no cinematic shows, pin his packages and jump to
# Basaran (boss index 8), then set both up against each other with Wander as spectator.
# ./re/cvc_arena.sh [seconds]   (env CAMDIST, PATCHSETS)
# PRODUCT=<file>: write the battle script the Free Play page starts (runtime resource) instead of
# running a test: no shots, logging or quit, hour-long helpers, back to the title after the knockout.
set -e
cd /d/sotc-pc
secs=${1:-240}
source re/cvc_base.sh 8   # for the patch sets; its story-walk script $s is not used
# GUEST / HOST: boss indices (0-15, the boss table's order) of the colossus brought along and of
# the one whose arena the fight is in. Default Gaius (2) in Basaran's valley (8), the tuned pair.
GUEST=${GUEST:-2}; HOST=${HOST:-8}
eval "$(python re/cvc/target_patch.py)"
eval "$(python re/cvc/contact_patch.py)"
eval "$(python re/cvc/camera_patch.py)"
# Keep TGT2's object-update wrapper (0x746ee0 sets the slot to the object being updated).
TGT4="$(echo "$TGT2" | tr , '\n' | grep -E '^patch=(1593a00|1593a40|746ee0):' | paste -sd,),$TGT4"
STUB=803d6900000000740ec605600000000055e85aed23ff5d554889e553e96456fefe
s="12:patch=1593980:$STUB,12:patch=579000:e97ba90101,12:patch=7b95fc:4885c07403488b005dc3"
s="$s,12:awaitmoved=238:200:1593ef8:${CALM_A:-10}:8"   # 2 s without a cinematic, after his intro (it starts 1-3 s after he appears; a jump made before it never arrived), or 8 s
# The loading screen stays over the setup (Gaius's arena, his intro, the jump) until the fight is
# set up, the camera is on it and no cutscene plays (the host's first-sight scene, after which
# the guest is placed beside the host again).
s="0.1:cover=hold,$s"
for t in $(seq 20 1 240); do s="$s,$t:ifset=1593ef0:ifset=1593ae8:ifplay:cover=release"; done
# Behind it, the intros are skipped (Space held, the game's HOLD TO SKIP): Gaius's, until he is
# seen moving, and the host's first-sight scene once the fight is set up (skipped before that, the
# host was torn down and the game crashed).
for t in $(seq 6 3 120); do
    s="$s,$t:ifzero=1593ef8:key=space*2500"
done
# The host's first-sight scene: Space held 2.5 s every 3 s while it shows (pressed again sooner,
# the game's HOLD TO SKIP starts over).
for t in $(seq 20 3 120); do s="$s,$t.5:ifset=1593ef0:ifzero=1593b50:ifcine:key=space*2500"; done
eval "$(python re/cvc/contact_patch.py | grep -E '^(PARENTGUARD|SAFEWS)=')"
s="$s,12:${PARENTGUARD//,/,12:},12:${SAFEWS//,/,12:}"   # from the start: the jump's loading can need them
# The jump, once: Gaius's packages pinned (an extra reference each) so he stays loaded.
jump="ifset=1593ef8:ifzero=1593ef9"
for t in $(seq 15 2 200); do
    if [ -n "$PINS" ]; then   # PINS="da db ...": exactly these stage indices
        for id in $PINS; do s="$s,$t:$jump:pokeb=3026330>$id:+1"; done
    elif [ "$GUEST" = 2 ]; then
        for id in f8 f9 113 154 33b 33c 33f 340 67f ${EXTRA_PINS:-}; do s="$s,$t:$jump:pokeb=3026330>$id:+1"; done
    else
        s="$s,$t:$jump:pinloaded"   # any other guest: everything loaded now stays
    fi
    s="$s,$t:$jump:poke=325e8b4:$HOST,$t:$jump:poke=325e8b8:0,$t:$jump:patch=15939f0:01"
    s="$s,$t:$jump:pokeb=1593ef9:1"
done
# After the jump the second entry is first a stand-in that is destroyed: wait for B to move too.
s="$s,20:awaitmoved=238#1:300:1593efa:${CALM_B:-8}:0:${STILL_B:-6}"   # 1.6 s without a cinematic (at 1 s, now and then the host was not loaded yet)
ready="ifset=1593b08:ifset=1593b10:ifzero=1593ef0"
PATCHSETS=${PATCHSETS:-NC TGT4 CUR4 DUAL3 MGR SAFEWS PARENTGUARD}
[ "${STEER:-2}" = 2 ] && PATCHSETS="$PATCHSETS CAMHOOK"
for t in $(seq 20 ${SETUP_STEP:-2} 220); do   # (every 4 s, the setup waited up to 4 s a stage)
    s="$s,$t:ifset=1593efa:ifzero=1593ef0:cvctarget=238#0:238#1:$GUEST:$HOST"
    u=$((t + 1))
    for name in $PATCHSETS; do
        patches=${!name}
        s="$s,$u:$ready:${patches//,/,$u:$ready:}"
    done
    # The guest beside the host, kept on its ground; OPEN_AT=x/z puts it on the side toward that
    # point, else toward Wander; OPEN_AT=x/z+d also walks the host d units toward it after its
    # first-sight scene (Basaran: out from under the rock arch, into the open geyser field, past
    # the vent nearest the arch, whose eruptions hid the fight).
    [ "$GUEST$HOST" = 28 ] && OPEN_AT=${OPEN_AT-930/-900+230}
    s="$s,$u:$ready:cvcplace=${PLACE:-55}${OPEN_AT:+@$OPEN_AT}"
    s="$s,$u:$ready:pokeb=2dcbfb5:0"   # ShowHints off: no Dormin captions over the fight
    s="$s,$u:$ready:pokef=16049a8:0"   # HUD opacity (HUDAlpha option) 0: no health bars or meters
    # The HUD options' per-element modes (player health, stamina, weapon, position, boss health).
    for hud in 1604994 1604998 160499c 16049a0 16049a4; do s="$s,$u:$ready:poke=$hud:${HUDMODE:-0}"; done
    life=${PRODUCT:+3600}
    s="$s,$u:$ready:cvckeep=${life:-400}"
    s="$s,$u:$ready:cvcbrawl=${life:-400}:${DMG_A:-0.3}:${DMG_B:-0.25}:${REACH:-11}:${SWING:-60}:1200"
    s="$s,$u:$ready:cvccam=${life:-400}:${CAMDIST:-45}:${CAMHEIGHT:-60}:${STEER:-2}"
    [ -n "$HOLDKEY" ] && s="$s,$u:$ready:key=$HOLDKEY*300000"
    s="$s,$u:ifset=1593b08:ifset=1593b10:pokeb=1593ef0:1"
done
if [ -n "$PRODUCT" ]; then
    # Once the knockout has been shown, back to the title (as a finished time attack goes).
    # Once: 0x1593b68 marks it done (cvctarget zeroes it).
    for t in $(seq 60 2 3600); do
        s="$s,$t:ifset=1593ef0:ifset=1593b50:ifzero=1593b68:leavefreeplay,$t:ifset=1593ef0:ifset=1593b50:pokeb=1593b68:1"
    done
    printf %s "$s" > "$PRODUCT"
    echo "wrote $PRODUCT ($(wc -c < "$PRODUCT") bytes)"
    exit 0
fi
for t in $(seq 30 8 $((secs - 10))); do
    s="$s,$t:ifset=1593ef0:objlist=238"
done
# KILL=A|B KILLAT=<s>: that colossus's health to 0 at that time (defeat experiments).
if [ -n "$KILL" ]; then
    m=$([ "$KILL" = A ] && echo 1593e00 || echo 1593e08)
    s="$s,${KILLAT:-120}:ifset=1593ef0:pokef=$m>50:0"
fi
for t in $(seq 40 ${SHOTSTEP:-10} $((secs - 10))); do s="$s,$t:shot"; done
# FILM=1: record (SOTC_RECORD) from when the colossi close in until 30 s after the knockout, then quit.
if [ -n "$FILM" ]; then
    for t in $(seq 25 1 $((secs - 6))); do
        s="$s,$t:ifset=1593ef0:ifset=1593b60:ifzero=1593b58:rec=start,$t:ifset=1593ef0:ifset=1593b60:ifzero=1593b58:pokeb=1593b58:1"
        s="$s,$t.5:ifset=1593ef0:ifset=1593b50:quit"
    done
fi
[ -n "$EXTRA_SCRIPT" ] && s="$s,$EXTRA_SCRIPT"
s="$s,$((secs - 5)):quit"
printf %s "$s" > testrun/cvc_script.txt
SAVE=usercopy ./test.sh $((secs + 10)) SOTC_SCRIPT="@D:/sotc-pc/testrun/cvc_script.txt" \
    SOTC_FREEPLAY="+nointro +skipSplash=true +boss=$GUEST +shadow_noDamage=true +shadow_disableCameraAutoCenter=true ${EXTRA_ARGS:-}"
grep -E "cvcbrawl|awaitmoved|cvctarget: A|cvcbones: (obj|Wander)|cvchits: [AB]|wins|cvccam: Wander to|Unhandled Exception" \
    testrun/user/log/shad_log.txt | sed 's/.*lambda: //; s/.*ScriptTick: //' | uniq | tail -20
grep -E "cvchits: checks" testrun/user/log/shad_log.txt | sed 's/.*lambda: //' | tail -1
python re/cvc/probe_result.py | grep separation | tail -6 || true
