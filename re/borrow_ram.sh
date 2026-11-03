#!/bin/bash
# Borrow the PC's RAM from the BT3 league (D:\bt3-league\PAUSE: its pcsx2-qt workers finish their fight
# and exit). Another session may remove PAUSE when its own batch ends, so a keeper sets it again every
# 10 s while a game of ours runs (D:\sotc-pc\GPU_RUNNING) or for the next 60 s. Waits at most $1 seconds
# (default 240) for the workers to leave. "borrow_ram.sh done" gives the PC back.
if [ "$1" = done ]; then touch /d/sotc-pc/RAM_DONE; rm -f /d/bt3-league/PAUSE; exit 0; fi
rm -f /d/sotc-pc/RAM_DONE
touch /d/bt3-league/PAUSE
( for ((i = 0; i < 6 || $([ -e /d/sotc-pc/GPU_RUNNING ] && echo 1 || echo 0); i++)); do
      [ -e /d/sotc-pc/RAM_DONE ] && break; touch /d/bt3-league/PAUSE; sleep 10; done ) > /dev/null 2>&1 &
for ((t = 0; t < ${1:-240}; t += 5)); do
    [ "$(tasklist | grep -ci pcsx2-qt)" = 0 ] && exit 0
    sleep 5
done
echo "league still running"; exit 1
