#!/bin/bash
# Continues the stored save to gameplay at the altar, then reports frame rate and renderer stats.
# Usage: ./gameplay.sh [VAR=value ...]
cd /d/sotc-pc || exit 1
SAVE=1 ./test.sh 112 SOTC_SCRIPT="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross,82:key=w*8000,98:shot,102:quit" "$@" | tail -3
L=testrun/user/log/shad_log.txt
grep "Flip: fps" $L | tail -8 | sed -E 's/^.*fps //' | tr '\n' ' '; echo
grep "Tick:" $L | tail -4 | sed -E 's/^.*Tick: //' | cut -c1-300
