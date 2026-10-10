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

## What is here

| Path | What it is |
|---|---|
| `build.bat`, `build-lto.bat`, `build-release.bat`, `build-pgo.bat` | Build the runtime (checked out in `runtime\`) |
| `dist\Install.ps1`, `Install.bat` | Copy the built program beside the scripts, create shortcuts |
| `dist\Settings.ps1`, `Settings.bat` | Graphics and display settings; also run by the in-game PC Settings page |
| `dist\FreePlay.ps1`, `Play.bat`, `PlayColossus.bat` | Start the game, or one colossus battle |
| `dist\glyphs.py`, `dist\glyphs\` | Draws the keyboard and Xbox button pictures that replace the PlayStation ones; the pictures are drawn by the script, not taken from the game |
| `dist\user\config.json`, `dist\user\input_config\` | Default settings and key bindings |
| `dist\ini_update.py` | Brings an older bindings file up to date |
| `test.sh`, `bossarena.sh`, `preptest.ps1`, `memwatch.ps1`, `freeplay_video.sh` and the other scripts | Scripted test runs, measurements and captures on a separate test profile |
| `showcase\BENCHMARK.md`, `PERFORMANCE.md`, `NOTES.txt` | Measurements and notes |
| `upstream\*.patch` | Two small general fixes kept apart from the game-specific work |

## What is not here

- Any file of the game: program, assets, saves, texture or memory dumps, trophy data.
- PlayStation system software, modules, keys or firmware.
- Shader and pipeline caches, logs, built programs, screenshots and video.
- The program icon used on the author's PC (it was drawn after another artist's work), and the
  names and descriptions of the game's trophies. The published runtime shows "Trophy 1",
  "Trophy 2", ... in the achievements panel instead.

## Known limits

- One PC tested. A graphics driver reset (seen with a screen recorder running on AMD) closes the
  game with a message.
- Loading a new area can stall for a moment; level loads stall for about 1.6 s once.
- A rare crash inside the game's own code after pause > Quit Game > Yes (once in about six runs).

## Licence

The runtime is a derivative of shadPS4 and is licensed GPL-2.0-or-later, with shadPS4's
copyright notices kept. The scripts in this repository are under the same licence; see
`LICENSE`.
