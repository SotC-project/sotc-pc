# Shadow of the Colossus (PS4, CUSA08809) on PC: wrapper and tools

Unofficial. Not affiliated with, endorsed by or connected to Sony Interactive Entertainment,
Bluepoint Games or the shadPS4 project.

This repository holds the scripts and settings around a game-specific build of
[shadPS4](https://github.com/shadps4-emu/shadPS4), a PlayStation 4 emulator. The build itself
lives in a second repository (the runtime fork, branch `sotc-pc`). Together they start one game,
Shadow of the Colossus (2018, CUSA08809), from a single program on Windows with keyboard, mouse
and controller support, PC graphics settings, a free play mode and an achievements panel.

It is not a recompilation or a decompilation. The game's own PlayStation 4 program runs
unchanged on the runtime, which translates its system and graphics calls to Windows and Vulkan.

## What you need

- Your own, legally obtained copy of the game, dumped from your own console. No game file is in
  this repository and none may be added.
- Windows 10 or 11, a Vulkan 1.3 graphics card. Developed and tested on one PC only: Ryzen 7
  5700X3D, Radeon RX 9060 XT 16 GB, 32 GB RAM. Intel and NVIDIA graphics are untested.
- To build: Visual Studio 2022 with CMake, and the LLVM/Clang toolchain the `build*.bat` files
  point to.

## How to play, step by step

No ready-made program is published. You build it once from source (steps 2 to 4); after that
you start the game from a desktop shortcut like any other.

### Getting it running

1. **Have the game on your PC.** You need the game's folder as dumped from your own PlayStation 4
   copy (about 14 GB; it holds `eboot.bin` and several `.psarc` files). Put it anywhere, for
   example `D:\Games\CUSA08809`. This project does not provide the game and cannot tell you how
   to get it.
2. **Install the build tools.** Visual Studio 2022 with the "Desktop development with C++"
   workload (it includes CMake), Git, and PowerShell 7.
3. **Get the source.** In a folder of your choice:
   ```
   git clone https://github.com/SotC-project/sotc-pc
   cd sotc-pc
   git clone --recursive -b sotc-pc https://github.com/SotC-project/sotc-pc-runtime runtime
   ```
   The build files expect the Clang compiler in `toolchain\clang+llvm-23.1.3-x86_64-pc-windows-msvc`.
   Download that release from the LLVM project and unpack it there, or change the `PATH` line
   near the top of `build-lto.bat` to where your Clang is.
4. **Build.** Double-click `build-lto.bat`. The first build takes a while. It ends with
   `runtime\Build\x64-Clang-Release\shadps4.exe`.
5. **Install.** In PowerShell, from the `dist` folder:
   ```
   pwsh -File Install.ps1 -GameDir "D:\Games\CUSA08809"
   ```
   This copies the program into `dist` as `ShadowOfTheColossus.exe`, remembers where your game
   folder is, and puts a "Shadow of the Colossus" shortcut on the desktop and in the Start menu.
6. **Start the game** from the shortcut.

### The first start

7. Two short messages appear, this once only.
8. You are asked whether to **prepare shaders**. Choose **Prepare** (recommended): the game
   builds its graphics programs now, which takes a few minutes once, and then plays without the
   small stops that building them during play would cause. **Skip** starts at once and prepares
   them as you go; expect short stops the first time you see each place.
9. The title menu appears: **New Game** (and **Continue** once you have a save), **Settings**,
   **Image Gallery**, **Free Play**, **Achievements**, **Quit**. Move with the arrow keys, the
   mouse or a controller; confirm with Enter, a click, or the bottom face button.

### Controls

A controller works as on the console; button pictures follow the device you last touched. These
are the default keys. Change any of them under Settings > Key Bindings.

| Action | Keyboard / mouse | Controller |
|---|---|---|
| Move | W A S D | Left stick |
| Look | Mouse | Right stick |
| Jump | Space | Cross / A |
| Attack, fire the bow | Left mouse button | Square / X |
| Grab, hold on, use, pray | E or right mouse button (hold) | R2 / RT |
| Crouch, roll | C or Left Ctrl | Circle / B |
| Raise the sword to the light, aim the bow | Q (hold) | R1 / RB |
| Mount and spur Agro | Left Shift | Triangle / Y |
| Walk instead of run (on and off) | Left Shift | (tilt the stick less) |
| View the colossus | R (hold) | L2 / LT |
| Camera focus | Z | L1 / LB |
| Change sword / change ranged weapon | Mouse wheel, or 1 and 2 | D-pad right / left |
| Unequip | X | D-pad up |
| Photo mode | P | D-pad down |
| World map | Tab | Touchpad / Back |
| Zoom | F | R3 |
| Pause | Esc | Options / Start |

With the bow: hold Q to aim, hold the left mouse button to draw, let go to shoot.

### Settings

10. Choose **Settings** on the title menu or in the pause menu. It offers **Graphics**,
    **Key Bindings** and **Game Settings** (the game's own display, audio, camera and controls).
11. The first start picks graphics settings from your graphics card and screen. To change them,
    open **Graphics** and choose a **Preset**: Medium is a good start; try High if the game runs
    well, Low if it is slow (Low is clearly the fastest). You can also set the frame rate limit
    (30 to 240, or uncapped), VSync, full screen, window size, FSR upscaling and each effect by
    itself. Then choose **Apply and Restart**; the game restarts by itself.
12. If the camera moves the wrong way for you, change it under Game Settings > Camera.

### Saving

13. The game saves by itself as you play. **Manual Save** in the pause menu saves on demand.
    Saves live in `dist\user`; keep that folder if you move or rebuild the program.
14. **Main Menu** in the pause menu goes back to the title menu. **Quit** on the title menu
    closes the game.

### Free Play and Time Attack

15. Choose **Free Play** on the title menu. Pick a **Colossus** and a **Difficulty** with left
    and right. The other lines (all weapons, invincibility, unlimited grip, maximum damage) are
    optional helpers.
16. Choose **Begin Battle**. The game restarts and loads that battle; this takes some seconds.
17. For a graded fight, turn **Time Attack** on first. The helpers are then off. A small clock
    appears top left and starts when you first move. When the colossus falls, or you do, a
    result page shows your time, the target time, the health you lost and a rank from D to S+.
18. During a free play battle, **F2** (or both sticks pressed together) opens a menu with
    **Return to Main Menu**.

### Achievements

19. **Achievements** on the title menu or in the pause menu lists all 38 with your progress.
    They unlock as the game awards its trophies, with a small notice on screen.

### If something goes wrong

- **The game stops and starts again by itself.** If the game's own program faults, the port
  starts the game again and says so on screen. Your progress up to the last save is kept. A
  fault that used to happen about once in nine returns to the title menu has been fixed; if
  you see a restart, please report it with the log (see below).
- **Short stops when entering a new area.** Choosing Prepare at the first start removes most of
  them; a level load can still stall for a second or so.
- **A message that the graphics driver stopped.** Seen with a screen recorder running on an AMD
  card. Close the recorder and start the game again.
- **Intel and NVIDIA graphics cards have not been tested.** It may work; nobody has checked.
- **To report a problem**, open an issue in this repository and attach `dist\user\log\shad_log.txt`
  from the run that went wrong. Say what you did, your graphics card and your driver version.
  Do not attach game files or saves.

## What is here

| Path | What it is |
|---|---|
| `build.bat`, `build-lto.bat`, `build-release.bat`, `build-pgo.bat` | Build the runtime (checked out in `runtime\`) |
| `dist\Install.ps1`, `Install.bat` | Copy the built program beside the scripts, create shortcuts |
| `dist\Settings.ps1`, `Settings.bat` | Graphics and display settings; also run by the in-game Settings page |
| `dist\FreePlay.ps1`, `Play.bat`, `PlayColossus.bat` | Start the game, or one colossus battle |
| `dist\glyphs.py`, `dist\glyphs\` | Draws the keyboard and Xbox button pictures that replace the PlayStation ones; the pictures are drawn by the script, not taken from the game |
| `dist\user\config.json`, `dist\user\input_config\` | Default settings and key bindings |
| `dist\ini_update.py` | Brings an older bindings file up to date |
| `cursor_art.py` | Draws the menu pointer (a stone arrowhead) into the runtime's source |
| `publish_filter.py` | The filter applied to the runtime before it is published |
| `test.sh`, `bossarena.sh`, `preptest.ps1`, `memwatch.ps1`, `freeplay_video.sh` and the other scripts | Scripted test runs, measurements and captures on a separate test profile |
| `showcase\BENCHMARK.md`, `PERFORMANCE.md`, `NOTES.txt` | Measurements and notes |
| `upstream\*.patch` | Two small general fixes kept apart from the game-specific work |

## What is not here

- Any file of the game: program, assets, saves, texture or memory dumps, trophy data.
- PlayStation system software, modules, keys or firmware.
- Shader and pipeline caches, logs, built programs, screenshots and video.
- The program icon used on the author's PC (it was drawn after another artist's work). The
  published runtime uses shadPS4's icon.

## Known limits

- One PC tested. A graphics driver reset (seen with a screen recorder running on AMD) closes the
  game with a message.
- Loading a new area can stall for a moment; level loads stall for about half a second to a
  second and a half once.
- Until 11 October 2026 the game faulted about once in nine returns to the title menu (14 times
  in 112 scripted runs). The cause was in the port: a read fault taken while Windows was
  re-making a mapping the game had partly unmapped was answered by copying old GPU buffer
  contents over the game's memory. With that corrected, 70 scripted runs of
  pause > Main Menu > Yes gave no fault (10 more on the release build). Should the game's own program fault for another
  reason, the port starts the game again and says so.

## Licence

The runtime is a derivative of shadPS4 and is licensed GPL-2.0-or-later, with shadPS4's
copyright notices kept. The scripts in this repository are under the same licence; see
`LICENSE`.
