#!/bin/bash
# Starts a live-steered session (see live.sh) and returns once the game is in gameplay.
# The boot script is timed; if the menus were not ready it lands in a menu (frame rate above
# 90), in which case the session is restarted. Usage: ./livestart.sh [seconds] [VAR=value ...]
cd /d/sotc-pc || exit 1
secs=${1:-1800}; shift
for attempt in 1 2 3 4; do
    : > dist/live_cmds.txt
    (SAVE=1 ./test.sh "$secs" SOTC_SCRIPT_FILE="D:/sotc-pc/dist/live_cmds.txt" "$@" \
        SOTC_SCRIPT="40:options,42:down,43:down,44:down,46:cross,56:cross,63:cross" > /dev/null 2>&1 &)
    sleep 5
    until [ "$(grep -c 'Flip: fps' testrun/user/log/shad_log.txt 2>/dev/null)" -ge 30 ] ||
          [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus")" = "0" ]; do sleep 3; done
    fps=$(grep "Flip: fps" testrun/user/log/shad_log.txt | tail -1 | sed -E 's/^.*fps ([0-9]+).*/\1/')
    if [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus")" != "0" ] && [ "${fps:-999}" -lt 90 ]; then
        echo "in gameplay (attempt $attempt, $fps fps)"; exit 0
    fi
    echo quit >> dist/live_cmds.txt
    until [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus")" = "0" ]; do sleep 2; done
done
echo "could not reach gameplay"; exit 1
