#!/bin/bash
# Records the free play flow with the runtime's own frame capture (no screen recorder):
# title menu > Free Play > options > Begin Battle > the battle. The game restarts itself for the
# battle, so two raw files are written and joined: ./freeplay_video.sh <out.mp4>
out=$1
cd /d/sotc-pc/testrun || exit 1
for ((w = 0; w < 120; w++)); do [ "$(tasklist | grep -ci "i3.exe")" = "0" ] && break; sleep 5; done
if [ "$(tasklist | grep -ciE "shadps4|ShadowOfTheColossus|i3.exe|SnippingTool")" != "0" ]; then echo "REFUSED: a game instance or the screen recorder is running"; exit 1; fi
sd=user/home/1000/savedata
marker=user/home/1000/.test_owns_savedata
if [ -e "$marker" ]; then [ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
elif [ -d "$sd" ]; then [ -d "$sd.user" ] && mv "$sd.user" "$sd.user.prev$(date +%s)"; mv "$sd" "$sd.user"; fi
touch "$marker"
cp -r "$sd.fixture" "$sd"
f="/d/Games/CUSA08809T-mods/CommandLineArgs.txt"
cp "$f" "$f.bak"; printf '+nointro\r\n+skipSplash=true\r\n' >> "$f"
rm -f rec_*.raw
# First instance: into the field, pause > Quit Game > Yes to reach the title menu, then record.
first="40:key=escape,43:key=down,44:key=down,45:key=down,47:key=enter,49:key=left,50:key=enter"
first="$first,68:rec=ui,71:key=down,72:key=down,73:key=down,75:key=enter"
first="$first,78:key=right,79:key=right,81:key=down,82:key=right,84:key=down,85:key=right,87:key=left"
first="$first,89:key=down,90:key=right,92:key=down,93:key=down,94:key=down,95:key=down,97:key=enter,101:rec=stop,106:quit"
# Second instance (the battle): record from the start, move, look, swing, then stop.
second="4:rec=ui,40:key=w*5000,46:mouse=40*1500,49:key=w*5000,55:key=leftbutton,57:key=leftbutton,59:key=space,62:mouse=-40*1500,66:key=w*4000,72:rec=stop,74:quit"
env SOTC_HEADLESS=1 SOTC_MUTE=1 SOTC_SCRIPT="$first" SOTC_SCRIPT_NEXT="$second" SOTC_RECORD="rec_%p.raw" \
    ../runtime/Build/x64-Clang-RelWithDebInfo/shadps4.exe -f false --game "D:/Games/CUSA08809T/eboot.bin" > /dev/null 2>&1 &
sleep 20
for ((i = 0; i < 230; i++)); do
    [ "$(tasklist | grep -ci "shadps4.exe")" = "0" ] && break
    [ "$(tasklist | grep -ci ShadowOfTheColossus)" != "0" ] && { echo "ABORTED: the user's game started"; break; }
    sleep 1
done
taskkill //IM shadps4.exe //F > /dev/null 2>&1
sleep 2
mv "$f.bak" "$f"
[ -d "$sd" ] && mv "$sd" "$sd.bak$(date +%s)"
[ -d "$sd.user" ] && cp -r "$sd.user" "$sd"
rm -f "$marker"
ls -la --time-style=long-iso rec_*.raw
# Oldest file first: the menu, then the battle.
list=$(ls -tr rec_*.raw)
rm -f rec_all.raw; for r in $list; do cat "$r" >> rec_all.raw; done
ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s 960x540 -r 30 -i rec_all.raw -vf scale=1280:720:flags=lanczos -c:v libx264 -crf 20 -pix_fmt yuv420p "$out" && ls -la "$out"
rm -f rec_all.raw
