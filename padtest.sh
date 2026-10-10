#!/bin/bash
# Gamepad path test with SDL's virtual pad: camera stick, movement stick, a face button.
# The real DualSense is hidden from the run so the virtual pad gets controller slot 0.
SCRIPT="46:shot,48:pad=rightx:1*1500,51:shot,53:pad=lefty:-1*3000,57:shot,59:pad=righty:1*800,61:shot,63:pad=a*300,64:shot" \
RUNENV="SOTC_VPAD=1 SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054C/0x0CE6" \
    /d/sotc-pc/bossarena.sh "$1" 72 +nointro +skipSplash=true +boss=0
