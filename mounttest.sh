#!/bin/bash
# Mounts Agro in the fourth colossus's arena (he starts beside Wander) and rides: ./mounttest.sh <sheet.png>
SCRIPT="41:key=a*500,42:key=lshift*300,44:shot,45:key=a*300,46:key=lshift*300,48:shot,49:key=a*300,50:key=lshift*300,52:shot,54:key=w*5000,58:shot,62:shot" \
    /d/sotc-pc/bossarena.sh "$1" 72 +nointro +skipSplash=true +boss=3
